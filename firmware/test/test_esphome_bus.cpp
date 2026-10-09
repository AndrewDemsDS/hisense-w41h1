// Bus scheduler (firmware/esphome/components/hisense_ac/hisense_bus.*) against a simulated A/C.
//
// The scheduler replaces the original driver's FreeRTOS bus task, so these checks encode that
// task's contract (hisense_rs485.cpp hisense_bus_task / hisense_transact) as observable wire
// behaviour: frame order and content, envelope stamping, DE timing, the 500 ms reply window, the
// 1 s cycle, link loss and recovery, reply-class correlation and checksum rejection. Time advances
// in 1 ms steps; the simulated A/C answers each frame after a configurable delay.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "hisense_bus.h"
#include "hisense_protocol.h"

namespace H = esphome::hisense_ac;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond, ...) \
  do { \
    g_checks++; \
    if (!(cond)) { \
      g_fail++; \
      printf("  FAIL %s:%d ", __FILE__, __LINE__); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } \
  } while (0)

static uint32_t g_now = 1000;

// A frame as the A/C would send it: direction 0x01, LEN = len - 9, valid checksum.
static std::vector<uint8_t> ac_frame(size_t len, uint8_t cls, uint8_t sub) {
  std::vector<uint8_t> f(len, 0);
  f[0] = 0xF4;
  f[1] = 0xF5;
  f[2] = 0x01;
  f[3] = 0x40;
  f[4] = (uint8_t) (len - 9);
  f[13] = cls;
  f[14] = sub;
  uint16_t sum = H::checksum_range(f.data(), 2, len - 4);
  f[len - 4] = (uint8_t) (sum >> 8);
  f[len - 3] = (uint8_t) sum;
  f[len - 2] = 0xF4;
  f[len - 1] = 0xFB;
  return f;
}

static std::vector<uint8_t> status_frame(uint8_t setpoint, bool bad_checksum = false) {
  std::vector<uint8_t> f(160, 0);
  f[0] = 0xF4;
  f[1] = 0xF5;
  f[2] = 0x01;
  f[3] = 0x40;
  f[4] = 151;
  f[13] = 0x66;
  f[16] = 0x0A;         // fan low
  f[18] = 0x20 | 0x04;  // status mode nibble 2 = cool (raw, not the command encoding), running
  f[19] = setpoint;
  f[20] = 23;
  uint16_t sum = H::checksum_range(f.data(), 2, 156);
  f[156] = (uint8_t) (sum >> 8);
  f[157] = (uint8_t) sum;
  if (bad_checksum)
    f[157] ^= 0x01;
  f[158] = 0xF4;
  f[159] = 0xFB;
  return f;
}

struct Write {
  uint32_t t;
  bool de_high;
  std::vector<uint8_t> bytes;
};

struct DeEdge {
  uint32_t t;
  bool high;
};

class SimAC : public H::BusIO {
 public:
  bool responsive = true;
  bool corrupt_status = false;
  bool stale_1e_before_status = false;
  bool deaf_to_commands = false;     // hears everything except 0x65
  bool stale_1e_before_ack = false;  // a late 0x1E lands in the command's window, ahead of its ack
  uint32_t ack_starts = 0, ack_ends = 0;
  bool truncate_next_status = false;  // one status reply cut off after 60 bytes
  // Time of the last byte of the newest reply queued, to tell whether a write overlaps a reply.
  uint32_t reply_ends = 0;
  uint32_t reply_delay = 40;
  uint8_t devtype_hi = 0x01, devtype_lo = 0x02;
  uint8_t setpoint = 22;

  std::vector<Write> writes;
  std::vector<DeEdge> de;
  bool de_high = false;
  int set_de_calls = 0;

