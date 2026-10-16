// Bus scheduler (firmware/esphome/components/hisense_ac/hisense_bus.*) against a simulated A/C.
//
// The scheduler replaces the original driver's FreeRTOS bus task, so these checks encode that
// task's contract (hisense_rs485.cpp hisense_bus_task / hisense_transact) as observable wire
// behaviour: frame order and content, envelope stamping, DE timing, the reply window, the
// 1 s cycle, link loss and recovery, reply-class correlation and checksum rejection. Time advances
// in 1 ms steps; the simulated A/C answers each frame after a configurable delay.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "hisense_bus.h"
#include "hisense_map.h"
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
  int deaf_commands_left = 0;        // misses this many 0x65 frames, then hears again
  bool stale_1e_before_ack = false;  // a late 0x1E lands in the command's window, ahead of its ack
  uint32_t ack_starts = 0, ack_ends = 0;
  bool truncate_next_status = false;  // one status reply cut off after 60 bytes
  bool deaf_to_devtype = false;       // hears everything except the 0x0A probe
  int corrupt_devtype_left = 0;       // this many DevType replies arrive with one flipped bit in byte 17
  bool corrupt_link_reply = false;    // every 0x1E reply arrives with one flipped bit in its checksum
  // Time of the last byte of the newest reply queued, to tell whether a write overlaps a reply.
  uint32_t reply_ends = 0;
  uint32_t reply_delay = 40;
  uint8_t devtype_hi = 0x01, devtype_lo = 0x02;
  uint8_t setpoint = 22;
  // bus_write() returns only once the bytes are on the wire, as on a platform whose UART write
  // waits on the transmit FIFO (LibreTiny) instead of queueing into a driver buffer (ESP-IDF).
  bool blocking_write = false;

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
    if (blocking_write)
      g_now += H::bus_tx_time_ms(len);
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
    if (cls == 0x0A && deaf_to_devtype) {
      return;
    } else if (cls == 0x0A) {
      auto f = ac_frame(24, 0x0A, 0x04);  // type at 16/17 needs a body past the checksum
      f[16] = devtype_hi;
      f[17] = devtype_lo;
      uint16_t s = H::checksum_range(f.data(), 2, f.size() - 4);
      f[f.size() - 4] = (uint8_t) (s >> 8);
      f[f.size() - 3] = (uint8_t) s;
      if (corrupt_devtype_left > 0) {
        corrupt_devtype_left--;
        f[17] ^= 0x40;  // 01 02 arrives as 01 42, under the checksum of 01 02
      }
      queue_(f, at);
    } else if (cls == 0x07) {
      queue_(ac_frame(20, 0x07, 0x00), at);
    } else if (cls == 0x1E) {
      auto f = ac_frame(28, 0x1E, 0x00);
      if (corrupt_link_reply)
        f[25] ^= 0x01;
      queue_(f, at);
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
      if (deaf_commands_left > 0) {
        deaf_commands_left--;
      } else if (!deaf_to_commands) {
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
  // Set to read, per report, whether the scheduler said it would send the frame again.
  H::BusScheduler *bus = nullptr;
  std::vector<bool> resends;
  std::vector<H::BusTicket> tickets;
  void on_bus_command(bool answered, uint8_t reply_class) override {
    commands.push_back({answered, reply_class});
    resends.push_back(bus != nullptr && bus->command_resend_pending());
    tickets.push_back(bus != nullptr ? bus->command_ticket() : H::BUS_TICKET_NONE);
  }
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
  // The queue goes out in the command slot, between the heartbeat and the status poll, as many
  // frames per cycle as start within BUS_COMMAND_SLOT_MS of the cycle's start. Eight do not fit
  // one cycle: the rest follow in the next, and the heartbeat and the status poll keep their pace.
  size_t hb = before;
  while (hb < ac.writes.size() && cls(ac.writes[hb]) != 0x1E)
    hb++;
  int sent = 0, cycles_used = 0;
  bool order_ok = true, slot_ok = true;
  uint32_t cycle_at = 0;
  for (size_t i = hb; i < ac.writes.size() && sent < 8; i++) {
    const auto &w = ac.writes[i];
    if (cls(w) == 0x1E) {
      // Every cycle that carries commands: heartbeat, commands, status poll.
      if (cycle_at != 0)
        CHECK(w.t - cycle_at == 1000, "the next heartbeat is 1000 ms after the last (%u)", (unsigned) (w.t - cycle_at));
      cycle_at = w.t;
      cycles_used++;
      continue;
    }
    if (cls(w) != 0x65) {
      order_ok = order_ok && cls(w) == 0x66 && cls(ac.writes[i - 1]) != 0x1E;
      continue;
    }
    sent++;
    CHECK(w.bytes[7] == 0x01 && w.bytes[8] == 0x02 && H::status_checksum_ok(w.bytes.data(), w.bytes.size()),
          "queued command %d stamped", sent);
    slot_ok = slot_ok && (cls(ac.writes[i - 1]) == 0x1E || w.t - cycle_at < H::BUS_COMMAND_SLOT_MS + 5);
    order_ok = order_ok && (cls(ac.writes[i - 1]) == 0x1E || cls(ac.writes[i - 1]) == 0x65);
  }
  CHECK(sent == 8 && bus.queued() == 0, "all eight sent, queue empty (%d)", sent);
  CHECK(order_ok, "commands only between the heartbeat and the status poll");
  CHECK(slot_ok, "no command starts later than BUS_COMMAND_SLOT_MS into its cycle");
  CHECK(cycles_used == 2, "eight frames take two cycles (%d)", cycles_used);
  CHECK(rec.timeouts == 0 && rec.links.empty(), "and nothing else is disturbed");

  // The rule itself. The first command of a cycle always goes; after that the slot closes at
  // BUS_COMMAND_SLOT_MS.
  CHECK(H::BUS_COMMAND_SLOT_MS == 600, "the slot is 600 ms");
  CHECK(H::command_slot_open(0, 0) && H::command_slot_open(0, 599) && H::command_slot_open(0, 600) &&
            H::command_slot_open(0, 5000) && H::command_slot_open(0, 0xFFFFFFFFu),
        "first command of a cycle: always");
  for (int n : {1, 2, 7, 255}) {
    CHECK(H::command_slot_open((uint8_t) n, 0) && H::command_slot_open((uint8_t) n, 599), "command %d before 600 ms",
          n + 1);
    CHECK(!H::command_slot_open((uint8_t) n, 600) && !H::command_slot_open((uint8_t) n, 601) &&
              !H::command_slot_open((uint8_t) n, 0xFFFFFFFFu),
          "command %d at or after 600 ms waits for the next cycle", n + 1);
  }
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
  run(bus, 24000);
  CHECK(rec.frames == frames_before, "a silent A/C reports no frames");
  CHECK(rec.timeouts - timeouts_before >= 10, "and every unanswered transaction reports a timeout (%d)",
        rec.timeouts - timeouts_before);
  CHECK(rec.links.size() == 1 && !rec.links[0].second, "exactly one link-lost edge");
  // Five status polls unanswered: each costs a heartbeat window and a status window (+ TX), so
  // the edge lands after the fifth poll, ~5 cycles in.
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
  // Reply window: with the A/C silent, the next frame never starts before DE-low + the window.
  for (size_t i = before + 1; i < ac.writes.size(); i++) {
    uint32_t fell = 0;
    for (auto &e : ac.de)
      if (!e.high && e.t < ac.writes[i].t)
        fell = e.t;
    uint32_t gap = ac.writes[i].t - fell;
    CHECK(gap >= H::BUS_REPLY_TIMEOUT_MS + 5, "gap before frame %zu is %u ms", i, (unsigned) gap);
  }
  ac.responsive = true;
  run(bus, 4000);
  CHECK(rec.links.size() == 2 && rec.links[1].second, "exactly one link-restored edge");
  CHECK(bus.link_losses() == 1, "one link loss counted (%u)", (unsigned) bus.link_losses());
  // The recovery handshake is a new bring-up: ProductType is asked once more, and only once.
  {
    size_t pt_before = 0, pt_after = 0;
    for (size_t i = 0; i < ac.writes.size(); i++)
      if (cls(ac.writes[i]) == 0x66 && ac.writes[i].bytes[14] == 0x40)
        (i < before ? pt_before : pt_after)++;
    CHECK(pt_before == 1 && pt_after == 1, "one ProductType poll per handshake (%zu before, %zu after)", pt_before,
          pt_after);
    H::AcFeatures ft;
    CHECK(bus.features(&ft) && ft.valid && rec.features == 2, "features re-read after the recovery (%d)", rec.features);
  }
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

// Nothing is taken from a frame that fails its checksum: not the device type (it would be stamped
// into bytes 7/8 of every later frame, and a wrong pair there makes the unit reject them all, RE
// docs/10 section 4.5), not the handshake answer, and not "I have heard the unit".
static void test_damaged_frames_are_not_believed() {
  printf("-- a frame with a bad checksum: no device type, no handshake answer, no 'heard'\n");
  {
    SimAC ac;
    ac.corrupt_devtype_left = 3;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 6000);
    size_t probes = 0;
    while (probes < ac.writes.size() && cls(ac.writes[probes]) == 0x0A)
      probes++;
    CHECK(probes == 4, "three damaged replies are three failed tries, the fourth is answered (%zu probes)", probes);
    for (size_t k = 1; k < probes; k++) {
      // A damaged reply ends its window when it arrives, then the usual retry wait before the next try.
      uint32_t gap = ac.writes[k].t - ac.writes[k - 1].t;
      CHECK(gap > H::BUS_BOOT_RETRY_MS &&
                gap < 5 + H::bus_tx_time_ms(20) + 25 + H::BUS_REPLY_TIMEOUT_MS + H::BUS_BOOT_RETRY_MS,
            "try %zu follows after %u ms", k, (unsigned) gap);
    }
    uint8_t hi = 0, lo = 0;
    CHECK(bus.link_token(&hi, &lo) && hi == 0x01 && lo == 0x02, "device type from the whole reply (%02X %02X)", hi, lo);
    bool wrong = false;
    for (auto &w : ac.writes)
      if (cls(w) != 0x0A && (w.bytes[7] != 0x01 || w.bytes[8] != 0x02))
        wrong = true;
    CHECK(!wrong, "no frame carries the damaged pair 01 42");
    CHECK(bus.checksum_mismatches() == 3 && rec.checksum_errors == 3,
          "each damaged reply counted and reported (%u, %d)", (unsigned) bus.checksum_mismatches(),
          rec.checksum_errors);
    CHECK(rec.statuses >= 2 && rec.links.empty(), "the link comes up as usual afterwards");
  }
  {
    // Every DevType reply damaged: ten tries, then on with the seeded 01 01, as for a silent unit.
    SimAC ac;
    ac.corrupt_devtype_left = 1000;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 12000);
    size_t probes = 0;
    while (probes < ac.writes.size() && cls(ac.writes[probes]) == 0x0A)
      probes++;
    uint8_t hi = 0xEE, lo = 0xEE;
    CHECK(probes == 10 && cls(ac.writes[probes]) == 0x07, "ten tries, then 0x07 (%zu)", probes);
    CHECK(!bus.link_token(&hi, &lo) && hi == 0x01 && lo == 0x01, "no device type learned, the seed stays (%02X %02X)",
          hi, lo);
    CHECK(ac.writes[probes].bytes[7] == 0x01 && ac.writes[probes].bytes[8] == 0x01, "0x07 carries the seed 01 01");
  }
  {
    // The heartbeat's byte 16 turns from 0xF0 to 0xB0 once the unit's reply was heard. A reply
    // that fails its checksum is not that.
    SimAC ac;
    ac.corrupt_link_reply = true;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 4500);
    int beats = 0, heard = 0;
    for (auto &w : ac.writes) {
      if (cls(w) != 0x1E)
        continue;
      beats++;
      heard += w.bytes[16] == 0xB0 ? 1 : 0;
    }
    CHECK(beats >= 4 && heard == 0, "damaged 0x1E replies: the heartbeat keeps saying not-heard (%d of %d)", heard,
          beats);
    CHECK(rec.statuses >= 3, "status still flows (%d)", rec.statuses);
    ac.corrupt_link_reply = false;
    size_t mark = ac.writes.size();
    run(bus, 2500);
    bool now_heard = false;
    for (size_t i = mark; i < ac.writes.size(); i++)
      if (cls(ac.writes[i]) == 0x1E && ac.writes[i].bytes[16] == 0xB0)
        now_heard = true;
    CHECK(now_heard, "a whole reply turns it to heard");
  }
}

