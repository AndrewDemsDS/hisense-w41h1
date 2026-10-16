// Edge cases and stress for the bus scheduler (firmware/esphome/components/hisense_ac/hisense_bus.*)
// against a simulated A/C and a controllable millisecond clock.
//
// test_esphome_bus.cpp pins the scheduler's contract on a healthy bus. This file goes after the
// edges: the 32 bit clock wrapping under every deadline, a main loop that is late or blocked, a unit
// that is silent, half answering or babbling, damaged and misplaced replies, command bursts, the
// receive path at its limits and the DE line. Three things carry most of the weight:
//
//   1. Invariants checked after EVERY poll of every scenario (Rig::check_): state in range, counters
//      monotonic and consistent with what the listener was told, DE high exactly while
//      wants_fast_loop() says so, no read while transmitting, every frame on the wire well formed,
//      never two transactions open, every accepted command sent one to three times and reported,
//      and the scheduler always moving.
//   2. Time translation. Nothing here may depend on the absolute value of millis(), so each
//      deterministic scenario is run from several start times, some of them a few seconds before
//      2^32 ms, and the complete trace (writes, DE edges, listener calls, all relative to the start)
//      has to be identical to the reference run. Any deadline that mishandles the wrap changes it.
//   3. Seeded random runs: the unit misbehaves at random, the loop is late at random, commands
//      arrive at random, and the invariants plus a convergence check at the end have to hold.
//
// A defect found here is registered with XFAIL: the assertion states the behaviour that would be
// right, the suite stays green while the defect is there, and it goes red the day the defect is
// fixed, so the registration is removed and not forgotten.
//
// The unit here is stricter and nastier than test_esphome_bus.cpp's: it has one transmitter (a late
// reply delays the next one instead of overlapping it), it can lose what it sends while our DE is
// high (half duplex), its UART buffer can be finite, and it can insist on the DevType probe before
// it answers anything else. HisenseAC itself cannot be built on a host; MiniHub below restates its
// main command path over the same hisense_map.h decisions, so what it shows about the hub is the
// evidence of a model.
//
// Build and run (from firmware/test, about 1 s to run; -O1 because every poll is checked):
//   g++ -std=c++17 -Wall -O1 -I../esphome/components/hisense_ac test_esphome_bus_stress.cpp
//       ../esphome/components/hisense_ac/hisense_bus.cpp
//       ../esphome/components/hisense_ac/hisense_protocol.cpp -o test_esphome_bus_stress.bin
// `--soak [seeds]` runs the random part for longer (2000 seeds of 10 min bus time by default, not
// part of the default suite). `--seed N` runs one random seed and prints its configuration.
// `--traces` prints a hash of every scenario's trace, to compare two builds of the scheduler.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "hisense_bus.h"
#include "hisense_commands.h"
#include "hisense_map.h"
#include "hisense_protocol.h"

namespace H = esphome::hisense_ac;

static int g_fail = 0;
static unsigned long long g_checks = 0;
static int g_known = 0;
static int g_printed = 0;
static const char *g_printed_ctx = nullptr;
static const char *g_ctx = "";
static uint32_t g_ctx_start = 0;

#define CHECK(cond, ...) \
  do { \
    g_checks++; \
    if (!(cond)) { \
      g_fail++; \
      if (g_printed_ctx != g_ctx) { \
        g_printed_ctx = g_ctx; \
        g_printed = 0; \
      } \
      if (g_printed++ < 6) { \
        printf("  FAIL %s:%d [%s, start %u] ", __FILE__, __LINE__, g_ctx, (unsigned) g_ctx_start); \
        printf(__VA_ARGS__); \
        printf("\n"); \
      } \
    } \
  } while (0)

// A known defect. `right` is the behaviour that would be correct. While it is false the defect is
// counted and the run stays green. If it turns true the defect is gone: fail, so this line is
// turned into a CHECK.
#define XFAIL(right, ...) \
  do { \
    g_checks++; \
    if (right) { \
      g_fail++; \
      printf("  FIXED? %s:%d [%s] this known defect no longer shows, make it a CHECK: ", __FILE__, __LINE__, g_ctx); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } else { \
      g_known++; \
    } \
  } while (0)

static uint32_t g_now = 0;

// Wrap-safe "t is not after now", for times less than 2^31 ms apart.
static bool not_after(uint32_t t, uint32_t now) { return static_cast<int32_t>(now - t) >= 0; }

// ---- Deterministic random numbers ----------------------------------------------------------------
struct Rng {
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed * 2654435761u + 0x9E3779B9u) {
    if (s == 0)
      s = 1;
  }
  uint32_t next() {  // xorshift32
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  uint32_t below(uint32_t n) { return next() % n; }
  uint32_t range(uint32_t lo, uint32_t hi) { return lo + below(hi - lo + 1); }
  bool one_in(uint32_t n) { return below(n) == 0; }
};

// ---- The trace: everything observable, relative to the start time ---------------------------------
struct Trace {
  uint32_t start = 0;
  uint64_t hash = 1469598103934665603ull;  // FNV-1a
  uint32_t entries = 0;
  void byte(uint8_t b) {
    hash ^= b;
    hash *= 1099511628211ull;
  }
  void word(uint32_t v) {
    for (int i = 0; i < 4; i++)
      byte(static_cast<uint8_t>(v >> (8 * i)));
  }
  void note(uint8_t kind, uint32_t a = 0, const uint8_t *p = nullptr, size_t n = 0) {
    entries++;
    byte(kind);
    word(g_now - start);
    word(a);
    for (size_t i = 0; i < n; i++)
      byte(p[i]);
  }
};

// ---- Frames as the A/C sends them ----------------------------------------------------------------
// Framing from hisense_protocol.h: F4 F5 start, F4 FB end, byte 2 = 0x01 for A/C to module (what
// the assembler accepts), byte 3 = 0x40 (two byte checksum), byte 4 = total length - 9, class at
// byte 13, checksum over [2, len - 4).
static void seal(std::vector<uint8_t> &f) {
  size_t len = f.size();
  uint16_t sum = H::checksum_range(f.data(), 2, len - 4);
  f[len - 4] = static_cast<uint8_t>(sum >> 8);
  f[len - 3] = static_cast<uint8_t>(sum);
  f[len - 2] = 0xF4;
  f[len - 1] = 0xFB;
}

static std::vector<uint8_t> ac_frame(size_t len, uint8_t cls, uint8_t sub) {
  std::vector<uint8_t> f(len, 0);
  f[0] = 0xF4;
  f[1] = 0xF5;
  f[2] = 0x01;
  f[3] = 0x40;
  f[4] = static_cast<uint8_t>(len - 9);
  f[13] = cls;
  f[14] = sub;
  seal(f);
  return f;
}

// Undo the doubling of a 0xF4 checksum byte. Only our own frames need it (the builders stuff, the
// simulated unit reads them).
static std::vector<uint8_t> unstuff(const std::vector<uint8_t> &w) {
  if (w.size() < 5)
    return w;
  size_t want = static_cast<size_t>(w[4]) + H::FRAME_LEN_OVERHEAD;
  if (w.size() <= want || want < 6)
    return w;
  std::vector<uint8_t> out(w.begin(), w.begin() + static_cast<long>(want - 4));
  size_t i = want - 4;
  for (int k = 0; k < 2 && i < w.size(); k++) {
    out.push_back(w[i]);
    if (w[i] == 0xF4 && i + 1 < w.size() && w[i + 1] == 0xF4)
      i++;
    i++;
  }
  for (; i < w.size(); i++)
    out.push_back(w[i]);
  return out;
}

// ---- The simulated unit and wire -----------------------------------------------------------------
enum Fault : uint8_t {
  FAULT_NONE,
  FAULT_DROP,         // no reply
  FAULT_LATE,         // reply delayed by `arg` ms on top of the usual latency
  FAULT_BAD_SUM,      // one checksum bit flipped
  FAULT_TRUNCATE,     // cut after `arg` bytes
  FAULT_WRONG_CLASS,  // a frame of another class instead
  FAULT_DUPLICATE,    // the reply twice, back to back
  FAULT_GARBAGE,      // `arg` noise bytes (none of them 0xF4) ahead of the reply
  FAULT_LONE_F4,      // a single 0xF4 ahead of the reply
  FAULT_STX_PAIR,     // F4 F5 ahead of the reply (a frame start that goes nowhere)
  FAULT_FLIP,         // one bit of body byte 16 + `arg` % 2 flipped on the way, checksum left as sent
  FAULT_KIND_COUNT,
};

struct FaultSpec {
  Fault kind;
  uint32_t arg;
};

struct Write {
  uint32_t t;
  bool de_high;
  uint32_t frames_seen;  // listener frame count when this write happened
  uint32_t closures;     // listener frames + timeouts when this write happened
  std::vector<uint8_t> bytes;
};

struct DeEdge {
  uint32_t t;
  bool high;
};

class Rec;

class Wire : public H::BusIO {
 public:
  // The unit.
  bool silent = false;         // answers nothing
  bool only_devtype = false;   // answers the DevType probe and nothing else
  int deaf_commands = 0;       // 0x65 frames to miss from now on; -1 = all of them
  bool takes_nothing = false;  // acknowledges every command and applies none
  // hisense_protocol.h: "The A/C ignores the status poll until it has seen 0x0A and 0x07 once".
  // With strict_link a unit that is not linked answers the DevType probe and nothing else; clear
  // `linked` to model a unit that lost power.
  bool strict_link = false;
  bool linked = true;
  uint32_t reply_delay = 40;  // ms between our last byte and its first
  // Device type in the DevType reply, frame bytes 16/17 (devtype_from_reply). 01 02 is the pair
  // test_esphome_bus.cpp uses; the scheduler's default before any reply is 01 01.
  uint8_t dev_hi = 0x01, dev_lo = 0x02;
  uint8_t link_bits_once = 0;  // request bits for byte 17 of the next 0x1E reply
  bool power = true;
  uint8_t mode = 2;  // H::MODE_COOL, which the status frame reports as nibble 2
  uint8_t setpoint = 22;
  std::function<FaultSpec(uint8_t cls, uint8_t sub)> fault;
  Rng noise{12345};

  // The wire and the UART.
  bool echo = false;            // the transceiver hears our own frames (receiver always enabled)
  bool half_duplex = false;     // bytes the unit sends while our DE is high are lost
  bool blocking_write = false;  // bus_write returns once the bytes are out (LibreTiny)
  size_t fifo_cap = 0;          // receive buffer size, 0 = unbounded

  // What happened.
  std::vector<Write> writes;
  std::vector<DeEdge> de;
  bool de_high = false;
  uint32_t de_rise = 0;
  uint32_t reads_while_de = 0, writes_without_de = 0, flushes = 0, overruns = 0, collided = 0;
  uint32_t heard_commands = 0, replies = 0;
  Trace *trace = nullptr;
  Rec *rec = nullptr;
  bool has_de = true;

  void bus_set_de(bool high) override;
  void bus_write(const uint8_t *data, size_t len) override;
  void bus_flush() override { flushes++; }
  int bus_read() override;

  // Put bytes on the wire towards us, one per ms from `at`.
  void inject(const std::vector<uint8_t> &f, uint32_t at) {
    for (size_t i = 0; i < f.size(); i++)
      push_(at + static_cast<uint32_t>(i), f[i]);
  }
  // The unit stops mid-frame: everything not yet on the wire is gone.
  void cut() {
    while (!rx_.empty() && !not_after(rx_.back().t, g_now))
      rx_.pop_back();
  }
  size_t rx_pending() const { return rx_.size(); }
  std::vector<uint8_t> status_frame() const;

 private:
  struct Rx {
    uint32_t t;
    uint8_t b;
  };
  void push_(uint32_t t, uint8_t b) {
    // Kept in arrival order even when a late frame and the next reply overlap in time.
    auto it = rx_.end();
    while (it != rx_.begin() && static_cast<int32_t>((it - 1)->t - t) > 0)
      --it;
    rx_.insert(it, Rx{t, b});
  }
  void send_(const std::vector<uint8_t> &f, uint32_t at) {
    // The unit has one transmitter: a frame waits for the one before it (a late reply, say).
    if (tx_busy_ && static_cast<int32_t>(tx_end_ - at) >= 0)
      at = tx_end_ + 1;
    tx_busy_ = true;
    tx_end_ = at + static_cast<uint32_t>(f.size());
    // A checksum byte of 0xF4 is doubled on the wire, as the real unit does.
    uint32_t t = at;
    for (size_t i = 0; i < f.size(); i++) {
      push_(t, f[i]);
      if ((i == f.size() - 4 || i == f.size() - 3) && f[i] == 0xF4 && f.size() >= 6)
        push_(t, 0xF4);
      t++;
    }
  }
  void respond_(const std::vector<uint8_t> &req);
  void apply_command_(const std::vector<uint8_t> &req);
  std::deque<Rx> rx_;
  bool tx_busy_ = false;
  uint32_t tx_end_ = 0;
};

// ---- The listener --------------------------------------------------------------------------------
struct LinkEdge {
  uint32_t t;
  bool up;
};

struct CmdReport {
  uint32_t t;
  bool answered;
  uint8_t cls;
  bool resend;
  uint8_t verdict;
  uint16_t tag;
};

class Rec : public H::BusListener {
 public:
  Wire *ac = nullptr;
  H::BusScheduler *bus = nullptr;
  Trace *trace = nullptr;
  uint32_t statuses = 0, features = 0, frames = 0, checksum_errors = 0, timeouts = 0;
  H::AcState last{};
  std::vector<LinkEdge> links;
  std::vector<CmdReport> commands;
  std::vector<std::pair<uint32_t, uint8_t>> setup_requests;
  std::function<void(const H::AcState &)> status_hook;
  std::function<void(bool)> link_hook;
  std::function<void(bool)> command_hook;

