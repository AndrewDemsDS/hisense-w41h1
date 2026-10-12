// Hand-written edge cases for the receive-side frame assembler, the ones a fuzzer reaches only by
// luck: the LEN byte at both extremes, doubled 0xF4 bytes at every position and at the buffer's
// end, a frame cut off at every possible offset, frames back to back with no gap, and the longest
// frame the assembler says it accepts. Plain host test, no libFuzzer; run.sh --smoke builds it
// with the sanitizers and runs it (under a second).
//
// Two kinds of check:
//   CHECK  a property that must hold.
//   PIN    a number that describes today's behaviour where it is weaker than it could be (how many
//          good frames are lost after a damaged one). The test fails when the number changes in
//          either direction, so an improvement is noticed and the pin is updated on purpose.
#include <cstdio>

#include "fuzz_common.h"

using namespace fz;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, ...) \
  do { \
    g_checks++; \
    if (!(cond)) { \
      g_fail++; \
      printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } \
  } while (0)
#define PIN(value, expected, what) \
  CHECK((value) == (expected), "%s: pinned at %d, now %d (update the pin if this is an improvement)", what, \
        static_cast<int>(expected), static_cast<int>(value))

// A sealed A/C -> module frame of `n` bytes, class `cls`, body filled with `fill`.
static void make(uint8_t *f, size_t n, uint8_t cls, uint8_t fill) {
  memset(f, fill, n);
  f[2] = 0x01;  // A/C -> module
  f[3] = 0x40;  // CTRL: 16 bit checksum
  if (n > 13)
    f[13] = cls;
  model_seal(f, n);
}

// Feed wire bytes; returns how many frames came out, the last one's length in *last.
static int feed(H::FrameAssembler &fa, const uint8_t *wire, size_t w, size_t *last = nullptr) {
  int frames = 0;
  for (size_t i = 0; i < w; i++) {
    size_t n = fa.feed(wire[i]);
    if (n != 0) {
      frames++;
      if (last != nullptr)
        *last = n;
    }
  }
  return frames;
}

// Feed one frame as the A/C sends it (every 0xF4 between the tags doubled). True when exactly
// that frame came out, on its last byte.
static bool deliver(H::FrameAssembler &fa, const uint8_t *f, size_t n) {
  uint8_t wire[420];
  size_t w = model_stuff(f, n, 2, wire);
  for (size_t i = 0; i < w; i++) {
    size_t got = fa.feed(wire[i]);
    if (i + 1 < w && got != 0)
      return false;
    if (i + 1 == w)
      return got == n && memcmp(fa.data(), f, n) == 0;
  }
  return false;
}

// Good frames lost before the first one is delivered again, out of `tries`.
static int lost_after(H::FrameAssembler &fa, const uint8_t *good, size_t n, int tries = 12) {
  for (int i = 0; i < tries; i++)
    if (deliver(fa, good, n))
      return i;
  return tries;
}

static void test_len_extremes() {
  printf("[LEN byte at the extremes]\n");
  uint8_t good[28];
  gold_link_reply(good, 0x00);
  int delivered = 0, refused = 0;
  for (int len = 0; len <= 255; len++) {
    size_t n = static_cast<size_t>(len) + 9;
    uint8_t f[264];
    make(f, n, 0x66, 0x11);
    H::FrameAssembler fa;
    bool ok = deliver(fa, f, n);
    if (n <= H::RX_FRAME_MAX) {
      CHECK(ok, "LEN %d (%zu bytes) must be delivered", len, n);
      delivered++;
    } else {
      CHECK(!ok, "LEN %d (%zu bytes) is more than RX_FRAME_MAX and must be dropped", len, n);
      refused++;
      // The dropped frame's body (no 0xF4 in it here) is skipped, and the next frame is taken.
      CHECK(deliver(fa, good, sizeof(good)), "good frame right after an oversize LEN %d", len);
    }
  }
  // LEN 0 is a 9 byte frame, LEN 191 the 200 byte maximum, LEN 192 the first refused.
  CHECK(delivered == 192 && refused == 64, "192 lengths accepted, 64 refused (got %d, %d)", delivered, refused);
  // The smallest frame is delivered but is too short to carry a class byte; consumers must (and
  // do) test the length before reading frame[13].
  uint8_t tiny[9];
  make(tiny, 9, 0, 0);
  H::FrameAssembler fa;
  CHECK(deliver(fa, tiny, 9), "9 byte frame delivered");
  uint8_t bits = 0, hi = 0, lo = 0;
  H::AcState st;
  H::AcFeatures ft;
  CHECK(!H::parse_status(fa.data(), 9, &st) && !H::parse_features(fa.data(), 9, &ft) &&
            !H::link_request_bits(fa.data(), 9, &bits) && !H::devtype_from_reply(fa.data(), 9, &hi, &lo),
        "no parser accepts the 9 byte frame");
}