// The recovery probe opens the cycle and the status poll comes later in it. A unit that returns
// between the two answers the poll, the link is up again, and no probe would follow: it was taken
// back without being asked for its device type. One more probe goes out in the next cycle.
static void test_return_between_probe_and_poll() {
  printf("-- the unit comes back between the recovery probe and the status poll\n");
  for (int deaf = 0; deaf < 2; deaf++) {
    SimAC ac;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 2500);
    ac.responsive = false;
    run(bus, 13000);  // five unanswered polls, two windows per cycle
    CHECK(rec.links.size() == 1 && !rec.links[0].second, "silent: link lost");
    // Run to the next recovery probe, let it go unanswered, then bring the unit back as another
    // model (or one that does not answer the probe at all).
    size_t n = ac.writes.size();
    while (ac.writes.size() == n || cls(ac.writes.back()) != 0x0A)
      run(bus, 1);
    ac.devtype_hi = 0x03;
    ac.devtype_lo = 0x07;
    ac.deaf_to_devtype = deaf != 0;
    ac.responsive = true;
    size_t back = ac.writes.size();
    run(bus, 6000);
    CHECK(rec.links.size() == 2 && rec.links[1].second, "back: link up");
    size_t probes = 0, first_probe = 0;
    for (size_t i = back; i < ac.writes.size(); i++) {
      if (cls(ac.writes[i]) != 0x0A)
        continue;
      probes++;
      if (first_probe == 0)
        first_probe = i;
    }
    CHECK(probes == 1, "%s: exactly one more probe after the unit is back (%zu)",
          deaf ? "deaf to the probe" : "answering", probes);
    CHECK(first_probe > back && cls(ac.writes[first_probe + 1]) == 0x1E,
          "it opens the next cycle, ahead of the heartbeat");
    uint8_t hi = 0, lo = 0;
    bus.link_token(&hi, &lo);
    if (deaf) {
      CHECK(hi == 0x01 && lo == 0x02, "unanswered: the device type known before stays (%02X %02X)", hi, lo);
    } else {
      CHECK(hi == 0x03 && lo == 0x07, "answered: the device type is learned again (%02X %02X)", hi, lo);
      CHECK(ac.writes.back().bytes[7] == 0x03 && ac.writes.back().bytes[8] == 0x07, "and stamped from then on");
    }
    bool heard = false;
    for (size_t i = first_probe; i < ac.writes.size(); i++)
      heard = heard || (cls(ac.writes[i]) == 0x1E && ac.writes[i].bytes[16] == 0xB0);
    CHECK(heard, "the extra probe does not put the heartbeat back to not-heard");
  }
}

