// The remote's "77" gesture in the ESPHome component: the request bits of the A/C's 0x1E reply,
// their debounce and lockout (hisense_protocol.*), the heartbeat bit that puts "77" on the panel,
// and the bus scheduler (hisense_bus.*) reporting one request per gesture.
//
// The first three groups are the cases test_codec.cpp runs against the original driver's
// hisense_recommission_debounce() and hisense_smartcfg_lockout_mask(), run here against the port.
// The lockout takes a start time and an armed flag where the original takes a deadline, so those
// cases are restated in elapsed time. The rest is what the port adds: a checksum on the reply,
// bit 3 ignored while the module's own flag is on the wire and until a reply without it, and a
// lockout that starts at boot and at every accepted request.

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

static const uint8_t SMARTCFG = H::LINK_REQ_SMARTCFG;
static const uint8_t RECONFIG = H::LINK_REQ_RECONFIG;
static const uint8_t HOLD = H::SETUP_REQUEST_HOLD_FRAMES;

// ---- Ported from test_codec.cpp: "[recommission debounce]" ---------------------------------------
static void test_debounce() {
  printf("-- debounce: once per request, only after the hold\n");
  {
    uint8_t streak = 0;
    bool latched = false;
    CHECK(H::setup_request_debounce(RECONFIG, &streak, &latched, HOLD) == 0, "no fire on frame 1");
    CHECK(H::setup_request_debounce(RECONFIG, &streak, &latched, HOLD) == 0, "no fire on frame 2");
    CHECK(H::setup_request_debounce(RECONFIG, &streak, &latched, HOLD) == 1, "fire on frame 3 (hold reached)");
    CHECK(H::setup_request_debounce(RECONFIG, &streak, &latched, HOLD) == 0, "no re-fire while held");
    CHECK(H::setup_request_debounce(RECONFIG, &streak, &latched, HOLD) == 0, "no re-fire while held (2)");
    CHECK(H::setup_request_debounce(0, &streak, &latched, HOLD) == -1, "clear after fire reports the end");
    CHECK(H::setup_request_debounce(RECONFIG, &streak, &latched, HOLD) == 0, "re-arm: no fire frame 1");
    CHECK(H::setup_request_debounce(RECONFIG, &streak, &latched, HOLD) == 0, "re-arm: no fire frame 2");
    CHECK(H::setup_request_debounce(RECONFIG, &streak, &latched, HOLD) == 1, "re-arm: fire frame 3");
    CHECK(H::setup_request_debounce(0, &streak, &latched, HOLD) == -1, "re-arm: clear reports the end");
  }
  {
    uint8_t streak = 0;
    bool latched = false;
    CHECK(H::setup_request_debounce(RECONFIG, &streak, &latched, HOLD) == 0, "glitch: no fire");
    CHECK(H::setup_request_debounce(0, &streak, &latched, HOLD) == 0, "glitch clear reports nothing (never fired)");
  }
  {
    // A request that never lasts the hold (one frame on, one off, as a reflection would) never fires.
    uint8_t streak = 0;
    bool latched = false;
    int fires = 0;
    for (int i = 0; i < 50; i++) {
      fires += H::setup_request_debounce(RECONFIG, &streak, &latched, HOLD);
      fires += H::setup_request_debounce(0, &streak, &latched, HOLD);
    }
    CHECK(fires == 0, "alternating request never fires (%d)", fires);
  }
  {
    uint8_t streak = 0;
    bool latched = false;
    H::setup_request_debounce(SMARTCFG, &streak, &latched, HOLD);
    H::setup_request_debounce(SMARTCFG, &streak, &latched, HOLD);
    CHECK(H::setup_request_debounce(SMARTCFG, &streak, &latched, HOLD) == 1, "smart-config bit fires after the hold");
  }
  {
    // The hold the tracker uses for bit 5 alone: the unit sends one reply and no more.
    uint8_t streak = 0;
    bool latched = false;
    CHECK(H::setup_request_debounce(SMARTCFG, &streak, &latched, 1) == 1, "hold 1: fires on the first frame");
    CHECK(H::setup_request_debounce(0, &streak, &latched, 1) == -1, "hold 1: the next frame ends it");
  }
  {
    // The streak counter saturates and does not wrap into a second fire.
    uint8_t streak = 0;
    bool latched = false;
    int fires = 0;
    for (int i = 0; i < 1000; i++)
      fires += H::setup_request_debounce(RECONFIG, &streak, &latched, HOLD);
    CHECK(fires == 1 && streak == 0xFF, "a request held for 1000 frames fires once (%d, streak %u)", fires, streak);
  }
}

