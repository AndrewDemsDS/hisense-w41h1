// libFuzzer harness: every frame builder of the ESPHome codec, with arbitrary field values and
// arbitrary output capacities, and the two "stamp" functions that rewrite a finished frame.
//
// Input: a list of operations. Each starts [op, capacity], then the operation's own fields.
// The output buffer is a heap block of exactly `capacity` bytes (AddressSanitizer).
//
// Properties, for every builder:
//  - it returns 0 or the length of exactly one frame on the wire that fits the capacity and passes
//    its own length and checksum rules (check_tx_wire in fuzz_common.h);
//  - it returns 0 exactly when the header says so (capacity below the documented minimum, a
//    setpoint outside the range in a mode that carries one, an enum field that holds no named
//    value, a single-field offset that is not a body byte);
//  - round trip: the frame is decoded again by a decoder written here from the byte tables in
//    reverse-engineering/docs/03-rs485-ac-protocol.md and the header comments, and every field
//    must come back, with every byte the builder does not own left at zero.
//  STRICT only (KNOWN_FAILURES):
//    tx-f4-in-body: a builder never puts an un-doubled 0xF4 into the body of a frame.
#include "fuzz_common.h"

using namespace fz;

static bool body_has_f4(const uint8_t *f, size_t n) {
  for (size_t i = 2; i + 4 < n; i++)
    if (f[i] == 0xF4)
      return true;
  return false;
}

static bool fan_named(uint8_t fan) { return fan == 0 || fan == 1 || (fan >= 5 && fan <= 9) || fan == 0xFF; }
static bool swing_named(uint8_t s) { return s == 0 || s == 1 || s == 3; }

static void op_command(Reader &r, size_t cap) {
  H::AcCommand c;
  uint8_t mode = r.u8(), fan = r.u8(), vs = r.u8(), hs = r.u8(), feat = r.u8(), disp = r.u8();
  // Half the time fold the enums into their named values, so the fuzzer spends time there.
  if (r.flag()) {
    static const uint8_t fans[] = {0, 1, 5, 6, 7, 8, 9, 0xFF};
    static const uint8_t swings[] = {0, 1, 3};
    mode %= 5;
    fan = fans[fan % 8];
    vs = swings[vs % 3];
    hs = swings[hs % 3];
    feat %= 4;
    disp %= 3;
  }
  c.mode = static_cast<H::Mode>(mode);
  c.fan = static_cast<H::FanSpeed>(fan);
  c.vswing = static_cast<H::SwingMode>(vs);
  c.hswing = static_cast<H::SwingMode>(hs);
  c.feature = static_cast<H::Feature>(feat);
  c.display = static_cast<H::Display>(disp);
  c.setpoint = static_cast<int8_t>(r.u8());
  c.fahrenheit = r.flag();
  c.power_on = r.flag();

  Exact out(cap);
  size_t n = H::build_command(c, out.data(), cap);

  const bool carries_setpoint = mode != H::MODE_DRY && mode != H::MODE_FAN;
  const bool sp_ok = c.fahrenheit ? (c.setpoint >= 61 && c.setpoint <= 90) : (c.setpoint >= 16 && c.setpoint <= 32);
  const bool named = mode <= 4 && fan_named(fan) && swing_named(vs) && swing_named(hs) && feat <= 3 && disp <= 2;
  const bool expect_zero = cap < H::CMD_FRAME_MAX || (carries_setpoint && !sp_ok);
  FUZZ_CHECK(H::command_enums_valid(c) == named, "command_enums_valid disagrees for mode %u fan %u swing %u/%u", mode,
             fan, vs, hs);
  if (!named) {
    FUZZ_CHECK(n == 0, "build_command packed mode %u fan %u vswing %u hswing %u feature %u display %u", mode, fan, vs,
               hs, feat, disp);
    return;
  }
  FUZZ_CHECK((n == 0) == expect_zero, "build_command returned %zu, cap %zu mode %u setpoint %d %s", n, cap, mode,
             c.setpoint, c.fahrenheit ? "F" : "C");
  if (n == 0)
    return;

  uint8_t f[80];
  size_t u = check_tx_wire("build_command", out.data(), n, cap, f, sizeof(f));
  FUZZ_CHECK(u == H::CMD_FRAME_LEN, "command frame is %zu bytes", u);
  FUZZ_CHECK(!body_has_f4(f, u), "command frame carries 0xF4 in its body");
  // Header: F4 F5 00 40 29 .. 65 (CMD_HEADER in hisense_protocol.h, captured from the stock module).
  FUZZ_CHECK(memcmp(f, H::CMD_HEADER, H::CMD_HEADER_LEN) == 0, "command header");

  uint8_t want[50] = {};
  memcpy(want, H::CMD_HEADER, 16);
  // 16: fan index * 2 + 1 (RE doc 03 command table); auto 0x01 when dry or "no change".
  want[16] = (mode == H::MODE_DRY || fan == 0xFF) ? 0x01 : static_cast<uint8_t>(fan * 2 + 1);
  // 18: (mode * 2 + 1) << 4, e.g. auto 0x90 sniffed from the stock module; run field 0x0C = on.
  want[18] = static_cast<uint8_t>(((mode * 2 + 1) << 4) | (c.power_on ? 0x0C : 0x00));
  // 19: setpoint * 2 + 1 in the panel's unit, absent in dry and fan-only.
  want[19] = carries_setpoint ? static_cast<uint8_t>(c.setpoint * 2 + 1) : 0x00;
  want[23] = 0x04;  // baseline with the buzzer bit, every captured command
  want[31] = 0x01;  // the marker every combined frame writes
  // 32: vertical in bits 6-7 (0b01 hold, 0b11 swing), horizontal in bits 4-5.
  want[32] = static_cast<uint8_t>(((vs == 3 ? 3 : 1) << 6) | ((hs == 3 ? 3 : (hs == 1 ? 1 : 0)) << 4));
  static const uint8_t feature_byte[4] = {0x04, 0x30, 0x0C, 0x10};  // none, eco, turbo, eco off
  want[33] = feature_byte[feat];
  static const uint8_t display_byte[3] = {0x00, 0xC0, 0x40};  // no change, on, off
  want[36] = display_byte[disp];
  want[37] = 0x00;  // the vendor writer's 8-degree-heat field (RE doc 09); never a swing companion
  for (size_t i = 0; i < 46; i++)
    FUZZ_CHECK(f[i] == want[i], "command byte %zu is %02X, the decoder expects %02X", i, f[i], want[i]);
}

