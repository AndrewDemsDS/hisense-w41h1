// Shared by the libFuzzer harnesses in this directory. Header-only, no ESPHome, no STL containers
// in the hot path.
//
// Conventions:
//  - FUZZ_CHECK(cond, fmt, ...) is the only way a harness reports a broken property. It prints
//    and abort()s, which libFuzzer records as a crash and minimises like any sanitizer report.
//  - FUZZ_STRICT (a compile-time define) switches on the properties that are KNOWN to fail today.
//    Each one is listed in regressions/KNOWN_FAILURES with its reproducer. The default build
//    leaves them out so the smoke run stays green; the smoke run also replays every known failure
//    against a strict build and complains when one stops failing (the defect was fixed: delete
//    the entry and the exclusion).
//  - Buffers handed to the code under test are heap blocks of EXACTLY the stated size, so
//    AddressSanitizer sees a read or write one byte past the end.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hisense_protocol.h"

namespace fz {

namespace H = esphome::hisense_ac;

#define FUZZ_CHECK(cond, ...) \
  do { \
    if (!(cond)) { \
      fprintf(stderr, "FUZZ_CHECK failed at %s:%d: %s\n  ", __FILE__, __LINE__, #cond); \
      fprintf(stderr, __VA_ARGS__); \
      fprintf(stderr, "\n"); \
      abort(); \
    } \
  } while (0)

#ifdef FUZZ_STRICT
static const bool STRICT = true;
#else
static const bool STRICT = false;
#endif

// Sequential reader over the fuzz input. Reads past the end give zeroes, so every input is valid.
struct Reader {
  const uint8_t *p;
  size_t n;
  Reader(const uint8_t *data, size_t size) : p(data), n(size) {}
  bool empty() const { return n == 0; }
  uint8_t u8() {
    if (n == 0)
      return 0;
    n--;
    return *p++;
  }
  bool flag() { return (u8() & 1) != 0; }
  uint16_t u16() {
    uint16_t hi = u8();
    return static_cast<uint16_t>((hi << 8) | u8());
  }
  uint32_t u32() {
    uint32_t hi = u16();
    return (hi << 16) | u16();
  }
  // Up to `want` bytes into `out`; returns how many were there.
  size_t take(uint8_t *out, size_t want) {
    size_t k = want < n ? want : n;
    if (k != 0)
      memcpy(out, p, k);
    p += k;
    n -= k;
    return k;
  }
};

// A heap block of exactly `size` bytes. For size 0 the pointer is the one-past-the-end address of
// a live block, so that even a read of byte 0 is reported.
class Exact {
 public:
  explicit Exact(size_t size, uint8_t fill = 0xA5) : size_(size) {
    this->block_ = static_cast<uint8_t *>(malloc(size == 0 ? 8 : size));
    if (this->block_ == nullptr)
      abort();
    memset(this->block_, fill, size == 0 ? 8 : size);
  }
  Exact(const uint8_t *src, size_t size) : Exact(size) {
    if (size != 0)
      memcpy(this->block_, src, size);
  }
  ~Exact() { free(this->block_); }
  Exact(const Exact &) = delete;
  Exact &operator=(const Exact &) = delete;
  uint8_t *data() { return this->size_ == 0 ? this->block_ + 8 : this->block_; }
  size_t size() const { return this->size_; }

 private:
  uint8_t *block_;
  size_t size_;
};

// ---- An independent, deliberately dumb model of the wire format ---------------------------------
// Written from reverse-engineering/docs/03-rs485-ac-protocol.md, not from the code under test:
// F4 F5, direction, CTRL 0x40, LEN, ..., 16-bit big-endian byte sum over [2, n-4), F4 FB. Total
// length is LEN + 9. A 0xF4 between the start and end tags is doubled on the wire.

static inline uint16_t model_sum(const uint8_t *f, size_t start, size_t end) {
  uint32_t s = 0;
  for (size_t i = start; i < end; i++)
    s += f[i];
  return static_cast<uint16_t>(s);
}

// True when `f` (un-stuffed, `n` bytes) is a complete, self-consistent frame.
static inline bool model_frame_ok(const uint8_t *f, size_t n) {
  if (n < 13 || n != static_cast<size_t>(f[4]) + 9)
    return false;
  if (f[0] != 0xF4 || f[1] != 0xF5 || f[n - 2] != 0xF4 || f[n - 1] != 0xFB)
    return false;
  uint16_t s = model_sum(f, 2, n - 4);
  return f[n - 4] == (s >> 8) && f[n - 3] == (s & 0xFF);
}

// Write LEN, the checksum and the end tag into an `n`-byte frame whose other bytes are set.
static inline void model_seal(uint8_t *f, size_t n) {
  f[0] = 0xF4;
  f[1] = 0xF5;
  f[4] = static_cast<uint8_t>(n - 9);
  uint16_t s = model_sum(f, 2, n - 4);
  f[n - 4] = static_cast<uint8_t>(s >> 8);
  f[n - 3] = static_cast<uint8_t>(s & 0xFF);
  f[n - 2] = 0xF4;
  f[n - 1] = 0xFB;
}

// Frame -> wire. `stuff_from` is the first index whose 0xF4 is doubled: 2 for a sender that
// escapes the whole body (what the receive side un-does), n - 4 for one that escapes only the
// checksum (what this project's builders do). Returns the wire length; `wire` needs 2 * n bytes.
static inline size_t model_stuff(const uint8_t *f, size_t n, size_t stuff_from, uint8_t *wire) {
  size_t w = 0;
  for (size_t i = 0; i < n; i++) {
    wire[w++] = f[i];
    if (f[i] == 0xF4 && i >= stuff_from && i < n - 2)
      wire[w++] = 0xF4;
  }
  return w;
}

// Wire -> frame for one of OUR transmitted frames (direction 0x00), as a receiver that knows the
// rules would do it: LEN gives the un-stuffed length, the body is literal, each checksum byte that
// is 0xF4 must be followed by its double, then F4 FB and nothing else. Returns the un-stuffed
// length, or 0 when the wire bytes are not exactly one such frame.
static inline size_t model_unstuff_tx(const uint8_t *wire, size_t w, uint8_t *f, size_t f_cap) {
  if (w < 13 || wire[0] != 0xF4 || wire[1] != 0xF5)
    return 0;
  size_t n = static_cast<size_t>(wire[4]) + 9;
  if (n < 13 || n > f_cap || w < n)
    return 0;
  memcpy(f, wire, n - 4);
  size_t i = n - 4;
  for (size_t k = n - 4; k < n - 2; k++) {
    if (i >= w)
      return 0;
    f[k] = wire[i++];
    if (f[k] == 0xF4) {
      if (i >= w || wire[i] != 0xF4)
        return 0;
      i++;
    }
  }
  if (i + 2 != w || wire[i] != 0xF4 || wire[i + 1] != 0xFB)
    return 0;
  f[n - 2] = 0xF4;
  f[n - 1] = 0xFB;
  return n;
}

// The whole contract of a builder's output: fits the capacity, is exactly one frame on the wire,
// and that frame passes its own length and checksum rules. Leaves the un-stuffed frame in `f`.
static inline size_t check_tx_wire(const char *what, const uint8_t *wire, size_t w, size_t cap, uint8_t *f,
                                   size_t f_cap) {
  FUZZ_CHECK(w <= cap, "%s: wrote %zu bytes into a %zu byte buffer", what, w, cap);
  size_t n = model_unstuff_tx(wire, w, f, f_cap);
  FUZZ_CHECK(n != 0, "%s: %zu wire bytes are not one well-formed frame", what, w);
  FUZZ_CHECK(model_frame_ok(f, n), "%s: frame fails its own length or checksum rule", what);
  FUZZ_CHECK(H::status_checksum_ok(f, n), "%s: the codec's own checksum test rejects the frame", what);
  return n;
}

// Golden frames, used as the "known good traffic" after garbage and as templates to mutate.
// A/C -> module DevType reply: header of test_codec.cpp ("#49 devtype from reply"), 24 bytes long.
// 0x1E reply: 28 bytes, direction 0x01, as test_esphome_setup_request.cpp builds it.
// Status: the 160-byte 0x66 frame of test_codec.cpp make_status(), header bytes from there.
static const uint8_t GOLD_STATUS_HDR[16] = {0xF4, 0xF5, 0x01, 0x40, 0x97, 0x01, 0x00, 0xFE,
                                            0x01, 0x01, 0x01, 0x01, 0x00, 0x66, 0x00, 0x01};

// COOL, running, 22 C set, 21 C room, fan auto: the capture the RE doc quotes for bytes 18 to 20.
static inline size_t gold_status(uint8_t *f) {
  memset(f, 0, 160);
  memcpy(f, GOLD_STATUS_HDR, 16);
  f[16] = 0x01;  // fan auto (RE doc 03, status byte map)
  f[18] = 0x28;  // mode nibble 2 = cool, run field 0b10 (RE doc 03: 0x28)
  f[19] = 0x16;  // 22 C (RE doc 03)
  f[20] = 0x15;  // 21 C (RE doc 03)
  f[44] = 0x20;  // outdoor 32 C
  model_seal(f, 160);
  return 160;
}

static inline size_t gold_link_reply(uint8_t *f, uint8_t flags) {
  memset(f, 0, 28);
  f[2] = 0x01;  // A/C -> module
  f[3] = 0x40;  // CTRL: 16-bit checksum
  f[9] = 0xFE;
  f[10] = 0x01;
  f[13] = 0x1E;  // link class
  f[17] = flags;
  model_seal(f, 28);
  return 28;
}

static inline size_t gold_devtype_reply(uint8_t *f, uint8_t hi, uint8_t lo) {
  // Header from test_codec.cpp ("#49 devtype from reply"): F4 F5 01 40 .. 0A 00 00 <hi> <lo>.
  // 24 bytes as test_esphome_bus.cpp's simulated A/C sends it, so that the type bytes at 16/17
  // are payload and not the checksum (in a 20-byte frame they would be).
  static const uint8_t head[16] = {0xF4, 0xF5, 0x01, 0x40, 0x0F, 0x01, 0x00, 0xFE,
                                   0x01, 0x02, 0x03, 0x04, 0x00, 0x0A, 0x04, 0x00};
  memset(f, 0, 24);
  memcpy(f, head, 16);
  f[16] = hi;
  f[17] = lo;
  model_seal(f, 24);
  return 24;
}

}  // namespace fz