// ---- Ported from test_codec.cpp: "[recommission re-entry lockout #69]" ---------------------------
static void test_lockout_mask() {
  printf("-- lockout: bit 5 ignored for 10 s, bit 3 never touched\n");
  const uint32_t SINCE = 90000;  // the original's deadline of 100000 less the 10 s
  const uint8_t BOTH = H::LINK_REQ_MASK;
  CHECK(H::SETUP_REQUEST_LOCKOUT_MS == 10000, "the lockout is the original's 10 s");
  CHECK(H::smartcfg_lockout_mask(SMARTCFG, 95000, true, SINCE) == 0, "lockout: lone 0x20 pulse gated to 0");
  CHECK(H::smartcfg_lockout_mask(RECONFIG, 95000, true, SINCE) == RECONFIG, "lockout: 0x08 passes through");
  CHECK(H::smartcfg_lockout_mask(BOTH, 95000, true, SINCE) == RECONFIG, "lockout: mixed 0x28 -> 0x08 only");
  CHECK(H::smartcfg_lockout_mask(SMARTCFG, 100000, true, SINCE) == SMARTCFG, "at the deadline it has expired");
  CHECK(H::smartcfg_lockout_mask(SMARTCFG, 99999, true, SINCE) == 0, "one millisecond before, it has not");
  CHECK(H::smartcfg_lockout_mask(SMARTCFG, 110000, true, SINCE) == SMARTCFG, "after lockout: 0x20 passes through");
  CHECK(H::smartcfg_lockout_mask(SMARTCFG, 0, false, 0) == SMARTCFG, "no lockout armed (boot state): nothing stripped");
  CHECK(H::smartcfg_lockout_mask(SMARTCFG, 5, false, 0) == SMARTCFG, "not armed: a start time of 0 means nothing");
  // Started just before the 32 bit clock wraps, read just after.
  CHECK(H::smartcfg_lockout_mask(SMARTCFG, 50, true, 0xFFFFFFF0u) == 0, "lockout survives the clock wrap");
  CHECK(H::smartcfg_lockout_mask(SMARTCFG, 10000, true, 0xFFFFFFF0u) == SMARTCFG, "and still ends 10 s after it began");
  {
    // The pulse of the press that closed the mode, fed to the debounce through the gate, does nothing.
    uint8_t streak = 0;
    bool latched = false;
    uint8_t gated = H::smartcfg_lockout_mask(SMARTCFG, 95000, true, SINCE);
    CHECK(H::setup_request_debounce(gated, &streak, &latched, 1) == 0, "gated pulse does not fire");
    gated = H::smartcfg_lockout_mask(SMARTCFG, 110000, true, SINCE);
    CHECK(H::setup_request_debounce(gated, &streak, &latched, 1) == 1, "the same pulse fires once the lockout is over");
  }
}