  void on_bus_status(const H::AcState &s) override {
    statuses++;
    last = s;
    trace->note(
        'S', static_cast<uint32_t>(s.setpoint_c) | (s.power_on ? 0x100u : 0u) | (static_cast<uint32_t>(s.mode) << 12));
    if (status_hook)
      status_hook(s);
  }
  void on_bus_features(const H::AcFeatures &) override {
    features++;
    trace->note('P');
  }
  void on_bus_link(bool up) override {
    links.push_back({g_now, up});
    trace->note('L', up);
    if (link_hook)
      link_hook(up);
  }
  void on_bus_frame(const uint8_t *f, size_t len) override {
    frames++;
    trace->note('F', static_cast<uint32_t>(len), f, len);
  }
  void on_bus_checksum_error(const uint8_t *, size_t) override {
    checksum_errors++;
    trace->note('X');
  }
  void on_bus_timeout(uint8_t expect) override {
    timeouts++;
    trace->note('T', expect);
  }
  void on_bus_command(bool answered, uint8_t reply_class) override {
    // Called from inside the command's own transaction, so the last write is that command.
    uint16_t tag = 0;
    if (!ac->writes.empty() && ac->writes.back().bytes.size() >= H::CMD_FRAME_LEN)
      tag = static_cast<uint16_t>((ac->writes.back().bytes[44] << 8) | ac->writes.back().bytes[45]);
    commands.push_back({g_now, answered, reply_class, bus->command_resend_pending(), bus->command_reply(), tag});
    trace->note('C', (answered ? 1u : 0u) | (static_cast<uint32_t>(reply_class) << 8) |
                         (bus->command_resend_pending() ? 0x10000u : 0u));
    if (command_hook)
      command_hook(answered);
  }
  void on_bus_setup_request(uint8_t request) override {
    setup_requests.push_back({g_now, request});
    trace->note('R', request);
  }
};

void Wire::bus_set_de(bool high) {
  de.push_back({g_now, high});
  trace->note('D', high);
  if (high && !de_high)
    de_rise = g_now;
  if (!high && de_high && half_duplex && !echo) {
    // Whatever the unit sent while we were driving the bus never reached the receiver.
    for (auto it = rx_.begin(); it != rx_.end();) {
      if (not_after(de_rise, it->t) && not_after(it->t, g_now)) {
        it = rx_.erase(it);
        collided++;
      } else {
        ++it;
      }
    }
  }
  de_high = high;
}

void Wire::bus_write(const uint8_t *data, size_t len) {
  if (has_de && !de_high)
    writes_without_de++;
  writes.push_back({g_now, de_high, rec->frames, rec->frames + rec->timeouts, std::vector<uint8_t>(data, data + len)});
  trace->note('W', static_cast<uint32_t>(len), data, len);
  if (echo)
    inject(writes.back().bytes, g_now);
  respond_(writes.back().bytes);
  if (blocking_write)
    g_now += H::bus_tx_time_ms(len);
}

int Wire::bus_read() {
  if (de_high)
    reads_while_de++;
  if (rx_.empty() || !not_after(rx_.front().t, g_now))
    return -1;
  if (fifo_cap > 0) {
    // Bytes that arrived since the last read and did not fit the receive buffer are lost.
    size_t arrived = 0;
    while (arrived < rx_.size() && not_after(rx_[arrived].t, g_now))
      arrived++;
    if (arrived > fifo_cap) {
      overruns += static_cast<uint32_t>(arrived - fifo_cap);
      rx_.erase(rx_.begin() + static_cast<long>(fifo_cap), rx_.begin() + static_cast<long>(arrived));
    }
  }
  int b = rx_.front().b;
  rx_.pop_front();
  return b;
}

// The 160 byte status frame (LEN 0x97, hisense_protocol.h). Byte 16 fan 0x0A = low, byte 18 = mode
// nibble and run bits (0x04 = running), byte 19 setpoint and byte 20 indoor in whole degrees C:
// the same layout test_esphome_bus.cpp feeds the parser.
std::vector<uint8_t> Wire::status_frame() const {
  auto f = ac_frame(160, 0x66, 0x00);
  f[16] = 0x0A;
  f[18] = static_cast<uint8_t>((mode << 4) | (power ? 0x04 : 0x00));
  f[19] = setpoint;
  f[20] = 23;
  seal(f);
  return f;
}

// What a command frame does to the unit. Byte 18: the literal power frames carry 0x0C (on) and 0x04
// (off) with no mode; a combined frame carries (mode * 2 + 1) << 4, plus 0x0C when it also switches
// the unit on. Byte 19 is setpoint * 2 + 1, 0 when the frame carries none (build_command).
void Wire::apply_command_(const std::vector<uint8_t> &req) {
  if (req.size() < H::CMD_FRAME_LEN || takes_nothing)
    return;
  uint8_t b18 = req[18];
  if ((b18 & 0xF0) == 0) {
    if ((b18 & 0x0C) == 0x0C)
      power = true;
    else if ((b18 & 0x0C) == 0x04)
      power = false;
    return;
  }
  if ((b18 & 0x0C) == 0x0C)
    power = true;
  if (!power)
    return;  // a powered-down unit ignores a mode change
  mode = static_cast<uint8_t>(((b18 >> 4) - 1) / 2);
  if (req[19] != 0)
    setpoint = static_cast<uint8_t>((req[19] - 1) / 2);
}

void Wire::respond_(const std::vector<uint8_t> &wire_req) {
  std::vector<uint8_t> req = unstuff(wire_req);
  if (silent || req.size() <= 14)
    return;
  uint8_t cls = req[13];
  uint8_t sub = req[14];
  if (only_devtype && cls != 0x0A)
    return;
  if (cls == 0x0A)
    linked = true;
  if (strict_link && !linked)
    return;
  uint32_t at = g_now + H::bus_tx_time_ms(wire_req.size()) + reply_delay;
  std::vector<uint8_t> f;
  if (cls == 0x0A) {
    f = ac_frame(24, 0x0A, 0x04);
    f[16] = dev_hi;
    f[17] = dev_lo;
  } else if (cls == 0x07) {
    f = ac_frame(20, 0x07, 0x00);
  } else if (cls == 0x1E) {
    f = ac_frame(28, 0x1E, 0x00);
    f[17] = link_bits_once;  // request bits, LINK_FLAGS_OFFSET
    link_bits_once = 0;
  } else if (cls == 0x66 && sub == 0x40) {
    f = ac_frame(64, 0x66, 0x40);
    f[18] = 0x80;  // cool_heat, parse_features
  } else if (cls == 0x66) {
    f = status_frame();
  } else if (cls == 0x65) {
    if (deaf_commands != 0) {
      if (deaf_commands > 0)
        deaf_commands--;
      return;
    }
    heard_commands++;
    apply_command_(req);
    f = ac_frame(20, 0x65, sub);
  } else {
    return;
  }
  seal(f);
  FaultSpec fs = fault ? fault(cls, sub) : FaultSpec{FAULT_NONE, 0};
  switch (fs.kind) {
    case FAULT_DROP:
      return;
    case FAULT_LATE:
      at += fs.arg;
      break;
    case FAULT_BAD_SUM:
      f[f.size() - 3] ^= 0x01;
      break;
    case FAULT_FLIP:
      f[16 + fs.arg % 2] ^= 0x40;
      break;
    case FAULT_TRUNCATE:
      f.resize(1 + fs.arg % (f.size() - 1));
      inject(f, at);  // no end tag left to stuff around
      replies++;
      return;
    case FAULT_WRONG_CLASS:
      f = (cls == 0x1E) ? status_frame() : ac_frame(28, 0x1E, 0x00);
      break;
    case FAULT_DUPLICATE:
      send_(f, at);
      break;
    case FAULT_GARBAGE:
      for (uint32_t i = 0; i < fs.arg; i++) {
        uint8_t b = static_cast<uint8_t>(noise.next());
        push_(at + i, b == 0xF4 ? 0x00 : b);
      }
      at += fs.arg;
      break;
    case FAULT_LONE_F4:
      push_(at, 0xF4);
      at += 1;
      break;
    case FAULT_STX_PAIR:
      push_(at, 0xF4);
      push_(at + 1, 0xF5);
      at += 2;
      break;
    default:
      break;
  }
  send_(f, at);
  replies++;
}

// ---- The rig: scheduler, unit, listener, and the invariants --------------------------------------
struct Probe : H::BusScheduler {
  int phase() const { return static_cast<int>(this->phase_); }
  int step() const { return static_cast<int>(this->step_); }
  bool idle() const { return this->phase_ == Phase::IDLE; }
  bool listening() const { return this->phase_ == Phase::LISTEN; }
  uint32_t until() const { return this->phase_until_; }
  size_t head() const { return this->queue_head_; }
  unsigned head_sends() const { return this->head_sends_; }
  unsigned boot_tries() const { return this->boot_tries_; }
  unsigned link_miss() const { return this->link_miss_; }
  bool link_down() const { return this->link_down_; }
};

struct TagInfo {
  std::vector<uint8_t> frame;  // as enqueued
  int sends = 0, reports = 0, finals = 0;
  bool answered = false;
};

struct Rig {
  Trace trace;
  Wire ac;
  Rec rec;
  Probe bus;
  bool has_de = true;
  bool begun = false;
  int burst = 1;  // polls per step, all at the same time
  // Longest stretch without a frame that a regular loop may show: a reply window plus the DE timing,
  // the retry wait and some slack.
  uint32_t progress_ms = H::BUS_REPLY_TIMEOUT_MS + 900;

  std::map<uint16_t, TagInfo> tags;
  uint16_t next_tag = 1;
  uint16_t last_new_tag = 0;
  size_t writes_seen = 0, commands_seen = 0, edges_seen = 0, de_seen = 0;
  uint32_t c_timeouts = 0, c_unanswered = 0, c_resends = 0, c_losses = 0, c_chk = 0;
  uint32_t r_unanswered = 0, r_resends = 0;
  uint32_t quiet_ms = 0;
  int idle_polls = 0;
  uint32_t steps = 0;

  explicit Rig(uint32_t start) {
    g_now = start;
    g_ctx_start = start;
    trace.start = start;
    ac.trace = &trace;
    ac.rec = &rec;
    rec.ac = &ac;
    rec.bus = &bus;
    rec.trace = &trace;
  }
  Rig(const Rig &) = delete;

  void begin() {
    if (begun)
      return;
    begun = true;
    ac.has_de = has_de;
    bus.setup(&ac, &rec, has_de);
  }
  void step(uint32_t dt = 1) {
    begin();
    for (int i = 0; i < burst; i++) {
      bus.poll(g_now);
      check_(i + 1 == burst ? dt : 0);
    }
    g_now += dt;
    steps++;
  }
  void run(uint32_t ms, uint32_t dt = 1) {
    for (uint32_t spent = 0; spent < ms; spent += dt)
      step(dt);
  }
  // A blocked main loop: time passes, nobody polls.
  void block(uint32_t ms) {
    g_now += ms;
    quiet_ms = 0;
  }
  // Run until the scheduler has just sent a frame of this class (and subtype, when given).
  bool run_until_write(uint8_t cls, int sub = -1, uint32_t limit = 5000) {
    size_t before = ac.writes.size();
    for (uint32_t i = 0; i < limit; i++) {
      step();
      if (ac.writes.size() > before) {
        const auto &b = ac.writes.back().bytes;
        if (b.size() > 14 && b[13] == cls && (sub < 0 || b[14] == sub))
          return true;
        before = ac.writes.size();
      }
    }
    return false;
  }

  // Enqueue a command frame carrying a tag in bytes 44/45. Those are body bytes no builder writes
  // (zero in every frame of hisense_protocol.cpp) and the scheduler never reads; the tag is how the
  // test follows one frame from the queue to the wire. stamp_link_token() redoes the checksum.
  H::BusTicket last_ticket = H::BUS_TICKET_NONE;  // of the newest frame send() queued
  bool send(const uint8_t *frame, size_t len, uint16_t *tag_out = nullptr) {
    begin();
    uint8_t tagged[H::CMD_FRAME_MAX];
    std::vector<uint8_t> f = unstuff(std::vector<uint8_t>(frame, frame + len));
    uint16_t tag = next_tag;
    f[44] = static_cast<uint8_t>(tag >> 8);
    f[45] = static_cast<uint8_t>(tag);
    size_t n = H::stamp_link_token(f.data(), f.size(), f[7], f[8], tagged, sizeof(tagged));
    if (n == 0)
      return false;
    last_ticket = bus.enqueue(tagged, n);
    if (last_ticket == H::BUS_TICKET_NONE)
      return false;
    next_tag++;
    tags[tag].frame.assign(tagged, tagged + n);
    if (tag_out != nullptr)
      *tag_out = tag;
    return true;
  }
  bool send_cmd(const H::AcCommand &c, uint16_t *tag_out = nullptr) {
    uint8_t f[H::CMD_FRAME_MAX];
    size_t n = H::build_command(c, f, sizeof(f));
    return n != 0 && send(f, n, tag_out);
  }
  bool send_setpoint(int8_t sp, uint16_t *tag_out = nullptr) {
    H::AcCommand c;
    c.setpoint = sp;
    return send_cmd(c, tag_out);
  }

  size_t count(uint8_t cls, size_t from = 0, int sub = -1) const {
    size_t n = 0;
    for (size_t i = from; i < ac.writes.size(); i++) {
      const auto &b = ac.writes[i].bytes;
      if (b.size() > 14 && b[13] == cls && (sub < 0 || b[14] == sub))
        n++;
    }
    return n;
  }
  size_t edges(bool up) const {
    size_t n = 0;
    for (auto &e : rec.links)
      n += e.up == up;
    return n;
  }
  std::vector<uint16_t> wire_tags(size_t from = 0) const {
    std::vector<uint16_t> out;
    for (size_t i = from; i < ac.writes.size(); i++) {
      const auto &b = ac.writes[i].bytes;
      if (b.size() >= H::CMD_FRAME_LEN && b[13] == 0x65)
        out.push_back(static_cast<uint16_t>((b[44] << 8) | b[45]));
    }
    return out;
  }

  void check_write_(size_t i);
  void check_(uint32_t dt);
  // Every accepted command has run its course: call when the queue has had time to drain.
  void audit_commands() {
    CHECK(bus.queued() == 0, "queue drained (%zu left)", bus.queued());
    for (auto &kv : tags) {
      const TagInfo &t = kv.second;
      CHECK(t.sends >= 1 && t.sends <= H::COMMAND_SENDS_MAX, "command %u was sent %d times", kv.first, t.sends);
      CHECK(t.reports == t.sends, "command %u: %d sends, %d reports", kv.first, t.sends, t.reports);
      CHECK(t.finals == 1, "command %u has %d final reports", kv.first, t.finals);
    }
  }
};