  void bus_set_de(bool high) override {
    set_de_calls++;
    de_high = high;
    de.push_back({g_now, high});
  }
  void bus_write(const uint8_t *data, size_t len) override {
    writes.push_back({g_now, de_high, std::vector<uint8_t>(data, data + len)});
    if (responsive)
      respond_(writes.back().bytes);
  }
  void bus_flush() override {}
  int bus_read() override {
    if (rx_.empty() || rx_.front().first > g_now)
      return -1;
    int b = rx_.front().second;
    rx_.pop_front();
    return b;
  }

 private:
  void queue_(const std::vector<uint8_t> &f, uint32_t at) {
    // One byte per ~1 ms (9600 baud), stuffing a checksum 0xF4 as the real A/C would.
    uint32_t t = at;
    for (size_t i = 0; i < f.size(); i++) {
      rx_.push_back({t, f[i]});
      if ((i == f.size() - 4 || i == f.size() - 3) && f[i] == 0xF4)
        rx_.push_back({t, 0xF4});
      reply_ends = t;
      t++;
    }
  }
  void respond_(const std::vector<uint8_t> &req) {
    uint32_t at = g_now + H::bus_tx_time_ms(req.size()) + reply_delay;
    uint8_t cls = req[13];
    if (cls == 0x0A) {
      auto f = ac_frame(24, 0x0A, 0x04);  // type at 16/17 needs a body past the checksum
      f[16] = devtype_hi;
      f[17] = devtype_lo;
      uint16_t s = H::checksum_range(f.data(), 2, f.size() - 4);
      f[f.size() - 4] = (uint8_t) (s >> 8);
      f[f.size() - 3] = (uint8_t) s;
      queue_(f, at);
    } else if (cls == 0x07) {
      queue_(ac_frame(20, 0x07, 0x00), at);
    } else if (cls == 0x1E) {
      queue_(ac_frame(28, 0x1E, 0x00), at);
    } else if (cls == 0x66 && req[14] == 0x40) {
      auto f = ac_frame(64, 0x66, 0x40);
      f[18] = 0x80;  // cool_heat
      uint16_t s = H::checksum_range(f.data(), 2, f.size() - 4);
      f[f.size() - 4] = (uint8_t) (s >> 8);
      f[f.size() - 3] = (uint8_t) s;
      queue_(f, at);
    } else if (cls == 0x66) {
      if (stale_1e_before_status) {
        queue_(ac_frame(28, 0x1E, 0x00), at);
        at += 40;
      }
      auto f = status_frame(setpoint, corrupt_status);
      if (truncate_next_status) {
        truncate_next_status = false;
        f.resize(60);
      }
      queue_(f, at);
    } else if (cls == 0x65) {
      if (stale_1e_before_ack) {
        queue_(ac_frame(28, 0x1E, 0x00), at);
        at += 40;
      }
      if (!deaf_to_commands) {
        ack_starts = at;
        queue_(ac_frame(20, 0x65, 0x00), at);
        ack_ends = reply_ends;
      }
    }
  }
  std::deque<std::pair<uint32_t, uint8_t>> rx_;
};

class Recorder : public H::BusListener {
 public:
  int statuses = 0, features = 0;
  std::vector<std::pair<uint32_t, bool>> links;
  H::AcState last{};
  void on_bus_status(const H::AcState &s) override {
    statuses++;
    last = s;
  }
  void on_bus_features(const H::AcFeatures &) override { features++; }
  void on_bus_link(bool up) override { links.push_back({g_now, up}); }
  // The diagnostic hooks the hub logs from.
  int frames = 0, checksum_errors = 0, timeouts = 0;
  void on_bus_frame(const uint8_t *, size_t len) override {
    if (len > 0)
      frames++;
  }
  void on_bus_checksum_error(const uint8_t *, size_t) override { checksum_errors++; }
  void on_bus_timeout(uint8_t) override { timeouts++; }
  std::vector<std::pair<bool, uint8_t>> commands;
  void on_bus_command(bool answered, uint8_t reply_class) override { commands.push_back({answered, reply_class}); }
};

static void run(H::BusScheduler &bus, uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    bus.poll(g_now);
    g_now++;
  }
}