// ---- The heartbeat's provisioning flag ----------------------------------------------------------
static void test_heartbeat() {
  printf("-- heartbeat: the provisioning flag and its checksum\n");
  uint8_t plain[H::LINK_HEARTBEAT_LEN + 2];
  uint8_t prov[H::LINK_HEARTBEAT_LEN + 2];
  for (int heard = 0; heard < 2; heard++) {
    const size_t n_plain = H::build_link_heartbeat(heard != 0, false, plain, sizeof(plain));
    const size_t n_prov = H::build_link_heartbeat(heard != 0, true, prov, sizeof(prov));
    CHECK(n_plain == H::LINK_HEARTBEAT_LEN && n_prov == H::LINK_HEARTBEAT_LEN, "28 bytes, nothing to stuff");
    CHECK(plain[17] == 0x80, "heard=%d: byte 17 is 0x80 without the flag (0x%02X)", heard, plain[17]);
    CHECK(prov[17] == 0x88, "heard=%d: byte 17 is 0x88 with it (0x%02X)", heard, prov[17]);
    CHECK(H::status_checksum_ok(prov, n_prov), "heard=%d: the checksum covers the flag", heard);
    CHECK(H::status_checksum_ok(plain, n_plain), "heard=%d: and is right without it", heard);
    for (size_t i = 0; i < n_prov; i++) {
      if (i == 17 || i == 24 || i == 25)
        continue;
      CHECK(plain[i] == prov[i], "heard=%d: byte %u is the same in both", heard, (unsigned) i);
    }
    // The three-argument builder, which the bus used before, is the frame without the flag.
    uint8_t old_form[H::LINK_HEARTBEAT_LEN + 2];
    const size_t n_old = H::build_link_heartbeat(heard != 0, old_form, sizeof(old_form));
    CHECK(n_old == n_plain && memcmp(old_form, plain, n_plain) == 0, "heard=%d: old builder unchanged", heard);
  }
  // The frame the original driver sends while a commissioning window is open, byte for byte: the
  // captured heartbeat with byte 17 = 0x88, so the checksum moves from 0x0302 to 0x030A.
  H::build_link_heartbeat(true, true, prov, sizeof(prov));
  CHECK(prov[16] == 0xB0 && prov[24] == 0x03 && prov[25] == 0x0A, "heard: checksum 03 0A (%02X %02X)", prov[24],
        prov[25]);
  H::build_link_heartbeat(false, true, prov, sizeof(prov));
  CHECK(prov[16] == 0xF0 && prov[24] == 0x03 && prov[25] == 0x4A, "not heard: checksum 03 4A (%02X %02X)", prov[24],
        prov[25]);
  CHECK(H::build_link_heartbeat(true, true, prov, H::LINK_HEARTBEAT_LEN + 1) == 0,
        "refuses a buffer with no stuffing room");
  CHECK(H::build_link_heartbeat(true, true, nullptr, 64) == 0, "refuses a null buffer");
}

// A 0x1E reply as the A/C sends it: 28 bytes, direction 0x01, request bits in byte 17.
static std::vector<uint8_t> link_reply(uint8_t flags, bool bad_checksum = false) {
  std::vector<uint8_t> f(28, 0);
  f[0] = 0xF4;
  f[1] = 0xF5;
  f[2] = 0x01;
  f[3] = 0x40;
  f[4] = 28 - 9;
  f[13] = 0x1E;
  f[17] = flags;
  uint16_t sum = H::checksum_range(f.data(), 2, 24);
  f[24] = (uint8_t) (sum >> 8);
  f[25] = (uint8_t) sum;
  if (bad_checksum)
    f[25] ^= 0x01;
  f[26] = 0xF4;
  f[27] = 0xFB;
  return f;
}