// enqueue() gives every accepted frame a ticket, and on_bus_command() says which frame it is about.
struct TicketProbe : H::BusScheduler {
  void set_last_ticket(H::BusTicket t) { this->last_ticket_ = t; }
};

static void test_command_tickets() {
  printf("-- tickets: which frame a command report is about\n");
  SimAC ac;
  Recorder rec;
  TicketProbe bus;
  rec.bus = &bus;
  bus.setup(&ac, &rec, true);
  run(bus, 2500);
  uint8_t cmd[64];
  size_t n = H::build_command(H::AcCommand{}, cmd, sizeof(cmd));
  CHECK(bus.command_ticket() == H::BUS_TICKET_NONE, "no report yet: no ticket");
  H::BusTicket a = bus.enqueue(cmd, n), b = bus.enqueue(cmd, n), c = bus.enqueue(cmd, n);
  CHECK(a != H::BUS_TICKET_NONE && b == a + 1 && c == b + 1, "three frames, three tickets in a row (%u %u %u)", a, b,
        c);
  CHECK(bus.enqueue(cmd, 0) == H::BUS_TICKET_NONE && bus.enqueue(nullptr, n) == H::BUS_TICKET_NONE &&
            bus.enqueue(cmd, H::TX_FRAME_MAX + 1) == H::BUS_TICKET_NONE,
        "a refused frame gets none");
  H::BusTicket d = bus.enqueue(cmd, n);
  CHECK(d == c + 1, "and uses none up (%u)", d);
  // The second frame is missed once by the unit: its report comes twice, with the same ticket.
  ac.deaf_commands_left = 0;
  run(bus, 1);
  size_t sent_before = rec.tickets.size();
  while (rec.tickets.size() == sent_before)
    run(bus, 1);
  ac.deaf_commands_left = 1;
  run(bus, 4000);
  std::vector<H::BusTicket> want = {a, b, b, c, d};
  CHECK(rec.tickets == want, "reports carry the tickets in queue order, the re-sent frame twice (%zu reports)",
        rec.tickets.size());
  CHECK(rec.commands.size() == 5 && rec.commands[0].first && !rec.commands[1].first && rec.resends[1] &&
            rec.commands[2].first,
        "the repeated ticket is the unanswered send and its re-send");
  // Full queue: the ninth frame gets no ticket.
  for (int i = 0; i < 8; i++)
    CHECK(bus.enqueue(cmd, n) == (H::BusTicket)(d + 1 + i), "frame %d of a full queue", i);
  CHECK(bus.enqueue(cmd, n) == H::BUS_TICKET_NONE && !bus.enqueue(cmd, n), "the ninth is refused, and reads as false");
  run(bus, 4000);
  // The counter wraps past 0, which stays "refused".
  bus.set_last_ticket(0xFFFE);
  H::BusTicket x = bus.enqueue(cmd, n), y = bus.enqueue(cmd, n), z = bus.enqueue(cmd, n);
  CHECK(x == 0xFFFF && y == 1 && z == 2, "after 0xFFFF comes 1 (%u %u %u)", x, y, z);
  size_t mark = rec.tickets.size();
  run(bus, 3000);
  CHECK(rec.tickets.size() == mark + 3 && rec.tickets[mark] == 0xFFFF && rec.tickets[mark + 1] == 1 &&
            rec.tickets[mark + 2] == 2,
        "and the reports follow");
}