static uint8_t cls(const Write &w) { return w.bytes.size() > 13 ? w.bytes[13] : 0; }

static void test_boot_and_cycle() {
  printf("-- boot, stamping, cycle order, DE timing, cadence\n");
  SimAC ac;
  Recorder rec;
  H::BusScheduler bus;
  bus.setup(&ac, &rec, true);
  run(bus, 5500);

  CHECK(ac.writes.size() > 12, "enough traffic (%zu writes)", ac.writes.size());
  // Boot: the DevType probe goes out verbatim (pre-link 00 00), then 0x07 stamped with the type.
  CHECK(ac.writes[0].bytes == std::vector<uint8_t>(H::LINK_INIT_0A, H::LINK_INIT_0A + H::LINK_INIT_0A_LEN),
        "first frame is the verbatim DevType probe");
  CHECK(cls(ac.writes[1]) == 0x07 && ac.writes[1].bytes[7] == 0x01 && ac.writes[1].bytes[8] == 0x02 &&
            H::status_checksum_ok(ac.writes[1].bytes.data(), ac.writes[1].bytes.size()),
        "0x07 is stamped with the learned device type 01 02 and re-checksummed");
  uint8_t hi = 0, lo = 0;
  CHECK(bus.link_token(&hi, &lo) && hi == 0x01 && lo == 0x02, "link token latched from the DevType reply");

  // First cycle: heartbeat (not heard yet: 0xF0), status poll, ProductType poll.
  CHECK(cls(ac.writes[2]) == 0x1E && ac.writes[2].bytes[16] == 0xF0, "first heartbeat says not-heard (0xF0)");
  CHECK(cls(ac.writes[3]) == 0x66 && ac.writes[3].bytes[14] == 0x00, "then the status poll");
  CHECK(cls(ac.writes[4]) == 0x66 && ac.writes[4].bytes[14] == 0x40, "then ProductType on the first heard cycle");
  CHECK(cls(ac.writes[5]) == 0x1E && ac.writes[5].bytes[16] == 0xB0, "next heartbeat says heard (0xB0)");
  CHECK(cls(ac.writes[6]) == 0x66 && ac.writes[6].bytes[14] == 0x00, "second cycle polls status");
  CHECK(cls(ac.writes[7]) == 0x1E, "and no ProductType again until 60 cycles");
  for (size_t i = 1; i < ac.writes.size(); i++) {
    const auto &b = ac.writes[i].bytes;
    CHECK(b[7] == 0x01 && b[8] == 0x02, "frame %zu stamped", i);
  }

  // Cadence: heartbeats start 1000 ms apart once replies are fast.
  std::vector<uint32_t> hb;
  for (auto &w : ac.writes)
    if (cls(w) == 0x1E)
      hb.push_back(w.t);
  for (size_t i = 2; i < hb.size(); i++)
    CHECK(hb[i] - hb[i - 1] == 1000, "cycle %zu period %u ms", i, (unsigned) (hb[i] - hb[i - 1]));

  // DE: every write happens with DE high, >= 5 ms after it rose, and DE falls >= wire time + 25 ms
  // after the write.
  for (auto &w : ac.writes) {
    CHECK(w.de_high, "write at %u with DE high", (unsigned) w.t);
    uint32_t rose = 0, fell = 0;
    for (auto &e : ac.de) {
      if (e.high && e.t <= w.t)
        rose = e.t;
      if (!e.high && e.t > w.t && fell == 0)
        fell = e.t;
    }
    CHECK(w.t - rose >= 5, "settle %u ms", (unsigned) (w.t - rose));
    uint32_t hold = fell - w.t, need = H::bus_tx_time_ms(w.bytes.size()) + 25;
    CHECK(hold >= need && hold <= need + 1, "DE hold %u ms (need %u)", (unsigned) hold, (unsigned) need);
  }

  CHECK(rec.statuses >= 4 && rec.last.setpoint_c == 22 && rec.last.mode == H::MODE_COOL, "status delivered (%d)",
        rec.statuses);
  CHECK(rec.features == 1, "features delivered once (%d)", rec.features);
  CHECK(rec.links.empty(), "no link edges on a healthy boot");
}