// One frame of ours on the wire.
void Rig::check_write_(size_t i) {
  const Write &w = ac.writes[i];
  std::vector<uint8_t> f = unstuff(w.bytes);
  size_t n = f.size();
  // CMD_HEADER and the link frames in hisense_protocol.h: F4 F5, direction 0x00 (module to A/C),
  // 0x40, LEN = length - 9, F4 FB.
  bool formed = n >= 16 && n <= H::TX_FRAME_MAX && f[0] == 0xF4 && f[1] == 0xF5 && f[2] == 0x00 && f[3] == 0x40 &&
                static_cast<size_t>(f[4]) + H::FRAME_LEN_OVERHEAD == n && f[n - 2] == 0xF4 && f[n - 1] == 0xFB &&
                H::status_checksum_ok(f.data(), n);
  CHECK(formed, "write %zu (%zu bytes) is a well formed frame", i, w.bytes.size());
  if (!formed)
    return;
  CHECK(w.bytes.size() <= H::TX_FRAME_MAX + 2, "write %zu fits the transmit buffer", i);
  uint8_t cls = f[13];
  if (cls == H::CLASS_DEVTYPE) {
    // The probe goes out with the pre-link 00 00 (LINK_INIT_0A, captured from the vendor module).
    CHECK(w.bytes == std::vector<uint8_t>(H::LINK_INIT_0A, H::LINK_INIT_0A + H::LINK_INIT_0A_LEN),
          "write %zu: the DevType probe is sent verbatim", i);
  } else {
    // 00 00 means "not linked" to the vendor module and is never stamped on anything else.
    CHECK(f[7] != 0 || f[8] != 0, "write %zu (class 0x%02X) carries a device type", i, cls);
  }
  if (i > 0) {
    // One transaction at a time: the previous one was closed by a frame or by its timeout.
    CHECK(w.closures > ac.writes[i - 1].closures, "write %zu started with the previous transaction still open", i);
    if (w.frames_seen == ac.writes[i - 1].frames_seen && !ac.blocking_write) {
      // Nothing was heard, so the whole reply window has to lie between the two frames.
      uint32_t prev_end = ac.writes[i - 1].t + (has_de ? H::bus_tx_time_ms(ac.writes[i - 1].bytes.size()) + 25 : 0);
      uint32_t this_start = w.t - (has_de ? 5 : 0);
      CHECK(static_cast<int32_t>(this_start - prev_end) >= static_cast<int32_t>(H::BUS_REPLY_TIMEOUT_MS),
            "write %zu came %d ms after an unanswered frame's DE release", i, (int) (this_start - prev_end));
    }
  }
  if (cls == H::CLASS_COMMAND && n >= H::CMD_FRAME_LEN) {
    uint16_t tag = static_cast<uint16_t>((f[44] << 8) | f[45]);
    auto it = tags.find(tag);
    CHECK(it != tags.end(), "write %zu: command %u on the wire was never enqueued", i, tag);
    if (it == tags.end())
      return;
    TagInfo &t = it->second;
    CHECK(t.finals == 0, "command %u sent again after its final report", tag);
    CHECK(!t.answered, "command %u sent again after it was answered", tag);
    if (t.sends == 0) {
      CHECK(tag > last_new_tag, "command %u overtook command %u", tag, last_new_tag);
      last_new_tag = tag;
    } else {
      CHECK(tag == last_new_tag, "command %u re-sent after a newer one (%u) went out", tag, last_new_tag);
    }
    t.sends++;
    // The frame on the wire is the frame that was enqueued, except for the device type in bytes 7/8
    // and the checksum that covers them.
    std::vector<uint8_t> q = unstuff(t.frame);
    bool same = q.size() == n;
    for (size_t k = 0; same && k + 4 < n; k++)
      same = k == 7 || k == 8 || q[k] == f[k];
    CHECK(same, "command %u reached the wire unchanged", tag);
  }
}

void Rig::check_(uint32_t dt) {
  // State in range.
  CHECK(bus.phase() >= 0 && bus.phase() <= 4, "phase %d", bus.phase());
  CHECK(bus.step() >= 0 && bus.step() <= 8, "step %d", bus.step());
  CHECK(bus.queued() <= H::BUS_TX_QUEUE_LEN && bus.head() < H::BUS_TX_QUEUE_LEN, "queue %zu at %zu", bus.queued(),
        bus.head());
  CHECK(bus.head_sends() <= H::COMMAND_SENDS_MAX, "head sends %u", bus.head_sends());
  CHECK(bus.boot_tries() <= H::BUS_BOOT_TRIES, "boot tries %u", bus.boot_tries());
  uint8_t hi = 0, lo = 0;
  bus.link_token(&hi, &lo);
  CHECK(hi != 0 || lo != 0, "device type is never 00 00");

  // Counters only go up, and agree with what the listener was told.
  CHECK(bus.reply_timeouts() >= c_timeouts && bus.unanswered_commands() >= c_unanswered &&
            bus.command_resends() >= c_resends && bus.link_losses() >= c_losses && bus.checksum_mismatches() >= c_chk,
        "a counter went backwards");
  c_timeouts = bus.reply_timeouts();
  c_unanswered = bus.unanswered_commands();
  c_resends = bus.command_resends();
  c_losses = bus.link_losses();
  c_chk = bus.checksum_mismatches();
  CHECK(c_timeouts == rec.timeouts, "timeouts: counter %u, listener %u", (unsigned) c_timeouts,
        (unsigned) rec.timeouts);
  CHECK(c_chk == rec.checksum_errors, "checksum errors: counter %u, listener %u", (unsigned) c_chk,
        (unsigned) rec.checksum_errors);
  CHECK(c_resends <= c_unanswered && c_unanswered <= c_timeouts, "resends %u <= unanswered %u <= timeouts %u",
        (unsigned) c_resends, (unsigned) c_unanswered, (unsigned) c_timeouts);

  // Link edges: lost first, then alternating, each counted once.
  for (; edges_seen < rec.links.size(); edges_seen++) {
    bool want_up = (edges_seen % 2) == 1;
    CHECK(rec.links[edges_seen].up == want_up, "link edge %zu is %s", edges_seen,
          rec.links[edges_seen].up ? "restored" : "lost");
  }
  CHECK(c_losses == edges(false), "link losses: counter %u, edges %zu", (unsigned) c_losses, edges(false));
  CHECK(bus.link_down() == (rec.links.size() % 2 == 1), "link state follows the edges");

  // DE: high exactly while the scheduler says it is transmitting, never read from while high,
  // nothing written without it, flushed before every release.
  if (has_de) {
    CHECK(ac.de_high == bus.wants_fast_loop(), "DE %d, wants_fast_loop %d", ac.de_high, bus.wants_fast_loop());
    CHECK(ac.de_high == bus.transmitting(), "DE %d, transmitting %d", ac.de_high, bus.transmitting());
    CHECK(!(ac.de_high && bus.listening()), "listening with DE high");
    CHECK(!ac.de.empty() && ac.flushes == (ac.de.size() - 1) / 2, "one flush per DE release (%u flushes, %zu edges)",
          (unsigned) ac.flushes, ac.de.size());
  } else {
    CHECK(ac.de.empty(), "DE touched without a DE pin");
  }
  CHECK(ac.reads_while_de == 0, "read while transmitting (%u)", (unsigned) ac.reads_while_de);
  CHECK(ac.writes_without_de == 0, "wrote with DE low (%u)", (unsigned) ac.writes_without_de);

  // Deadlines are near: no phase can be parked beyond the longest wait there is.
  if (!bus.idle())
    CHECK(static_cast<int32_t>(bus.until() - g_now) <= static_cast<int32_t>(H::BUS_CYCLE_MS),
          "phase %d parked %d ms ahead", bus.phase(), (int) (bus.until() - g_now));
  idle_polls = bus.idle() ? idle_polls + 1 : 0;
  CHECK(idle_polls <= 2, "idle for %d polls in a row", idle_polls);

  // New frames on the wire, new command reports.
  bool wrote = writes_seen < ac.writes.size();
  for (; writes_seen < ac.writes.size(); writes_seen++)
    check_write_(writes_seen);
  for (; commands_seen < rec.commands.size(); commands_seen++) {
    const CmdReport &c = rec.commands[commands_seen];
    r_unanswered += !c.answered;
    r_resends += c.resend;
    CHECK(!(c.answered && c.resend), "an answered command is not sent again");
    CHECK(c.answered == (c.verdict != H::COMMAND_REPLY_NONE), "verdict %u for answered=%d", c.verdict, c.answered);
    auto it = tags.find(c.tag);
    if (it == tags.end())
      continue;
    it->second.reports++;
    it->second.answered = it->second.answered || c.answered;
    if (!c.resend)
      it->second.finals++;
    CHECK(it->second.reports == it->second.sends, "command %u: report %d for send %d", c.tag, it->second.reports,
          it->second.sends);
  }
  CHECK(r_unanswered == c_unanswered && r_resends == c_resends, "command counters match the reports");

  // DE timing, once the release is in: 5 ms settle before the write, and the wire time plus 25 ms
  // after it (BUS_DE_SETTLE_MS, BUS_DE_DRAIN_MS). Edge 0 is the release in setup(); after that a
  // rise and a fall bracket exactly one write.
  for (; de_seen < ac.de.size(); de_seen++) {
    CHECK(ac.de[de_seen].high == ((de_seen % 2) == 1), "DE edge %zu is %s", de_seen,
          ac.de[de_seen].high ? "up" : "down");
    if (de_seen < 2 || ac.de[de_seen].high)
      continue;
    size_t wi = de_seen / 2 - 1;
    CHECK(wi < ac.writes.size(), "DE went up and down with nothing written");
    if (wi >= ac.writes.size())
      continue;
    const Write &w = ac.writes[wi];
    uint32_t rise = ac.de[de_seen - 1].t, fall = ac.de[de_seen].t;
    CHECK(w.t - rise >= H::BUS_DE_SETTLE_MS && w.t - rise < 0x80000000u, "settle %u ms", (unsigned) (w.t - rise));
    CHECK(fall - w.t >= H::bus_tx_time_ms(w.bytes.size()) + H::BUS_DE_DRAIN_MS && fall - w.t < 0x80000000u,
          "DE held %u ms after a %zu byte write", (unsigned) (fall - w.t), w.bytes.size());
  }

  // Progress: under a regular loop a frame goes out at least this often, whatever the unit does.
  quiet_ms = wrote ? 0 : quiet_ms + dt;
  if (dt > 150)
    quiet_ms = 0;  // a jump: judged from here
  CHECK(quiet_ms <= progress_ms, "no frame sent for %u ms (phase %d, step %d)", (unsigned) quiet_ms, bus.phase(),
        bus.step());
}

// ---- Running a scenario from several start times --------------------------------------------------
typedef void (*Script)(Rig &r);

// Start times: an ordinary one, zero, a few seconds before the 2^32 wrap (two distances, so the
// wrap lands in different places), and just before 2^31, where a signed comparison of raw
// timestamps would flip.
static const uint32_t STARTS[] = {1000u, 0u, 0xFFFFFFFFu - 2500u, 0xFFFFFFFFu - 9100u, 0x7FFFFFFFu - 4300u};

// Set by --traces: print the hash of every scenario's trace, one line each. Two builds of the
// scheduler that print the same lines put the same bytes on the wire at the same times and told the
// listener the same things, in every scenario here. That is the check for a change that is meant to
// leave the behaviour alone: run it before and after and compare the output.
static bool g_print_traces = false;

static uint64_t run_script(const char *name, Script s, uint32_t start, uint32_t *entries = nullptr) {
  g_ctx = name;
  Rig r(start);
  s(r);
  if (entries != nullptr)
    *entries = r.trace.entries;
  if (g_print_traces)
    printf("   trace %016llx %u entries, start %u: %s\n", (unsigned long long) r.trace.hash, (unsigned) r.trace.entries,
           (unsigned) start, name);
  return r.trace.hash;
}

// `sweep_span` > 0 adds starts at 2^32 - k for k = 1 .. span in steps of `sweep_step`, so that the
// wrap falls inside every phase of the scenario's first `span` ms.
static void everywhere(const char *name, Script s, uint32_t sweep_span = 0, uint32_t sweep_step = 1) {
  printf("-- %s\n", name);
  uint32_t entries = 0;
  uint64_t ref = run_script(name, s, STARTS[0], &entries);
  CHECK(entries > 10, "the scenario did something (%u trace entries)", (unsigned) entries);
  for (size_t i = 1; i < sizeof(STARTS) / sizeof(STARTS[0]); i++) {
    uint64_t h = run_script(name, s, STARTS[i]);
    CHECK(h == ref, "trace from start %u differs from the reference run", (unsigned) STARTS[i]);
  }
  for (uint32_t k = 1; k <= sweep_span; k += sweep_step) {
    uint64_t h = run_script(name, s, 0u - k);
    CHECK(h == ref, "trace with the wrap %u ms in differs from the reference run", (unsigned) k);
  }
}

static uint8_t cls_of(const Write &w) { return w.bytes.size() > 13 ? w.bytes[13] : 0; }

// ---- Time ----------------------------------------------------------------------------------------
// Healthy unit, one command. Reply timeouts never fire here, so this is DE settle and drain, the
// 1 s cycle and the reply path across the wrap.
static void script_healthy(Rig &r) {
  r.run(2600);
  CHECK(r.send_setpoint(25), "enqueue");
  r.run(3400);
  std::vector<uint32_t> hb;
  for (auto &w : r.ac.writes)
    if (cls_of(w) == 0x1E)
      hb.push_back(w.t);
  CHECK(hb.size() >= 5, "heartbeats (%zu)", hb.size());
  for (size_t i = 2; i < hb.size(); i++)
    CHECK(hb[i] - hb[i - 1] == H::BUS_CYCLE_MS, "cycle %zu is %u ms", i, (unsigned) (hb[i] - hb[i - 1]));
  CHECK(r.rec.timeouts == 0 && r.rec.links.empty(), "no timeout, no link edge");
  CHECK(r.rec.statuses >= 5 && r.rec.features == 1, "status %u, features %u", (unsigned) r.rec.statuses,
        (unsigned) r.rec.features);
  CHECK(r.ac.setpoint == 25 && r.rec.last.setpoint_c == 25, "the command took and is reported");
  r.audit_commands();
}

// The same, polled three times per millisecond: a burst of loop calls changes nothing.
static void script_healthy_burst(Rig &r) {
  r.burst = 3;
  script_healthy(r);
}

