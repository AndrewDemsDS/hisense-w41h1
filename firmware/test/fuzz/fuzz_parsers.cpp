// libFuzzer harness: every function of the ESPHome codec that takes a received frame, fed
// arbitrary bytes at every truncation.
//
// Input: the bytes are the "frame". Each parser is called on the whole input and on every prefix
// of it, each time from a heap block of exactly that length, so a read past the stated length is
// an AddressSanitizer report.
//
// Properties (port only; the comparison with the reference driver is fuzz_differential.cpp):
//  - parse_status accepts only a frame that passes the model's length, tag and checksum rules,
//    and leaves the output alone when it refuses.
//  - parse_features accepts only class 0x66 subtype 0x40 with byte 35 in front of the trailer, and
//    its result packs into the documented bitmap.
//  - parse_faults: true exactly when byte 39 is payload; a refusal leaves a cleared, invalid
//    struct; `any` is the OR of the raw bytes (bit 7 of the protection byte excluded).
//  - devtype_from_reply accepts only class 0x0A long enough for bytes 16/17 to be payload (22
//    bytes), never reads past the frame, tolerates null outputs.
//  - No parser takes a value out of the last 4 bytes of the frame (checksum and end tag).
//  - link_request_bits accepts only a checksum-good class 0x1E frame and returns masked bits.
//  - status_checksum_ok agrees with the model.
#include "fuzz_common.h"

using namespace fz;

// Byte `at` of a `len` byte frame is payload: it lies in front of the checksum and the end tag.
static bool payload(size_t len, size_t at) { return len >= at + 5; }