static void test_queue() {
  printf("-- command queue\n");
  SimAC ac;
  Recorder rec;
  H::BusScheduler bus;
  bus.setup(&ac, &rec, true);
  run(bus, 2500);
  uint8_t cmd[64];
  H::AcCommand c;
  size_t n = H::build_command(c, cmd, sizeof(cmd));
  for (int i = 0; i < 8; i++)
    CHECK(bus.enqueue(cmd, n), "enqueue %d", i);
  CHECK(!bus.enqueue(cmd, n), "ninth enqueue refused (queue of 8)");
  CHECK(!bus.enqueue(cmd, 0) && !bus.enqueue(cmd, 65), "empty and oversize frames refused");
  size_t before = ac.writes.size();
  run(bus, 3000);
  // Find the first cycle after the enqueue: heartbeat, 8 commands, status.
  size_t hb = before;
  while (hb < ac.writes.size() && cls(ac.writes[hb]) != 0x1E)
    hb++;
  CHECK(hb + 9 < ac.writes.size(), "cycle present");
  for (int i = 1; i <= 8; i++) {
    const auto &w = ac.writes[hb + i];
    CHECK(cls(w) == 0x65 && w.bytes[7] == 0x01 && w.bytes[8] == 0x02 &&
              H::status_checksum_ok(w.bytes.data(), w.bytes.size()),
          "queued command %d sent in the command slot, stamped", i);
  }
  CHECK(cls(ac.writes[hb + 9]) == 0x66, "status poll after the drained queue");
  CHECK(bus.queued() == 0, "queue empty");
}

static void test_link_loss_and_recovery() {
  printf("-- link loss, recovery handshake, reply window\n");
  SimAC ac;
  Recorder rec;
  H::BusScheduler bus;
  bus.setup(&ac, &rec, true);
  run(bus, 3000);
  CHECK(rec.frames >= rec.statuses && rec.statuses > 0, "every delivered status was first reported as a frame");
  ac.responsive = false;
  size_t before = ac.writes.size();
  uint32_t silent_at = g_now;
  int frames_before = rec.frames, timeouts_before = rec.timeouts;
  run(bus, 12000);
  CHECK(rec.frames == frames_before, "a silent A/C reports no frames");
  CHECK(rec.timeouts - timeouts_before >= 10, "and every unanswered transaction reports a timeout (%d)",
        rec.timeouts - timeouts_before);
  CHECK(rec.links.size() == 1 && !rec.links[0].second, "exactly one link-lost edge");
  // Five status polls unanswered: each costs heartbeat 500 + status 500 (+ TX), so the edge lands
  // after the fifth poll, ~5 cycles in.
  CHECK(!rec.links.empty() && rec.links[0].first - silent_at >= 4000, "lost after 5 missed polls (%u ms)",
        rec.links.empty() ? 0u : (unsigned) (rec.links[0].first - silent_at));
  // The edge fires on exactly the fifth unanswered status poll: count the polls sent after the
  // A/C fell silent, up to the edge.
  if (!rec.links.empty()) {
    int polls = 0;
    for (size_t i = before; i < ac.writes.size(); i++)
      if (cls(ac.writes[i]) == 0x66 && ac.writes[i].bytes[14] == 0x00 && ac.writes[i].t < rec.links[0].first)
        polls++;
    CHECK(polls == 5, "link lost on the 5th unanswered status poll (%d)", polls);
  }
  // While lost, each cycle opens with a DevType probe, and heartbeats say not-heard.
  bool saw_recover = false, saw_f0 = false;
  for (size_t i = before; i + 1 < ac.writes.size(); i++) {
    if (cls(ac.writes[i]) == 0x0A && cls(ac.writes[i + 1]) == 0x1E) {
      saw_recover = true;
      saw_f0 = ac.writes[i + 1].bytes[16] == 0xF0;
    }
  }
  CHECK(saw_recover && saw_f0, "recovery DevType probe, then a not-heard heartbeat");
  // Reply window: with the A/C silent, the next frame never starts before DE-low + 500 ms.
  for (size_t i = before + 1; i < ac.writes.size(); i++) {
    uint32_t fell = 0;
    for (auto &e : ac.de)
      if (!e.high && e.t < ac.writes[i].t)
        fell = e.t;
    uint32_t gap = ac.writes[i].t - fell;
    CHECK(gap >= 500 + 5, "gap before frame %zu is %u ms", i, (unsigned) gap);
  }
  ac.responsive = true;
  run(bus, 4000);
  CHECK(rec.links.size() == 2 && rec.links[1].second, "exactly one link-restored edge");
  CHECK(bus.link_losses() == 1, "one link loss counted (%u)", (unsigned) bus.link_losses());
  CHECK((int) bus.reply_timeouts() == rec.timeouts && bus.unanswered_commands() == 0,
        "every timeout counted (%u), none of them a command", (unsigned) bus.reply_timeouts());
}

