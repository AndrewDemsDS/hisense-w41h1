// Edge cases of the remote's "77" setup request: the tracker (debounce, bit 3 quieting, bit 5
// lockout) in hisense_protocol.*, and the bus scheduler in hisense_bus.* that feeds it.
// test_esphome_setup_request.cpp covers the main paths. This file asks three questions of every
// input order it can build: does one gesture ever count twice, can a request be lost for good, and
// can a request come out with no gesture behind it.
//
// What "should" happen is taken from the comments in hisense_protocol.h ("Setup request"), from
// firmware/esphome/README.md ("Wi-Fi setup mode and the 77 gesture") and from
// reverse-engineering/docs/03-rs485-ac-protocol.md ("Recommission"):
//   - 0x20 (bit 5) is one reply per gesture on the bench unit and counts at once.
//   - 0x08 (bit 3) has to be there for three replies in a row, is ignored while the module's own
//     flag is on the wire and until a reply without it, and is ignored when it stands at boot.
//   - Bit 5 is ignored for 10 s after a start, after an accepted request, and after the indication
//     was switched off. Bit 3 is never locked out.
//   - A reply with a bad checksum never counts.
//   - One request is reported once.
//
// KNOWN(...) marks a check that fails today. The defect is described next to it and in the pull
// request that added this file. A KNOWN check that starts to pass fails the run, so the marker is
// removed together with the fix.
//
// Byte values: 0x20 and 0x08 in byte 17 of the 0x1E reply are LINK_REQ_SMARTCFG and
// LINK_REQ_RECONFIG (hisense_protocol.h, from the vendor firmware's handler and the bench capture of
// 2026-07-20). 0x80 and 0x88 in byte 17 of the heartbeat are the frame without and with the
// provisioning flag (bench injection of 2026-07-09, same header).

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
static int g_known = 0;
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
// A check that states the wanted behaviour and is known not to hold. Counted, never silent.
#define KNOWN(cond, ...) \
  do { \
    g_checks++; \
    if (cond) { \
      g_fail++; \
      printf("  FAIL %s:%d known defect no longer reproduces, remove the KNOWN marker: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } else { \
      g_known++; \
      printf("  known %s:%d ", __FILE__, __LINE__); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } \
  } while (0)

static const uint8_t SMARTCFG = H::LINK_REQ_SMARTCFG;  // 0x20
static const uint8_t RECONFIG = H::LINK_REQ_RECONFIG;  // 0x08
static const uint8_t BOTH = H::LINK_REQ_MASK;          // 0x28
static const uint32_t LOCKOUT = H::SETUP_REQUEST_LOCKOUT_MS;

// Feeds `count` replies with the same bits, `step_ms` apart, and returns how many requests came out.
static int feed(H::SetupRequestTracker *t, uint8_t bits, bool indicating, uint32_t *now, int count,
                uint8_t *last = nullptr, uint32_t step_ms = 1000) {
  int fired = 0;
  for (int i = 0; i < count; i++) {
    *now += step_ms;
    const uint8_t r = H::setup_request_step(t, bits, indicating, *now);
    if (r != 0) {
      fired++;
      if (last != nullptr)
        *last = r;
    }
  }
  return fired;
}

// A tracker as the scheduler leaves it after a start: the lockout armed at the first poll.
static H::SetupRequestTracker booted(uint32_t now) {
  H::SetupRequestTracker t;
  H::setup_request_lockout(&t, now);
  return t;
}

// ---- Every order of replies, exhaustively --------------------------------------------------------
// Eight kinds of reply (request bits 0x00, 0x08, 0x20, 0x28, each with the module's flag off or on
// the wire), every sequence of seven, at two spacings: 1 s (the real pace, all inside one lockout)
// and 4 s (so a lockout ends inside the sequence). 2 * 8^7 sequences.
static const int SEQ_LEN = 7;
static const uint8_t SYMBOL_BITS[4] = {0x00, RECONFIG, SMARTCFG, BOTH};

struct Violation {
  long count{0};
  long first{-1};
  uint32_t step_ms{0};
};

static void print_sequence(const char *label, long s, uint32_t step_ms) {
  printf("    %s, %u ms apart:", label, (unsigned) step_ms);
  for (int i = 0; i < SEQ_LEN; i++) {
    const int sym = (int) (s % 8);
    s /= 8;
    printf(" %02X%s", SYMBOL_BITS[sym & 3], (sym & 4) != 0 ? "i" : "");
  }
  printf("\n");
}

static void test_exhaustive() {
  printf("-- every order of seven replies, flag on or off the wire, 1 s and 4 s apart\n");
  long total = 1;
  for (int i = 0; i < SEQ_LEN; i++)
    total *= 8;
  Violation not_in_reply;      // a request with a bit the reply did not carry
  Violation while_indicating;  // bit 3 reported while the module's own flag was on the wire
  Violation no_hold;           // bit 3 reported without three replies in a row carrying it
  Violation smartcfg_early;    // two requests with bit 5 less than 10 s apart
  Violation in_boot_lockout;   // bit 5 reported less than 10 s after the start
  Violation twice_per_run;     // two requests out of one unbroken run of replies carrying a request
  Violation lost_smartcfg;     // afterwards: a 0x20 pulse well past any lockout is not taken
  Violation lost_reconfig;     // afterwards: a quiet reply, then 0x08 three times, is not taken
  const uint32_t steps[2] = {1000, 4000};
  for (int si = 0; si < 2; si++) {
    const uint32_t step_ms = steps[si];
    for (long s = 0; s < total; s++) {
      const uint32_t start = 100000;
      uint32_t now = start;
      H::SetupRequestTracker t = booted(now);
      long v = s;
      int run_fires = 0;
      int reconfig_run = 0;  // replies in a row with bit 3 and the flag off the wire
      // Bit 3 is a request only once a reply without it was seen with the flag off the wire
      // (hisense_protocol.h, SetupRequestTracker). Kept here independently of the tracker.
      bool reconfig_armed = false;
      bool had_smartcfg = false;
      uint32_t last_smartcfg = 0;
      for (int i = 0; i < SEQ_LEN; i++) {
        const int sym = (int) (v % 8);
        v /= 8;
        const uint8_t bits = SYMBOL_BITS[sym & 3];
        const bool indicating = (sym & 4) != 0;
        now += step_ms;
        const uint8_t r = H::setup_request_step(&t, bits, indicating, now);
        reconfig_run = ((bits & RECONFIG) != 0 && !indicating) ? reconfig_run + 1 : 0;
#define NOTE(v_) \
  do { \
    (v_).count++; \
    if ((v_).first < 0) { \
      (v_).first = s; \
      (v_).step_ms = step_ms; \
    } \
  } while (0)
        if ((r & ~bits) != 0)
          NOTE(not_in_reply);
        if ((r & RECONFIG) != 0 && indicating)
          NOTE(while_indicating);
        if ((r & RECONFIG) != 0 && reconfig_run < H::SETUP_REQUEST_HOLD_FRAMES)
          NOTE(no_hold);
        if ((r & SMARTCFG) != 0) {
          if (now - start < LOCKOUT)
            NOTE(in_boot_lockout);
          if (had_smartcfg && now - last_smartcfg < LOCKOUT)
            NOTE(smartcfg_early);
          had_smartcfg = true;
          last_smartcfg = now;
        }
        // What the unit is asking as far as the module can tell: bit 3 does not count while the
        // module's own flag is on the wire, nor before a reply without it (a bit 3 standing since
        // the start, or handed back after the flag went off, is not a request, and two gestures
        // made over it are two requests).
        if (indicating) {
          reconfig_armed = false;
        } else if ((bits & RECONFIG) == 0) {
          reconfig_armed = true;
        }
        const uint8_t asking = reconfig_armed ? bits : (uint8_t) (bits & SMARTCFG);
        if (asking == 0) {
          run_fires = 0;
        } else if (r != 0 && ++run_fires > 1) {
          NOTE(twice_per_run);
        }
      }
      // Whatever came before, the tracker must still take a clean request afterwards.
      H::SetupRequestTracker a = t;
      uint32_t at = now;
      feed(&a, 0, false, &at, 11);
      if (feed(&a, SMARTCFG, false, &at, 1) != 1)
        NOTE(lost_smartcfg);
      H::SetupRequestTracker b = t;
      at = now;
      feed(&b, 0, false, &at, 1);
      if (feed(&b, RECONFIG, false, &at, 3) != 1)
        NOTE(lost_reconfig);
#undef NOTE
    }
  }
  CHECK(not_in_reply.count == 0, "a request only ever carries bits the reply carried (%ld)", not_in_reply.count);
  CHECK(while_indicating.count == 0, "bit 3 is never a request while the module's flag is on the wire (%ld)",
        while_indicating.count);
  CHECK(no_hold.count == 0, "bit 3 is never a request before three replies in a row carried it (%ld)", no_hold.count);
  CHECK(smartcfg_early.count == 0, "two requests with bit 5 are never less than 10 s apart (%ld)",
        smartcfg_early.count);
  CHECK(in_boot_lockout.count == 0, "bit 5 is never taken in the first 10 s after a start (%ld)",
        in_boot_lockout.count);
  CHECK(lost_smartcfg.count == 0, "no order of replies leaves the tracker deaf to a later 0x20 pulse (%ld)",
        lost_smartcfg.count);
  CHECK(lost_reconfig.count == 0, "no order of replies leaves the tracker deaf to a later held 0x08 (%ld)",
        lost_reconfig.count);
  // Was a defect: a request that changed kind restarted the count, which also ran when bit 5 came
  // or went on top of a bit 3 that stayed held, and the same held bit 3 was reported a second
  // time. Shortest forms: 28 28 28 08 08 08 and 08 08 08 28 28 28 (two requests of 0x08), and
  // 20 08 08 08 (0x20, then 0x08 three replies later). With a toggle on the request that was setup
  // mode opened and closed again by one gesture. Now one unbroken run of replies that ask is one
  // request.
  CHECK(twice_per_run.count == 0, "one unbroken run of request replies gives two requests (%ld sequences)",
        twice_per_run.count);
  if (twice_per_run.first >= 0)
    print_sequence("first such sequence", twice_per_run.first, twice_per_run.step_ms);
}

// ---- The same double count, spelled out ---------------------------------------------------------
static void test_kind_changes() {
  printf("-- the request changes kind while it is asserted\n");
  uint32_t now = 100000;
  uint8_t reason = 0;
  {
    // 0x08 taken, then 0x20 alone: two kinds, two requests. This is what the comment in
    // setup_request_step describes, and the existing test asserts it after the lockout. Inside the
    // lockout the 0x20 is dropped like any other.
    H::SetupRequestTracker t = booted(now);
    feed(&t, 0, false, &now, 12);
    CHECK(feed(&t, RECONFIG, false, &now, 3) == 1, "0x08 held: taken");
    CHECK(feed(&t, SMARTCFG, false, &now, 3) == 0, "0x20 right after, inside the 10 s: ignored");
    CHECK(feed(&t, 0, false, &now, 8) == 0, "quiet");
    CHECK(feed(&t, SMARTCFG, false, &now, 1, &reason) == 1 && reason == SMARTCFG, "0x20 after the 10 s: taken");
  }
  {
    // 0x20 taken, then 0x08 held with no quiet reply between: the same run, so no second request.
    // On a unit that raised both for one gesture two requests would open setup mode and close it
    // again.
    H::SetupRequestTracker t = booted(now);
    feed(&t, 0, false, &now, 12);
    CHECK(feed(&t, SMARTCFG, false, &now, 1) == 1, "0x20: taken");
    CHECK(feed(&t, RECONFIG, false, &now, 30) == 0,
          "0x08 held straight after a taken 0x20, no quiet reply between: no second request");
    CHECK(feed(&t, 0, false, &now, 1) == 0, "quiet");
    CHECK(feed(&t, RECONFIG, false, &now, 3) == 1, "0x08 held after a quiet reply: its own request");
  }
  {
    // The same with the listener doing what node.yaml does: indication on from the next reply.
    H::SetupRequestTracker t = booted(now);
    feed(&t, 0, false, &now, 12);
    CHECK(feed(&t, SMARTCFG, false, &now, 1) == 1, "0x20: taken");
    CHECK(feed(&t, RECONFIG, true, &now, 30) == 0, "0x08 with the flag on the wire: nothing more");
    CHECK(feed(&t, BOTH, true, &now, 1, &reason) == 1 && reason == SMARTCFG,
          "0x28 with the flag on the wire, past the lockout: taken as 0x20 alone");
  }
  {
    // One held 0x08 with a single 0x20 reply on top, well after the first request.
    H::SetupRequestTracker t = booted(now);
    feed(&t, 0, false, &now, 12);
    CHECK(feed(&t, RECONFIG, false, &now, 3) == 1, "0x08 held: taken");
    CHECK(feed(&t, RECONFIG, false, &now, 12) == 0, "still held: nothing");
    int fires = feed(&t, BOTH, false, &now, 1);
    fires += feed(&t, RECONFIG, false, &now, 5);
    CHECK(fires == 0, "a held, already taken 0x08 is reported again after one 0x28 reply (%d)", fires);
  }
  {
    // Both bits from the start, bit 5 dropping after one reply (a pulse on top of a held request).
    H::SetupRequestTracker t = booted(now);
    feed(&t, 0, false, &now, 12);
    // Bit 3 has been there for three replies in a row on the third one, whatever bit 5 did
    // meanwhile. (The count used to start over when bit 5 dropped, which made it four.)
    CHECK(feed(&t, BOTH, false, &now, 1) == 0, "0x28 once: held, not taken yet");
    CHECK(feed(&t, RECONFIG, false, &now, 1) == 0, "0x08: bit 3 for the second reply in a row");
    CHECK(feed(&t, RECONFIG, false, &now, 1, &reason) == 1 && reason == RECONFIG, "third: taken once, as 0x08");
    CHECK(feed(&t, RECONFIG, false, &now, 20) == 0, "and not again");
  }
  {
    // Every pair of kinds with a quiet reply between them is two separate requests, once the
    // lockout allows the second.
    const uint8_t kinds[3] = {RECONFIG, SMARTCFG, BOTH};
    for (int a = 0; a < 3; a++) {
      for (int b = 0; b < 3; b++) {
        H::SetupRequestTracker t = booted(now);
        feed(&t, 0, false, &now, 12);
        const int first = feed(&t, kinds[a], false, &now, 3);
        feed(&t, 0, false, &now, 12);
        const int second = feed(&t, kinds[b], false, &now, 3);
        CHECK(first == 1 && second == 1, "0x%02X, quiet, 0x%02X: one request each (%d, %d)", kinds[a], kinds[b], first,
              second);
      }
    }
  }
}

// ---- A bit 5 that does not go away --------------------------------------------------------------
static void test_sustained_smartcfg() {
  printf("-- 0x20 held instead of pulsed (not seen on the bench, where it is one reply)\n");
  uint32_t now = 100000;
  {
    // Two or three replies in a row: the user still pressing. One request.
    H::SetupRequestTracker t = booted(now);
    feed(&t, 0, false, &now, 12);
    CHECK(feed(&t, SMARTCFG, false, &now, 3) == 1, "0x20 for three replies: one request");
    CHECK(feed(&t, 0, false, &now, 12) == 0, "quiet after it");
  }
  {
    // Was a weakness: the lockout dropped bit 5, the debounce saw the request as gone and let its
    // latch go, and when the lockout ended the same held bit counted as new. A unit that held
    // 0x20 would have toggled setup mode every 10 s.
    H::SetupRequestTracker t = booted(now);
    feed(&t, 0, false, &now, 12);
    const int fires = feed(&t, SMARTCFG, false, &now, 60);
    CHECK(fires == 1, "0x20 held for a minute is reported %d times", fires);
    CHECK(feed(&t, 0, false, &now, 1) == 0 && feed(&t, SMARTCFG, false, &now, 1) == 1,
          "released and pressed again: taken");
  }
  {
    // The same at a start: neither bit standing at boot is taken, however long it stays.
    H::SetupRequestTracker t = booted(now);
    const int fires = feed(&t, SMARTCFG, false, &now, 600);
    CHECK(fires == 0, "0x20 standing at boot is taken when the boot lockout ends (%d)", fires);
    CHECK(feed(&t, 0, false, &now, 1) == 0 && feed(&t, SMARTCFG, false, &now, 1) == 1,
          "after a reply without it, a pulse is taken");
  }
}

// ---- Bit 3 and the module's own flag ------------------------------------------------------------
static void test_own_flag() {
  printf("-- the module's own flag on the wire, and the replies after it clears\n");
  uint32_t now = 100000;
  uint8_t reason = 0;
  {
    // The unit hands the flag back for as long as it is shown and for a few replies after.
    for (int tail = 0; tail <= 5; tail++) {
      H::SetupRequestTracker t = booted(now);
      feed(&t, 0, false, &now, 12);
      int fires = feed(&t, RECONFIG, true, &now, 20);
      fires += feed(&t, RECONFIG, false, &now, tail);
      fires += feed(&t, 0, false, &now, 5);
      CHECK(fires == 0, "flag handed back, still there for %d replies after it clears: no request", tail);
      CHECK(feed(&t, RECONFIG, false, &now, 3) == 1, "and a real request after that is taken (tail %d)", tail);
    }
  }
  {
    // The very first reply after the flag clears is quiet: bit 3 counts again from the next one.
    H::SetupRequestTracker t = booted(now);
    feed(&t, 0, false, &now, 12);
    feed(&t, 0, true, &now, 20);
    CHECK(feed(&t, 0, false, &now, 1) == 0, "first reply after the flag clears, quiet");
    CHECK(feed(&t, RECONFIG, false, &now, 2) == 0, "0x08 twice: not yet");
    CHECK(feed(&t, RECONFIG, false, &now, 1) == 1, "third: taken");
  }
  {
    // A reply without bit 3 while the flag is on the wire does not count as the quiet reply: the
    // unit may only start handing the flag back later.
    H::SetupRequestTracker t = booted(now);
    feed(&t, 0, false, &now, 12);
    feed(&t, 0, true, &now, 5);
    CHECK(feed(&t, RECONFIG, false, &now, 30) == 0, "0x08 from the first reply after the flag: never taken");
  }
  {
    // The flag does not hide bit 5: the gesture is the way out of setup mode.
    H::SetupRequestTracker t = booted(now);
    feed(&t, 0, false, &now, 12);
    CHECK(feed(&t, SMARTCFG, false, &now, 1) == 1, "the gesture: taken");
    CHECK(feed(&t, RECONFIG, true, &now, 12) == 0, "flag on the wire and handed back");
    CHECK(feed(&t, BOTH, true, &now, 1, &reason) == 1 && reason == SMARTCFG, "the gesture again: taken, as 0x20");
  }
  {
    // WEAKNESS, by design of the quieting: a unit that asks with bit 3 only (the vendor firmware's
    // path, VERIFY in hisense_protocol.h) cannot end setup mode with the gesture, because its
    // request is the bit the module is sending. Setup mode then ends by timeout only. The README
    // says the gesture repeated ends the mode, which holds for the 0x20 unit on the bench.
    H::SetupRequestTracker t = booted(now);
    feed(&t, 0, false, &now, 12);
    CHECK(feed(&t, RECONFIG, false, &now, 3) == 1, "bit 3 unit, the gesture: taken");
    feed(&t, 0, true, &now, 15);
    KNOWN(feed(&t, RECONFIG, true, &now, 600) == 1,
          "bit 3 unit, the gesture again while \"77\" is shown: never taken, so it cannot close the mode");
  }
}

// ---- A request standing at boot -----------------------------------------------------------------
static void test_boot() {
  printf("-- requests already standing at a start\n");
  uint32_t now = 0;  // the first poll of a fresh boot
  {
    H::SetupRequestTracker t = booted(now);
    CHECK(feed(&t, RECONFIG, false, &now, 3600) == 0, "0x08 from the first reply, for an hour: never taken");
    CHECK(t.streak == 0 && !t.latched, "and nothing builds up behind it");
    CHECK(feed(&t, 0, false, &now, 1) == 0, "one reply without it");
    CHECK(feed(&t, RECONFIG, false, &now, 2) == 0, "0x08 twice");
    CHECK(feed(&t, RECONFIG, false, &now, 1) == 1, "third: taken");
  }
  {
    // 0x28 standing at boot: bit 3 is dropped until a quiet reply, bit 5 until the boot lockout
    // ends. What is left after 10 s is a held 0x20 (see test_sustained_smartcfg).
    H::SetupRequestTracker t = booted(now = 0);
    CHECK(feed(&t, BOTH, false, &now, 9) == 0, "0x28 from the first reply: nothing in the first 9 s");
  }
  {
    // A single pulse in the boot lockout: the press that asked for the restart, arriving late.
    for (uint32_t at = 1; at < LOCKOUT; at += 997) {
      H::SetupRequestTracker t = booted(0);
      CHECK(H::setup_request_step(&t, SMARTCFG, false, at) == 0, "pulse %u ms after the start: ignored", (unsigned) at);
      CHECK(H::setup_request_step(&t, 0, false, at + 1000) == 0, "quiet");
      CHECK(H::setup_request_step(&t, SMARTCFG, false, LOCKOUT + 500) == SMARTCFG,
            "and the next gesture after the 10 s is taken (first pulse at %u ms)", (unsigned) at);
    }
  }
  {
    // Zero-initialised with no lockout call at all (the header calls that the boot state): the
    // tracker itself does not lock out, the scheduler's first poll does.
    H::SetupRequestTracker t;
    CHECK(H::setup_request_step(&t, SMARTCFG, false, 1) == SMARTCFG,
          "a tracker nobody called setup_request_lockout on takes a pulse at once");
  }
}

// ---- The lockout boundary and the clock ---------------------------------------------------------
static void test_lockout_boundary() {
  printf("-- the 10 s lockout: its edges, its restart, and the 32 bit clock\n");
  {
    // The window is [since, since + 10000): 9999 ms is inside, 10000 ms is outside.
    const uint32_t starts[6] = {0, 1, 123456, 0x7FFFFFFFu, 0xFFFFFFFFu - LOCKOUT, 0xFFFFFFFFu};
    for (int i = 0; i < 6; i++) {
      const uint32_t since = starts[i];
      H::SetupRequestTracker t;
      H::setup_request_lockout(&t, since);
      CHECK(H::setup_request_step(&t, SMARTCFG, false, since) == 0, "start 0x%08X: a pulse at +0 is ignored",
            (unsigned) since);
      CHECK(H::setup_request_step(&t, 0, false, since + 1) == 0, "quiet");
      CHECK(H::setup_request_step(&t, SMARTCFG, false, since + LOCKOUT - 1) == 0,
            "start 0x%08X: a pulse at +9999 ms is ignored", (unsigned) since);
      CHECK(t.lockout_armed, "and the lockout is still armed");
      CHECK(H::setup_request_step(&t, 0, false, since + LOCKOUT - 1) == 0, "quiet");
      CHECK(H::setup_request_step(&t, SMARTCFG, false, since + LOCKOUT) == SMARTCFG,
            "start 0x%08X: a pulse at +10000 ms is taken", (unsigned) since);
      CHECK(t.lockout_armed && t.lockout_since == since + LOCKOUT, "and it starts the next lockout from there");
    }
  }
  {
    // The pure mask on the same edges, across the wrap.
    const uint32_t since = 0xFFFFFFF0u;
    CHECK(H::smartcfg_lockout_mask(BOTH, since + LOCKOUT - 1, true, since) == RECONFIG, "mask at +9999, wrapped");
    CHECK(H::smartcfg_lockout_mask(BOTH, since + LOCKOUT, true, since) == BOTH, "mask at +10000, wrapped");
    CHECK(H::smartcfg_lockout_mask(BOTH, since, false, since) == BOTH, "not armed: nothing removed");
    CHECK(H::smartcfg_lockout_mask(0xFF, since, true, since) == 0xDF, "only bit 5 is ever removed");
  }
  {
    // A pulse inside the lockout does not extend it: pulses every 4 s must not hold the tracker
    // shut for ever. Taken at 0, ignored at 4 and 8, taken at 12, ignored at 16 and 20, taken at 24.
    H::SetupRequestTracker t;
    uint32_t now = 0xFFFFFFFFu - 9000;  // the series crosses the wrap
    int fires = 0;
    int pattern = 0;
    for (int i = 0; i < 7; i++) {
      const uint8_t r = H::setup_request_step(&t, SMARTCFG, false, now);
      pattern = (pattern << 1) | (r != 0 ? 1 : 0);
      fires += r != 0 ? 1 : 0;
      H::setup_request_step(&t, 0, false, now + 1000);
      now += 4000;
    }
    CHECK(fires == 3 && pattern == 0x49, "a pulse every 4 s is taken every third time (%d, pattern 0x%02X)", fires,
          pattern);
  }
  {
    // Restarting the lockout (the indication switched off) pushes the end out, from that moment.
    H::SetupRequestTracker t;
    uint32_t now = 50000;
    H::setup_request_lockout(&t, now);
    H::setup_request_step(&t, 0, false, now + 9000);
    H::setup_request_lockout(&t, now + 9000);
    CHECK(H::setup_request_step(&t, SMARTCFG, false, now + 10000) == 0, "restarted at 9 s: 10 s is now inside");
    CHECK(H::setup_request_step(&t, 0, false, now + 11000) == 0, "quiet");
    CHECK(H::setup_request_step(&t, SMARTCFG, false, now + 18999) == 0, "18.999 s: still inside");
    CHECK(H::setup_request_step(&t, 0, false, now + 18999) == 0, "quiet");
    CHECK(H::setup_request_step(&t, SMARTCFG, false, now + 19000) == SMARTCFG, "19 s: taken");
  }
  {
    // Bit 3 is not held back by the lockout, on either side of the wrap.
    H::SetupRequestTracker t;
    uint32_t now = 0xFFFFFFFFu - 1500;
    H::setup_request_step(&t, 0, false, now);
    H::setup_request_lockout(&t, now);
    CHECK(feed(&t, RECONFIG, false, &now, 2) == 0, "0x08 twice, the second past the wrap");
    CHECK(feed(&t, RECONFIG, false, &now, 1) == 1, "third: taken, inside the lockout");
  }
  {
    // Long uptime with the unit answering: every wrap of millis() (49.7 days) is harmless because
    // each reply disarms an expired lockout. Ten wraps, one reply every 1000 s, a gesture now and
    // then. Each gesture is taken exactly once.
    H::SetupRequestTracker t = booted(0);
    uint64_t wall = 0;
    int fires = 0;
    int gestures = 0;
    const uint64_t end = 10ull * 0x100000000ull;
    while (wall < end) {
      wall += 1000000;
      const bool gesture = (wall / 1000000) % 1237 == 0;
      if (gesture)
        gestures++;
      if (H::setup_request_step(&t, gesture ? SMARTCFG : 0, false, (uint32_t) wall) != 0)
        fires++;
    }
    CHECK(gestures > 30 && fires == gestures, "ten clock wraps: %d gestures, %d requests", gestures, fires);
  }
  {
    // A held 0x08 over a very long time: the streak saturates and the latch holds. One request.
    H::SetupRequestTracker t = booted(0);
    uint32_t now = 0;
    feed(&t, 0, false, &now, 12);
    const int fires = feed(&t, RECONFIG, false, &now, 100000, nullptr, 100000);  // 115 days
    CHECK(fires == 1 && t.streak == 0xFF && t.latched, "0x08 held across two clock wraps: one request (%d)", fires);
    CHECK(feed(&t, 0, false, &now, 1) == 0, "released");
    CHECK(feed(&t, RECONFIG, false, &now, 3) == 1, "asked again: taken");
  }
  {
    // Was a weakness: an expired lockout was disarmed in setup_request_step only. With no 0x1E
    // reply at all between the end of a lockout and one clock revolution later (the unit silent
    // for 49.7 days), the old lockout read as running again for 10 s and one gesture was ignored.
    // The scheduler now calls setup_request_expire on every poll, reply or not. On a 32 bit clock
    // "5 s after the lockout began" and "one revolution and 5 s after" are the same number, so
    // without a call in between nothing can tell them apart, which the second half pins.
    H::SetupRequestTracker t;
    H::setup_request_lockout(&t, 20000);
    H::setup_request_expire(&t, 20000 + LOCKOUT - 1);
    CHECK(t.lockout_armed, "9.999 s: the lockout is still armed");
    H::setup_request_expire(&t, 20000 + 0x80000000u);  // some poll during the 49.7 days of silence
    CHECK(!t.lockout_armed, "a poll after the 10 s disarms it without any reply");
    CHECK(H::setup_request_step(&t, SMARTCFG, false, 20000 + 5000) == SMARTCFG,
          "one whole clock revolution later: a pulse \"5 s into\" the old lockout is taken");
    H::SetupRequestTracker never_polled;
    H::setup_request_lockout(&never_polled, 20000);
    CHECK(H::setup_request_step(&never_polled, SMARTCFG, false, 20000 + 5000) == 0,
          "with no call at all in between, 5 s into the lockout is what it is");
  }
}

// ---- Frames -------------------------------------------------------------------------------------
// A 0x1E reply as the A/C sends it: direction 0x01, class at 13, request bits in byte 17. 28 bytes
// on hardware (hisense_protocol.h: "link 0x13 -> 28").
static std::vector<uint8_t> link_reply(uint8_t flags, size_t len = 28, uint8_t fill = 0x00) {
  std::vector<uint8_t> f(len, fill);
  f[0] = 0xF4;
  f[1] = 0xF5;
  f[2] = 0x01;
  f[3] = 0x40;
  f[4] = (uint8_t) (len - 9);
  f[13] = 0x1E;
  if (len > 17 + 4)
    f[17] = flags;
  const uint16_t sum = H::checksum_range(f.data(), 2, len - 4);
  f[len - 4] = (uint8_t) (sum >> 8);
  f[len - 3] = (uint8_t) sum;
  f[len - 2] = 0xF4;
  f[len - 1] = 0xFB;
  return f;
}

static void test_reply_parsing() {
  printf("-- damaged, short and foreign 0x1E frames\n");
  uint8_t bits = 0xEE;
  {
    // The shortest frame that still has byte 17 in front of its checksum is 22 bytes.
    auto f = link_reply(0x28, 22);
    CHECK(H::link_request_bits(f.data(), f.size(), &bits) && bits == BOTH, "22 bytes, well formed: read (0x%02X)",
          bits);
    // One byte shorter and byte 17 is the high byte of the checksum. Filled so that this byte is
    // 0x0B (bytes 2 to 16 sum to 0x0B60 with 0xFF in the free bytes): without the length guard
    // it would read as a 0x08 request from a frame that is valid in every other way.
    f = link_reply(0x00, 21, 0xFF);
    CHECK(H::status_checksum_ok(f.data(), f.size()) && (f[17] & RECONFIG) != 0,
          "the 21 byte frame is well formed and its byte 17 has bit 3 (0x%02X)", f[17]);
    bits = 0xEE;
    CHECK(!H::link_request_bits(f.data(), f.size(), &bits) && bits == 0xEE,
          "21 bytes, well formed: refused, byte 17 is not payload");
  }
  {
    // Every single-bit error anywhere in a quiet reply and in a 0x20 reply: the result is either
    // refused or unchanged, never a different request.
    for (int base = 0; base < 2; base++) {
      const uint8_t flags = base == 0 ? 0x00 : 0x20;
      const auto good = link_reply(flags);
      int wrong = 0;
      for (size_t i = 0; i < good.size(); i++) {
        for (int bit = 0; bit < 8; bit++) {
          auto f = good;
          f[i] ^= (uint8_t) (1 << bit);
          uint8_t got = 0xEE;
          if (H::link_request_bits(f.data(), f.size(), &got) && got != flags)
            wrong++;
        }
      }
      CHECK(wrong == 0, "reply 0x%02X: no single flipped bit turns it into another request (%d)", flags, wrong);
    }
  }
  {
    // Cut short at every length: nothing shorter than the whole frame is read.
    const auto good = link_reply(0x20);
    int read = 0;
    for (size_t n = 0; n < good.size(); n++) {
      if (H::link_request_bits(good.data(), n, &bits))
        read++;
    }
    CHECK(read == 0, "a 0x20 reply cut at any length is refused (%d)", read);
  }
  {
    // WEAKNESS of the protocol, not of this code: the check is a 16 bit sum, so two errors that
    // cancel pass it. Bit 5 set in byte 17 and cleared in another byte is a valid frame.
    auto f = link_reply(0x00);
    f[16] = 0x20;
    const uint16_t sum = H::checksum_range(f.data(), 2, 24);
    f[24] = (uint8_t) (sum >> 8);
    f[25] = (uint8_t) sum;
    f[16] ^= 0x20;
    f[17] ^= 0x20;
    CHECK(H::link_request_bits(f.data(), f.size(), &bits) && bits == SMARTCFG,
          "two bit errors that cancel in the sum read as a request (a limit of the checksum)");
  }
  {
    // The module's own heartbeat (direction 0x00, byte 17 = 0x88 with the flag) never reaches the
    // tracker: the assembler drops module -> A/C frames, so an echo of it cannot be read as the
    // unit asking with bit 3.
    uint8_t hb[H::LINK_HEARTBEAT_LEN + 2];
    const size_t n = H::build_link_heartbeat(true, true, hb, sizeof(hb));
    CHECK(n == H::LINK_HEARTBEAT_LEN && hb[17] == 0x88 && hb[13] == 0x1E,
          "the heartbeat with the flag is a 0x1E frame");
    H::FrameAssembler rx;
    size_t got = 0;
    for (size_t i = 0; i < n; i++)
      got += rx.feed(hb[i]);
    CHECK(got == 0, "an echo of our own heartbeat is not assembled as a frame");
    // Had it been, the codec would read a request from it, so the assembler is the only guard.
    CHECK(H::link_request_bits(hb, n, &bits) && bits == RECONFIG,
          "the codec alone does not tell our heartbeat from a reply (0x%02X)", bits);
  }
}

// ---- The scheduler against a simulated A/C ------------------------------------------------------
static uint32_t g_now = 1000;

class SimAC : public H::BusIO {
 public:
  uint8_t flags = 0x00;       // byte 17 of the reply to the next heartbeat
  bool silent = false;        // the unit answers nothing at all
  bool bad_checksum = false;  // damage the 0x1E reply
  size_t truncate = 0;        // send only this many bytes of the 0x1E reply (0 = all)
  bool echo = false;          // the transceiver hands back every byte the module sends
  bool mirror = false;        // the unit hands the module's bit 3 back in its reply
  int mirror_tail = 0;        // and keeps it for this many replies after the module dropped it
  int heartbeats = 0;
  std::vector<uint8_t> heartbeats_17;

  void bus_set_de(bool) override {}
  void bus_write(const uint8_t *data, size_t len) override {
    uint32_t at = g_now + H::bus_tx_time_ms(len) + 40;
    const uint8_t cls = data[13];
    if (cls == 0x1E) {
      heartbeats++;
      heartbeats_17.push_back(data[17]);
    }
    if (echo) {
      for (size_t i = 0; i < len; i++)
        rx_.push_back({g_now, data[i]});
    }
    if (silent)
      return;
    if (cls == 0x0A) {
      std::vector<uint8_t> f(24, 0);
      fill_(&f, 0x0A);
      f[16] = 0x01;
      f[17] = 0x01;
      seal_(&f);
      queue_(f, at, 0);
    } else if (cls == 0x07) {
      std::vector<uint8_t> f(20, 0);
      fill_(&f, 0x07);
      seal_(&f);
      queue_(f, at, 0);
    } else if (cls == 0x1E) {
      uint8_t out = flags;
      if (mirror) {
        if ((data[17] & 0x08) != 0) {
          mirror_left_ = mirror_tail;
          out |= 0x08;
        } else if (mirror_left_ > 0) {
          mirror_left_--;
          out |= 0x08;
        }
      }
      std::vector<uint8_t> f = link_reply(out);
      if (bad_checksum)
        f[25] ^= 0x01;
      queue_(f, at, truncate);
    } else if (cls == 0x66 && data[14] == 0x40) {
      std::vector<uint8_t> f(64, 0);
      fill_(&f, 0x66);
      f[14] = 0x40;
      seal_(&f);
      queue_(f, at, 0);
    } else if (cls == 0x66) {
      std::vector<uint8_t> f(160, 0);
      fill_(&f, 0x66);
      f[18] = 0x24;
      f[19] = 22;
      f[20] = 23;
      seal_(&f);
      queue_(f, at, 0);
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
  void queue_(const std::vector<uint8_t> &f, uint32_t at, size_t cut) {
    const size_t n = cut != 0 && cut < f.size() ? cut : f.size();
    for (size_t i = 0; i < n; i++) {
      rx_.push_back({at, f[i]});
      // A checksum byte of 0xF4 is doubled on the wire.
      if ((i == f.size() - 4 || i == f.size() - 3) && f[i] == 0xF4)
        rx_.push_back({at, 0xF4});
      at++;
    }
  }
  std::deque<std::pair<uint32_t, uint8_t>> rx_;
  int mirror_left_ = 0;
};

class Recorder : public H::BusListener {
 public:
  std::vector<uint8_t> requests;
  int statuses = 0;
  bool link_down = false;
  int link_edges = 0;
  void on_bus_status(const H::AcState &) override { statuses++; }
  void on_bus_features(const H::AcFeatures &) override {}
  void on_bus_link(bool up) override {
    link_down = !up;
    link_edges++;
  }
  void on_bus_setup_request(uint8_t request) override { requests.push_back(request); }
};

struct Rig {
  SimAC ac;
  Recorder rec;
  H::BusScheduler bus;
  explicit Rig(uint32_t start = 1000) {
    g_now = start;
    bus.setup(&ac, &rec, true);
  }
  // Run until `n` more heartbeats have gone out and that cycle's status poll is done.
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
  void pulse(uint8_t bits) {
    ac.flags = bits;
    cycles(1);
    ac.flags = 0x00;
  }
};

static void test_scheduler_damage() {
  printf("-- scheduler: truncated and damaged replies, and an echo of our own frames\n");
  {
    // A 0x1E reply cut short at every length, with the request in it: never a request, the link
    // stays up (the status poll is a separate transaction), and the next whole reply is read.
    int requests = 0;
    int downs = 0;
    int recovered = 0;
    for (size_t cut = 1; cut < 28; cut++) {
      Rig r;
      r.cycles(12);
      r.ac.truncate = cut;
      r.ac.flags = 0x20;
      r.cycles(3);
      requests += (int) r.rec.requests.size();
      downs += r.rec.link_down ? 1 : 0;
      r.ac.truncate = 0;
      r.cycles(1);
      recovered += r.rec.requests.size() == 1 ? 1 : 0;
    }
    CHECK(requests == 0, "a 0x20 reply cut short at any length is not a request (%d)", requests);
    CHECK(downs == 0, "and does not take the link down (%d)", downs);
    CHECK(recovered == 27, "the same reply whole, right after: one request (%d of 27)", recovered);
  }
  {
    // A held 0x08 where every other reply is damaged: the damaged ones are not seen at all, so
    // the good ones count as "in a row". Three good replies are still needed.
    Rig r;
    r.cycles(12);
    r.ac.flags = 0x08;
    r.cycles(1);
    r.ac.bad_checksum = true;
    r.cycles(1);
    r.ac.bad_checksum = false;
    r.cycles(1);
    r.ac.bad_checksum = true;
    r.cycles(3);
    CHECK(r.rec.requests.empty(), "two good 0x08 replies among damaged ones: not yet");
    r.ac.bad_checksum = false;
    r.cycles(1);
    CHECK(r.rec.requests.size() == 1 && r.rec.requests[0] == RECONFIG, "the third good one: one request");
  }
  {
    // A transceiver that hands the module's own bytes back (no DE gating on the receiver). The
    // heartbeat with the flag is a 0x1E frame with bit 3 set. It must not be read as the unit
    // asking, in setup mode or after it.
    Rig r;
    r.ac.echo = true;
    r.cycles(12);
    r.bus.set_setup_indicator(true, g_now);
    r.cycles(20);
    r.bus.set_setup_indicator(false, g_now);
    r.cycles(15);
    CHECK(r.rec.requests.empty(), "our own heartbeats echoed back, flag on for 20 s: no request (%u)",
          (unsigned) r.rec.requests.size());
    CHECK(r.rec.statuses > 30 && !r.rec.link_down, "and the unit's replies are still read through the echo");
    r.pulse(0x20);
    CHECK(r.rec.requests.size() == 1, "a real gesture through the echo: one request");
  }
}

static void test_scheduler_own_flag() {
  printf("-- scheduler: a unit that hands the module's flag back\n");
  for (int tail = 0; tail <= 4; tail++) {
    Rig r;
    r.ac.mirror = true;
    r.ac.mirror_tail = tail;
    r.cycles(12);
    r.pulse(0x20);
    CHECK(r.rec.requests.size() == 1, "tail %d: the gesture is taken", tail);
    r.bus.set_setup_indicator(true, g_now);
    r.cycles(15);
    CHECK(r.rec.requests.size() == 1, "tail %d: the flag handed back for 15 s is not a request", tail);
    r.bus.set_setup_indicator(false, g_now);
    r.cycles(15);
    CHECK(r.rec.requests.size() == 1, "tail %d: nor the replies after the flag went off (%u)", tail,
          (unsigned) r.rec.requests.size());
    r.pulse(0x20);
    CHECK(r.rec.requests.size() == 2, "tail %d: a later gesture is taken", tail);
  }
  {
    // Was a weakness: bit 3 was quieted only by a reply read while the flag was on. Switched on
    // and off again within one cycle, the flag went out in one heartbeat and no reply was read
    // with it on, so a unit that kept handing it back for three replies was read as asking. The
    // scheduler now notes the flag when the heartbeat that carries it is built.
    Rig r;
    r.ac.mirror = true;
    r.ac.mirror_tail = 3;
    r.cycles(12);
    const int before = r.ac.heartbeats;
    r.bus.set_setup_indicator(true, g_now);
    while (r.ac.heartbeats == before) {
      g_now++;
      r.bus.poll(g_now);
    }
    r.bus.set_setup_indicator(false, g_now);  // the heartbeat is out, its reply not yet in
    r.cycles(8);
    CHECK(r.rec.requests.empty(), "flag on for one heartbeat only, handed back for 3 more replies: a request (%u)",
          (unsigned) r.rec.requests.size());
    r.cycles(4);  // past the 10 s that switching the indication off started
    r.pulse(0x20);
    CHECK(r.rec.requests.size() == 1, "and a gesture after it is taken");
  }
}

static void test_scheduler_link_loss() {
  printf("-- scheduler: the unit goes silent in the middle of a request\n");
  {
    // Was a weakness: "replies in a row" is counted in replies, not in time, and a lost link did
    // not clear the count. Two 0x08 replies, a minute of silence, one more: a request. The hold
    // exists to reject what a disturbed bus produces, and a link that drops is a disturbed bus.
    // The link-lost edge now starts the hold again.
    Rig r;
    r.cycles(12);
    r.ac.flags = 0x08;
    r.cycles(2);
    CHECK(r.rec.requests.empty(), "0x08 twice: nothing yet");
    r.ac.silent = true;
    r.cycles(60);
    CHECK(r.rec.link_down, "a minute of silence: the link is reported down");
    r.ac.silent = false;
    r.cycles(1);
    CHECK(r.rec.requests.empty(), "the first 0x08 reply after a minute of silence completes the hold (%u)",
          (unsigned) r.rec.requests.size());
    r.cycles(1);
    CHECK(r.rec.requests.empty(), "nor the second");
    r.cycles(5);
    CHECK(r.rec.requests.size() == 1 && !r.rec.link_down, "the link is back and the held request counted once");
  }
  {
    // Silence does not produce a request, and the lockout a pulse started is over when the link
    // returns: the next gesture is taken.
    Rig r;
    r.cycles(12);
    r.pulse(0x20);
    CHECK(r.rec.requests.size() == 1, "gesture: taken");
    r.ac.silent = true;
    r.cycles(30);
    r.ac.silent = false;
    r.cycles(3);
    CHECK(r.rec.requests.size() == 1 && !r.rec.link_down, "silence and recovery: no request of their own");
    r.pulse(0x20);
    CHECK(r.rec.requests.size() == 2, "gesture after the link came back: taken");
  }
  {
    // The unit silent from the start, then answering with a request already up. The boot lockout
    // counts from the first poll, not from the first reply, so after 30 s it is long over. Bit 3
    // still needs its quiet reply first.
    Rig r;
    r.ac.silent = true;
    r.cycles(30);
    r.ac.silent = false;
    r.ac.flags = 0x08;
    r.cycles(20);
    CHECK(r.rec.requests.empty(), "0x08 standing when the unit first answers: never taken");
    r.ac.flags = 0x00;
    r.cycles(1);
    r.ac.flags = 0x08;
    r.cycles(3);
    CHECK(r.rec.requests.size() == 1, "after one quiet reply it is");
  }
}

static void test_scheduler_boot() {
  printf("-- scheduler: the first seconds, and a start near the clock wrap\n");
  {
    // The boot lockout runs from the first poll. Pulses at successive heartbeats: none of the
    // first nine is taken, and one at 11 s is.
    for (int at = 1; at <= 9; at++) {
      Rig r;
      r.cycles(at - 1);
      r.pulse(0x20);
      CHECK(r.rec.requests.empty(), "a pulse in reply %d after the start is not acted on", at);
    }
    Rig r;
    r.cycles(11);
    r.pulse(0x20);
    CHECK(r.rec.requests.size() == 1, "a pulse 12 s after the start is taken");
  }
  {
    // The same start 3 s before millis() wraps: the lockout holds across the wrap and ends on time.
    Rig early(0xFFFFFFFFu - 3000);
    early.cycles(5);
    early.pulse(0x20);
    CHECK(early.rec.requests.empty(), "started 3 s before the wrap: a pulse 6 s in is ignored");
    early.cycles(6);
    early.pulse(0x20);
    CHECK(early.rec.requests.size() == 1 && !early.rec.link_down,
          "and one 13 s in is taken, with the link up across the wrap");
  }
  {
    // The indication switched off restarts the lockout. Switched off when it was already off,
    // it does not (set_setup_indicator returns early), so a stray "off" cannot be used to hold
    // the gesture shut.
    Rig r;
    r.cycles(12);
    r.bus.set_setup_indicator(false, g_now);
    r.pulse(0x20);
    CHECK(r.rec.requests.size() == 1, "\"off\" while already off starts no lockout");
  }
}

int main() {
  // run_tests.sh gives a test one line, so the heading the other layers echo is printed here.
  printf("\n== ESPHome setup request (\"77\"): edge cases of the tracker, the lockout and the scheduler ==\n");
  test_exhaustive();
  test_kind_changes();
  test_sustained_smartcfg();
  test_own_flag();
  test_boot();
  test_lockout_boundary();
  test_reply_parsing();
  test_scheduler_damage();
  test_scheduler_own_flag();
  test_scheduler_link_loss();
  test_scheduler_boot();
  printf("\n%d checks, %d failed, %d known defects or weaknesses still open\n", g_checks, g_fail, g_known);
  if (g_fail == 0)
    printf("== ESPHome setup request edge cases OK ==\n");
  return g_fail == 0 ? 0 : 1;
}
