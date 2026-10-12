// libFuzzer harness: receive-side round trips. The harness plays the A/C: it encodes a frame
// from fuzzed field values with its own encoder (written from the byte tables in
// reverse-engineering/docs/03-rs485-ac-protocol.md), puts it on the wire with every 0xF4 between
// the tags doubled, runs it through the assembler and then through the parser for its class, and
// compares what comes out with what went in.
//
// (The transmit-side round trip, build a command and decode it again, is in fuzz_builders.cpp.)
//
// Input: [class pick, length pick, lead-in pick, repeat pick], then the frame's bytes.
// Properties:
//  - the assembler delivers the frame exactly once, on its last byte, byte for byte, whatever
//    0xF4 bytes the payload holds and wherever they fall, also back to back with no gap and after
//    a lead-in of line noise that holds no 0xF4;
//  - the parser for the class accepts it and every decoded field matches the encoder's value.
#include <cmath>

#include "fuzz_common.h"

using namespace fz;

static int8_t model_f_to_c(uint8_t f) {
  long c = lround((static_cast<double>(f) - 32.0) * 5.0 / 9.0);
  if (c > 127)
    c = 127;
  if (c < -128)
    c = -128;
  return static_cast<int8_t>(c);
}

// Deliver `f` through the assembler `times` times back to back and check each delivery.
static void deliver(H::FrameAssembler *fa, const uint8_t *f, size_t n, const uint8_t *lead, size_t lead_len,
                    int times) {
  uint8_t wire[420];
  size_t w = model_stuff(f, n, 2, wire);
  for (size_t i = 0; i < lead_len; i++)
    FUZZ_CHECK(fa->feed(lead[i]) == 0, "line noise without 0xF4 produced a frame");
  for (int t = 0; t < times; t++) {
    for (size_t i = 0; i < w; i++) {
      size_t got = fa->feed(wire[i]);
      if (i + 1 < w) {
        FUZZ_CHECK(got == 0, "frame delivered early, at wire byte %zu of %zu (pass %d)", i, w, t);
      } else {
        FUZZ_CHECK(got == n, "frame of %zu bytes not delivered on its last byte (got %zu, pass %d)", n, got, t);
        FUZZ_CHECK(memcmp(fa->data(), f, n) == 0, "delivered frame differs from the one sent (pass %d)", t);
      }
    }
  }
}

static void check_status(const uint8_t *f, size_t n) {
  H::AcState st;
  FUZZ_CHECK(H::parse_status(f, n, &st), "parse_status refused a well-formed %zu byte status frame", n);
  const bool unit_f = (f[26] & 0x02) != 0;          // byte 26 bit 1: Fahrenheit panel
  uint8_t nib = (f[18] >> 4) & 0x7;                 // byte 18 bits 4-6: mode
  uint8_t mode = (nib == 5 || nib == 6) ? 4 : nib;  // auto reads back as 5, or 6 after an AUTO command
  FUZZ_CHECK(st.valid && st.power_on == (((f[18] >> 2) & 0x3) != 0), "power (byte 18 run field)");
  FUZZ_CHECK(static_cast<uint8_t>(st.mode) == mode, "mode %u, byte 18 = %02X", static_cast<unsigned>(st.mode), f[18]);
  FUZZ_CHECK(st.temp_unit_f == unit_f, "temperature unit");
  // 19 setpoint and 20 room temperature are plain integers in the panel's unit.
  int8_t sp = unit_f ? model_f_to_c(f[19]) : static_cast<int8_t>(f[19]);
  int8_t room = unit_f ? model_f_to_c(f[20]) : static_cast<int8_t>(f[20]);
  FUZZ_CHECK(st.setpoint_c == sp && st.indoor_temp_c == room, "setpoint %d (want %d), room %d (want %d)", st.setpoint_c,
             sp, st.indoor_temp_c, room);
  FUZZ_CHECK(st.fan_raw == f[16] && st.sleep_raw == f[17] && st.sleep_on == (f[17] != 0), "fan and sleep bytes");
  // 35: 0x80 vswing, 0x40 hswing, 0x10 aux heat relay, 0x04 eco, 0x02 turbo. 36: 0x04 mute.
  FUZZ_CHECK(st.vswing_on == ((f[35] & 0x80) != 0) && st.hswing_on == ((f[35] & 0x40) != 0) &&
                 st.heat_relay_on == ((f[35] & 0x10) != 0) && st.eco_on == ((f[35] & 0x04) != 0) &&
                 st.turbo_on == ((f[35] & 0x02) != 0) && st.mute_on == ((f[36] & 0x04) != 0),
             "flag bytes 35/36");
  // Purify: the codec reads byte 36 bit 7 and marks it VERIFY. RE doc 03 and 05 say bit 5 (0x20).
  // The harness follows the codec here; the disagreement is in the findings, not asserted.
  FUZZ_CHECK(st.purify_on == ((f[36] & 0x80) != 0), "purify bit");
  FUZZ_CHECK(st.compressor_freq == f[42] && st.outdoor_temp_c == static_cast<int8_t>(f[44]), "compressor, outdoor");
  FUZZ_CHECK(st.coil_temp_c == (n > 45 ? static_cast<int8_t>(f[45]) : 0) && st.voltage_raw == (n > 50 ? f[50] : 0) &&
                 st.current_raw == (n > 55 ? f[55] : 0),
             "length-gated telemetry bytes");
  // Same frame, fault view. Wire byte = 15 + payload offset: 39 indoor, 40 module, 64 outdoor, 66.
  H::AcFaults fl;
  bool has = H::parse_faults(f, n, &fl);
  FUZZ_CHECK(has == (n > 39), "parse_faults");
  if (has)
    FUZZ_CHECK(fl.valid && fl.raw_indoor == f[39] && fl.raw_outdoor == (n > 64 ? f[64] : 0), "fault bytes");
}