static void test_correlation_and_checksum() {
  printf("-- reply-class correlation, checksum rejection\n");
  {
    SimAC ac;
    ac.stale_1e_before_status = true;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 4500);
    CHECK(rec.statuses >= 3 && rec.links.empty(), "a stale 0x1E ahead of the status reply is skipped (%d)",
          rec.statuses);
  }
  {
    SimAC ac;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 2500);
    ac.corrupt_status = true;
    run(bus, 1000);  // let a reply already in flight (valid) land first
    int ok_before = rec.statuses;
    run(bus, 7000);
    CHECK(rec.statuses == ok_before, "corrupt status frames are not delivered");
    CHECK(bus.checksum_mismatches() >= 5, "mismatches counted (%u)", (unsigned) bus.checksum_mismatches());
    CHECK(rec.checksum_errors == (int) bus.checksum_mismatches(), "each mismatch reaches the listener (%d)",
          rec.checksum_errors);
    CHECK(rec.links.size() == 1 && !rec.links[0].second, "and count as link misses");
  }
}

static void test_boot_retries() {
  printf("-- boot retries against a silent A/C\n");
  SimAC ac;
  ac.responsive = false;
  Recorder rec;
  H::BusScheduler bus;
  bus.setup(&ac, &rec, true);
  run(bus, 12000);
  size_t probes = 0;
  size_t i = 0;
  while (i < ac.writes.size() && cls(ac.writes[i]) == 0x0A) {
    probes++;
    i++;
  }
  CHECK(probes == 10, "ten DevType tries (%zu)", probes);
  CHECK(i < ac.writes.size() && cls(ac.writes[i]) == 0x07, "then 0x07");
  for (size_t k = 1; k < probes; k++) {
    uint32_t gap = ac.writes[k].t - ac.writes[k - 1].t;
    uint32_t want = 5 + H::bus_tx_time_ms(20) + 25 + 500 + 500;
    CHECK(gap == want, "try %zu gap %u ms (want %u)", k, (unsigned) gap, (unsigned) want);
  }
  uint8_t hi, lo;
  CHECK(!bus.link_token(&hi, &lo) && hi == 0x01 && lo == 0x01, "token stays the 01 01 default");
}

static void test_peripheral_de() {
  printf("-- peripheral-owned DE (flow_control_pin)\n");
  SimAC ac;
  Recorder rec;
  H::BusScheduler bus;
  bus.setup(&ac, &rec, false);
  run(bus, 3500);
  CHECK(ac.set_de_calls == 0, "DE never touched");
  CHECK(rec.statuses >= 2, "link works (%d statuses)", rec.statuses);
  CHECK(!bus.wants_fast_loop(), "no DE timing, no fast loop needed");
}