// ---- Reading the request out of a reply ---------------------------------------------------------
static void test_parse() {
  printf("-- request bits of a 0x1E reply\n");
  uint8_t bits = 0xEE;
  auto f = link_reply(0x00);
  CHECK(H::link_request_bits(f.data(), f.size(), &bits) && bits == 0, "a quiet reply reads as 0");
  f = link_reply(0x20);
  CHECK(H::link_request_bits(f.data(), f.size(), &bits) && bits == SMARTCFG, "0x20 is the smart-config request");
  f = link_reply(0x08);
  CHECK(H::link_request_bits(f.data(), f.size(), &bits) && bits == RECONFIG, "0x08 is the reconfigure request");
  // Bits 0 and 6 of the same byte are the binding token and an action code: not requests.
  f = link_reply(0xFF);
  CHECK(H::link_request_bits(f.data(), f.size(), &bits) && bits == 0x28, "only bits 3 and 5 are taken (0x%02X)", bits);
  f = link_reply(0xD7);
  CHECK(H::link_request_bits(f.data(), f.size(), &bits) && bits == 0, "every other bit set reads as 0");

  bits = 0xEE;
  f = link_reply(0x20, true);
  CHECK(!H::link_request_bits(f.data(), f.size(), &bits) && bits == 0xEE, "a bad checksum is not a request");
  // One flipped bit is the damage a noisy bus does, and it is all it takes to make 0x20 out of 0x00.
  f = link_reply(0x00);
  f[17] ^= 0x20;
  CHECK(!H::link_request_bits(f.data(), f.size(), &bits), "a reply with bit 5 flipped on the wire is refused");
  f = link_reply(0x20);
  f[13] = 0x66;
  uint16_t sum = H::checksum_range(f.data(), 2, 24);
  f[24] = (uint8_t) (sum >> 8);
  f[25] = (uint8_t) sum;
  CHECK(!H::link_request_bits(f.data(), f.size(), &bits), "another class is not a link reply");
  f = link_reply(0x20);
  CHECK(!H::link_request_bits(f.data(), 17, &bits), "a frame that ends before byte 17 is refused");
  CHECK(!H::link_request_bits(f.data(), 21, &bits), "and one too short to hold a checksum after it");
  CHECK(!H::link_request_bits(nullptr, 28, &bits) && !H::link_request_bits(f.data(), 28, nullptr), "null arguments");
}

// ---- The tracker: a stream of replies, about one a second ----------------------------------------
// Feeds `count` replies with the same bits, 1 s apart, and returns how many requests came out.
static int feed(H::SetupRequestTracker *t, uint8_t bits, bool indicating, uint32_t *now, int count,
                uint8_t *last = nullptr) {
  int fired = 0;
  for (int i = 0; i < count; i++) {
    *now += 1000;
    const uint8_t r = H::setup_request_step(t, bits, indicating, *now);
    if (r != 0) {
      fired++;
      if (last != nullptr)
        *last = r;
    }
  }
  return fired;
}

static void test_tracker_smartcfg() {
  printf("-- tracker: the one-reply 0x20 pulse (what the bench unit sends)\n");
  H::SetupRequestTracker t;
  uint32_t now = 500000;
  uint8_t reason = 0;
  CHECK(feed(&t, 0, false, &now, 5) == 0, "quiet replies do nothing");
  CHECK(feed(&t, SMARTCFG, false, &now, 1, &reason) == 1 && reason == SMARTCFG, "one pulse, one request (0x%02X)",
        reason);
  CHECK(feed(&t, 0, false, &now, 3) == 0, "nothing after it");
  // The request started the lockout by itself: a second pulse 4 s later is the same gesture, or
  // the user still pressing, and does not undo what the first one did.
  CHECK(feed(&t, SMARTCFG, false, &now, 1) == 0, "a second pulse inside 10 s is ignored");
  CHECK(feed(&t, 0, false, &now, 6) == 0, "quiet");
  CHECK(feed(&t, SMARTCFG, false, &now, 1) == 1, "a pulse after the lockout is a new request (the way out of setup)");

  // Bench, 2026-07-20: three gestures gave pulses at replies 93, 100 and 114. The first is taken,
  // the second (7 s later) falls inside the lockout, the third (21 s) is taken.
  H::SetupRequestTracker bench;
  now = 0;
  int fired_at[3] = {0, 0, 0};
  int fires = 0;
  for (int reply = 1; reply <= 120; reply++) {
    now += 1000;
    const bool pulse = reply == 93 || reply == 100 || reply == 114;
    if (H::setup_request_step(&bench, pulse ? SMARTCFG : 0, false, now) != 0 && fires < 3)
      fired_at[fires++] = reply;
  }
  CHECK(fires == 2 && fired_at[0] == 93 && fired_at[1] == 114, "bench pulses: taken at 93 and 114 (%d: %d, %d)", fires,
        fired_at[0], fired_at[1]);

  // Closing the mode restarts the lockout (issue 69): the press that closed it is not a new request.
  H::SetupRequestTracker closed;
  now = 800000;
  feed(&closed, 0, false, &now, 20);
  H::setup_request_lockout(&closed, now);
  CHECK(feed(&closed, SMARTCFG, false, &now, 1) == 0, "pulse 1 s after the mode closed is ignored");
  CHECK(feed(&closed, 0, false, &now, 7) == 0, "quiet");
  CHECK(feed(&closed, SMARTCFG, false, &now, 1) == 0, "pulse 9 s after is still ignored");
  CHECK(feed(&closed, SMARTCFG, false, &now, 1) == 1, "a deliberate gesture 10 s after is taken");
}