static void test_stuffing_everywhere() {
  printf("[doubled 0xF4 at every position, and at the end of the buffer]\n");
  // Maximum-length frame, one 0xF4 data byte at each body position in turn.
  int ok = 0;
  for (size_t at = 5; at < H::RX_FRAME_MAX - 4; at++) {
    uint8_t f[200];
    make(f, sizeof(f), 0x66, 0x22);
    if (at == 13)
      continue;
    f[at] = 0xF4;
    model_seal(f, sizeof(f));
    H::FrameAssembler fa;
    ok += deliver(fa, f, sizeof(f)) ? 1 : 0;
  }
  CHECK(ok == 190, "a single 0xF4 at each of 190 body positions of a 200 byte frame (%d delivered)", ok);

  // Every body byte 0xF4: 200 bytes un-stuffed, far more on the wire. The buffer limit is about the
  // un-stuffed length, so this must fit.
  uint8_t all[200];
  make(all, sizeof(all), 0x66, 0xF4);
  uint8_t wire[420];
  size_t w = model_stuff(all, sizeof(all), 2, wire);
  H::FrameAssembler fa;
  CHECK(w > 380 && deliver(fa, all, sizeof(all)), "200 byte frame of all 0xF4 (%zu wire bytes) delivered", w);

  // Checksum low byte 0xF4 with the last body byte 0xF4 as well: F4 F4 .. F4 F4 F4 FB at the end.
  uint8_t f[200];
  bool built = false;
  for (int v = 0; v < 256 && !built; v++) {
    make(f, sizeof(f), 0x66, 0x00);
    f[195] = 0xF4;
    f[100] = static_cast<uint8_t>(v);
    model_seal(f, sizeof(f));
    built = f[197] == 0xF4 && f[100] != 0xF4;
  }
  H::FrameAssembler fb;
  CHECK(built && deliver(fb, f, sizeof(f)), "0xF4 as last body byte and as checksum low byte, at byte 195 to 199");

  // The checksum HIGH byte cannot be 0xF4 in any frame this assembler accepts: 194 summed bytes of
  // at most 0xFF give 0xC13E. Recorded because both codecs carry code for that case.
  CHECK(194u * 255u < 0xF400u, "checksum high byte 0xF4 is out of reach below 201 bytes");
}

static void test_split_everywhere() {
  printf("[a frame cut off at every offset, then good frames with no reset]\n");
  uint8_t link[28], status[160];
  gold_link_reply(link, 0x20);
  gold_status(status);
  struct {
    const uint8_t *f;
    size_t n;
    const char *name;
  } frames[] = {{link, sizeof(link), "0x1E reply"}, {status, sizeof(status), "status"}};
  for (auto &fr : frames) {
    int worst = 0, worst_at = 0, clean = 0;
    for (size_t cut = 0; cut < fr.n; cut++) {
      H::FrameAssembler fa;
      size_t last = 0;
      CHECK(feed(fa, fr.f, cut, &last) == 0, "%s cut at %zu produced a frame", fr.name, cut);
      int lost = lost_after(fa, fr.f, fr.n);
      CHECK(lost == 0 || cut == fr.n - 1, "%s cut at %zu costs %d good frame(s)", fr.name, cut, lost);
      if (lost > worst) {
        worst = lost;
        worst_at = static_cast<int>(cut);
      }
      clean += lost == 0 ? 1 : 0;
      // With a reset in between (what the scheduler does at the start of every transaction)
      // nothing is ever lost.
      H::FrameAssembler fb;
      feed(fb, fr.f, cut);
      fb.reset();
      CHECK(deliver(fb, fr.f, fr.n), "%s cut at %zu, reset, good frame", fr.name, cut);
    }
    printf("  %s: worst case %d good frame(s) lost (cut at %d); %d of %zu cut points lose none\n", fr.name, worst,
           worst_at, clean, fr.n);
    // Before the assembler followed a later F4 F5 as a second candidate, a frame cut anywhere past
    // its first byte cost the next good frame too (2 of n cut points lost none). One cut point is
    // left: a frame cut between the F4 and the FB of its end tag. The next frame's F4 is then the
    // second of a pair, which on this wire means one escaped data byte, so the start tag is not
    // seen. The frame after that is taken.
    PIN(worst, 1, "good frames lost after a cut frame");
    PIN(clean, static_cast<int>(fr.n) - 1, "cut points that lose no frame");
  }

  // The same split, but delivered: every way of cutting one good frame into two UART chunks.
  for (size_t cut = 0; cut <= sizeof(status); cut++) {
    H::FrameAssembler fa;
    size_t last = 0;
    int a = feed(fa, status, cut, &last);
    int b = feed(fa, status + cut, sizeof(status) - cut, &last);
    CHECK(a + b == 1 && last == sizeof(status) && memcmp(fa.data(), status, sizeof(status)) == 0,
          "status frame in two chunks, split at %zu", cut);
  }
}