static size_t count_cls(const SimAC &ac, size_t from, uint8_t want) {
  size_t n = 0;
  for (size_t i = from; i < ac.writes.size(); i++)
    if (cls(ac.writes[i]) == want)
      n++;
  return n;
}

// What happens to a command frame, which is what a user write turns into.
static void test_command_fate() {
  printf("-- command fate: answered, unanswered, queued behind a poll, queued during boot\n");
  uint8_t cmd[64];
  size_t n = H::build_command(H::AcCommand{}, cmd, sizeof(cmd));
  uint8_t pwr[64];
  size_t pn = H::build_power_frame(true, pwr, sizeof(pwr));

  {  // Answered: the listener hears it, with the class of the reply.
    SimAC ac;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 2500);
    CHECK(bus.enqueue(cmd, n), "enqueue");
    run(bus, 2000);
    CHECK(rec.commands.size() == 1 && rec.commands[0].first && rec.commands[0].second == 0x65,
          "answered command reported once with class 0x65 (%zu)", rec.commands.size());
  }
  {  // Unanswered: reported as such, sent exactly once, and the link is not blamed for it.
    SimAC ac;
    ac.deaf_to_commands = true;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 2500);
    size_t before = ac.writes.size();
    CHECK(bus.enqueue(cmd, n), "enqueue");
    run(bus, 4000);
    CHECK(rec.commands.size() == 1 && !rec.commands[0].first && rec.commands[0].second == 0,
          "unanswered command reported (%zu)", rec.commands.size());
    CHECK(count_cls(ac, before, 0x65) == 1, "and not sent again (%zu)", count_cls(ac, before, 0x65));
    CHECK(bus.unanswered_commands() == 1 && bus.reply_timeouts() == 1 && bus.link_losses() == 0,
          "counted: %u unanswered, %u timeouts, %u link losses", (unsigned) bus.unanswered_commands(),
          (unsigned) bus.reply_timeouts(), (unsigned) bus.link_losses());
    CHECK(rec.links.empty(), "a lost command is not a lost link");
  }
  {
    // Enqueued while the status poll is on the wire: it waits for the next cycle's command slot,
    // goes out once, and the status delivered in between is the one from before the command.
    SimAC ac;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 2500);
    size_t at = ac.writes.size();
    while (ac.writes.size() == at || cls(ac.writes.back()) != 0x66 || ac.writes.back().bytes[14] != 0x00)
      run(bus, 1);
    size_t poll = ac.writes.size() - 1;
    int statuses = rec.statuses;
    CHECK(bus.enqueue(cmd, n), "enqueue during the status poll");
    run(bus, 2500);
    size_t sent = 0;
    for (size_t i = poll + 1; i < ac.writes.size() && sent == 0; i++)
      if (cls(ac.writes[i]) == 0x65)
        sent = i;
    CHECK(sent > poll + 1 && cls(ac.writes[sent - 1]) == 0x1E, "command follows the next heartbeat");
    CHECK(count_cls(ac, poll, 0x65) == 1, "sent once");
    CHECK(rec.statuses > statuses, "the poll in flight still delivered its (pre-command) status");
    uint32_t wait = ac.writes[sent].t - ac.writes[poll].t;
    CHECK(wait <= 1200, "worst-case wait for the command slot is about one cycle (%u ms)", (unsigned) wait);
  }
  {
    // Power-on then the combined frame, as a mode change on a powered-down unit sends them: same
    // cycle, in order, and the second never starts while the reply to the first is still arriving.
    // It does start the moment that reply ends: there is no guard time, which is the assumption a
    // capture has to back (does the unit take a command 5 ms after it acknowledged power-on).
    SimAC ac;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 2500);
    size_t before = ac.writes.size();
    CHECK(bus.enqueue(pwr, pn) && bus.enqueue(cmd, n), "enqueue power-on, then the combined frame");
    uint32_t first_reply_end = 0, second_de = 0;
    for (int i = 0; i < 2500; i++) {
      size_t writes = ac.writes.size(), edges = ac.de.size();
      bus.poll(g_now);
      if (ac.writes.size() > writes && cls(ac.writes.back()) == 0x65 && first_reply_end == 0) {
        first_reply_end = ac.reply_ends;
      } else if (ac.de.size() > edges && ac.de.back().high && first_reply_end != 0 && second_de == 0) {
        second_de = g_now;
      }
      g_now++;
    }
    size_t a = 0, b = 0;
    for (size_t i = before; i < ac.writes.size(); i++) {
      if (cls(ac.writes[i]) != 0x65)
        continue;
      if (a == 0) {
        a = i;
      } else if (b == 0) {
        b = i;
      }
    }
    CHECK(a != 0 && b == a + 1, "the two frames are consecutive on the wire");
    CHECK(a != 0 && ac.writes[a].bytes[18] == 0x0C, "power-on first");
    CHECK(second_de >= first_reply_end, "second frame waits for the first reply to finish");
    CHECK(second_de - first_reply_end <= 1, "and starts DE %u ms after it (no guard time)",
          (unsigned) (second_de - first_reply_end));
    CHECK(rec.commands.size() == 2 && rec.commands[0].first && rec.commands[1].first, "both answered");
  }
  {
    // Enqueued while the boot handshake is still knocking on a silent A/C: the frames survive
    // the ten tries, go out in the first cycle, and are reported lost.
    SimAC ac;
    ac.responsive = false;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 1000);
    CHECK(bus.enqueue(pwr, pn) && bus.enqueue(cmd, n), "enqueue during boot");
    CHECK(count_cls(ac, 0, 0x65) == 0, "nothing sent before the handshake is over");
    run(bus, 14000);
    CHECK(count_cls(ac, 0, 0x65) == 2, "both sent once the handshake gives up (%zu)", count_cls(ac, 0, 0x65));
    CHECK(rec.commands.size() == 2 && !rec.commands[0].first && !rec.commands[1].first, "both reported unanswered");
  }
}