static void test_tracker_reconfig() {
  printf("-- tracker: the held 0x08 request (vendor firmware, not seen on the bench)\n");
  uint32_t now = 100000;
  uint8_t reason = 0;
  {
    H::SetupRequestTracker t;
    feed(&t, 0, false, &now, 2);
    CHECK(feed(&t, RECONFIG, false, &now, 2) == 0, "two replies are not enough");
    CHECK(feed(&t, RECONFIG, false, &now, 1, &reason) == 1 && reason == RECONFIG, "the third is");
    CHECK(feed(&t, RECONFIG, false, &now, 30) == 0, "held for half a minute: still one request");
    CHECK(feed(&t, 0, false, &now, 1) == 0, "released");
    CHECK(feed(&t, RECONFIG, false, &now, 3) == 1, "asked again: a second request");
  }
  {
    // The bit is already there in the first reply after a start: a unit that keeps asking until
    // the module has been set up. Taking it would open setup again after every restart.
    H::SetupRequestTracker t;
    CHECK(feed(&t, RECONFIG, false, &now, 600) == 0, "a request standing at boot is never taken");
    CHECK(feed(&t, 0, false, &now, 1) == 0, "one reply without it");
    CHECK(feed(&t, RECONFIG, false, &now, 3) == 1, "then it counts");
  }
  {
    // The module's own flag is the same bit. A unit that hands it back must not be read as the
    // user asking: with a toggle on the request, that would end setup three seconds after it began.
    H::SetupRequestTracker t;
    feed(&t, 0, false, &now, 2);
    CHECK(feed(&t, RECONFIG, false, &now, 3) == 1, "request taken");
    CHECK(feed(&t, RECONFIG, true, &now, 600) == 0, "bit 3 while the flag is on the wire is never a request");
    // The flag goes off. The unit takes a reply or two to drop it.
    CHECK(feed(&t, RECONFIG, false, &now, 10) == 0, "nor right after, however long it stays");
    CHECK(feed(&t, 0, false, &now, 1) == 0, "the unit drops it");
    CHECK(feed(&t, RECONFIG, false, &now, 3) == 1, "from here it is a request again");
  }
  {
    // A single reply with the bit, as a glitch or one late reflection gives.
    H::SetupRequestTracker t;
    feed(&t, 0, false, &now, 2);
    int fires = 0;
    for (int i = 0; i < 50; i++) {
      fires += feed(&t, RECONFIG, false, &now, 1);
      fires += feed(&t, 0, false, &now, 1);
    }
    CHECK(fires == 0, "bit 3 on every other reply never fires (%d)", fires);
  }
  {
    // Both bits: bit 3 brings the hold with it.
    H::SetupRequestTracker t;
    feed(&t, 0, false, &now, 2);
    CHECK(feed(&t, H::LINK_REQ_MASK, false, &now, 2) == 0, "0x28 is held like 0x08");
    CHECK(feed(&t, H::LINK_REQ_MASK, false, &now, 1, &reason) == 1 && reason == H::LINK_REQ_MASK,
          "and reports both bits");
  }
  {
    // The kind changes with no quiet reply in between: the second kind is not lost in the first
    // one's latch. 0x08 held and taken, then 0x20 alone once the lockout is over.
    H::SetupRequestTracker t;
    feed(&t, 0, false, &now, 2);
    CHECK(feed(&t, RECONFIG, false, &now, 3) == 1, "0x08 taken");
    CHECK(feed(&t, RECONFIG, false, &now, 10) == 0, "still held, lockout over");
    CHECK(feed(&t, SMARTCFG, false, &now, 1, &reason) == 1 && reason == SMARTCFG,
          "0x20 straight after is its own request");
  }
  {
    // Inside the lockout a 0x28 reply is 0x08 to the debounce, so it still needs the hold.
    H::SetupRequestTracker t;
    feed(&t, 0, false, &now, 2);
    H::setup_request_lockout(&t, now);
    CHECK(feed(&t, H::LINK_REQ_MASK, false, &now, 2) == 0, "0x28 in the lockout: two replies are not enough");
    CHECK(feed(&t, H::LINK_REQ_MASK, false, &now, 1, &reason) == 1 && reason == RECONFIG, "the third is, as 0x08");
  }
}