// Silent from boot: ten probes 1551 ms apart (5 settle + 21 wire + 25 drain + 1000 window + 500
// wait, the last two being BUS_REPLY_TIMEOUT_MS and BUS_BOOT_RETRY_MS), then 0x07, then cycles that
// time out twice each. The link is reported lost once, on the fifth unanswered status poll, and
// the unit coming back is reported once.
static void script_silent_boot(Rig &r) {
  r.ac.silent = true;
  r.ac.strict_link = true;  // powered up later: it wants the probe before it answers a poll
  r.ac.linked = false;
  r.run(36000);  // ten tries, the 0x07 window, then five cycles of two unanswered windows
  size_t probes = 0;
  while (probes < r.ac.writes.size() && cls_of(r.ac.writes[probes]) == 0x0A)
    probes++;
  CHECK(probes == H::BUS_BOOT_TRIES, "%zu boot probes", probes);
  for (size_t i = 1; i < probes; i++)
    CHECK(r.ac.writes[i].t - r.ac.writes[i - 1].t ==
              5 + H::bus_tx_time_ms(20) + 25 + H::BUS_REPLY_TIMEOUT_MS + H::BUS_BOOT_RETRY_MS,
          "probe %zu after %u ms", i, (unsigned) (r.ac.writes[i].t - r.ac.writes[i - 1].t));
  CHECK(probes < r.ac.writes.size() && cls_of(r.ac.writes[probes]) == 0x07, "then 0x07");
  CHECK(r.rec.links.size() == 1 && !r.rec.links[0].up && r.bus.link_losses() == 1, "one link-lost edge");
  size_t polls = 0;
  for (auto &w : r.ac.writes)
    if (cls_of(w) == 0x66 && !r.rec.links.empty() && not_after(w.t, r.rec.links[0].t))
      polls++;
  CHECK(polls == H::BUS_LINK_LOST_POLLS, "lost on status poll %zu", polls);
  CHECK(r.rec.frames == 0 && r.rec.statuses == 0, "nothing heard");
  uint8_t hi = 0, lo = 0;
  CHECK(!r.bus.link_token(&hi, &lo) && hi == 0x01 && lo == 0x01, "device type stays at the 01 01 default");
  // Every frame while lost carries that default, and the heartbeat says "not heard" (0xF0 at byte
  // 16, build_link_heartbeat).
  for (auto &w : r.ac.writes) {
    if (cls_of(w) == 0x1E)
      CHECK(w.bytes[16] == 0xF0, "heartbeat byte 16 is 0x%02X while nothing was heard", w.bytes[16]);
    if (cls_of(w) != 0x0A)
      CHECK(w.bytes[7] == 0x01 && w.bytes[8] == 0x01, "default device type on the wire");
  }
  r.ac.silent = false;
  r.run(4000);
  CHECK(r.rec.links.size() == 2 && r.rec.links[1].up, "one link-restored edge");
  CHECK(r.bus.link_token(&hi, &lo) && hi == 0x01 && lo == 0x02, "device type learned on recovery");
  CHECK(r.rec.statuses >= 2 && r.rec.features == 1, "status and capabilities arrive");
  CHECK(r.ac.writes.back().bytes[7] == 0x01 && r.ac.writes.back().bytes[8] == 0x02, "and frames carry it");
}

// A unit that hears everything except commands: three sends one cycle apart, then given up.
// Retry spacing and the timeout of a command window across the wrap.
static void script_deaf_command(Rig &r) {
  r.run(2600);
  r.ac.deaf_commands = -1;
  uint16_t a = 0, b = 0;
  CHECK(r.send_setpoint(20, &a) && r.send_setpoint(21, &b), "enqueue two");
  size_t from = r.ac.writes.size();
  r.run(9000);
  std::vector<uint16_t> order = r.wire_tags(from);
  std::vector<uint16_t> want = {a, a, a, b, b, b};
  CHECK(order == want, "wire order: first frame three times, then the second three times (%zu sends)", order.size());
  std::vector<uint32_t> at;
  for (size_t i = from; i < r.ac.writes.size(); i++)
    if (cls_of(r.ac.writes[i]) == 0x65)
      at.push_back(r.ac.writes[i].t);
  // The unanswered command holds its window open, so the cycle runs long by about the window.
  for (size_t i = 1; i < at.size() && i < 3; i++)
    CHECK(at[i] - at[i - 1] >= 1000 && at[i] - at[i - 1] <= 1100 + H::BUS_REPLY_TIMEOUT_MS, "re-send %zu after %u ms",
          i, (unsigned) (at[i] - at[i - 1]));
  CHECK(r.bus.unanswered_commands() == 6 && r.bus.command_resends() == 4 && r.rec.links.empty(),
        "six unanswered, four re-sends, link not blamed");
  CHECK(r.ac.setpoint == 22, "the unit never took either");
  r.audit_commands();
}

// The setup-request lockout (SETUP_REQUEST_LOCKOUT_MS = 10 s) at the three places it starts: boot,
// an accepted request, and the indication being switched off. Bit 5 (LINK_REQ_SMARTCFG, 0x20, one
// reply per gesture on the bench unit) is what the remote's gesture raises.
static void pulse(Rig &r, bool expect, const char *what) {
  size_t before = r.rec.setup_requests.size();
  r.ac.link_bits_once = H::LINK_REQ_SMARTCFG;
  r.run(1500);
  size_t got = r.rec.setup_requests.size() - before;
  CHECK(got == (expect ? 1u : 0u), "%s: %zu request(s)", what, got);
  if (got == 1)
    CHECK(r.rec.setup_requests.back().second == H::LINK_REQ_SMARTCFG, "request bits 0x%02X",
          r.rec.setup_requests.back().second);
}

static void script_setup_lockout(Rig &r) {
  r.run(3000);
  pulse(r, false, "gesture 3 s after boot is ignored");
  r.run(4000);
  pulse(r, false, "gesture 8.5 s after boot is ignored");
  r.run(1500);  // 11.5 s
  pulse(r, true, "gesture after the boot lockout is taken");
  r.run(4000);
  pulse(r, false, "gesture 5 s after an accepted one is ignored");
  r.run(6000);
  pulse(r, true, "gesture after that lockout is taken");
  r.bus.set_setup_indicator(true, g_now);
  r.run(12000);
  // The panel indication rides the heartbeat: byte 17 bit 3 (LINK_FLAG_PROVISIONING).
  size_t last_hb = r.ac.writes.size();
  while (last_hb > 0 && cls_of(r.ac.writes[last_hb - 1]) != 0x1E)
    last_hb--;
  CHECK(last_hb > 0 && (r.ac.writes[last_hb - 1].bytes[17] & H::LINK_FLAG_PROVISIONING) != 0,
        "heartbeat carries the flag");
  r.bus.set_setup_indicator(false, g_now);
  r.run(3000);
  pulse(r, false, "gesture 3 s after the indication went off is ignored");
  r.run(7000);
  pulse(r, true, "gesture after that lockout is taken");
  CHECK(r.rec.setup_requests.size() == 3, "three requests in all (%zu)", r.rec.setup_requests.size());
}

// A blocked main loop: the clock jumps by seconds, minutes and hours, in every phase. A reply that
// was already buffered is still taken; at worst the transaction in flight is lost; the link is
// never declared lost for it, and the cycle resumes at once instead of catching up.
static void script_clock_jumps(Rig &r) {
  static const uint32_t JUMPS[] = {700, 5000, 60000, 600000, 3 * 3600000u, 1200, 90000};
  r.run(2600);
  CHECK(r.send_setpoint(27), "enqueue");
  Rng rng(7);
  uint32_t timeouts_allowed = 0;
  for (uint32_t j : JUMPS) {
    r.run(3000 + rng.below(1000));
    uint32_t statuses = r.rec.statuses;
    size_t writes = r.ac.writes.size();
    r.block(j);
    timeouts_allowed++;
    r.run(2500);
    CHECK(r.rec.statuses > statuses, "status arrives again after a %u ms block", (unsigned) j);
    size_t sent = r.ac.writes.size() - writes;
    CHECK(sent >= 4 && sent <= 12, "%zu frames in the 2.5 s after a %u ms block (no catching up)", sent, (unsigned) j);
  }
  CHECK(r.rec.links.empty(), "a blocked loop is not a lost link");
  CHECK(r.rec.timeouts <= timeouts_allowed, "at most one transaction lost per block (%u timeouts)",
        (unsigned) r.rec.timeouts);
  CHECK(r.ac.setpoint == 27, "the command queued before the first block took");
  r.audit_commands();
}

// The same blocks on a half-duplex wire: a block while DE is high keeps the transceiver driving the
// bus over the unit's reply. That costs the reply, once per block at most.
static void script_clock_jumps_half_duplex(Rig &r) {
  r.ac.half_duplex = true;
  script_clock_jumps(r);
}

// Loop calls that arrive late: every 1 to 30 ms at random. The unit answers 80 ms after our last
// byte, so DE (25 ms drain plus one late loop) is down in time.
static void script_late_loop(Rig &r) {
  r.ac.half_duplex = true;
  r.ac.reply_delay = 80;
  Rng rng(11);
  uint32_t end = g_now + 20000;
  bool queued = false;
  while (static_cast<int32_t>(end - g_now) > 0) {
    r.step(rng.range(1, 30));
    if (!queued && r.rec.statuses >= 3)
      queued = r.send_setpoint(19);
  }
  CHECK(r.rec.timeouts == 0 && r.ac.collided == 0, "no reply lost (%u timeouts, %u bytes collided)",
        (unsigned) r.rec.timeouts, (unsigned) r.ac.collided);
  CHECK(r.rec.statuses >= 17 && r.rec.links.empty(), "status every cycle (%u)", (unsigned) r.rec.statuses);
  CHECK(r.ac.setpoint == 19, "command took");
  r.audit_commands();
}

// ---- Link bring-up and loss ----------------------------------------------------------------------
// The unit answers the first handshake step and then nothing.
static void script_first_step_only(Rig &r) {
  r.ac.only_devtype = true;
  r.run(14000);  // the 0x07 window, then five cycles of two unanswered windows
  CHECK(r.count(0x0A) >= 2 && cls_of(r.ac.writes[0]) == 0x0A && cls_of(r.ac.writes[1]) == 0x07,
        "one probe answered, then 0x07");
  uint8_t hi = 0, lo = 0;
  CHECK(r.bus.link_token(&hi, &lo) && lo == 0x02, "device type learned from the one reply");
  CHECK(r.rec.links.size() == 1 && !r.rec.links[0].up, "link reported lost once");
  CHECK(r.rec.statuses == 0, "no status");
  // The recovery probe is answered every cycle and the status poll is not: still lost, no flapping.
  r.run(20000);
  CHECK(r.rec.links.size() == 1 && r.bus.link_losses() == 1, "still exactly one edge after 20 s more");
  r.ac.only_devtype = false;
  r.run(3000);
  CHECK(r.rec.links.size() == 2 && r.rec.links[1].up, "restored once");
}

// Silent for `gap` ms after a healthy start, then back.
static uint32_t g_gap = 0;
static uint32_t g_gap_dt = 1;
// A silence at least this long loses the link: five unanswered status polls, each cycle costing a
// heartbeat window and a status window plus the frames, and up to one more cycle of slack.
static const uint32_t LINK_LOST_BY_MS = 5 * (2 * H::BUS_REPLY_TIMEOUT_MS + 300) + 1000;
static void script_gap(Rig &r) {
  r.run(3200);
  uint16_t before = 0, during = 0;
  CHECK(r.send_setpoint(18, &before), "enqueue just before the unit falls silent");
  r.ac.silent = true;
  r.ac.cut();
  r.run(2500);
  CHECK(r.send_setpoint(29, &during), "enqueue while it is silent");
  uint32_t statuses = r.rec.statuses;
  uint32_t left = g_gap > 2500 ? g_gap - 2500 : 0;
  r.run(left, left > 60000 ? g_gap_dt : 1);
  bool lost = g_gap >= LINK_LOST_BY_MS;
  CHECK(r.rec.statuses == statuses, "no status while silent");
  CHECK(r.rec.links.size() == (lost ? 1u : 0u), "%zu link edge(s) after %u ms of silence", r.rec.links.size(),
        (unsigned) g_gap);
  CHECK(r.bus.link_miss() <= 255, "miss counter saturates");
  size_t writes = r.ac.writes.size();
  // A silent unit gets one command per cycle (the slot closes behind the heartbeat's empty reply
  // window), so the second one may still be waiting when the unit returns after a short silence.
  const int during_sent = r.tags[during].sends;
  r.ac.silent = false;
  r.run(4000);
  CHECK(r.rec.links.size() == (lost ? 2u : 0u) && r.bus.link_losses() == (lost ? 1u : 0u), "restored once (%zu edges)",
        r.rec.links.size());
  CHECK(r.rec.statuses >= statuses + 2, "status flows again");
  // A command that went out into the silence is not sent again once the unit is back: it went out
  // once and was reported unanswered. The hub's "commands unconfirmed when the link dropped" path
  // counts on it. One that was still queued when the unit returned goes out then, once, and is
  // answered.
  CHECK(r.tags[before].sends >= 1 && r.tags[during].sends == 1, "each accepted command was sent");
  CHECK(during_sent == 1 || g_gap < LINK_LOST_BY_MS,
        "a silence long enough to lose the link carries both commands (%u ms)", (unsigned) g_gap);
  CHECK(r.count(0x65, writes) == (during_sent == 1 ? 0u : 1u), "after the unit came back: only what was still queued");
  size_t unanswered = 0;
  for (auto &c : r.rec.commands)
    unanswered += c.answered ? 0 : 1;
  CHECK(r.rec.commands.size() - unanswered == (during_sent == 1 ? 0u : 1u), "what went into the silence is unanswered");
  if (during_sent == 1) {
    CHECK(r.ac.heard_commands == 0 && r.ac.setpoint == 22, "the unit heard neither");
  } else {
    CHECK(r.ac.heard_commands == 1 && r.ac.setpoint == 29, "the unit heard the one that waited");
  }
  r.audit_commands();
}