// KNOWN AND NOT FIXED. A command's reply window accepts a frame of any class, as the shared driver's
// does. If a late frame of another class lands in it (the sim's stale 0x1E, which the status poll
// is already hardened against), the window closes on that frame and the next frame is transmitted
// at once, on top of the acknowledgement the A/C is about to send. On a half-duplex bus that
// garbles the next frame. When that frame is the combined command behind a power-on, the unit
// comes up in its last mode. Whether the real A/C ever sends such a frame needs a capture. The
// tell on hardware is the hub's log line "Command answered (class 0x1E)".
// This test pins what the scheduler does today, so that correlating the command reply (accept
// 0x65 and 0x66 only, keep listening otherwise) shows up as a deliberate change here.
static void test_stale_frame_in_command_window() {
  printf("-- KNOWN: a stale frame in a command's reply window\n");
  SimAC ac;
  Recorder rec;
  H::BusScheduler bus;
  bus.setup(&ac, &rec, true);
  run(bus, 2500);
  ac.stale_1e_before_ack = true;
  uint8_t cmd[64];
  size_t n = H::build_command(H::AcCommand{}, cmd, sizeof(cmd));
  uint8_t pwr[64];
  size_t pn = H::build_power_frame(true, pwr, sizeof(pwr));
  size_t before = ac.writes.size();
  CHECK(bus.enqueue(pwr, pn) && bus.enqueue(cmd, n), "enqueue power-on, then the combined frame");
  uint32_t first_ack_start = 0, first_ack_end = 0;
  for (int i = 0; i < 2500; i++) {
    size_t writes = ac.writes.size();
    bus.poll(g_now);
    if (ac.writes.size() > writes && cls(ac.writes.back()) == 0x65 && first_ack_start == 0) {
      first_ack_start = ac.ack_starts;
      first_ack_end = ac.ack_ends;
    }
    g_now++;
  }
  size_t second = 0;
  for (size_t i = before, seen = 0; i < ac.writes.size(); i++) {
    if (cls(ac.writes[i]) == 0x65 && ++seen == 2)
      second = i;
  }
  CHECK(second != 0, "both frames went out");
  CHECK(rec.commands.size() == 2 && rec.commands[0].first && rec.commands[0].second == 0x1E,
        "the power-on window closed on the stale 0x1E, not on its own ack");
  if (second != 0) {
    uint32_t tx_start = ac.writes[second].t;
    uint32_t tx_end = tx_start + H::bus_tx_time_ms(ac.writes[second].bytes.size());
    bool overlap = tx_start <= first_ack_end && tx_end >= first_ack_start;
    CHECK(overlap, "the combined frame (%u..%u ms) is sent over the power-on ack (%u..%u ms)", (unsigned) tx_start,
          (unsigned) tx_end, (unsigned) first_ack_start, (unsigned) first_ack_end);
  }
}

