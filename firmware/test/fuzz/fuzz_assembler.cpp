// libFuzzer harness: the receive-side frame assembler (FrameAssembler in hisense_protocol.cpp)
// fed an arbitrary byte stream.
//
// Input: byte 0 picks the known-good frame used for the resynchronisation check, the rest is the
// stream. feed() takes one byte at a time, so how the UART chunks the stream cannot matter to it;
// chunking and time are exercised one level up, in fuzz_bus.cpp.
//
// Properties:
//  1. Memory: the assembler lives in a heap block of exactly its own size (AddressSanitizer), and
//     its buffer is a fixed array (UndefinedBehaviorSanitizer bounds check on every index).
//  2. Every emitted frame obeys the framing rules: length is LEN + 9 and at most RX_FRAME_MAX,
//     F4 F5 start, direction 0x01, F4 FB end.
//  3. Resynchronisation: after ANY stream, with no reset, a run of back-to-back good frames is
//     picked up again within a bounded number of frames, and from the first good one on every
//     frame is delivered.
//  4. reset() clears the state: the next good frame is delivered whatever came before.
//  STRICT only (KNOWN_FAILURES: asm-no-checksum): an emitted frame also passes its checksum.
#include "fuzz_common.h"

using namespace fz;

static void check_emitted(const H::FrameAssembler *fa, size_t n) {
  const uint8_t *d = fa->data();
  FUZZ_CHECK(n >= 9 && n <= H::RX_FRAME_MAX, "emitted length %zu outside 9..%zu", n, H::RX_FRAME_MAX);
  FUZZ_CHECK(n == static_cast<size_t>(d[4]) + H::FRAME_LEN_OVERHEAD, "emitted %zu bytes but LEN says %zu", n,
             static_cast<size_t>(d[4]) + H::FRAME_LEN_OVERHEAD);
  FUZZ_CHECK(d[0] == 0xF4 && d[1] == 0xF5, "emitted frame does not start F4 F5");
  FUZZ_CHECK(d[2] == 0x01, "emitted frame is not A/C -> module (byte 2 = %02X)", d[2]);
  FUZZ_CHECK(d[n - 2] == 0xF4 && d[n - 1] == 0xFB, "emitted frame does not end F4 FB");
  if (STRICT)
    FUZZ_CHECK(H::status_checksum_ok(d, n), "emitted a %zu byte frame whose checksum is wrong", n);
}

// Feed one good frame as it travels (every 0xF4 between the tags doubled). True when the
// assembler delivered exactly that frame on its last byte.
static bool feed_good(H::FrameAssembler *fa, const uint8_t *frame, size_t n) {
  uint8_t wire[400];
  size_t w = model_stuff(frame, n, 2, wire);
  bool exact = false;
  for (size_t i = 0; i < w; i++) {
    size_t got = fa->feed(wire[i]);
    if (got != 0) {
      check_emitted(fa, got);
      exact = (i == w - 1) && got == n && memcmp(fa->data(), frame, n) == 0;
    }
  }
  return exact;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Reader r(data, size);
  uint8_t pick = r.u8();

  uint8_t good[160];
  size_t good_len;
  switch (pick % 3) {
    case 0:
      good_len = gold_link_reply(good, static_cast<uint8_t>(pick & 0x28));
      break;
    case 1:
      good_len = gold_status(good);
      break;
    default:
      good_len = gold_devtype_reply(good, 0x01, 0x02);
      break;
  }

  H::FrameAssembler *fa = new H::FrameAssembler();
  while (!r.empty()) {
    size_t n = fa->feed(r.u8());
    if (n != 0)
      check_emitted(fa, n);
  }

  // 3. The stream can leave the assembler inside a frame that claims up to RX_FRAME_MAX bytes,
  // and those bytes are taken from whatever follows. So the bound is "enough good frames to fill
  // the largest claim, plus the one cut in half, plus one".
  const int bound = static_cast<int>(H::RX_FRAME_MAX / good_len) + 3;
  int first_good = -1;
  for (int i = 0; i < bound + 3; i++) {
    bool exact = feed_good(fa, good, good_len);
    if (exact && first_good < 0)
      first_good = i;
    FUZZ_CHECK(first_good < 0 || exact, "good frame %d lost after frame %d had been delivered", i, first_good);
  }
  FUZZ_CHECK(first_good >= 0 && first_good < bound, "no good frame delivered within %d back-to-back frames (first %d)",
             bound, first_good);

  // 4. Dirty the state again with the head of the input, then reset.
  for (size_t i = 0; i < size && i < 8; i++)
    fa->feed(data[i]);
  fa->reset();
  FUZZ_CHECK(feed_good(fa, good, good_len), "good frame not delivered right after reset()");

  delete fa;
  return 0;
}