static void test_tracker_clock() {
  printf("-- tracker: the 32 bit millisecond clock\n");
  {
    // A lockout that began just before the wrap ends 10 s later, on the other side of it.
    H::SetupRequestTracker t;
    uint32_t now = 0xFFFFFFFFu - 3000;
    H::setup_request_step(&t, 0, false, now);
    H::setup_request_lockout(&t, now);
    CHECK(H::setup_request_step(&t, SMARTCFG, false, now + 5000) == 0, "5 s later, past the wrap: still locked out");
    CHECK(H::setup_request_step(&t, 0, false, now + 6000) == 0, "quiet");
    CHECK(H::setup_request_step(&t, SMARTCFG, false, now + 10000) == SMARTCFG, "10 s later: taken");
  }
  {
    // A lockout from long ago must not come back when the clock has gone once around: with a
    // stored start time and nothing else, a pulse 49.7 days later would land "5 s after" it.
    H::SetupRequestTracker t;
    uint32_t now = 20000;
    H::setup_request_lockout(&t, now);
    for (int i = 0; i < 20; i++) {
      now += 1000;
      H::setup_request_step(&t, 0, false, now);
    }
    CHECK(!t.lockout_armed, "an expired lockout is disarmed by the next reply");
    CHECK(H::setup_request_step(&t, SMARTCFG, false, 20000 + 5000) == SMARTCFG, "one clock revolution later: taken");
  }
}

// ---- The scheduler against a simulated A/C ------------------------------------------------------
static uint32_t g_now = 1000;

class SimAC : public H::BusIO {
 public:
  uint8_t flags = 0x00;                // byte 17 of the reply to the next heartbeat
  bool bad_checksum = false;           // damage that reply
  bool late_copy = false;              // a second 0x1E lands in the status poll's window
  std::vector<uint8_t> heartbeats_17;  // byte 17 of every heartbeat written
  bool heartbeats_valid = true;
  int heartbeats = 0;

  void bus_set_de(bool) override {}
  void bus_write(const uint8_t *data, size_t len) override {
    uint32_t at = g_now + H::bus_tx_time_ms(len) + 40;
    const uint8_t cls = data[13];
    if (cls == 0x0A) {
      std::vector<uint8_t> f(24, 0);
      fill_(&f, 0x0A);
      f[16] = 0x01;
      f[17] = 0x01;
      seal_(&f);
      queue_(f, at);
    } else if (cls == 0x07) {
      std::vector<uint8_t> f(20, 0);
      fill_(&f, 0x07);
      seal_(&f);
      queue_(f, at);
    } else if (cls == 0x1E) {
      heartbeats++;
      heartbeats_17.push_back(data[17]);
      if (!H::status_checksum_ok(data, len))
        heartbeats_valid = false;
      queue_(link_reply(flags, bad_checksum), at);
    } else if (cls == 0x66 && data[14] == 0x40) {
      std::vector<uint8_t> f(64, 0);
      fill_(&f, 0x66);
      f[14] = 0x40;
      seal_(&f);
      queue_(f, at);
    } else if (cls == 0x66) {
      if (late_copy) {
        queue_(link_reply(flags), at);
        at += 40;
      }
      std::vector<uint8_t> f(160, 0);
      fill_(&f, 0x66);
      f[18] = 0x24;
      f[19] = 22;
      f[20] = 23;
      seal_(&f);
      queue_(f, at);
    }
  }
  void bus_flush() override {}
  int bus_read() override {
    if (rx_.empty() || rx_.front().first > g_now)
      return -1;
    const int b = rx_.front().second;
    rx_.pop_front();
    return b;
  }