// One reply cut short on the wire costs that one poll and nothing else.
static void test_truncated_reply() {
  printf("-- truncated status reply\n");
  SimAC ac;
  Recorder rec;
  H::BusScheduler bus;
  bus.setup(&ac, &rec, true);
  run(bus, 2500);
  int before = rec.statuses;
  ac.truncate_next_status = true;
  run(bus, 1000);
  int during = rec.statuses;
  run(bus, 3000);
  CHECK(during - before <= 1, "the cut frame is not delivered");
  CHECK(rec.statuses - during >= 2, "the polls after it are (%d)", rec.statuses - during);
  CHECK(rec.links.empty(), "one bad poll is not a link loss");
}

// millis() wraps after 49.7 days. Every deadline here is a signed difference, so the cycle has to
// run straight through the wrap: same cadence, no link edge, a queued command still sent.
static void test_millis_rollover() {
  printf("-- millis() rollover\n");
  uint32_t saved = g_now;
  g_now = 0xFFFFFFFFu - 3500;
  SimAC ac;
  Recorder rec;
  H::BusScheduler bus;
  bus.setup(&ac, &rec, true);
  run(bus, 3000);
  uint8_t cmd[64];
  size_t n = H::build_command(H::AcCommand{}, cmd, sizeof(cmd));
  CHECK(bus.enqueue(cmd, n), "enqueue just before the wrap");
  int statuses = rec.statuses;
  run(bus, 5000);
  CHECK(g_now < 0x10000u, "the clock wrapped (now %u)", (unsigned) g_now);
  std::vector<uint32_t> hb;
  for (auto &w : ac.writes)
    if (cls(w) == 0x1E)
      hb.push_back(w.t);
  CHECK(hb.size() >= 7, "heartbeats kept coming (%zu)", hb.size());
  for (size_t i = 2; i < hb.size(); i++)
    CHECK(hb[i] - hb[i - 1] >= 1000 && hb[i] - hb[i - 1] <= 1200, "cycle %zu period %u ms across the wrap", i,
          (unsigned) (hb[i] - hb[i - 1]));
  CHECK(rec.statuses - statuses >= 4, "status kept arriving (%d)", rec.statuses - statuses);
  CHECK(rec.links.empty(), "no link edge at the wrap");
  CHECK(rec.commands.size() == 1 && rec.commands[0].first, "the queued command went out and was answered");
  g_now = saved;
}

int main() {
  printf("== ESPHome bus scheduler vs the original bus task's contract ==\n");
  test_boot_and_cycle();
  test_queue();
  test_link_loss_and_recovery();
  test_correlation_and_checksum();
  test_boot_retries();
  test_peripheral_de();
  test_command_fate();
  test_stale_frame_in_command_window();
  test_truncated_reply();
  test_millis_rollover();
  printf("  %d checks, %d failed\n", g_checks, g_fail);
  printf(g_fail ? "== BUS SCHEDULER FAILED ==\n" : "== BUS SCHEDULER OK ==\n");
  return g_fail ? 1 : 0;
}