// Silent in the middle of each kind of transaction: the reply is cut after `g_cut` bytes.
static uint8_t g_cut_cls = 0;
static uint32_t g_cut = 0;
static void script_cut_mid_reply(Rig &r) {
  r.run(2600);
  if (g_cut_cls == 0x65)
    CHECK(r.send_setpoint(30), "enqueue");
  CHECK(r.run_until_write(g_cut_cls, g_cut_cls == 0x66 ? 0 : -1), "transaction started");
  // The reply starts reply_delay after our last byte; let `g_cut` bytes of it through.
  r.run(5 + H::bus_tx_time_ms(r.ac.writes.back().bytes.size()) + r.ac.reply_delay + g_cut);
  r.ac.silent = true;
  r.ac.cut();
  r.run(LINK_LOST_BY_MS + 1000);
  CHECK(r.rec.links.size() == 1 && !r.rec.links[0].up, "link lost once");
  r.ac.silent = false;
  r.run(4000 + 2 * H::BUS_REPLY_TIMEOUT_MS);  // the open windows of the silent cycle, then a recovery cycle
  CHECK(r.rec.links.size() == 2 && r.rec.links[1].up, "restored once");
  CHECK(r.bus.checksum_mismatches() == 0, "a cut frame is not a checksum error");
  r.audit_commands();
}

// ---- Replies -------------------------------------------------------------------------------------
// One reply of class `g_f_cls` gets fault `g_f`, everything else is clean. Whatever the fault, it
// costs at most that transaction and the one after, never the link.
static uint8_t g_f_cls = 0;
static FaultSpec g_f = {FAULT_NONE, 0};
static uint32_t g_f_statuses = 0, g_f_timeouts = 0, g_f_chk = 0;
static bool g_f_cmd_answered = false;
static uint8_t g_f_cmd_cls = 0;
static int g_f_cmd_sends = 0;
static void script_one_fault(Rig &r) {
  r.run(2600);
  int left = 1;
  r.ac.fault = [&left](uint8_t cls, uint8_t sub) {
    if (left > 0 && cls == g_f_cls && !(cls == 0x66 && sub == 0x40)) {
      left--;
      return g_f;
    }
    return FaultSpec{FAULT_NONE, 0};
  };
  uint16_t tag = 0;
  if (g_f_cls == 0x65)
    CHECK(r.send_setpoint(28, &tag), "enqueue");
  uint32_t statuses = r.rec.statuses;
  r.run(6000);
  CHECK(left == 0, "the fault was injected");
  CHECK(r.rec.links.empty(), "one damaged reply is not a lost link");
  g_f_statuses = r.rec.statuses - statuses;
  g_f_timeouts = r.rec.timeouts;
  g_f_chk = r.bus.checksum_mismatches();
  CHECK(g_f_statuses >= 4, "status keeps arriving (%u in 6 s)", (unsigned) g_f_statuses);
  CHECK(g_f_timeouts <= 2, "at most two windows lost (%u)", (unsigned) g_f_timeouts);
  if (g_f_cls == 0x65) {
    g_f_cmd_answered = !r.rec.commands.empty() && r.rec.commands[0].answered;
    g_f_cmd_cls = r.rec.commands.empty() ? 0 : r.rec.commands[0].cls;
    g_f_cmd_sends = r.tags[tag].sends;
    CHECK(r.ac.setpoint == 28, "the command took");
  }
  r.audit_commands();
  // The run ends clean: five more seconds without a single timeout.
  uint32_t t = r.rec.timeouts;
  r.run(5000);
  CHECK(r.rec.timeouts == t, "clean again afterwards");
}

static const char *fault_name(Fault f) {
  static const char *const NAMES[] = {"none",      "dropped", "late",    "bad checksum", "truncated", "wrong class",
                                      "duplicate", "garbage", "lone F4", "F4 F5 prefix", "bit flip"};
  return NAMES[f];
}

static void test_reply_faults() {
  printf("-- one damaged reply, per class and kind of damage\n");
  static const uint8_t CLASSES[] = {0x1E, 0x66, 0x65};
  static const FaultSpec FAULTS[] = {
      {FAULT_DROP, 0},    {FAULT_LATE, 470},   {FAULT_LATE, 900},    {FAULT_LATE, 1020},     {FAULT_LATE, 1400},
      {FAULT_BAD_SUM, 0}, {FAULT_TRUNCATE, 3}, {FAULT_TRUNCATE, 17}, {FAULT_WRONG_CLASS, 0}, {FAULT_DUPLICATE, 0},
      {FAULT_GARBAGE, 1}, {FAULT_GARBAGE, 40}, {FAULT_LONE_F4, 0},
  };
  for (uint8_t cls : CLASSES) {
    for (const FaultSpec &f : FAULTS) {
      g_f_cls = cls;
      g_f = f;
      static char name[64];
      snprintf(name, sizeof(name), "reply to 0x%02X: %s (%u)", cls, fault_name(f.kind), (unsigned) f.arg);
      g_ctx = name;
      uint64_t ref = run_script(name, script_one_fault, STARTS[0]);
      uint32_t timeouts = g_f_timeouts, chk = g_f_chk;
      bool answered = g_f_cmd_answered;
      uint8_t answered_cls = g_f_cmd_cls;
      int sends = g_f_cmd_sends;
      CHECK(run_script(name, script_one_fault, STARTS[2]) == ref, "same trace across the wrap");
      g_ctx = name;
      // What each kind of damage is allowed to cost.
      switch (f.kind) {
        case FAULT_GARBAGE:
        case FAULT_LONE_F4:
          // Noise ahead of a frame, a lone start byte included, does not cost the frame.
          CHECK(timeouts == 0 && chk == 0, "noise ahead of the reply costs nothing (%u timeouts)", (unsigned) timeouts);
          break;
        case FAULT_DUPLICATE:
          CHECK(timeouts == 0 && chk == 0, "a repeated reply costs nothing (%u timeouts)", (unsigned) timeouts);
          break;
        case FAULT_BAD_SUM:
          // Every damaged frame is counted once, whichever class it is.
          CHECK(chk == 1u, "checksum errors counted: %u", (unsigned) chk);
          break;
        case FAULT_DROP:
        case FAULT_TRUNCATE:
          CHECK(timeouts == 1, "exactly the one window is lost (%u)", (unsigned) timeouts);
          break;
        case FAULT_WRONG_CLASS:
          // The status poll waits for a 0x66 and nothing else: a 0x1E in its window does not close
          // it. The heartbeat and the command take any class.
          CHECK(timeouts == (cls == 0x66 ? 1u : 0u), "wrong class: %u window(s) timed out", (unsigned) timeouts);
          break;
        default:
          break;
      }
      if (cls == 0x65) {
        if (f.kind == FAULT_DROP || f.kind == FAULT_TRUNCATE) {
          CHECK(!answered && sends == 2, "unanswered once, then sent again and answered (%d sends)", sends);
        } else if (f.kind == FAULT_LATE && f.arg + 40 > H::BUS_REPLY_TIMEOUT_MS) {
          // The acknowledgement comes after the reply window: the unit took the frame, the
          // scheduler cannot know, and sends it again. Commands are absolute, so the second copy
          // is harmless apart from a second beep.
          CHECK(!answered && sends == 2, "late ack: sent twice (%d)", sends);
        } else if (f.kind == FAULT_WRONG_CLASS) {
          // Pinned, see "KNOWN AND NOT FIXED" in test_esphome_bus.cpp: any class closes the window.
          CHECK(answered && answered_cls == 0x1E && sends == 1, "a 0x1E in the command window counts as its answer");
        } else {
          CHECK(answered && answered_cls == 0x65 && sends == 1, "answered by its own ack (%d sends)", sends);
        }
      }
    }
  }
}

// Only a status frame with a good checksum says the unit is there. One that answers every status
// poll with something else (here a 0x1E), or with a damaged frame, is reported lost on the fifth
// poll like a silent one, and the heartbeat's own reply does not hide it.
static Fault g_alive_fault = FAULT_NONE;
static void script_not_alive(Rig &r) {
  r.run(2600);
  r.ac.fault = [](uint8_t cls, uint8_t sub) {
    return FaultSpec{(cls == 0x66 && sub == 0x00) ? g_alive_fault : FAULT_NONE, 1};
  };
  size_t from = r.ac.writes.size();
  uint32_t statuses = r.rec.statuses;
  r.run(9000);
  CHECK(r.rec.links.size() == 1 && !r.rec.links[0].up, "link lost once");
  size_t polls = 0;
  for (size_t i = from; i < r.ac.writes.size(); i++)
    if (cls_of(r.ac.writes[i]) == 0x66 && r.ac.writes[i].bytes[14] == 0x00 && !r.rec.links.empty() &&
        not_after(r.ac.writes[i].t, r.rec.links[0].t))
      polls++;
  CHECK(polls == H::BUS_LINK_LOST_POLLS, "on status poll %zu", polls);
  CHECK(r.rec.statuses == statuses, "nothing delivered as status");
  if (g_alive_fault == FAULT_BAD_SUM || g_alive_fault == FAULT_FLIP)
    CHECK(r.bus.checksum_mismatches() >= H::BUS_LINK_LOST_POLLS, "each damaged frame counted (%u)",
          (unsigned) r.bus.checksum_mismatches());
  r.ac.fault = nullptr;
  r.run(3000);
  CHECK(r.rec.links.size() == 2 && r.rec.links[1].up, "restored once");
}

// A status reply cut at every possible length costs that one poll and nothing else.
static void script_truncate_sweep(Rig &r) {
  r.run(2600);
  for (uint32_t cut = 1; cut < 160; cut += 1) {
    int left = 1;
    r.ac.fault = [&left, cut](uint8_t cls, uint8_t sub) {
      if (left > 0 && cls == 0x66 && sub == 0x00) {
        left--;
        return FaultSpec{FAULT_TRUNCATE, cut - 1};
      }
      return FaultSpec{FAULT_NONE, 0};
    };
    uint32_t timeouts = r.rec.timeouts, statuses = r.rec.statuses;
    // The cut reply holds its window open, so the cycle with it runs long by the window; the next
    // cycle's poll is the one delivered in this stretch.
    r.run(2000 + H::BUS_REPLY_TIMEOUT_MS);
    CHECK(left == 0 && r.rec.timeouts == timeouts + 1, "cut at %u: one window lost (%u)", (unsigned) cut,
          (unsigned) (r.rec.timeouts - timeouts));
    CHECK(r.rec.statuses >= statuses + 1, "cut at %u: the other poll in this stretch is delivered (%u)", (unsigned) cut,
          (unsigned) (r.rec.statuses - statuses));
  }
  CHECK(r.rec.links.empty() && r.bus.checksum_mismatches() == 0, "no link edge, no checksum error");
}

// The transceiver hears our own frames. Every one of them is dropped by its direction byte, and the
// reply behind it is taken: the trace of what the listener is told is that of a quiet wire.
static void script_echo(Rig &r) {
  r.ac.echo = true;
  r.run(2600);
  CHECK(r.send_setpoint(24), "enqueue");
  H::AcCommand heat;
  heat.mode = H::MODE_HEAT;
  CHECK(r.send_cmd(heat), "enqueue");
  r.run(4000);
  CHECK(r.rec.timeouts == 0 && r.rec.links.empty() && r.bus.checksum_mismatches() == 0, "nothing lost to the echo");
  CHECK(r.rec.frames == r.ac.replies, "every frame reported is one the unit sent (%u of %u)", (unsigned) r.rec.frames,
        (unsigned) r.ac.replies);
  for (auto &c : r.rec.commands)
    CHECK(c.answered && c.cls == 0x65 && c.verdict != H::COMMAND_REPLY_NONE,
          "command answered by the unit, not by its echo");
  CHECK(r.ac.mode == H::MODE_HEAT && r.ac.setpoint == 24, "both commands took");
  r.audit_commands();
}

// The device type changes while the link is down (a different indoor board answers after a power
// cut). Frames carry the old pair until the recovery probe is answered, then the new one, and a
// 00 00 reply ("not linked yet" to the vendor module) is never adopted.
static void script_token_change(Rig &r) {
  r.run(3200);
  uint8_t hi = 0, lo = 0;
  CHECK(r.bus.link_token(&hi, &lo) && hi == 0x01 && lo == 0x02, "first device type");
  // While the link is up the probe is not repeated: a change on the unit's side goes unseen.
  r.ac.dev_lo = 0x07;
  size_t probes = r.count(0x0A);
  r.run(5000);
  CHECK(r.count(0x0A) == probes, "no probe while the link is up");
  r.bus.link_token(&hi, &lo);
  CHECK(lo == 0x02, "old device type kept");
  r.ac.silent = true;
  r.run(LINK_LOST_BY_MS + 1000);
  for (size_t i = 0; i < r.ac.writes.size(); i++)
    if (cls_of(r.ac.writes[i]) != 0x0A)
      CHECK(r.ac.writes[i].bytes[8] == 0x02, "frame %zu still carries the old device type", i);
  // A unit that lost power is not linked: it waits for the probe before it answers a poll.
  r.ac.strict_link = true;
  r.ac.linked = false;
  r.ac.silent = false;
  r.ac.dev_hi = 0x00;
  r.ac.dev_lo = 0x00;
  size_t probes_before = r.count(0x0A);
  r.run(3000);
  r.bus.link_token(&hi, &lo);
  CHECK(r.count(0x0A) > probes_before && r.ac.linked, "the recovery probe was answered");
  CHECK(hi == 0x01 && lo == 0x02, "a 00 00 reply is not adopted");
  r.ac.dev_hi = 0x03;
  r.ac.dev_lo = 0x07;
  r.ac.silent = true;
  r.run(LINK_LOST_BY_MS + 1000);
  r.ac.silent = false;
  r.ac.linked = false;
  size_t from = r.ac.writes.size();
  r.run(3000);
  CHECK(r.bus.link_token(&hi, &lo) && hi == 0x03 && lo == 0x07, "new device type adopted on recovery");
  bool probed = false;
  for (size_t i = from; i < r.ac.writes.size(); i++) {
    const auto &b = r.ac.writes[i].bytes;
    if (b[13] == 0x0A)
      probed = true;
    else if (probed)
      CHECK(b[7] == 0x03 && b[8] == 0x07, "frame %zu after the probe carries the new device type", i);
  }
  CHECK(r.rec.links.size() == 4, "two losses, two recoveries (%zu edges)", r.rec.links.size());
}