// The small things around the cycle: the accessors the hub reads, a poll before setup, the
// ProductType poll coming round again, and a unit whose DevType reply says 00 00.
static void test_accessors_and_corners() {
  printf("-- accessors, a poll before setup, ProductType once per handshake, device type 00 00\n");
  {
    H::BusScheduler idle;
    for (uint32_t t = 0; t < 3000; t += 7)
      idle.poll(t);  // no I/O set: nothing to do, and nothing to crash on
    H::AcFaults fl;
    H::AcFeatures ft;
    uint8_t hi = 0, lo = 0;
    CHECK(!idle.faults(&fl) && !idle.features(&ft) && !idle.link_token(&hi, &lo) && hi == 0x01 && lo == 0x01 &&
              idle.queued() == 0 && !idle.transmitting() && !idle.wants_fast_loop(),
          "a scheduler that was never set up does nothing and knows nothing");
  }
  SimAC ac;
  Recorder rec;
  H::BusScheduler bus;
  bus.setup(&ac, &rec, true);
  H::AcFaults fl;
  H::AcFeatures ft;
  CHECK(!bus.faults(&fl) && !bus.features(&ft), "before the first replies: no faults, no capabilities");
  run(bus, 5000);
  CHECK(bus.faults(&fl) && fl.valid && !fl.any, "faults() after a status frame: valid, none");
  CHECK(bus.features(&ft) && ft.valid && ft.cool_heat && rec.features == 1, "features() after the ProductType reply");
  // Switching the indication to what it already is changes nothing, the lockout included.
  bus.set_setup_indicator(false, g_now);
  CHECK(!bus.setup_indicator(), "off stays off");
  bus.set_setup_indicator(true, g_now);
  bus.set_setup_indicator(true, g_now + 5);
  CHECK(bus.setup_indicator(), "on stays on");
  bus.set_setup_indicator(false, g_now);
  // The ProductType poll: once on the first heard cycle, and never again while the link holds
  // (the stock module asks once per bring-up; a periodic re-poll cost one timeout each, #186).
  size_t polls = 0;
  for (auto &w : ac.writes)
    polls += cls(w) == 0x66 && w.bytes[14] == 0x40 ? 1 : 0;
  CHECK(polls == 1, "one ProductType poll in the first 5 s (%zu)", polls);
  run(bus, 125000);
  polls = 0;
  for (auto &w : ac.writes)
    polls += cls(w) == 0x66 && w.bytes[14] == 0x40 ? 1 : 0;
  CHECK(polls == 1, "still one ProductType poll after 130 s (%zu)", polls);
  CHECK(rec.features == 1 && rec.timeouts == 0, "answered once");
  {
    // 00 00 in a whole DevType reply is "not linked yet" to the stock module: the handshake is
    // answered, the pair is not adopted, the seeded 01 01 stays.
    SimAC zero;
    zero.devtype_hi = 0x00;
    zero.devtype_lo = 0x00;
    Recorder r2;
    H::BusScheduler b2;
    b2.setup(&zero, &r2, true);
    run(b2, 3000);
    uint8_t hi = 0xEE, lo = 0xEE;
    CHECK(!b2.link_token(&hi, &lo) && hi == 0x01 && lo == 0x01, "00 00 is not adopted (%02X %02X)", hi, lo);
    CHECK(cls(zero.writes[0]) == 0x0A && cls(zero.writes[1]) == 0x07 && zero.writes[1].bytes[7] == 0x01 &&
              zero.writes[1].bytes[8] == 0x01,
          "one probe, then 0x07 with the seed");
    CHECK(r2.statuses >= 1 && bus.checksum_mismatches() == 0, "and the link comes up");
  }
}