static void op_fixed(Reader &r, size_t cap, uint8_t which) {
  Exact out(cap);
  uint8_t f[80];
  switch (which) {
    case 0: {  // power
      bool on = r.flag();
      size_t n = H::build_power_frame(on, out.data(), cap);
      FUZZ_CHECK((n == 0) == (cap < H::CMD_FRAME_LEN), "build_power_frame returned %zu for cap %zu", n, cap);
      if (n == 0)
        return;
      size_t u = check_tx_wire("build_power_frame", out.data(), n, cap, f, sizeof(f));
      // Run field of byte 18: 0x0C on, 0x04 off (hardware-proven literals, hisense_protocol.h).
      FUZZ_CHECK(u == 50 && f[13] == 0x65 && f[18] == (on ? 0x0C : 0x04), "power frame content");
      return;
    }
    case 1: {  // mute
      bool on = r.flag();
      size_t n = H::build_mute_frame(on, out.data(), cap);
      FUZZ_CHECK((n == 0) == (cap < H::CMD_FRAME_MAX), "build_mute_frame returned %zu for cap %zu", n, cap);
      if (n == 0)
        return;
      size_t u = check_tx_wire("build_mute_frame", out.data(), n, cap, f, sizeof(f));
      FUZZ_CHECK(u == 50 && f[35] == (on ? 0x30 : 0x10) && f[23] == 0x04 && f[31] == 0x01, "mute frame content");
      return;
    }
    case 2: {  // sleep
      uint8_t profile = r.u8();
      size_t n = H::build_sleep_frame(profile, out.data(), cap);
      FUZZ_CHECK((n == 0) == (cap < H::CMD_FRAME_MAX), "build_sleep_frame returned %zu for cap %zu", n, cap);
      if (n == 0)
        return;
      size_t u = check_tx_wire("build_sleep_frame", out.data(), n, cap, f, sizeof(f));
      // Byte 17 = profile * 2 + 1, profiles 1..4; anything else is "off" = 0x01.
      uint8_t want = (profile >= 1 && profile <= 4) ? static_cast<uint8_t>(profile * 2 + 1) : 0x01;
      FUZZ_CHECK(u == 50 && f[17] == want && f[23] == 0x04 && f[31] == 0x01, "sleep frame content");
      return;
    }
    case 3: {  // ProductType request
      size_t n = H::build_producttype_request(out.data(), cap);
      FUZZ_CHECK((n == 0) == (cap < H::STATUS_REQUEST_LEN), "build_producttype_request returned %zu for cap %zu", n,
                 cap);
      if (n == 0)
        return;
      size_t u = check_tx_wire("build_producttype_request", out.data(), n, cap, f, sizeof(f));
      FUZZ_CHECK(u == 21 && f[2] == 0x00 && f[13] == 0x66 && f[14] == 0x40, "ProductType request content");
      return;
    }
    default: {  // link heartbeat
      bool heard = r.flag();
      bool prov = r.flag();
      size_t n = H::build_link_heartbeat(heard, prov, out.data(), cap);
      FUZZ_CHECK((n == 0) == (cap < H::LINK_HEARTBEAT_LEN + 2), "build_link_heartbeat returned %zu for cap %zu", n,
                 cap);
      if (n == 0)
        return;
      size_t u = check_tx_wire("build_link_heartbeat", out.data(), n, cap, f, sizeof(f));
      FUZZ_CHECK(u == 28 && f[13] == 0x1E, "heartbeat is a 28 byte 0x1E frame");
      // 16: 0xB0 once the A/C was heard, 0xF0 before. 17: 0x80, bit 3 = provisioning ("77").
      FUZZ_CHECK(f[16] == (heard ? 0xB0 : 0xF0) && f[17] == (prov ? 0x88 : 0x80), "heartbeat bytes 16/17");
      for (size_t i = 0; i < 24; i++)
        if (i != 16 && i != 17)
          FUZZ_CHECK(f[i] == H::LINK_HEARTBEAT[i], "heartbeat byte %zu differs from the captured template", i);
      return;
    }
  }
}