// Was a defect: the device type was taken from a DevType reply without looking at its checksum, so
// one damaged reply during the handshake stamped a wrong pair into bytes 7/8 of every frame that
// followed, until the link had been lost and the recovery probe was answered cleanly.
// hisense_protocol.cpp says of the wrong pair in that place that it "kills the link on hardware".
// Now the damaged reply is a failed try: it is counted, nothing is adopted from it, and the probe
// goes out again BUS_BOOT_RETRY_MS later.
static void script_devtype_bad_checksum(Rig &r) {
  int left = 1;
  r.ac.fault = [&left](uint8_t cls, uint8_t) {
    if (left > 0 && cls == 0x0A) {
      left--;
      return FaultSpec{FAULT_FLIP, 1};
    }
    return FaultSpec{FAULT_NONE, 0};
  };
  // The damage: the unit sends 01 02, one bit flips on the way, and 01 42 arrives under the
  // checksum of 01 02.
  r.run(3000);
  uint8_t hi = 0, lo = 0;
  bool seen = r.bus.link_token(&hi, &lo);
  CHECK(!(seen && lo == 0x42), "a DevType reply with a bad checksum is not adopted (got %02X %02X)", hi, lo);
  bool stamped_wrong = false;
  for (auto &w : r.ac.writes)
    if (cls_of(w) != 0x0A && w.bytes[8] == 0x42)
      stamped_wrong = true;
  CHECK(!stamped_wrong, "no frame carries the damaged device type");
  CHECK(seen && hi == 0x01 && lo == 0x02, "the second, whole reply supplies the device type (got %02X %02X)", hi, lo);
  CHECK(r.count(0x0A) == 2 && cls_of(r.ac.writes[0]) == 0x0A && cls_of(r.ac.writes[1]) == 0x0A,
        "the damaged reply is a failed try: the probe is sent a second time (%zu probes)", r.count(0x0A));
  CHECK(r.bus.checksum_mismatches() == 1, "the damaged reply is counted (%u)", (unsigned) r.bus.checksum_mismatches());
}

// A frame start that goes nowhere (F4 F5) directly ahead of a reply used to take the reply with it:
// the assembler dropped the false start on its third byte and did not look at that byte again as a
// possible start, and one poll was lost. The byte is looked at again now.
static void script_stx_pair(Rig &r) {
  r.run(2600);
  int left = 1;
  r.ac.fault = [&left](uint8_t cls, uint8_t sub) {
    if (left > 0 && cls == 0x66 && sub == 0x00) {
      left--;
      return FaultSpec{FAULT_STX_PAIR, 0};
    }
    return FaultSpec{FAULT_NONE, 0};
  };
  r.run(4000);
  CHECK(r.rec.timeouts == 0, "a false start ahead of a reply does not cost the reply (%u timeouts)",
        (unsigned) r.rec.timeouts);
  CHECK(r.rec.links.empty(), "and no link edge");
}

// Frames the unit sends unasked, in the wait between two cycles, are flushed and change nothing.
static void script_unsolicited(Rig &r) {
  r.run(2600);
  uint32_t statuses = r.rec.statuses;
  for (int i = 0; i < 5; i++) {
    r.run(430);
    r.ac.inject(r.ac.status_frame(), g_now);
    r.run(570);
  }
  CHECK(r.rec.statuses == statuses + 5, "only the polled status frames are delivered (%u)",
        (unsigned) (r.rec.statuses - statuses));
  CHECK(r.rec.timeouts == 0 && r.rec.links.empty(), "nothing lost");
}

// ---- Commands ------------------------------------------------------------------------------------
// Offered faster than the bus carries them: a new frame every 20 ms for 4 s. What the queue cannot
// take is refused at the call (the hub logs that), and everything it took goes out once, in order.
static void script_burst(Rig &r) {
  r.run(2600);
  int accepted = 0, refused = 0;
  size_t from = r.ac.writes.size();
  for (int i = 0; i < 200; i++) {
    if (r.send_setpoint(static_cast<int8_t>(16 + i % 17)))
      accepted++;
    else
      refused++;
    r.run(20);
  }
  r.run(4000);
  std::vector<uint16_t> order = r.wire_tags(from);
  CHECK(refused > 0 && accepted > 8, "%d accepted, %d refused", accepted, refused);
  CHECK(static_cast<int>(order.size()) == accepted, "every accepted frame sent exactly once (%zu of %d)", order.size(),
        accepted);
  CHECK(std::is_sorted(order.begin(), order.end()), "in the order they were accepted");
  CHECK(r.rec.commands.size() == order.size(), "each reported");
  // The verdict on the ack byte is not asserted: its layout is still VERIFY in hisense_bus.h.
  for (auto &c : r.rec.commands)
    CHECK(c.answered && c.cls == 0x65, "answered by a 0x65");
  r.audit_commands();
}

// Was a weakness: the command slot emptied the whole queue before the status poll, and a queue
// that is refilled as fast as it drains never empties. While that lasted no heartbeat and no status
// poll was sent. The slot now closes BUS_COMMAND_SLOT_MS into the cycle and what is still queued
// waits for the next one.
static void script_starvation(Rig &r) {
  r.run(2600);
  size_t from = r.ac.writes.size();
  uint32_t statuses = r.rec.statuses;
  for (int i = 0; i < 10000; i++) {
    if (r.bus.queued() < 2)
      r.send_setpoint(static_cast<int8_t>(16 + i % 17));
    r.step();
  }
  size_t heartbeats = r.count(0x1E, from), polls = r.count(0x66, from);
  CHECK(heartbeats >= 9 && polls >= 9,
        "heartbeat and status poll keep their cycle under a steady stream of commands "
        "(%zu heartbeats, %zu polls in 10 s)",
        heartbeats, polls);
  CHECK(r.rec.statuses - statuses >= 9, "and status is delivered every cycle (%u)",
        (unsigned) (r.rec.statuses - statuses));
  CHECK(r.count(0x65, from) >= 30, "commands flowed the whole time, three or more a cycle (%zu)", r.count(0x65, from));
  CHECK(r.rec.links.empty(), "no link edge either: nothing counts the polls that were never sent");
  r.run(3000);
  CHECK(r.rec.statuses > statuses, "status resumes when the stream stops");
  r.audit_commands();
}

// Power and mode in one frame: build_command() with power_on. Heat with the power bits is 0x3C at
// byte 18, the stock module's own byte (stock image file 0x25bdc, RE docs/10 section 5b-2, as cited
// at CMD_MODE_BYTE in hisense_protocol.h). The scheduler carries it through untouched.
static void script_power_and_mode(Rig &r) {
  r.ac.power = false;
  r.run(2600);
  CHECK(!r.rec.last.power_on, "unit reports off");
  H::AcCommand c;
  c.mode = H::MODE_HEAT;
  c.setpoint = 23;
  c.power_on = true;
  size_t from = r.ac.writes.size();
  CHECK(r.send_cmd(c), "enqueue");
  r.run(3000);
  size_t n = 0;
  for (size_t i = from; i < r.ac.writes.size(); i++) {
    if (cls_of(r.ac.writes[i]) != 0x65)
      continue;
    n++;
    CHECK(r.ac.writes[i].bytes[18] == 0x3C, "byte 18 is 0x%02X", r.ac.writes[i].bytes[18]);
  }
  CHECK(n == 1, "one frame (%zu)", n);
  CHECK(r.rec.last.power_on && r.rec.last.mode == H::MODE_HEAT && r.rec.last.setpoint_c == 23,
        "the unit comes on in heat at 23");
  r.audit_commands();
}

// The queue is a ring of eight. Fill it, overflow it, drain it, many times over, with the head in
// the middle of a re-send some of the time.
static void script_queue_ring(Rig &r) {
  r.run(2600);
  Rng rng(3);
  for (int round = 0; round < 24; round++) {
    size_t room = H::BUS_TX_QUEUE_LEN - r.bus.queued();
    for (size_t i = 0; i < room; i++)
      CHECK(r.send_setpoint(static_cast<int8_t>(16 + rng.below(17))), "round %d: slot %zu taken", round, i);
    CHECK(!r.send_setpoint(20), "round %d: the ninth is refused", round);
    CHECK(r.bus.queued() == H::BUS_TX_QUEUE_LEN, "full");
    if (round % 3 == 1)
      r.ac.deaf_commands = 2;
    // Let a random part of it drain, so the head moves around the ring. A deaf round holds a
    // window open per re-send, so give it the window on top.
    r.run(300 + H::BUS_REPLY_TIMEOUT_MS + rng.below(1500));
  }
  r.run(12000 + 8 * H::BUS_REPLY_TIMEOUT_MS);  // the last ring drains, re-sends holding their windows open
  CHECK(r.next_tag > 30 && r.bus.queued() == 0, "%u frames went through", (unsigned) (r.next_tag - 1));
  r.audit_commands();
  uint8_t f[H::TX_FRAME_MAX + 1] = {0xF4, 0xF5};
  CHECK(!r.bus.enqueue(f, 0) && !r.bus.enqueue(f, H::TX_FRAME_MAX + 1) && !r.bus.enqueue(nullptr, 20),
        "empty, oversize and null frames refused");
}

// A command whose checksum, once the device type is stamped in, has a 0xF4 byte: that byte is
// doubled on the wire and the frame is 51 bytes. The stamping happens inside the scheduler, so the
// frame as enqueued need not have had one.
static void script_stuffed_checksum(Rig &r) {
  r.run(2600);
  // Search the tag space for a frame whose checksum low byte becomes 0xF4 under device type 01 02.
  bool found = false;
  size_t from = r.ac.writes.size();
  for (int tries = 0; tries < 4000 && !found; tries++) {
    uint8_t f[H::CMD_FRAME_MAX], probe[H::CMD_FRAME_MAX], stamped[H::CMD_FRAME_MAX];
    size_t n = H::build_command(H::AcCommand{}, f, sizeof(f));
    f[44] = static_cast<uint8_t>(r.next_tag >> 8);
    f[45] = static_cast<uint8_t>(r.next_tag);
    n = H::stamp_link_token(f, n, f[7], f[8], probe, sizeof(probe));
    size_t m = H::stamp_link_token(probe, n, 0x01, 0x02, stamped, sizeof(stamped));
    if (m == H::CMD_FRAME_LEN + 1) {
      found = r.send(f, H::CMD_FRAME_LEN);
    } else {
      r.next_tag++;
    }
  }
  CHECK(found, "a frame with a 0xF4 checksum byte exists in the tag space");
  r.run(2500);
  size_t n = 0;
  for (size_t i = from; i < r.ac.writes.size(); i++) {
    if (cls_of(r.ac.writes[i]) != 0x65)
      continue;
    n++;
    CHECK(r.ac.writes[i].bytes.size() == H::CMD_FRAME_LEN + 1, "51 bytes on the wire (%zu)",
          r.ac.writes[i].bytes.size());
  }
  CHECK(n == 1 && r.rec.commands.size() == 1 && r.rec.commands[0].answered, "sent once and answered");
  r.audit_commands();
}

// ---- Receive path limits -------------------------------------------------------------------------
// Polled so coarsely that a whole reply sits in the UART buffer before it is read. Without a DE pin
// (the UART owns DE) nothing depends on the loop being on time.
static uint32_t g_coarse_dt = 1;
static size_t g_coarse_fifo = 0;
static void script_coarse(Rig &r) {
  r.has_de = false;
  r.ac.fifo_cap = g_coarse_fifo;
  r.progress_ms = H::BUS_REPLY_TIMEOUT_MS + 900 + 4 * g_coarse_dt;
  r.run(3000, g_coarse_dt);
  CHECK(r.send_setpoint(26), "enqueue");
  r.run(12000, g_coarse_dt);
  CHECK(!r.bus.wants_fast_loop(), "no DE timing to run");
  CHECK(r.rec.timeouts == 0 && r.rec.links.empty() && r.ac.overruns == 0, "nothing lost (%u timeouts)",
        (unsigned) r.rec.timeouts);
  CHECK(r.rec.statuses >= 6, "status delivered (%u)", (unsigned) r.rec.statuses);
  CHECK(r.ac.setpoint == 26, "command took");
  r.audit_commands();
}

// A receive buffer smaller than the status frame, read too rarely: every status reply loses its
// tail. That is a dead link as far as anyone can tell, and it is reported as one, once. Read often
// enough again, the link is back.
static void script_overrun(Rig &r) {
  r.has_de = false;
  r.run(3000);
  r.ac.fifo_cap = 128;
  r.progress_ms = H::BUS_REPLY_TIMEOUT_MS + 1700;
  uint32_t statuses = r.rec.statuses;
  r.run(12000, 250);
  CHECK(r.ac.overruns > 0, "bytes were lost to the buffer (%u)", (unsigned) r.ac.overruns);
  CHECK(r.rec.statuses == statuses, "no status frame survives");
  CHECK(r.rec.links.size() == 1 && !r.rec.links[0].up, "reported as a lost link, once");
  CHECK(r.bus.checksum_mismatches() == 0, "a frame missing its tail is not a checksum error");
  r.run(4000, 1);
  CHECK(r.rec.links.size() == 2 && r.rec.links[1].up, "restored once the loop keeps up");
}

// Noise by the kilobyte in one reply window, then the reply: the scheduler reads it all in one poll
// and still finds the frame. The noise includes what looks like the start of a maximal frame, which
// the assembler follows for at most RX_FRAME_MAX bytes before letting go.
static void script_flood(Rig &r) {
  r.run(2600);
  CHECK(r.run_until_write(0x66, 0), "status poll sent");
  uint32_t statuses = r.rec.statuses;
  std::vector<uint8_t> noise;
  Rng rng(5);
  for (int i = 0; i < 3000; i++) {
    uint8_t b = static_cast<uint8_t>(rng.next());
    noise.push_back(b == 0xF4 ? 0x11 : b);
  }
  // F4 F5 01 40 then LEN 0xBF: a frame claiming exactly RX_FRAME_MAX (200) bytes that never ends
  // with F4 FB. Its 195 following bytes are noise, then come 5 bytes that close it without a tag.
  static const uint8_t CLAIM[] = {0xF4, 0xF5, 0x01, 0x40, 0xBF};
  noise.insert(noise.begin() + 1000, CLAIM, CLAIM + sizeof(CLAIM));
  for (size_t i = 0; i < noise.size(); i++)
    r.ac.inject({noise[i]}, g_now);  // all at once: more than any UART buffer between two loops
  r.run(600);
  CHECK(r.rec.statuses == statuses + 1, "the status reply behind 3 kB of noise is delivered");
  CHECK(r.rec.timeouts == 0, "within its window");
  // A frame of exactly RX_FRAME_MAX bytes is taken, one byte more is not (hisense_protocol.h).
  r.run(1000);
  CHECK(r.run_until_write(0x1E), "heartbeat sent");
  uint32_t frames = r.rec.frames;
  statuses = r.rec.statuses;
  r.ac.cut();
  r.ac.silent = true;
  r.ac.inject(ac_frame(200, 0x66, 0x00), g_now + 60);
  r.run(450);
  r.ac.silent = false;
  CHECK(r.rec.frames == frames + 1 && r.rec.statuses == statuses + 1, "a 200 byte status frame is taken");
  r.run(1500);
  CHECK(r.run_until_write(0x1E), "heartbeat sent");
  frames = r.rec.frames;
  r.ac.cut();
  r.ac.silent = true;
  std::vector<uint8_t> big = ac_frame(201, 0x66, 0x00);
  r.ac.inject(big, g_now + 60);
  r.run(800);
  r.ac.silent = false;
  CHECK(r.rec.frames == frames, "a 201 byte frame is dropped");
  r.run(3000);
  CHECK(r.rec.links.empty(), "and none of it costs the link");
}