 private:
  static void fill_(std::vector<uint8_t> *f, uint8_t cls) {
    (*f)[0] = 0xF4;
    (*f)[1] = 0xF5;
    (*f)[2] = 0x01;
    (*f)[3] = 0x40;
    (*f)[4] = (uint8_t) (f->size() - 9);
    (*f)[13] = cls;
  }
  static void seal_(std::vector<uint8_t> *f) {
    const size_t n = f->size();
    const uint16_t sum = H::checksum_range(f->data(), 2, n - 4);
    (*f)[n - 4] = (uint8_t) (sum >> 8);
    (*f)[n - 3] = (uint8_t) sum;
    (*f)[n - 2] = 0xF4;
    (*f)[n - 1] = 0xFB;
  }
  void queue_(const std::vector<uint8_t> &f, uint32_t at) {
    for (size_t i = 0; i < f.size(); i++) {
      rx_.push_back({at, f[i]});
      // A checksum byte of 0xF4 is doubled on the wire.
      if ((i == f.size() - 4 || i == f.size() - 3) && f[i] == 0xF4)
        rx_.push_back({at, 0xF4});
      at++;
    }
  }
  std::deque<std::pair<uint32_t, uint8_t>> rx_;
};

class Recorder : public H::BusListener {
 public:
  std::vector<uint8_t> requests;
  int statuses = 0;
  bool link_down = false;
  void on_bus_status(const H::AcState &) override { statuses++; }
  void on_bus_features(const H::AcFeatures &) override {}
  void on_bus_link(bool up) override { link_down = !up; }
  void on_bus_setup_request(uint8_t request) override { requests.push_back(request); }
};

struct Rig {
  SimAC ac;
  Recorder rec;
  H::BusScheduler bus;
  Rig() { bus.setup(&ac, &rec, true); }
  // Run until the A/C has answered `n` more heartbeats and that cycle's status poll is done.
  void cycles(int n) {
    const int target = ac.heartbeats + n;
    while (ac.heartbeats < target) {
      g_now++;
      bus.poll(g_now);
    }
    for (int i = 0; i < 900; i++) {
      g_now++;
      bus.poll(g_now);
    }
  }
};