static void op_single_field(Reader &r, size_t cap) {
  uint8_t offset = r.u8();
  uint8_t value = r.u8();
  Exact out(cap);
  size_t n = H::build_single_field(offset, value, out.data(), cap);
  FUZZ_CHECK(n <= cap, "build_single_field wrote %zu bytes into %zu", n, cap);
  const bool body_offset = offset >= 16 && offset < H::CMD_CHK_OFFSET;
  if (!STRICT && n != 0 && body_offset && value == 0xF4)
    return;  // known: 0xF4 is packed as is
  if (!body_offset) {
    FUZZ_CHECK(n == 0, "build_single_field accepted offset %u, which is not a body byte", offset);
    return;
  }
  FUZZ_CHECK((n == 0) == (cap < H::CMD_FRAME_MAX), "build_single_field returned %zu for cap %zu", n, cap);
  if (n == 0)
    return;
  uint8_t f[80];
  size_t u = check_tx_wire("build_single_field", out.data(), n, cap, f, sizeof(f));
  FUZZ_CHECK(u == 50 && f[offset] == value, "single field byte %u", offset);
  FUZZ_CHECK(memcmp(f, H::CMD_HEADER, 16) == 0, "single field header");
  FUZZ_CHECK((offset == 23 || f[23] == 0x04) && (offset == 31 || f[31] == 0x01), "single field baseline bytes");
  FUZZ_CHECK(!body_has_f4(f, u), "build_single_field put an un-doubled 0xF4 into the body");
}

// A finished frame of ours with arbitrary length and content, as it would sit in the transmit
// queue: direction 0x00, sealed, checksum bytes doubled where they are 0xF4.
static size_t make_tx_frame(Reader &r, uint8_t *f, size_t *n_out, uint8_t *wire) {
  size_t n = 13 + r.u8() % 60;  // 13..72; more than TX_FRAME_MAX (64) must be refused
  for (size_t i = 2; i < n; i++) {
    f[i] = r.u8();
    if (f[i] == 0xF4)
      f[i] = 0x00;  // body bytes are never 0xF4 on this bus
  }
  f[2] = 0x00;
  f[3] = 0x40;
  if (n > 13 && r.flag())
    f[13] = 0x65;
  model_seal(f, n);
  *n_out = n;
  return model_stuff(f, n, n - 4, wire);
}