// ---- DE ------------------------------------------------------------------------------------------
// The per-poll invariants hold DE to its contract in every scenario. This one adds the blocking
// UART write (LibreTiny) and asserts the release time against the last byte, across the wrap.
static void script_de_blocking(Rig &r) {
  r.ac.blocking_write = true;
  r.ac.half_duplex = true;
  r.run(2600);
  CHECK(r.send_setpoint(21), "enqueue");
  r.run(4000);
  for (size_t i = 0; i < r.ac.writes.size(); i++) {
    const Write &w = r.ac.writes[i];
    uint32_t fall = 0;
    bool found = false;
    for (auto &e : r.ac.de) {
      if (!e.high && !found && static_cast<int32_t>(e.t - w.t) > 0) {
        fall = e.t;
        found = true;
      }
    }
    if (!found)
      continue;  // still transmitting at the end of the run
    uint32_t after_last_byte = fall - (w.t + H::bus_tx_time_ms(w.bytes.size()));
    CHECK(after_last_byte >= 25 && after_last_byte <= 26, "write %zu: DE falls %u ms after the last byte", i,
          (unsigned) after_last_byte);
  }
  CHECK(r.rec.timeouts == 0 && r.ac.collided == 0 && r.rec.links.empty(), "no reply lost to a late release");
  CHECK(r.ac.setpoint == 21, "command took");
  r.audit_commands();
}

// Was a weakness: HisenseAC::bus_transmitting() was wants_fast_loop(), which is DE timing and
// nothing else. Without a DE pin it was false for the whole of every transmission, so board code
// that asks "is the port busy sending" (w41h1_module's serial restart) was told no while up to
// 52 ms of frame were still leaving the UART. transmitting() keeps the time the bytes need.
static void script_no_de_transmitting(Rig &r) {
  r.has_de = false;
  bool fast = false;
  size_t seen = 0;
  uint32_t end = g_now + 4000;
  while (static_cast<int32_t>(end - g_now) > 0) {
    r.step();  // polls at g_now, then advances the clock
    fast = fast || r.bus.wants_fast_loop();
    // As of that poll: transmitting exactly while the newest frame's bytes are still leaving.
    bool leaving = false;
    if (!r.ac.writes.empty()) {
      const Write &w = r.ac.writes.back();
      const uint32_t polled = g_now - 1;
      leaving = static_cast<int32_t>(polled - (w.t + H::bus_tx_time_ms(w.bytes.size()))) < 0;
      seen += leaving ? 1 : 0;
    }
    CHECK(r.bus.transmitting() == leaving, "transmitting() is %d with the last byte %s", r.bus.transmitting(),
          leaving ? "still to go" : "gone");
  }
  CHECK(r.count(0x66) >= 3, "frames were sent");
  CHECK(seen > 100, "the scheduler said it was transmitting while it sent without a DE pin (%zu ms)", seen);
  CHECK(!fast, "and still asks for no fast loop: there is no DE timing to keep");
}

// ---- The hub's decisions on top of the real scheduler --------------------------------------------
// HisenseAC cannot be built on a host, but its command tracker (CommandTracker, hisense_commands.h)
// can, and this is that class with the hub's loop around it: the parked status frame, the link
// edge, and frames that leave through the scheduler. test_esphome_confirm.cpp runs the same class
// against a unit that hears frames at once; here they pass through the command slot, the re-sends
// and the link detection.
struct MiniHub : H::CommandSink {
  Rig &r;
  H::CommandTracker t;
  H::AcState parked{};
  bool parked_valid = false, link_up = false, link_dirty = false;
  const H::AcCommand &cmd;  // the shadow
  const H::PendingCommand &pending;
  int retries = 0, failed = 0, dropped_on_link = 0, done = 0, refused = 0;

  explicit MiniHub(Rig &rig) : r(rig), cmd(t.cmd()), pending(t.main_pending()) {
    t.set_sink(this);
    r.rec.status_hook = [this](const H::AcState &s) {
      parked = s;
      parked_valid = true;
    };
    r.rec.link_hook = [this](bool up) {
      if (!up)
        parked_valid = false;
      link_up = up;
      link_dirty = true;
    };
    r.rec.command_hook = [this](bool answered) { t.on_command_done(r.bus.command_ticket(), answered, g_now); };
  }
  H::BusTicket ticket(bool queued) const { return queued ? r.last_ticket : H::BUS_TICKET_NONE; }
  H::BusTicket send_combined(const H::AcCommand &c) override { return ticket(r.send_cmd(c)); }
  H::BusTicket send_power(bool on) override {
    uint8_t f[H::CMD_FRAME_MAX];
    size_t n = H::build_power_frame(on, f, sizeof(f));
    return ticket(r.send(f, n));
  }
  H::BusTicket send_mute(bool) override { return H::BUS_TICKET_NONE; }
  H::BusTicket send_sleep(uint8_t) override { return H::BUS_TICKET_NONE; }
  void on_command_event(H::CommandEvent event, uint16_t, uint16_t count) override {
    if (event == H::CommandEvent::MAIN_CONFIRMED || event == H::CommandEvent::MAIN_YIELDED)
      done++;
    if (event == H::CommandEvent::DROPPED_ON_LINK_LOSS)
      dropped_on_link += count;
    if (event == H::CommandEvent::COMBINED_REFUSED_NO_STATUS)
      refused++;
  }
  void counters() {
    retries = static_cast<int>(t.command_retries());
    failed = static_cast<int>(t.failed_commands());
  }
  // HisenseClimate::control() for a setpoint and, optionally, a mode.
  void user(int8_t setpoint, int mode = -1) {
    uint16_t fields = H::CONFIRM_SETPOINT;
    bool power_on = false;
    H::AcCommand wanted = t.cmd();
    if (mode >= 0) {
      wanted.mode = static_cast<H::Mode>(mode);
      fields |= H::CONFIRM_MODE;
      power_on = !t.power_expected();
    }
    H::setpoint_to_cmd(setpoint, false, &wanted);
    t.send_user_command(wanted, fields, power_on, g_now);
  }
  void user_off() { t.send_user_power_off(g_now); }
  // HisenseAC::loop()
  void loop(uint32_t dt = 1) {
    r.step(dt);
    bool have = parked_valid;
    H::AcState st = parked;
    parked_valid = false;
    const bool edge = link_dirty;
    link_dirty = false;
    t.loop_pass(edge && !link_up, have ? &st : nullptr, g_now);
    if (have)
      link_up = true;
    counters();
  }
  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++)
      loop();
  }
};

// The same field set twice, with different values, before the first is confirmed. Both frames go
// out in order, the unit ends on the second, the first is never sent again and nothing is counted
// as a retry or a failure. Then the same with the first frame lost on the wire, and with the
// second lost: the scheduler's re-send keeps the order, so the unit still ends on the second.
static int g_lose_nth = 0;
static void script_set_twice(Rig &r) {
  MiniHub hub(r);
  hub.run(3000);
  size_t from = r.ac.writes.size();
  if (g_lose_nth == 1)
    r.ac.deaf_commands = 1;
  hub.user(20);
  hub.run(120);
  if (g_lose_nth == 2) {
    // Deaf to exactly the second frame: let the first be heard, then miss one.
    while (r.ac.heard_commands == 0)
      hub.loop();
    r.ac.deaf_commands = 1;
  }
  hub.user(28);
  hub.run(12000);
  std::vector<uint16_t> order = r.wire_tags(from);
  CHECK(std::is_sorted(order.begin(), order.end()), "the older frame never follows the newer one");
  CHECK(order.size() == (g_lose_nth == 0 ? 2u : 3u), "%zu frames on the wire", order.size());
  CHECK(r.ac.setpoint == 28 && r.rec.last.setpoint_c == 28, "the unit ends on the second value (%u)", r.ac.setpoint);
  CHECK(hub.done == 1 && hub.retries == 0 && hub.failed == 0, "confirmed once, no retry, no failure (%d/%d/%d)",
        hub.done, hub.retries, hub.failed);
  CHECK(hub.cmd.setpoint == 28, "the shadow holds the second value");
  r.audit_commands();
}

// Confirm and retry, exhausted. A unit that acknowledges and ignores: the frame, two re-sends
// (COMMAND_RESEND_MAX), then given up. A unit that does not even acknowledge: each of those three
// is itself sent three times by the scheduler (COMMAND_SENDS_MAX), nine frames in about 20 s.
static bool g_exhaust_deaf = false;
static void script_exhaustion(Rig &r) {
  MiniHub hub(r);
  hub.run(3000);
  if (g_exhaust_deaf)
    r.ac.deaf_commands = -1;
  else
    r.ac.takes_nothing = true;
  size_t from = r.ac.writes.size();
  uint32_t t0 = g_now;
  hub.user(30);
  uint32_t gave_up = 0;
  for (uint32_t i = 0; i < 40000; i++) {
    hub.loop();
    if (gave_up == 0 && hub.failed == 1)
      gave_up = g_now - t0;
  }
  size_t sends = r.count(0x65, from);
  CHECK(hub.failed == 1 && hub.retries == H::COMMAND_RESEND_MAX && hub.done == 0, "given up once after %d retries",
        hub.retries);
  CHECK(sends == (g_exhaust_deaf ? 9u : 3u), "%zu frames on the wire", sends);
  CHECK(gave_up > 0 && gave_up < 24500 + 3 * H::BUS_REPLY_TIMEOUT_MS, "given up %u ms after the request",
        (unsigned) gave_up);
  CHECK(!hub.pending.active && r.bus.queued() == 0, "nothing left waiting");
  CHECK(r.rec.links.empty(), "the link is not blamed");
  CHECK(r.ac.setpoint == 22, "the unit is where it was");
  // The hold-off has ended and the shadow follows the unit again: the next write starts clean.
  CHECK(hub.cmd.setpoint == 22, "shadow back on what the unit reports (%d)", hub.cmd.setpoint);
  r.audit_commands();
}

// A command waiting for its confirmation when the link drops is counted failed, once, and not sent
// again when the unit is back, however long that takes.
static void script_pending_at_link_loss(Rig &r) {
  MiniHub hub(r);
  hub.run(3000);
  r.ac.takes_nothing = true;  // so the command is still unconfirmed when the unit falls silent
  hub.user(30);
  hub.run(1500);
  r.ac.silent = true;
  r.ac.cut();
  hub.run(g_gap);
  CHECK(hub.dropped_on_link == 1 && hub.failed == 1, "counted failed at the link edge (%d)", hub.dropped_on_link);
  size_t from = r.ac.writes.size();
  r.ac.silent = false;
  r.ac.takes_nothing = false;
  hub.run(15000);
  CHECK(r.count(0x65, from) == 0, "nothing sent after the unit came back");
  CHECK(hub.failed == 1 && hub.retries <= 1, "and nothing more counted");
  CHECK(r.ac.setpoint == 22, "the unit is where it was");
  r.audit_commands();
}

// Was a WEAKNESS, fixed: a command issued while the link is ALREADY down is refused. Before: The hub drops what is
// pending at the link-lost edge and at no other time, so this one stays tracked. The scheduler
// sends it once into the silence. If the unit is back within CONFIRM_STALE_MS (20 s) the first
// status frame disagrees and the hub sends it again: a request made to a dead unit is carried out
// when it returns, which is the opposite of what the edge handler promises ("nothing still waiting
// is sent when it comes back"). After 20 s it is given up instead. Both outcomes are pinned here.
static void script_command_while_down(Rig &r) {
  MiniHub hub(r);
  hub.run(3000);
  r.ac.silent = true;
  r.ac.cut();
  hub.run(13000);  // five unanswered polls, two windows per cycle
  CHECK(!hub.link_up && r.rec.links.size() == 1, "link down");
  hub.user(30);
  CHECK(!hub.pending.active, "the command is refused while the link is down");
  hub.run(g_gap);
  size_t from = r.ac.writes.size();
  r.ac.silent = false;
  hub.run(15000);
  size_t after = r.count(0x65, from);
  CHECK(after == 0 && r.ac.setpoint == 22 && hub.failed == 0,
        "a command issued into a dead link is not carried out when the unit returns %u ms later (%zu frames, "
        "setpoint %u)",
        (unsigned) g_gap, after, r.ac.setpoint);
  r.audit_commands();
}

// ---- Seeded random runs --------------------------------------------------------------------------
struct StressResult {
  uint64_t hash;
  uint32_t steps, writes, statuses, timeouts, commands, losses;
};

static bool g_verbose = false;