static void test_back_to_back() {
  printf("[frames back to back, no gap]\n");
  uint8_t a[160], b[28], c[24], d[200], e[9];
  gold_status(a);
  gold_link_reply(b, 0x28);
  gold_devtype_reply(c, 0x01, 0x02);
  make(d, sizeof(d), 0x66, 0xF4);
  make(e, sizeof(e), 0, 0);
  const uint8_t *seq[] = {a, b, c, d, e, e, d, c, b, a, a, a};
  const size_t len[] = {160, 28, 24, 200, 9, 9, 200, 24, 28, 160, 160, 160};
  H::FrameAssembler fa;
  int ok = 0;
  for (int round = 0; round < 3; round++)
    for (size_t i = 0; i < sizeof(len) / sizeof(len[0]); i++)
      ok += deliver(fa, seq[i], len[i]) ? 1 : 0;
  CHECK(ok == 36, "36 frames of five kinds back to back, %d delivered", ok);
}

static void test_noise_before_a_frame() {
  printf("[stray bytes right before a good frame]\n");
  uint8_t good[28];
  gold_link_reply(good, 0x00);
  struct {
    uint8_t bytes[4];
    size_t n;
    int lost;  // good frames lost
    const char *what;
  } cases[] = {
      {{0x00, 0xFB, 0x12, 0x55}, 4, 0, "noise without 0xF4"},
      {{0xF4, 0xFB}, 2, 0, "the end tag of a frame whose start was missed"},
      {{0xF4}, 1, 0, "one stray 0xF4 (taken as a doubled start byte)"},
      {{0xF4, 0xF4, 0xF4, 0xFB}, 4, 0, "the tail of a frame with a doubled checksum byte"},
      // The two below used to cost the frame that follows: the stray bytes open a frame, and the
      // real start tag was then read as its contents. The byte that breaks the false start is now
      // looked at again as a start byte.
      {{0xF4, 0xF4}, 2, 0, "two stray 0xF4"},
      {{0xF4, 0xF5}, 2, 0, "a start tag with nothing behind it"},
      {{0xF4, 0xF5, 0x01}, 3, 0, "a start tag and the direction byte"},
      {{0xF4, 0xF5, 0x01, 0x40}, 4, 0, "a header cut in front of its LEN byte"},
  };
  for (auto &c : cases) {
    H::FrameAssembler fa;
    feed(fa, c.bytes, c.n);
    int lost = lost_after(fa, good, sizeof(good));
    PIN(lost, c.lost, c.what);
  }
  // Was the worst case: a header that claims the maximum length swallowed whatever followed until
  // it was full, seven 28 byte frames. The first frame behind it is now followed as a candidate
  // and delivered when it completes.
  const uint8_t greedy[] = {0xF4, 0xF5, 0x01, 0x40, 0xBF};  // LEN 0xBF = 191 -> 200 bytes
  H::FrameAssembler fa;
  feed(fa, greedy, sizeof(greedy));
  int lost = lost_after(fa, good, sizeof(good));
  printf("  a header claiming 200 bytes: %d good 28 byte frames lost before the next one is delivered\n", lost);
  PIN(lost, 0, "28 byte frames lost after a header claiming 200 bytes");
  // Two such headers in a row, then a frame cut short, then good frames: still none lost.
  H::FrameAssembler fb;
  feed(fb, greedy, sizeof(greedy));
  feed(fb, greedy, sizeof(greedy));
  feed(fb, good, 11);
  PIN(lost_after(fb, good, sizeof(good)), 0, "28 byte frames lost after two greedy headers and a cut frame");
}

static void test_tx_maximum() {
  printf("[the longest frame the transmit side accepts]\n");
  for (size_t n = 60; n <= 68; n++) {
    uint8_t f[80], out[80];
    memset(f, 0x11, sizeof(f));
    f[2] = 0x00;
    f[3] = 0x40;
    model_seal(f, n);
    size_t got = H::stamp_link_token(f, n, 0x01, 0x02, out, sizeof(out));
    CHECK((got != 0) == (n <= H::TX_FRAME_MAX), "stamp_link_token on a %zu byte frame returned %zu", n, got);
    got = H::stamp_beep(f, n, false, out, sizeof(out));
    CHECK((got != 0) == (n <= H::TX_FRAME_MAX), "stamp_beep on a %zu byte frame returned %zu", n, got);
  }
}

int main() {
  printf("== assembler and framing edge cases ==\n");
  test_len_extremes();
  test_stuffing_everywhere();
  test_split_everywhere();
  test_back_to_back();
  test_noise_before_a_frame();
  test_tx_maximum();
  printf("edge cases: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