static void op_stamp_token(Reader &r, size_t cap) {
  uint8_t f[80], wire[170];
  size_t n;
  size_t w = make_tx_frame(r, f, &n, wire);
  uint8_t hi = r.u8(), lo = r.u8();
  size_t cut = r.u8() % 4;  // 0: the whole frame, else that many bytes short
  size_t len = w - (cut < w ? cut : 0);
  Exact in(wire, len);
  Exact out(cap);
  size_t got = H::stamp_link_token(in.data(), len, hi, lo, out.data(), cap);
  // The function sizes the frame from LEN, so it needs at least that many input bytes.
  const bool expect_zero = n > H::TX_FRAME_MAX || len < n || cap < n + 2;
  FUZZ_CHECK((got == 0) == expect_zero, "stamp_link_token returned %zu (frame %zu, input %zu, cap %zu)", got, n, len,
             cap);
  if (got == 0)
    return;
  uint8_t g[80];
  size_t u = check_tx_wire("stamp_link_token", out.data(), got, cap, g, sizeof(g));
  FUZZ_CHECK(u == n && g[7] == hi && g[8] == lo, "token bytes 7/8");
  for (size_t i = 0; i + 4 < n; i++)
    if (i != 7 && i != 8)
      FUZZ_CHECK(g[i] == f[i], "stamp_link_token changed byte %zu", i);
  if (STRICT)
    FUZZ_CHECK(!body_has_f4(g, u), "stamp_link_token put an un-doubled 0xF4 (token %02X %02X) into the body", hi, lo);
}

static void op_stamp_beep(Reader &r, size_t cap) {
  uint8_t f[80], wire[170];
  size_t n;
  size_t w = make_tx_frame(r, f, &n, wire);
  bool beep = r.flag();
  size_t cut = r.u8() % 4;
  size_t len = w - (cut < w ? cut : 0);
  Exact in(wire, len);
  Exact out(cap);
  size_t got = H::stamp_beep(in.data(), len, beep, out.data(), cap);
  const bool expect_zero = n > H::TX_FRAME_MAX || len < n || cap < n + 2;
  FUZZ_CHECK((got == 0) == expect_zero, "stamp_beep returned %zu (frame %zu, input %zu, cap %zu)", got, n, len, cap);
  if (got == 0)
    return;
  FUZZ_CHECK(got <= cap, "stamp_beep wrote %zu bytes into %zu", got, cap);
  const bool is_command = n >= H::CMD_FRAME_LEN && f[13] == 0x65;
  if (is_command) {
    uint8_t g[80];
    size_t u = check_tx_wire("stamp_beep", out.data(), got, cap, g, sizeof(g));
    FUZZ_CHECK(u == n, "stamp_beep changed the frame length");
    // Frame byte 23 bit 2 is the buzzer (stock get_dev_control_cmd, t_beep << 2).
    FUZZ_CHECK(((g[23] & 0x04) != 0) == beep, "buzzer bit");
    for (size_t i = 0; i + 4 < n; i++)
      FUZZ_CHECK(i == 23 ? ((g[i] | 0x04) == (f[i] | 0x04)) : (g[i] == f[i]), "stamp_beep changed byte %zu", i);
    return;
  }
  // Any other frame: "copied through unchanged". With a complete input (cut == 0) the output must
  // be the same wire bytes, also when the checksum's low byte is a doubled 0xF4. (A doubled HIGH
  // byte would be mangled, but a 64 byte frame cannot sum to 0xF400, so that cannot be built.)
  if (cut != 0)
    return;
  FUZZ_CHECK(got == w && memcmp(out.data(), wire, w) == 0, "stamp_beep altered a frame it should copy through");
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Reader r(data, size);
  int budget = 64;
  while (!r.empty() && budget-- > 0) {
    uint8_t op = r.u8();
    size_t cap = r.u8() % 96;
    // Most real callers pass a comfortable buffer; make that case as likely as the tight ones.
    if ((op & 0x80) != 0)
      cap = 64 + cap % 32;
    switch ((op & 0x7F) % 9) {
      case 0:
        op_command(r, cap);
        break;
      case 1:
      case 2:
      case 3:
      case 4:
      case 5:
        op_fixed(r, cap, static_cast<uint8_t>((op & 0x7F) % 9 - 1));
        break;
      case 6:
        op_single_field(r, cap);
        break;
      case 7:
        op_stamp_token(r, cap);
        break;
      default:
        op_stamp_beep(r, cap);
        break;
    }
  }
  return 0;
}