static void test_boot_retries() {
  printf("-- boot retries against a silent A/C\n");
  SimAC ac;
  ac.responsive = false;
  Recorder rec;
  H::BusScheduler bus;
  bus.setup(&ac, &rec, true);
  run(bus, 18000);  // ten tries, each a window plus the retry wait
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
    uint32_t want = 5 + H::bus_tx_time_ms(20) + 25 + H::BUS_REPLY_TIMEOUT_MS + H::BUS_BOOT_RETRY_MS;
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
  {
    // Unanswered by a unit that answers everything else: sent again, once per cycle, three sends
    // in all, each reported, and the link is not blamed for it.
    SimAC ac;
    ac.deaf_to_commands = true;
    Recorder rec;
    H::BusScheduler bus;
    rec.bus = &bus;
    bus.setup(&ac, &rec, true);
    run(bus, 2500);
    size_t before = ac.writes.size();
    CHECK(bus.enqueue(cmd, n), "enqueue");
    run(bus, 8000);
    CHECK(rec.commands.size() == 3 && !rec.commands[0].first && rec.commands[0].second == 0,
          "each unanswered send reported (%zu)", rec.commands.size());
    CHECK(rec.resends.size() == 3 && rec.resends[0] && rec.resends[1] && !rec.resends[2],
          "the listener is told the first two go out again and the third does not");
    CHECK(count_cls(ac, before, 0x65) == H::COMMAND_SENDS_MAX, "three sends, then the frame is dropped (%zu)",
          count_cls(ac, before, 0x65));
    std::vector<uint32_t> at;
    std::vector<size_t> idx;
    for (size_t i = before; i < ac.writes.size(); i++) {
      if (cls(ac.writes[i]) == 0x65) {
        at.push_back(ac.writes[i].t);
        idx.push_back(i);
      }
    }
    for (size_t i = 1; i < at.size(); i++) {
      // An unanswered command holds its window open, so that cycle runs long by about the window.
      CHECK(at[i] - at[i - 1] >= 900 && at[i] - at[i - 1] <= 1200 + H::BUS_REPLY_TIMEOUT_MS,
            "send %zu is one cycle after the last (%u ms)", i, (unsigned) (at[i] - at[i - 1]));
      CHECK(cls(ac.writes[idx[i] - 1]) == 0x1E, "and sits in the command slot, behind the heartbeat");
    }
    CHECK(bus.unanswered_commands() == 3 && bus.reply_timeouts() == 3 && bus.command_resends() == 2 &&
              bus.link_losses() == 0,
          "counted: %u unanswered, %u timeouts, %u re-sends, %u link losses", (unsigned) bus.unanswered_commands(),
          (unsigned) bus.reply_timeouts(), (unsigned) bus.command_resends(), (unsigned) bus.link_losses());
    CHECK(rec.links.empty(), "a lost command is not a lost link");
    CHECK(bus.queued() == 0, "queue empty afterwards");
    CHECK(bus.command_reply() == H::COMMAND_REPLY_NONE, "verdict: no reply");
  }
  {  // The case from the hardware runs: one frame gets no reply. The second send is answered.
    SimAC ac;
    ac.deaf_commands_left = 1;
    Recorder rec;
    H::BusScheduler bus;
    rec.bus = &bus;
    bus.setup(&ac, &rec, true);
    run(bus, 2500);
    size_t before = ac.writes.size();
    CHECK(bus.enqueue(cmd, n), "enqueue");
    run(bus, 6000);
    CHECK(count_cls(ac, before, 0x65) == 2, "sent twice (%zu)", count_cls(ac, before, 0x65));
    CHECK(rec.commands.size() == 2 && !rec.commands[0].first && rec.commands[1].first && rec.resends[0] &&
              !rec.resends[1],
          "unanswered, then answered");
    CHECK(bus.unanswered_commands() == 1 && bus.command_resends() == 1, "one unanswered, one re-send");
  }
  {
    // A newer frame queued behind one that is being sent again never overtakes it: the order on
    // the wire is old, old again, new. Sent the other way round, the old request would win.
    SimAC ac;
    ac.deaf_commands_left = 1;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 2500);
    H::AcCommand heat;
    heat.mode = H::MODE_HEAT;
    uint8_t newer[64];
    size_t nn = H::build_command(heat, newer, sizeof(newer));
    size_t before = ac.writes.size();
    CHECK(bus.enqueue(cmd, n) && bus.enqueue(newer, nn), "enqueue cool, then heat");
    run(bus, 6000);
    std::vector<uint8_t> modes;
    for (size_t i = before; i < ac.writes.size(); i++)
      if (cls(ac.writes[i]) == 0x65)
        modes.push_back(ac.writes[i].bytes[18]);
    CHECK(modes.size() == 3 && modes[0] == 0x50 && modes[1] == 0x50 && modes[2] == 0x30,
          "wire order: cool, cool again, heat (%zu frames)", modes.size());
    CHECK(rec.commands.size() == 3 && rec.commands[2].first, "and heat is answered last");
  }
  {
    // A unit that answers nothing: no re-send. Five missed polls later the link is declared lost,
    // and a frame queued then still goes out once and is dropped.
    SimAC ac;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    run(bus, 2500);
    ac.responsive = false;
    run(bus, 3000);  // one status poll has gone unanswered (a heartbeat window and a status window)
    size_t before = ac.writes.size();
    CHECK(bus.enqueue(cmd, n), "enqueue into a unit that stopped answering");
    run(bus, 14000);
    CHECK(count_cls(ac, before, 0x65) == 1, "sent once, not again (%zu)", count_cls(ac, before, 0x65));
    CHECK(bus.command_resends() == 0, "no re-send counted");
    CHECK(!rec.links.empty() && !rec.links.back().second, "the link is reported lost");
    before = ac.writes.size();
    CHECK(bus.enqueue(cmd, n), "enqueue with the link down");
    run(bus, 6000);
    CHECK(count_cls(ac, before, 0x65) == 1 && bus.command_resends() == 0 && bus.queued() == 0,
          "one send into the dead bus, then gone (%zu)", count_cls(ac, before, 0x65));
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
    run(bus, 26000);  // ten tries of a window plus the retry wait, then two cycles with a command each
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

// The drain deadline is counted from the time poll() was given, which is before the write. That
// is what keeps DE correct on both kinds of UART: a write that only queues the bytes returns at
// once and they leave during the wait, and a write that blocks until they are out returns with
// the wire time already spent. Either way DE has to fall 25 ms after the last byte, not 25 ms
// plus a second wire time, or it would still be driving the bus when the unit answers.
static void check_de_release(const SimAC &ac, uint32_t slack, const char *what) {
  for (auto &w : ac.writes) {
    uint32_t fell = 0;
    for (auto &e : ac.de) {
      if (!e.high && e.t > w.t && fell == 0)
        fell = e.t;
    }
    uint32_t last_byte = w.t + H::bus_tx_time_ms(w.bytes.size());
    CHECK(w.de_high, "%s: write at %u with DE high", what, (unsigned) w.t);
    CHECK(fell >= last_byte + 25 && fell <= last_byte + 25 + slack, "%s: DE falls %d ms after the last byte", what,
          (int) (fell - last_byte));
    CHECK(fell < last_byte + ac.reply_delay, "%s: DE is down before the reply starts", what);
  }
}

static void test_blocking_write() {
  printf("-- a UART write that blocks until the bytes are out\n");
  SimAC ac;
  ac.blocking_write = true;
  Recorder rec;
  H::BusScheduler bus;
  rec.bus = &bus;
  bus.setup(&ac, &rec, true);
  run(bus, 3000);
  uint8_t cmd[64];
  size_t n = H::build_command(H::AcCommand{}, cmd, sizeof(cmd));
  CHECK(bus.enqueue(cmd, n), "enqueue a 50-byte command");
  run(bus, 4000);

  check_de_release(ac, 1, "blocking");
  std::vector<uint32_t> hb;
  for (auto &w : ac.writes)
    if (cls(w) == 0x1E)
      hb.push_back(w.t);
  CHECK(hb.size() >= 6, "heartbeats kept coming (%zu)", hb.size());
  for (size_t i = 2; i < hb.size(); i++)
    CHECK(hb[i] - hb[i - 1] >= 1000 && hb[i] - hb[i - 1] <= 1060, "cycle %zu period %u ms", i,
          (unsigned) (hb[i] - hb[i - 1]));
  CHECK(rec.statuses >= 5, "status delivered (%d)", rec.statuses);
  CHECK(rec.timeouts == 0, "no reply window closed empty (%d)", rec.timeouts);
  CHECK(rec.links.empty(), "no link edge");
  CHECK(rec.commands.size() == 1 && rec.commands[0].first, "the command went out once and was answered");
  CHECK(bus.unanswered_commands() == 0 && bus.command_resends() == 0, "nothing sent twice");
}

// ESPHome's loop runs about every 16 ms unless a component asks for a tight one, and other
// components take their turn in between. The scheduler must stay correct when it is polled that
// coarsely: DE is released late by at most one loop period and every reply is still taken.
static void test_coarse_loop() {
  printf("-- polled every 16 ms, with and without a blocking write\n");
  for (int blocking = 0; blocking < 2; blocking++) {
    SimAC ac;
    ac.blocking_write = blocking != 0;
    ac.reply_delay = 60;
    Recorder rec;
    H::BusScheduler bus;
    bus.setup(&ac, &rec, true);
    uint32_t end = g_now + 6000;
    while (static_cast<int32_t>(end - g_now) > 0) {
      bus.poll(g_now);
      g_now += 16;
    }
    check_de_release(ac, 16, blocking ? "coarse blocking" : "coarse");
    CHECK(rec.statuses >= 4, "status delivered (%d)", rec.statuses);
    CHECK(rec.timeouts == 0, "no reply window closed empty (%d)", rec.timeouts);
    CHECK(rec.links.empty(), "no link edge");
  }
}

int main() {
  printf("== ESPHome bus scheduler vs the original bus task's contract ==\n");
  test_boot_and_cycle();
  test_queue();
  test_link_loss_and_recovery();
  test_correlation_and_checksum();
  test_boot_retries();
  test_damaged_frames_are_not_believed();
  test_return_between_probe_and_poll();
  test_command_tickets();
  test_accessors_and_corners();
  test_peripheral_de();
  test_command_fate();
  test_stale_frame_in_command_window();
  test_truncated_reply();
  test_millis_rollover();
  test_blocking_write();
  test_coarse_loop();
  printf("  %d checks, %d failed\n", g_checks, g_fail);
  printf(g_fail ? "== BUS SCHEDULER FAILED ==\n" : "== BUS SCHEDULER OK ==\n");
  return g_fail ? 1 : 0;
}