static void one_length(const uint8_t *src, size_t len) {
  Exact buf(src, len);
  const uint8_t *f = buf.data();
  const bool well_formed = model_frame_ok(f, len);

  // ---- status_checksum_ok ----
  bool chk = H::status_checksum_ok(f, len);
  if (len >= 6) {
    uint16_t s = model_sum(f, 2, len - 4);
    bool want = f[len - 2] == 0xF4 && f[len - 1] == 0xFB && f[len - 4] == (s >> 8) && f[len - 3] == (s & 0xFF);
    FUZZ_CHECK(chk == want, "status_checksum_ok(len %zu) = %d, model says %d", len, chk, want);
  } else {
    FUZZ_CHECK(!chk, "status_checksum_ok accepted %zu bytes", len);
  }

  // ---- parse_status ----
  H::AcState st;
  st.compressor_freq = 0x5A;
  st.valid = true;
  bool ok = H::parse_status(f, len, &st);
  if (ok) {
    FUZZ_CHECK(well_formed && payload(len, 44), "parse_status accepted a frame the model rejects (len %zu)", len);
    FUZZ_CHECK(st.valid, "parse_status returned true with valid = false");
    FUZZ_CHECK(st.fan_raw == f[16] && st.sleep_raw == f[17] && st.compressor_freq == f[42], "raw fields");
    FUZZ_CHECK(st.power_on == ((f[18] & 0x0C) != 0), "power_on is the run field of byte 18");
    // 7 is the one field value with no Mode name; mode_from_status() in hisense_map.h filters it.
    FUZZ_CHECK(static_cast<uint8_t>(st.mode) <= 4 || static_cast<uint8_t>(st.mode) == 7, "mode %u",
               static_cast<unsigned>(st.mode));
  } else {
    FUZZ_CHECK(st.valid && st.compressor_freq == 0x5A, "parse_status refused the frame but wrote the output");
    FUZZ_CHECK(!(well_formed && payload(len, 44)), "parse_status refused a well-formed %zu byte frame", len);
  }
  FUZZ_CHECK(!H::parse_status(f, len, nullptr) && !H::parse_status(nullptr, len, &st), "null arguments refused");

  // ---- parse_features ----
  H::AcFeatures ft;
  bool fok = H::parse_features(f, len, &ft);
  bool fwant = payload(len, 35) && f[13] == 0x66 && f[14] == 0x40;
  FUZZ_CHECK(fok == fwant, "parse_features(len %zu) = %d, expected %d", len, fok, fwant);
  if (fok) {
    FUZZ_CHECK(ft.valid && ft.ext_valid == payload(len, 39), "features valid / ext_valid");
    FUZZ_CHECK(ft.power_display <= 3 && ft.demand_resp <= 3, "two-bit fields");
    FUZZ_CHECK(ft.reply_len == (len > 255 ? 255 : len), "reply_len");
    // The packed form, bit by bit as hisense_protocol.h lists the FEAT1_ indices (binary_sensor.py
    // reads the same numbers): the three ext bits only with the ext tier.
    uint32_t want = (ft.cool_heat ? 1u << 0 : 0) | (ft.ai ? 1u << 1 : 0) | (ft.infinite_fan ? 1u << 2 : 0) |
                    (ft.power_save ? 1u << 3 : 0) | (ft.fan_mute ? 1u << 4 : 0) | (ft.swing_dir_8 ? 1u << 5 : 0) |
                    (ft.swing_follow ? 1u << 6 : 0) | (ft.humidity ? 1u << 7 : 0) | (ft.heat_8c ? 1u << 8 : 0) |
                    (ft.purify ? 1u << 9 : 0) | (static_cast<uint32_t>(ft.power_display) << 16) |
                    (static_cast<uint32_t>(ft.demand_resp) << 18) | (1u << 31);
    if (ft.ext_valid)
      want |= (1u << 30) | (ft.q_display ? 1u << 10 : 0) | (ft.enable_8heat ? 1u << 11 : 0) |
              (ft.trans_102_64 ? 1u << 12 : 0);
    FUZZ_CHECK(H::features_to_bitmap32(ft) == want, "feature bitmap %08X, the fields say %08X",
               H::features_to_bitmap32(ft), want);
  } else {
    FUZZ_CHECK(!ft.valid, "parse_features refused the frame but set valid");
  }

  // ---- parse_faults ----
  H::AcFaults fl;
  fl.valid = true;
  fl.any = true;
  fl.raw_indoor = 0xEE;
  bool aok = H::parse_faults(f, len, &fl);
  FUZZ_CHECK(aok == payload(len, H::FAULT_BYTE_INDOOR), "parse_faults(len %zu) = %d", len, aok);
  if (aok) {
    FUZZ_CHECK(fl.valid && fl.raw_indoor == f[39], "indoor fault byte");
    FUZZ_CHECK(fl.raw_module == (payload(len, 40) ? f[40] : 0) && fl.raw_outdoor == (payload(len, 64) ? f[64] : 0) &&
                   fl.raw_protect == (payload(len, 66) ? f[66] : 0),
               "length-gated fault bytes");
    bool any = (fl.raw_indoor | fl.raw_module | fl.raw_outdoor | (fl.raw_protect & 0x7F)) != 0;
    FUZZ_CHECK(fl.any == any, "any = %d, raw bytes say %d", fl.any, any);
    uint32_t bm = H::faults_to_bitmap32(fl);
    FUZZ_CHECK(((bm >> H::FAULT1_VALID) & 1u) == 1u && ((bm >> H::FAULT1_ANY) & 1u) == (any ? 1u : 0u), "fault bitmap");
    FUZZ_CHECK((bm & 0x3FFC0000u) == 0, "fault bitmap sets an unassigned bit");
  } else {
    FUZZ_CHECK(!fl.valid && !fl.any && fl.raw_indoor == 0, "parse_faults refused the frame but left stale data");
  }

  // ---- devtype_from_reply ----
  uint8_t hi = 0x77, lo = 0x77;
  bool dok = H::devtype_from_reply(f, len, &hi, &lo);
  // In a frame shorter than 22 bytes, bytes 16/17 are the checksum or the end tag.
  FUZZ_CHECK(dok == (payload(len, 17) && f[13] == 0x0A), "devtype_from_reply(len %zu) = %d", len, dok);
  if (dok) {
    FUZZ_CHECK(hi == f[16] && lo == f[17], "device type is frame bytes 16/17");
    FUZZ_CHECK(H::devtype_from_reply(f, len, nullptr, nullptr), "null outputs are allowed");
  } else {
    FUZZ_CHECK(hi == 0x77 && lo == 0x77, "devtype_from_reply refused the frame but wrote the outputs");
  }

  // ---- link_request_bits ----
  uint8_t bits = 0xEE;
  bool lok = H::link_request_bits(f, len, &bits);
  if (lok) {
    FUZZ_CHECK(len >= 22 && f[13] == 0x1E && chk, "link_request_bits accepted a frame it must not");
    FUZZ_CHECK(bits == (f[17] & 0x28), "request bits %02X from byte 17 = %02X", bits, f[17]);
  } else {
    FUZZ_CHECK(bits == 0xEE, "link_request_bits refused the frame but wrote the output");
    FUZZ_CHECK(!(len >= 22 && f[13] == 0x1E && chk), "link_request_bits refused a good 0x1E reply");
  }
  FUZZ_CHECK(!H::link_request_bits(f, len, nullptr), "null output refused");
  FUZZ_CHECK(!H::parse_features(f, len, nullptr) && !H::parse_features(nullptr, len, &ft), "null arguments refused");
  FUZZ_CHECK(!H::parse_faults(f, len, nullptr) && !H::parse_faults(nullptr, len, &fl), "null arguments refused");
  FUZZ_CHECK(!H::devtype_from_reply(nullptr, len, &hi, &lo), "null frame refused");
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size > 260)
    size = 260;
  for (size_t len = 0; len <= size; len++)
    one_length(data, len);
  return 0;
}