static void test_scheduler() {
  printf("-- scheduler: one request per gesture, and the flag in the heartbeat\n");
  {
    Rig r;
    r.cycles(12);  // past the lockout that starts with the first poll
    CHECK(r.rec.requests.empty(), "a quiet unit: no request");
    CHECK(r.ac.heartbeats_valid, "every heartbeat has a good checksum");
    bool all_plain = true;
    for (uint8_t b : r.ac.heartbeats_17)
      all_plain = all_plain && b == 0x80;
    CHECK(all_plain, "byte 17 of every heartbeat is 0x80 until the indication is asked for");
    CHECK(r.bus.link_request() == 0 && !r.bus.setup_indicator(), "nothing asked, nothing shown");

    r.ac.flags = 0x20;
    r.cycles(1);
    r.ac.flags = 0x00;
    CHECK(r.rec.requests.size() == 1 && r.rec.requests[0] == SMARTCFG, "one 0x20 reply: one request (%u)",
          (unsigned) r.rec.requests.size());
    CHECK(r.bus.link_request() == SMARTCFG, "the raw bits are readable for diagnostics");
    r.cycles(2);
    CHECK(r.rec.requests.size() == 1 && r.bus.link_request() == 0, "and no more after it");

    // What the listener does with it: show "77".
    r.ac.heartbeats_17.clear();
    r.bus.set_setup_indicator(true, g_now);
    r.cycles(3);
    bool all_prov = !r.ac.heartbeats_17.empty();
    for (uint8_t b : r.ac.heartbeats_17)
      all_prov = all_prov && b == 0x88;
    CHECK(all_prov && r.bus.setup_indicator(), "every heartbeat now carries byte 17 = 0x88");
    CHECK(r.ac.heartbeats_valid, "with a good checksum");
    CHECK(r.rec.statuses > 0 && !r.rec.link_down, "the status poll goes on as before");

    // The unit hands the flag back while it is shown: not a request.
    r.ac.flags = 0x08;
    r.cycles(6);
    CHECK(r.rec.requests.size() == 1, "0x08 from the unit while the flag is on the wire: no request");

    // The gesture again, well after the first: the way out.
    r.ac.flags = 0x20;
    r.cycles(1);
    r.ac.flags = 0x00;
    CHECK(r.rec.requests.size() == 2 && r.rec.requests[1] == SMARTCFG, "second gesture: second request (%u)",
          (unsigned) r.rec.requests.size());
    r.ac.heartbeats_17.clear();
    r.bus.set_setup_indicator(false, g_now);
    r.cycles(2);
    bool back_to_plain = !r.ac.heartbeats_17.empty();
    for (uint8_t b : r.ac.heartbeats_17)
      back_to_plain = back_to_plain && b == 0x80;
    CHECK(back_to_plain, "indication off: byte 17 is 0x80 again from the next heartbeat");
    // Issue 69: the press that ended the mode pulses once more.
    r.ac.flags = 0x20;
    r.cycles(1);
    r.ac.flags = 0x00;
    CHECK(r.rec.requests.size() == 2, "a pulse right after the indication went off is ignored");
    r.cycles(11);
    r.ac.flags = 0x20;
    r.cycles(1);
    r.ac.flags = 0x00;
    CHECK(r.rec.requests.size() == 3, "and taken again once 10 s have passed");
  }
  {
    // The first seconds after a start.
    Rig r;
    r.ac.flags = 0x20;
    r.cycles(1);
    r.ac.flags = 0x00;
    r.cycles(3);
    CHECK(r.rec.requests.empty(), "a pulse in the first seconds after a start is not acted on");
  }
  {
    // A damaged reply.
    Rig r;
    r.cycles(12);
    r.ac.flags = 0x20;
    r.ac.bad_checksum = true;
    r.cycles(3);
    CHECK(r.rec.requests.empty() && r.bus.link_request() == 0, "0x20 in a reply with a bad checksum: no request");
    r.ac.bad_checksum = false;
    r.cycles(1);
    CHECK(r.rec.requests.size() == 1, "the same reply intact: one request");
  }
  {
    // A late second 0x1E in the status poll's window carries the same pulse. It is the same
    // gesture and must not count twice (the first one started the lockout).
    Rig r;
    r.cycles(12);
    r.ac.late_copy = true;
    r.ac.flags = 0x20;
    r.cycles(1);
    r.ac.flags = 0x00;
    r.cycles(2);
    CHECK(r.rec.requests.size() == 1, "a pulse seen in two replies of one cycle is one request (%u)",
          (unsigned) r.rec.requests.size());
    CHECK(r.rec.statuses > 0 && !r.rec.link_down, "and the status poll still got its answer");
  }
  {
    // The held kind, end to end.
    Rig r;
    r.cycles(3);
    r.ac.flags = 0x08;
    r.cycles(2);
    CHECK(r.rec.requests.empty(), "0x08 for two replies: nothing yet");
    r.cycles(1);
    CHECK(r.rec.requests.size() == 1 && r.rec.requests[0] == RECONFIG, "third reply: one request");
    r.cycles(5);
    CHECK(r.rec.requests.size() == 1, "held: still one");
  }
}

int main() {
  // run_tests.sh gives this test one line, so the heading the other layers echo is printed here.
  printf("\n== Layer 1i: ESPHome setup request, the remote's \"77\" gesture (debounce, lockout, heartbeat flag) ==\n");
  test_debounce();
  test_lockout_mask();
  test_heartbeat();
  test_parse();
  test_tracker_smartcfg();
  test_tracker_reconfig();
  test_tracker_clock();
  test_scheduler();
  printf("\n%d checks, %d failed\n", g_checks, g_fail);
  if (g_fail == 0)
    printf("== ESPHome setup request (\"77\") OK ==\n");
  return g_fail == 0 ? 0 : 1;
}