static StressResult stress(uint32_t seed, uint32_t start, uint32_t run_ms) {
  static char name[48];
  snprintf(name, sizeof(name), "random seed %u", (unsigned) seed);
  g_ctx = name;
  Rng rng(seed);
  Rig r(start);
  // The wiring and the loop, fixed for the run.
  r.has_de = !rng.one_in(4);
  r.ac.echo = rng.one_in(5);
  r.ac.half_duplex = r.has_de && !r.ac.echo && rng.one_in(2);
  r.ac.blocking_write = rng.one_in(4);
  r.ac.fifo_cap = rng.one_in(3) ? rng.range(64, 320) : 0;
  r.ac.reply_delay = rng.range(30, 120);
  r.ac.noise = Rng(seed ^ 0x5EED);
  uint32_t fault_rate = rng.one_in(4) ? 0 : rng.range(3, 40);  // percent of replies damaged
  uint32_t max_dt = rng.one_in(2) ? 1 : rng.range(2, 60);
  r.progress_ms = 1500 + 6 * max_dt;
  if (g_verbose)
    printf("  seed %u: de=%d echo=%d half_duplex=%d blocking=%d fifo=%zu delay=%u faults=%u%% max_dt=%u\n",
           (unsigned) seed, r.has_de, r.ac.echo, r.ac.half_duplex, r.ac.blocking_write, r.ac.fifo_cap,
           (unsigned) r.ac.reply_delay, (unsigned) fault_rate, (unsigned) max_dt);
  Rng frng(seed + 77);
  r.ac.fault = [&frng, fault_rate](uint8_t, uint8_t) {
    if (frng.below(100) >= fault_rate)
      return FaultSpec{FAULT_NONE, 0};
    Fault kind = static_cast<Fault>(frng.range(1, FAULT_KIND_COUNT - 1));
    uint32_t arg = frng.below(200);
    if (kind == FAULT_LATE)
      arg = frng.below(1500);
    return FaultSpec{kind, arg};
  };
  uint32_t silent_left = 0;
  // `run_ms` of polled time: what a blocked loop skips comes on top.
  for (uint32_t spent = 0; spent < run_ms;) {
    uint32_t dt = 1;
    if (max_dt > 1 && !rng.one_in(3))
      dt = rng.range(1, max_dt);
    // Rates are per millisecond of bus time, so a coarse loop sees as much as a fine one.
    auto every = [&rng, dt](uint32_t ms) { return rng.below(ms) < dt; };
    if (every(25000)) {
      // The loop is blocked: half a second to five minutes.
      static const uint32_t BLOCKS[] = {500, 1500, 4000, 20000, 300000};
      r.block(BLOCKS[rng.below(5)]);
    }
    if (every(2500))
      r.send_setpoint(static_cast<int8_t>(rng.range(16, 32)));
    if (every(20000)) {
      uint32_t n = rng.range(2, 12);
      for (uint32_t i = 0; i < n; i++)
        r.send_setpoint(static_cast<int8_t>(rng.range(16, 32)));
    }
    if (every(15000)) {
      H::AcCommand c;
      c.mode = static_cast<H::Mode>(rng.below(5));
      c.power_on = rng.one_in(2);
      r.send_cmd(c);
    }
    if (every(30000)) {
      uint8_t f[H::CMD_FRAME_MAX];
      r.send(f, H::build_power_frame(rng.one_in(2), f, sizeof(f)));
    }
    if (silent_left == 0 && every(15000)) {
      static const uint32_t GAPS[] = {300, 2000, 7000, 30000};
      silent_left = GAPS[rng.below(4)];
      r.ac.silent = true;
      if (rng.one_in(2))
        r.ac.cut();
    }
    if (every(20000))
      r.ac.deaf_commands = static_cast<int>(rng.range(1, 5));
    if (every(40000)) {
      r.ac.dev_hi = static_cast<uint8_t>(rng.range(0, 3));
      r.ac.dev_lo = static_cast<uint8_t>(rng.range(0, 9));
    }
    if (every(5000))
      r.ac.link_bits_once = static_cast<uint8_t>(rng.next()) & H::LINK_REQ_MASK;
    if (every(20000))
      r.bus.set_setup_indicator(!r.bus.setup_indicator(), g_now);
    if (every(9000)) {
      // Two statements: the order of evaluation inside one call is the compiler's choice.
      bool status = rng.one_in(2);
      uint32_t in = rng.below(400);
      r.ac.inject(status ? r.ac.status_frame() : ac_frame(28, 0x1E, 0x00), g_now + in);
    }
    r.step(dt);
    spent += dt;
    if (silent_left > 0) {
      silent_left = silent_left > dt ? silent_left - dt : 0;
      if (silent_left == 0)
        r.ac.silent = false;
    }
  }
  // Convergence: the unit and the wire behave again, the loop is on time. Whatever happened before,
  // within 15 s the queue is empty, every command has been reported, the link is up and status
  // arrives every cycle with no timeout.
  r.ac.silent = false;
  r.ac.fault = nullptr;
  r.ac.deaf_commands = 0;
  r.ac.fifo_cap = 0;
  r.ac.link_bits_once = 0;
  r.ac.reply_delay = 40;
  r.progress_ms = 1500;
  if (r.ac.dev_hi == 0 && r.ac.dev_lo == 0)
    r.ac.dev_lo = 0x02;
  r.run(15000);
  uint32_t statuses = r.rec.statuses, timeouts = r.rec.timeouts, chk = r.bus.checksum_mismatches();
  size_t edges = r.rec.links.size();
  r.run(5000);
  CHECK(r.rec.statuses - statuses >= 5 && r.rec.statuses - statuses <= 6, "status every cycle at the end (%u in 5 s)",
        (unsigned) (r.rec.statuses - statuses));
  CHECK(r.rec.timeouts == timeouts && r.bus.checksum_mismatches() == chk,
        "no timeout and no checksum error at the end");
  CHECK(r.rec.links.size() == edges && (edges % 2) == 0, "link up at the end (%zu edges)", edges);
  H::AcFeatures ft;
  CHECK(r.bus.features(&ft) && ft.cool_heat, "capabilities known at the end");
  r.audit_commands();
  return {r.trace.hash,       r.steps,        static_cast<uint32_t>(r.ac.writes.size()),
          r.rec.statuses,     r.rec.timeouts, static_cast<uint32_t>(r.rec.commands.size()),
          r.bus.link_losses()};
}

static void test_random(uint32_t seeds, uint32_t run_ms) {
  printf("-- %u random seeds, %u s of bus time each\n", (unsigned) seeds, (unsigned) (run_ms / 1000));
  uint64_t steps = 0, writes = 0, timeouts = 0, commands = 0, losses = 0;
  for (uint32_t seed = 1; seed <= seeds; seed++) {
    // Start times are spread so that some runs cross the wrap early, some late, some never.
    uint32_t start;
    switch (seed % 4) {
      case 0:
        start = 0u - (seed * 977u) % run_ms;
        break;
      case 1:
        start = 0x80000000u - (seed * 613u) % run_ms;
        break;
      case 2:
        start = seed * 100003u;
        break;
      default:
        start = 0u - 1500u;
        break;
    }
    StressResult a = stress(seed, start, run_ms);
    if (g_print_traces)
      printf("   trace %016llx seed %u, start %u\n", (unsigned long long) a.hash, (unsigned) seed, (unsigned) start);
    steps += a.steps;
    writes += a.writes;
    timeouts += a.timeouts;
    commands += a.commands;
    losses += a.losses;
    if (seed <= 6) {
      // And the same seed from another start time is the same run.
      StressResult b = stress(seed, start ^ 0x80000000u, run_ms);
      CHECK(a.hash == b.hash, "seed %u: the run depends on the start time", (unsigned) seed);
    }
  }
  printf("   %llu polls, %llu frames sent, %llu timeouts, %llu command reports, %llu link losses\n",
         (unsigned long long) steps, (unsigned long long) writes, (unsigned long long) timeouts,
         (unsigned long long) commands, (unsigned long long) losses);
  CHECK(timeouts > 0 && commands > 0 && losses > 0, "the random runs reached the paths they are for");
}

int main(int argc, char **argv) {
  uint32_t seeds = 32, run_ms = 60000;
  long one_seed = -1;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--soak") == 0) {
      seeds = 2000;
      run_ms = 600000;
      if (i + 1 < argc && argv[i + 1][0] != '-')
        seeds = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      one_seed = strtol(argv[++i], nullptr, 10);
    } else if (strcmp(argv[i], "--traces") == 0) {
      g_print_traces = true;
    } else {
      printf("usage: %s [--soak [seeds]] [--seed N] [--traces]\n", argv[0]);
      return 2;
    }
  }
  printf("== ESPHome bus scheduler: edge cases and stress ==\n");
  if (one_seed >= 0) {
    g_verbose = true;
    StressResult s = stress(static_cast<uint32_t>(one_seed), 0u - 1500u, run_ms);
    printf("  %u polls, %u frames, %u statuses, %u timeouts, %u command reports, %u link losses\n", (unsigned) s.steps,
           (unsigned) s.writes, (unsigned) s.statuses, (unsigned) s.timeouts, (unsigned) s.commands,
           (unsigned) s.losses);
    printf("  %llu checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
  }

  // Time. The sweeps put the wrap at every millisecond offset that matters for the scenario: the
  // healthy cycle (settle, drain, reply, pace), the boot retries and the link-loss count of a silent
  // unit, the command re-send spacing, and the three setup lockouts.
  everywhere("healthy cycle and one command", script_healthy, 2300, 11);
  everywhere("healthy cycle, three polls per ms", script_healthy_burst);
  {
    g_ctx = "burst against single";
    CHECK(run_script(g_ctx, script_healthy, 1000) == run_script(g_ctx, script_healthy_burst, 1000),
          "a burst of loop calls leaves the same trace as one call per ms");
  }
  everywhere("silent from boot, lost once, restored once", script_silent_boot, 17000, 331);
  everywhere("command re-send spacing", script_deaf_command, 6500, 211);
  everywhere("setup-request lockout: boot, accepted request, indication off", script_setup_lockout, 60000, 3331);
  everywhere("blocked loop: 0.7 s to 3 h", script_clock_jumps);
  everywhere("blocked loop on a half-duplex wire", script_clock_jumps_half_duplex);
  everywhere("late loop calls, 1 to 30 ms", script_late_loop);

  // Link.
  everywhere("unit answers the first handshake step only", script_first_step_only);
  // The short gap has to swallow two cycles: the command's first send and its one re-send both
  // go into the silence (each cycle a heartbeat window, a command window, a status window). The
  // second gap is the shortest silence that loses the link.
  static const uint32_t GAPS[] = {2 * (3 * H::BUS_REPLY_TIMEOUT_MS + 300) + 500, LINK_LOST_BY_MS + 1500, 5 * 60000u,
                                  2 * 3600000u};
  for (uint32_t gap : GAPS) {
    static char name[64];
    snprintf(name, sizeof(name), "silent for %u s, then back", (unsigned) (gap / 1000));
    g_gap = gap;
    g_gap_dt = 10;
    if (gap > 600000) {
      // Hours of silence cost millions of polls: reference and one wrap start only.
      printf("-- %s\n", name);
      CHECK(run_script(name, script_gap, 1000) == run_script(name, script_gap, 0u - (gap / 2)),
            "same trace with the wrap in the middle of the silence");
    } else {
      everywhere(name, script_gap);
    }
  }
  static const uint8_t CUT_CLASSES[] = {0x1E, 0x65, 0x66};
  for (uint8_t cls : CUT_CLASSES) {
    for (uint32_t cut : {0u, 1u, 9u, 19u}) {
      static char name[64];
      snprintf(name, sizeof(name), "silent %u bytes into the reply to 0x%02X", (unsigned) cut, cls);
      g_cut_cls = cls;
      g_cut = cut;
      g_ctx = name;
      CHECK(run_script(name, script_cut_mid_reply, 1000) == run_script(name, script_cut_mid_reply, 0u - 4000u),
            "same trace across the wrap");
    }
  }
  printf("-- unit falls silent in the middle of a reply (12 cases)\n");

  // Replies.
  test_reply_faults();
  g_alive_fault = FAULT_WRONG_CLASS;
  everywhere("status poll answered with another class every time", script_not_alive);
  g_alive_fault = FAULT_FLIP;
  everywhere("status poll answered with a damaged frame every time", script_not_alive);
  everywhere("status reply cut at every length", script_truncate_sweep);
  everywhere("our own frames echoed", script_echo);
  everywhere("device type changes while the link is down", script_token_change);
  everywhere("DevType reply with a bad checksum", script_devtype_bad_checksum);
  everywhere("false frame start ahead of a reply", script_stx_pair);
  everywhere("unsolicited frames between cycles", script_unsolicited);

  // Commands.
  everywhere("command burst, one every 20 ms", script_burst);
  everywhere("a steady command stream does not starve heartbeat and status", script_starvation);
  everywhere("power and mode in one frame", script_power_and_mode);
  everywhere("queue ring: fill, overflow, drain, re-send", script_queue_ring);
  everywhere("0xF4 in a stamped checksum", script_stuffed_checksum);
  for (int lose = 0; lose <= 2; lose++) {
    static char name[64];
    snprintf(name, sizeof(name), "same field set twice (frame lost: %d)", lose);
    g_lose_nth = lose;
    everywhere(name, script_set_twice);
  }
  g_exhaust_deaf = false;
  everywhere("confirm and retry exhausted: acknowledged, not applied", script_exhaustion);
  g_exhaust_deaf = true;
  everywhere("confirm and retry exhausted: never acknowledged", script_exhaustion);
  g_gap = 14000;
  everywhere("command unconfirmed when the link drops, back after 14 s", script_pending_at_link_loss);
  g_gap = 5 * 60000u;
  everywhere("command unconfirmed when the link drops, back after 5 min", script_pending_at_link_loss);
  g_gap = 4000;
  everywhere("command issued while the link is down, back after 4 s", script_command_while_down);
  g_gap = 40000;
  everywhere("command issued while the link is down, back after 40 s", script_command_while_down);

  // Receive path.
  static const uint32_t COARSE[] = {1, 50, 150, 300};
  for (uint32_t dt : COARSE) {
    static char name[64];
    snprintf(name, sizeof(name), "no DE pin, polled every %u ms, 256 byte buffer", (unsigned) dt);
    g_coarse_dt = dt;
    g_coarse_fifo = 256;
    everywhere(name, script_coarse);
  }
  everywhere("receive buffer smaller than a status frame", script_overrun);
  everywhere("3 kB of noise in one reply window, frames at the size limit", script_flood);

  // DE.
  everywhere("DE release with a blocking UART write", script_de_blocking, 2300, 31);
  everywhere("transmitting() without a DE pin", script_no_de_transmitting);

  test_random(seeds, run_ms);

  printf("  %llu checks, %d failed, %d known defects or weaknesses still present\n", g_checks, g_fail, g_known);
  printf(g_fail ? "== BUS STRESS FAILED ==\n" : "== BUS STRESS OK ==\n");
  return g_fail ? 1 : 0;
}