static void check_features(const uint8_t *f, size_t n) {
  H::AcFeatures ft;
  FUZZ_CHECK(H::parse_features(f, n, &ft) && ft.valid, "parse_features refused a %zu byte 0x66/40 reply", n);
  // Offsets are frame[13 + N] of the vendor module's parser (hisense_protocol.cpp comment).
  FUZZ_CHECK(ft.cool_heat == ((f[18] & 0x80) != 0) && ft.power_save == ((f[23] & 0x40) != 0) &&
                 ft.purify == ((f[23] & 0x08) != 0) && ft.fan_mute == ((f[24] & 0x40) != 0) &&
                 ft.infinite_fan == ((f[25] & 0x08) != 0) && ft.heat_8c == ((f[26] & 0x80) != 0) &&
                 ft.swing_follow == ((f[26] & 0x02) != 0) && ft.power_display == ((f[27] >> 6) & 3) &&
                 ft.ai == ((f[28] & 0x40) != 0) && ft.swing_dir_8 == ((f[28] & 0x10) != 0) &&
                 ft.humidity == ((f[32] & 0x01) != 0) && ft.demand_resp == (f[35] & 3),
             "base feature flags");
  FUZZ_CHECK(ft.ext_valid == (n > 39), "ext_valid");
  if (n > 39)
    FUZZ_CHECK(ft.q_display == ((f[39] & 0x40) != 0) && ft.enable_8heat == ((f[39] & 0x04) != 0) &&
                   ft.trans_102_64 == ((f[38] & 0x08) != 0),
               "extended feature flags");
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Reader r(data, size);
  uint8_t cls = r.u8() % 4;
  uint8_t len_pick = r.u8();
  uint8_t lead_pick = r.u8();
  int times = 1 + r.u8() % 3;

  static const size_t min_len[4] = {45, 36, 22, 22};                          // status, features, link reply, DevType
  size_t n = min_len[cls] + len_pick % (H::RX_FRAME_MAX - min_len[cls] + 1);  // up to RX_FRAME_MAX
  uint8_t f[200];
  for (size_t i = 0; i < n; i++)
    f[i] = r.u8();
  f[2] = 0x01;  // A/C -> module
  switch (cls) {
    case 0:
      f[13] = 0x66;
      f[14] = 0x00;
      break;
    case 1:
      f[13] = 0x66;
      f[14] = 0x40;
      break;
    case 2:
      f[13] = 0x1E;
      break;
    default:
      f[13] = 0x0A;
      break;
  }
  model_seal(f, n);

  uint8_t lead[8];
  size_t lead_len = lead_pick % 8;
  for (size_t i = 0; i < lead_len; i++) {
    lead[i] = static_cast<uint8_t>(lead_pick * 37 + i * 91);
    if (lead[i] == 0xF4)
      lead[i] = 0xFB;
  }

  H::FrameAssembler *fa = new H::FrameAssembler();
  deliver(fa, f, n, lead, lead_len, times);
  const uint8_t *got = fa->data();

  switch (cls) {
    case 0:
      check_status(got, n);
      break;
    case 1:
      check_features(got, n);
      break;
    case 2: {
      uint8_t bits = 0xEE;
      FUZZ_CHECK(H::link_request_bits(got, n, &bits), "link_request_bits refused a good %zu byte 0x1E reply", n);
      // Byte 17: bit 5 smart-config request, bit 3 reconfigure request (RE doc 03, "Recommission (77)").
      FUZZ_CHECK(bits == (f[17] & 0x28), "request bits %02X from flags %02X", bits, f[17]);
      break;
    }
    default: {
      uint8_t hi = 0, lo = 0;
      FUZZ_CHECK(H::devtype_from_reply(got, n, &hi, &lo) && hi == f[16] && lo == f[17], "device type round trip");
      break;
    }
  }
  delete fa;
  return 0;
}
