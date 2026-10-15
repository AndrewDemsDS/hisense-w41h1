#include "hisense_protocol.h"

#include <cstring>

namespace esphome::hisense_ac {

// Literal power frames. Source: on[] and off[] in components/airconintl/messages.h of the community
// project https://github.com/pslawinski/esphome_airconintl, byte for byte. Several bytes of `off`
// differ from every other single-purpose command and are not understood field by field, only that
// this exact sequence powers the unit off. Neither checksum low byte is 0xF4, so no stuffing.
static const uint8_t FRAME_ON[CMD_FRAME_LEN] = {
    0xF4, 0xF5, 0x00, 0x40, 0x29, 0x00, 0x00, 0x01, 0x01, 0xFE, 0x01, 0x00, 0x00, 0x65, 0x00, 0x00, 0x00,
    0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xDF, 0xF4, 0xFB};
static const uint8_t FRAME_OFF[CMD_FRAME_LEN] = {
    0xF4, 0xF5, 0x00, 0x40, 0x29, 0x00, 0x00, 0x01, 0x01, 0xFE, 0x01, 0x00, 0x00, 0x65, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x55,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x31, 0xF4, 0xFB};

// ---- Bitmaps -----------------------------------------------------------------------------------
uint32_t features_to_bitmap32(const AcFeatures &f) {
  uint32_t b = 0;
  if (f.cool_heat)
    b |= 1u << FEAT1_COOL_HEAT;
  if (f.ai)
    b |= 1u << FEAT1_AI;
  if (f.infinite_fan)
    b |= 1u << FEAT1_INFINITE_FAN;
  if (f.power_save)
    b |= 1u << FEAT1_POWER_SAVE;
  if (f.fan_mute)
    b |= 1u << FEAT1_FAN_MUTE;
  if (f.swing_dir_8)
    b |= 1u << FEAT1_SWING_DIR_8;
  if (f.swing_follow)
    b |= 1u << FEAT1_SWING_FOLLOW;
  if (f.humidity)
    b |= 1u << FEAT1_HUMIDITY;
  if (f.heat_8c)
    b |= 1u << FEAT1_HEAT_8C;
  if (f.purify)
    b |= 1u << FEAT1_PURIFY;
  b |= static_cast<uint32_t>(f.power_display & 0x3u) << FEAT1_POWER_DISPLAY_SHIFT;
  b |= static_cast<uint32_t>(f.demand_resp & 0x3u) << FEAT1_DEMAND_RESP_SHIFT;
  if (f.ext_valid) {
    b |= 1u << FEAT1_EXT_VALID;
    if (f.q_display)
      b |= 1u << FEAT1_Q_DISPLAY;
    if (f.enable_8heat)
      b |= 1u << FEAT1_ENABLE_8HEAT;
    if (f.trans_102_64)
      b |= 1u << FEAT1_TRANS_102_64;
  }
  if (f.valid)
    b |= 1u << FEAT1_VALID;
  return b;
}

uint32_t faults_to_bitmap32(const AcFaults &f) {
  const bool bits[] = {f.in_temp,   f.in_coil_temp, f.in_humidity,   f.water_full,   f.in_fan_motor, f.grille,
                       f.in_vzero,  f.in_com,       f.in_display,    f.in_keys,      f.in_wifi,      f.in_ele,
                       f.in_eeprom, f.out_eeprom,   f.out_coil_temp, f.out_gas_temp, f.out_temp,     f.over_temp};
  static_assert(sizeof(bits) / sizeof(bits[0]) == FAULT1_OVER_TEMP + 1, "fault bits are LSB-first, struct order");
  uint32_t b = 0;
  for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
    if (bits[i])
      b |= 1u << i;
  }
  if (f.any)
    b |= 1u << FAULT1_ANY;
  if (f.valid)
    b |= 1u << FAULT1_VALID;
  return b;
}

// ---- Temperature -------------------------------------------------------------------------------
int8_t f_to_c(int f) {
  int n = (f - 32) * 5;
  int c = (n >= 0) ? (n + 4) / 9 : (n - 4) / 9;
  if (c > 127)
    c = 127;
  if (c < -128)
    c = -128;
  return static_cast<int8_t>(c);
}

// Needed on the command side: the A/C reads the setpoint byte in its display unit. A Celsius 23
// sent to an F panel makes it target 23 F.
int8_t c_to_f(int c) {
  int n = c * 9;
  int f = ((n >= 0) ? (n + 2) / 5 : (n - 2) / 5) + 32;
  if (f > 127)
    f = 127;
  if (f < -128)
    f = -128;
  return static_cast<int8_t>(f);
}

bool setpoint_in_range(int8_t setpoint, bool fahrenheit) {
  return fahrenheit ? (setpoint >= SETPOINT_MIN_F && setpoint <= SETPOINT_MAX_F)
                    : (setpoint >= SETPOINT_MIN_C && setpoint <= SETPOINT_MAX_C);
}

bool shadow_setpoint_from_status(int8_t setpoint_c, bool temp_unit_f, int8_t *out) {
  int8_t wire = temp_unit_f ? c_to_f(setpoint_c) : setpoint_c;
  if (!setpoint_in_range(wire, temp_unit_f))
    return false;
  if (out != nullptr)
    *out = wire;
  return true;
}

// ---- Checksum and stuffing ---------------------------------------------------------------------
// A plain running byte sum, confirmed against the vendor module's parser and against sample frames.
uint16_t checksum_range(const uint8_t *frame, size_t start, size_t end) {
  uint32_t sum = 0;
  for (size_t i = start; i < end; i++)
    sum += frame[i];
  return static_cast<uint16_t>(sum & 0xFFFF);
}

bool status_checksum_ok(const uint8_t *frame, size_t n) {
  if (n < 6)
    return false;
  if (frame[n - 2] != FRAME_ETX1 || frame[n - 1] != FRAME_ETX2)
    return false;
  size_t chk_off = n - 4;
  uint16_t chk = checksum_range(frame, 2, chk_off);
  return frame[chk_off] == static_cast<uint8_t>(chk >> 8) && frame[chk_off + 1] == static_cast<uint8_t>(chk & 0xFF);
}

// The sum of a transmit frame cannot reach 0xF400, so of its two checksum bytes only the low one
// can be 0xF4 and be doubled on the wire. The two stamps rely on it when they are handed a frame
// that went through a builder or a stamp before: it is at most one byte longer than LEN + 9 says,
// and its first LEN + 9 bytes hold everything up to and including the checksum low byte.
static_assert(TX_FRAME_MAX * 255 < 0xF400, "a transmit frame's checksum high byte must not be able to reach 0xF4");

// Write the checksum over [2, chk_offset) at chk_offset, high byte first.
static void write_checksum(uint8_t *frame, size_t chk_offset) {
  const uint16_t chk = checksum_range(frame, 2, chk_offset);
  frame[chk_offset] = static_cast<uint8_t>(chk >> 8);
  frame[chk_offset + 1] = static_cast<uint8_t>(chk & 0xFF);
}

// `frame` (`len` bytes with its checksum in place, len >= 13) as it goes on the wire: a checksum
// byte of 0xF4 doubled so that the receiver does not take it for a marker (checksum 0x01F4 goes
// out as 01 F4 F4), then F4 FB. Both checksum bytes are looked at, although a frame this file
// sealed can only have the low one at 0xF4 (above): stamp_beep() passes a frame of another class
// through with whatever checksum it came with. 0 when `out` cannot hold len + 2 bytes.
static size_t stuff_checksum(const uint8_t *frame, size_t len, uint8_t *out, size_t out_cap) {
  if (out == nullptr || out_cap < len + 2)
    return 0;
  const size_t body = len - FRAME_TRAILER_LEN;
  std::memcpy(out, frame, body);
  size_t o = body;
  for (size_t i = body; i < body + 2; i++) {
    out[o++] = frame[i];
    if (frame[i] == FRAME_STX1)
      out[o++] = FRAME_STX1;
  }
  out[o++] = FRAME_ETX1;
  out[o++] = FRAME_ETX2;
  return o;
}

// The same for a frame whose checksum is still to be written.
static size_t seal_frame(uint8_t *frame, size_t len, uint8_t *out, size_t out_cap) {
  write_checksum(frame, len - FRAME_TRAILER_LEN);
  return stuff_checksum(frame, len, out, out_cap);
}

// The first LEN + 9 bytes of `in` into `frame` (TX_FRAME_MAX bytes). Returns that length, at
// least FRAME_STAMP_MIN_LEN, or 0 when `in` is not a frame a stamp can work on.
static constexpr size_t FRAME_STAMP_MIN_LEN = 13;  // the envelope up to the class byte
static size_t copy_for_stamp(const uint8_t *in, size_t len, uint8_t *frame) {
  if (in == nullptr || len < FRAME_STAMP_MIN_LEN || in[0] != FRAME_STX1 || in[1] != FRAME_STX2)
    return 0;
  const size_t unstuffed = static_cast<size_t>(in[4]) + FRAME_LEN_OVERHEAD;
  if (unstuffed < FRAME_STAMP_MIN_LEN || unstuffed > len || unstuffed > TX_FRAME_MAX)
    return 0;
  std::memcpy(frame, in, unstuffed);
  return unstuffed;
}

// The vendor module writes envelope bytes 7/8 before the checksum, and the checksum covers them, so
// a re-stamp is re-finalised.
size_t stamp_link_token(const uint8_t *in, size_t len, uint8_t hi, uint8_t lo, uint8_t *out, size_t out_cap) {
  uint8_t f[TX_FRAME_MAX];
  const size_t unstuffed = copy_for_stamp(in, len, f);
  if (unstuffed == 0)
    return 0;
  f[7] = hi;
  f[8] = lo;
  return seal_frame(f, unstuffed, out, out_cap);
}

size_t stamp_beep(const uint8_t *in, size_t len, bool beep, uint8_t *out, size_t out_cap) {
  uint8_t f[TX_FRAME_MAX];
  const size_t unstuffed = copy_for_stamp(in, len, f);
  if (unstuffed == 0)
    return 0;
  // The length first: a frame of 13 bytes has no byte 13 to look at. Any other frame goes back
  // out as it came, checksum included.
  if (unstuffed < CMD_FRAME_LEN || f[FRAME_CLASS_OFFSET] != CLASS_COMMAND)
    return stuff_checksum(f, unstuffed, out, out_cap);
  if (beep) {
    f[CMD_BEEP_BYTE] |= CMD_BEEP_BIT;
  } else {
    f[CMD_BEEP_BYTE] &= static_cast<uint8_t>(~CMD_BEEP_BIT);
  }
  return seal_frame(f, unstuffed, out, out_cap);
}

// ---- Builders ----------------------------------------------------------------------------------

size_t build_producttype_request(uint8_t *out, size_t out_cap) {
  if (out == nullptr || out_cap < STATUS_REQUEST_LEN)
    return 0;
  // The status poll with its subtype changed (body `66 40 00 00`). The checksum is 0x01F3, no
  // doubling, and the end tag is the template's.
  std::memcpy(out, STATUS_REQUEST, STATUS_REQUEST_LEN);
  out[14] = SUBTYPE_PRODUCT_TYPE;
  write_checksum(out, STATUS_REQUEST_LEN - FRAME_TRAILER_LEN);
  return STATUS_REQUEST_LEN;
}

size_t build_power_frame(bool power_on, uint8_t *out, size_t out_cap) {
  if (out == nullptr || out_cap < CMD_FRAME_LEN)
    return 0;
  std::memcpy(out, power_on ? FRAME_ON : FRAME_OFF, CMD_FRAME_LEN);
  return CMD_FRAME_LEN;
}

// VERIFY: only the named byte is hardware-confirmed (mute 35 = 0x30 / 0x10, sleep 17 = profile*2+1);
// the rest is the minimal baseline. Byte 31 = 0x01 is the marker every combined frame writes; without
// it a real A/C accepts both single-field frames and ignores them.
size_t build_single_field(uint8_t offset, uint8_t value, uint8_t *out, size_t out_cap) {
  if (offset < CMD_HEADER_LEN || offset >= CMD_CHK_OFFSET)
    return 0;
  uint8_t f[CMD_FRAME_LEN] = {};
  std::memcpy(f, CMD_HEADER, CMD_HEADER_LEN);
  f[23] = 0x04;
  f[31] = 0x01;
  f[offset] = value;
  return seal_frame(f, CMD_FRAME_LEN, out, out_cap);
}

size_t build_mute_frame(bool on, uint8_t *out, size_t out_cap) {
  return build_single_field(35, on ? 0x30 : 0x10, out, out_cap);
}

size_t build_sleep_frame(uint8_t profile, uint8_t *out, size_t out_cap) {
  // 0 = off (0x01); 1..4 = General / Old / Young / Kids. Unknown profiles are treated as off rather
  // than emitting an undefined wire byte.
  if (profile > 4)
    profile = 0;
  return build_single_field(17, static_cast<uint8_t>(profile * 2 + 1), out, out_cap);
}

static bool fan_speed_named(FanSpeed fan) {
  return fan == FAN_SPEED_AUTO || fan == FAN_SPEED_QUIET || (fan >= FAN_SPEED_LOW && fan <= FAN_SPEED_HIGH) ||
         fan == FAN_SPEED_NOCHANGE;
}

static bool swing_mode_named(SwingMode swing) {
  return swing == SWING_MODE_OFF || swing == SWING_MODE_DIRECTION || swing == SWING_MODE_SWING;
}

bool command_enums_valid(const AcCommand &cmd) {
  return cmd.mode <= MODE_AUTO && fan_speed_named(cmd.fan) && swing_mode_named(cmd.vswing) &&
         swing_mode_named(cmd.hswing) && cmd.feature <= FEATURE_ECO_OFF && cmd.display <= DISPLAY_OFF;
}

// Byte 33 and byte 36 for each enum value (command_enums_valid() has checked the range).
static constexpr uint8_t FEATURE_BYTES[] = {
    0x04,  // FEATURE_NONE: neutral and turbo-off, confirmed
    0x30,  // FEATURE_ECO, confirmed on W41H1
    0x0C,  // FEATURE_TURBO, confirmed on W41H1
    0x10,  // FEATURE_ECO_OFF. VERIFY, see the enum
};
static constexpr uint8_t DISPLAY_BYTES[] = {
    0x00,  // DISPLAY_NOCHANGE
    0xC0,  // DISPLAY_ON
    0x40,  // DISPLAY_OFF
};

// Byte 32: vertical in bits 6-7, horizontal in bits 4-5, 0b11 = swing, 0b01 = move to a position.
// Vertical is always commanded: "off" means hold the louvre (0b01), since emitting 0b00 left the
// unit swinging. Horizontal off is 0b00. SwingMode's values are those bit patterns, so vertical is
// the value with bit 0 set and horizontal the value itself. VERIFY the two combined: vertical is
// confirmed, and the test units have no horizontal motor.
static uint8_t swing_byte(SwingMode vswing, SwingMode hswing) {
  return static_cast<uint8_t>(((vswing | 0x1) << 6) | (hswing << 4));
}

size_t build_command(const AcCommand &cmd, uint8_t *out, size_t out_cap) {
  if (!command_enums_valid(cmd))
    return 0;
  // Lockouts, matching the vendor app: Dry and Fan-only carry no setpoint, Dry no fan change. A
  // stale or out-of-range setpoint must not drop the whole frame in those two modes.
  const bool carries_setpoint = cmd.mode != MODE_DRY && cmd.mode != MODE_FAN;
  const bool carries_fan = cmd.mode != MODE_DRY && cmd.fan != FAN_SPEED_NOCHANGE;
  if (carries_setpoint && !setpoint_in_range(cmd.setpoint, cmd.fahrenheit))
    return 0;

  // Everything not named below stays 0x00, among it byte 17 (sleep) and byte 35 (mute), which
  // have frames of their own.
  uint8_t frame[CMD_FRAME_LEN] = {};
  std::memcpy(frame, CMD_HEADER, CMD_HEADER_LEN);
  // 16: fan, index * 2 + 1. 0x01 (auto) is also the "unchanged" filler.
  frame[16] = carries_fan ? static_cast<uint8_t>(cmd.fan * 2 + 1) : 0x01;
  // 18: mode in bits 4-7, (mode * 2 + 1) << 4. Bits 2-3: power-on in the same frame, as the stock
  // module packs it (0x5C cool, 0x3C heat).
  frame[CMD_MODE_BYTE] = static_cast<uint8_t>(((cmd.mode * 2 + 1) << 4) | (cmd.power_on ? CMD_POWER_ON_BITS : 0));
  // 19: setpoint, value * 2 + 1 in whichever unit the panel uses.
  frame[19] = carries_setpoint ? static_cast<uint8_t>(cmd.setpoint * 2 + 1) : 0x00;
  // 23: baseline 0x04 in every non-unit-switch sample.
  frame[23] = 0x04;
  // 31: the marker every combined frame writes. Byte 37 stays 0: it is the 8-degree-heat field
  // in the vendor firmware's writer (RE doc 09), not a horizontal-swing companion as the first
  // driver assumed; horizontal swing is bits 4-5 of byte 32 alone.
  frame[31] = 0x01;
  frame[32] = swing_byte(cmd.vswing, cmd.hswing);
  frame[33] = FEATURE_BYTES[cmd.feature];
  frame[36] = DISPLAY_BYTES[cmd.display];
  return seal_frame(frame, CMD_FRAME_LEN, out, out_cap);
}

size_t build_link_heartbeat(bool heard_ac, bool provisioning, uint8_t *out, size_t out_cap) {
  uint8_t f[LINK_HEARTBEAT_LEN];
  std::memcpy(f, LINK_HEARTBEAT, LINK_HEARTBEAT_LEN);
  f[16] = heard_ac ? 0xB0 : 0xF0;  // bit 6 = "not yet heard the A/C's 0x1E"
  // Byte 17 is 0x80 | selftest << 5 | scan << 4 | prov << 3 in the vendor module's builder. With
  // prov set the panel shows "77" (bench, 2026-07-09).
  if (provisioning)
    f[LINK_FLAGS_OFFSET] |= LINK_FLAG_PROVISIONING;
  return seal_frame(f, LINK_HEARTBEAT_LEN, out, out_cap);
}

// ---- Setup request ("77") ----------------------------------------------------------------------
bool link_request_bits(const uint8_t *frame, size_t len, uint8_t *bits) {
  if (frame == nullptr || bits == nullptr)
    return false;
  // Bytes 0 to 17, then at least the checksum and the end tag.
  if (len < LINK_FLAGS_OFFSET + 5 || frame[FRAME_CLASS_OFFSET] != CLASS_LINK)
    return false;
  if (!status_checksum_ok(frame, len))
    return false;
  *bits = frame[LINK_FLAGS_OFFSET] & LINK_REQ_MASK;
  return true;
}

int setup_request_debounce(uint8_t req_bits, uint8_t *streak, bool *latched, uint8_t hold_frames) {
  if (req_bits != 0) {
    if (*streak < 0xFF)
      (*streak)++;
    if (*streak >= hold_frames && !*latched) {
      *latched = true;
      return 1;
    }
    return 0;
  }
  // The request is gone. Only one that had counted reports its end.
  const bool was_latched = *latched;
  *streak = 0;
  *latched = false;
  return was_latched ? -1 : 0;
}

uint8_t smartcfg_lockout_mask(uint8_t req_bits, uint32_t now_ms, bool armed, uint32_t since_ms) {
  if (armed && now_ms - since_ms < SETUP_REQUEST_LOCKOUT_MS)
    req_bits &= static_cast<uint8_t>(~LINK_REQ_SMARTCFG);
  return req_bits;
}

void setup_request_lockout(SetupRequestTracker *tracker, uint32_t now_ms) {
  tracker->lockout_armed = true;
  tracker->lockout_since = now_ms;
}

void setup_request_expire(SetupRequestTracker *tracker, uint32_t now_ms) {
  if (tracker->lockout_armed && now_ms - tracker->lockout_since >= SETUP_REQUEST_LOCKOUT_MS)
    tracker->lockout_armed = false;
}

void setup_request_flag_sent(SetupRequestTracker *tracker) { tracker->reconfig_quiet = false; }

void setup_request_link_lost(SetupRequestTracker *tracker) {
  tracker->streak = 0;
  tracker->latched = false;
}

uint8_t setup_request_step(SetupRequestTracker *tracker, uint8_t req_bits, bool indicating, uint32_t now_ms) {
  uint8_t asking = req_bits & LINK_REQ_MASK;
  // Bit 3 while the module's own flag is on the wire, or before a reply without it, is not a
  // request anyone made.
  if (indicating) {
    tracker->reconfig_quiet = false;
  } else if ((asking & LINK_REQ_RECONFIG) == 0) {
    tracker->reconfig_quiet = true;
  }
  if (!tracker->reconfig_quiet)
    asking &= static_cast<uint8_t>(~LINK_REQ_RECONFIG);
  setup_request_expire(tracker, now_ms);
  const bool locked_out =
      smartcfg_lockout_mask(LINK_REQ_SMARTCFG, now_ms, tracker->lockout_armed, tracker->lockout_since) == 0;

  // Bit 5 counts when it appears outside the lockout. One that appears inside it, or stays up, is
  // spent: it does not count later because the lockout has run out in the meantime.
  const bool smartcfg = (asking & LINK_REQ_SMARTCFG) != 0;
  if (!smartcfg) {
    tracker->smartcfg_live = false;
  } else if (!tracker->smartcfg_seen) {
    tracker->smartcfg_live = !locked_out;
  }
  tracker->smartcfg_seen = smartcfg;

  // Bit 3 counts once it has been there for SETUP_REQUEST_HOLD_FRAMES replies in a row.
  const uint8_t reconfig = asking & LINK_REQ_RECONFIG;
  const bool reconfig_counts =
      setup_request_debounce(reconfig, &tracker->streak, &tracker->latched, SETUP_REQUEST_HOLD_FRAMES) > 0;

  if (asking == 0) {
    tracker->taken = false;
    return 0;
  }
  // One unbroken run of replies that ask is one request, whichever bits come and go inside it.
  // And a request that includes bit 3 waits for bit 3: only that bit can be handed back.
  if (tracker->taken || (reconfig != 0 && !reconfig_counts))
    return 0;
  uint8_t request = reconfig;
  if (tracker->smartcfg_live && !locked_out)
    request |= LINK_REQ_SMARTCFG;
  if (request == 0)
    return 0;
  tracker->taken = true;
  setup_request_lockout(tracker, now_ms);
  return request;
}

// ---- Parsers -----------------------------------------------------------------------------------
// A whole status frame: the start tag, a length that agrees with LEN, the checksum and the end tag,
// and long enough for byte 44 (outdoor temperature), the last byte read without a test of its own.
static bool status_frame_ok(const uint8_t *buf, size_t len) {
  return frame_has_payload_byte(len, 44) && len == static_cast<size_t>(buf[4]) + FRAME_LEN_OVERHEAD &&
         buf[0] == FRAME_STX1 && buf[1] == FRAME_STX2 && status_checksum_ok(buf, len);
}

// Status byte 18 holds direction:2, run:2, mode:3; bit 7 is not part of the mode (stock t_work_mode
// record, RE docs/10 7.4a). AUTO is reported as 5 or 6 (the vendor module's AUTO command lands on
// 6). Kept identical to hisense_parse_status() in firmware/test/reference-driver/hisense_rs485.cpp:
// the codec parity test compares the two parsers.
static Mode status_mode(uint8_t packed) {
  const uint8_t field = (packed >> 4) & 0x7;
  return field == 5 || field == 6 ? MODE_AUTO : static_cast<Mode>(field);
}

// A temperature byte in the panel's unit, as Celsius.
static int8_t panel_temp_c(uint8_t raw, bool fahrenheit) { return fahrenheit ? f_to_c(raw) : static_cast<int8_t>(raw); }

bool parse_status(const uint8_t *buf, size_t len, AcState *out) {
  if (buf == nullptr || out == nullptr || !status_frame_ok(buf, len))
    return false;

  *out = AcState{};
  const uint8_t flags1 = buf[35];
  const uint8_t flags2 = buf[36];
  out->valid = true;
  out->power_on = ((buf[18] >> 2) & 0x3) != 0;
  out->mode = status_mode(buf[18]);
  out->fan_raw = buf[16];
  out->temp_unit_f = (buf[26] & 0x02) != 0;
  // Setpoint (19) and indoor (20) follow the panel unit; outdoor (44) and coil (45) stay Celsius.
  out->setpoint_c = panel_temp_c(buf[19], out->temp_unit_f);
  out->setpoint_raw = buf[19];
  out->indoor_temp_c = panel_temp_c(buf[20], out->temp_unit_f);
  out->vswing_on = (flags1 & 0x80) != 0;
  out->hswing_on = (flags1 & 0x40) != 0;
  out->turbo_on = (flags1 & 0x02) != 0;
  out->eco_on = (flags1 & 0x04) != 0;
  out->heat_relay_on = (flags1 & 0x10) != 0;
  out->mute_on = (flags2 & 0x04) != 0;
  out->purify_on = (flags2 & 0x80) != 0;
  out->sleep_raw = buf[17];
  out->sleep_on = buf[17] != 0;
  out->outdoor_temp_c = static_cast<int8_t>(buf[44]);
  out->coil_temp_c = frame_has_payload_byte(len, 45) ? static_cast<int8_t>(buf[45]) : 0;
  out->compressor_freq = buf[42];
  out->voltage_raw = frame_has_payload_byte(len, 50) ? buf[50] : 0;
  out->current_raw = frame_has_payload_byte(len, 55) ? buf[55] : 0;
  return true;
}

// Offsets are frame[13 + N], as in the vendor module's parser. The extended tier (frame 38/39) is
// gated on length, as the vendor module does, instead of rejecting a shorter reply.
bool parse_features(const uint8_t *buf, size_t len, AcFeatures *out) {
  if (buf == nullptr || out == nullptr)
    return false;
  if (!frame_has_payload_byte(len, 35) || buf[FRAME_CLASS_OFFSET] != CLASS_STATUS || buf[14] != SUBTYPE_PRODUCT_TYPE)
    return false;
  out->cool_heat = (buf[18] & 0x80) != 0;
  out->power_save = (buf[23] & 0x40) != 0;
  out->purify = (buf[23] & 0x08) != 0;
  out->fan_mute = (buf[24] & 0x40) != 0;
  out->infinite_fan = (buf[25] & 0x08) != 0;
  out->heat_8c = (buf[26] & 0x80) != 0;
  out->swing_follow = (buf[26] & 0x02) != 0;
  out->power_display = static_cast<uint8_t>((buf[27] >> 6) & 0x03);
  out->ai = (buf[28] & 0x40) != 0;
  out->swing_dir_8 = (buf[28] & 0x10) != 0;
  out->humidity = (buf[32] & 0x01) != 0;
  out->demand_resp = static_cast<uint8_t>(buf[35] & 0x03);
  out->ext_valid = frame_has_payload_byte(len, 39);
  out->q_display = out->ext_valid && (buf[39] & 0x40) != 0;
  out->enable_8heat = out->ext_valid && (buf[39] & 0x04) != 0;
  out->trans_102_64 = out->ext_valid && (buf[38] & 0x08) != 0;
  out->reply_len = static_cast<uint8_t>(len > 255 ? 255 : len);
  out->valid = true;
  return true;
}

// Each byte group is gated on its own, since short frames occur and the outdoor/protection bytes
// sit well past the indoor pair. Validated against induced faults.
bool parse_faults(const uint8_t *buf, size_t len, AcFaults *out) {
  if (buf == nullptr || out == nullptr)
    return false;
  *out = AcFaults{};
  if (!frame_has_payload_byte(len, FAULT_BYTE_INDOOR))
    return false;

  out->raw_indoor = buf[FAULT_BYTE_INDOOR];
  out->in_temp = (out->raw_indoor & 0x80) != 0;
  out->in_coil_temp = (out->raw_indoor & 0x40) != 0;
  out->in_humidity = (out->raw_indoor & 0x20) != 0;
  out->water_full = (out->raw_indoor & 0x10) != 0;
  out->in_fan_motor = (out->raw_indoor & 0x08) != 0;
  out->grille = (out->raw_indoor & 0x04) != 0;
  out->in_vzero = (out->raw_indoor & 0x02) != 0;
  out->in_com = (out->raw_indoor & 0x01) != 0;

  if (frame_has_payload_byte(len, FAULT_BYTE_MODULE)) {
    out->raw_module = buf[FAULT_BYTE_MODULE];
    out->in_display = (out->raw_module & 0x80) != 0;
    out->in_keys = (out->raw_module & 0x40) != 0;
    out->in_wifi = (out->raw_module & 0x20) != 0;
    out->in_ele = (out->raw_module & 0x10) != 0;
    out->in_eeprom = (out->raw_module & 0x08) != 0;
  }
  if (frame_has_payload_byte(len, FAULT_BYTE_OUTDOOR)) {
    out->raw_outdoor = buf[FAULT_BYTE_OUTDOOR];
    out->out_eeprom = (out->raw_outdoor & 0x40) != 0;
    out->out_coil_temp = (out->raw_outdoor & 0x20) != 0;
    out->out_gas_temp = (out->raw_outdoor & 0x10) != 0;
    out->out_temp = (out->raw_outdoor & 0x08) != 0;
  }
  if (frame_has_payload_byte(len, FAULT_BYTE_PROTECT)) {
    out->raw_protect = buf[FAULT_BYTE_PROTECT];
    out->over_temp = (out->raw_protect & 0x10) != 0;
  }
  out->any = (out->raw_indoor | out->raw_module | out->raw_outdoor |
              static_cast<uint8_t>(out->raw_protect & ~FAULT_NONFAULT_PROTECT)) != 0;
  out->valid = true;
  return true;
}

// The device type is the pair at frame 16/17, NOT the envelope 9/10 "session token": stamping that
// one kills the link on hardware.
bool devtype_from_reply(const uint8_t *reply, size_t n, uint8_t *hi, uint8_t *lo) {
  if (reply == nullptr || !frame_has_payload_byte(n, 17) || reply[FRAME_CLASS_OFFSET] != CLASS_DEVTYPE)
    return false;
  if (hi != nullptr)
    *hi = reply[16];
  if (lo != nullptr)
    *lo = reply[17];
  return true;
}

bool devtype_reply_usable(bool checksum_ok, const uint8_t *reply, size_t n, uint8_t *hi, uint8_t *lo) {
  uint8_t type;
  uint8_t sub;
  if (!checksum_ok || !devtype_from_reply(reply, n, &type, &sub) || (type == 0 && sub == 0))
    return false;
  if (hi != nullptr)
    *hi = type;
  if (lo != nullptr)
    *lo = sub;
  return true;
}

// ---- FrameAssembler ----------------------------------------------------------------------------
FrameProgress frame_progress(const uint8_t *frame, size_t n, size_t *expected) {
  switch (n) {
    case 2:
      return frame[1] == FRAME_STX2 ? FRAME_PROGRESS_MORE : FRAME_PROGRESS_BROKEN;
    case 3:
      return frame[2] == FRAME_DIR_FROM_AC ? FRAME_PROGRESS_MORE : FRAME_PROGRESS_BROKEN;
    case 5:
      *expected = static_cast<size_t>(frame[4]) + FRAME_LEN_OVERHEAD;
      return *expected <= RX_FRAME_MAX ? FRAME_PROGRESS_MORE : FRAME_PROGRESS_BROKEN;
    default:
      break;
  }
  // LEN + 9 is at least 9, so the frame cannot end on one of the header bytes judged above.
  if (*expected == 0 || n < *expected)
    return FRAME_PROGRESS_MORE;
  return frame[n - 2] == FRAME_ETX1 && frame[n - 1] == FRAME_ETX2 ? FRAME_PROGRESS_COMPLETE : FRAME_PROGRESS_BROKEN;
}

void FrameAssembler::reset() {
  this->len_ = 0;
  this->expected_ = 0;
  this->later_ = 0;
  this->later_expected_ = 0;
  this->prev_was_f4_ = false;
}

// Outside a frame every byte but the start byte is skipped.
void FrameAssembler::start_(uint8_t b) {
  this->reset();
  if (b != FRAME_STX1)
    return;
  this->buf_[0] = b;
  this->len_ = 1;
  this->prev_was_f4_ = true;
}

// Hand out the frame that begins at `start` in the buffer and forget everything else.
size_t FrameAssembler::deliver_(size_t start) {
  const size_t n = this->len_ - start;
  if (start != 0)
    std::memmove(this->buf_, this->buf_ + start, n);
  this->reset();
  return n;
}

size_t FrameAssembler::feed(uint8_t b) {
  if (this->len_ == 0) {
    this->start_(b);
    return 0;
  }
  // A doubled F4 is an escaped data byte: drop the second one.
  if (b == FRAME_STX1 && this->prev_was_f4_) {
    this->prev_was_f4_ = false;
    return 0;
  }
  const bool after_bare_f4 = this->prev_was_f4_;
  this->prev_was_f4_ = b == FRAME_STX1;
  // No overflow: the frame in front is judged on every byte and ends at `expected_`, at most
  // RX_FRAME_MAX, and until byte 4 has set `expected_` it holds fewer than 5 bytes.
  this->buf_[this->len_++] = b;

  const FrameProgress front = frame_progress(this->buf_, this->len_, &this->expected_);
  if (front == FRAME_PROGRESS_COMPLETE)
    return this->deliver_(0);

  if (this->later_ != 0) {
    const FrameProgress later =
        frame_progress(this->buf_ + this->later_, this->len_ - this->later_, &this->later_expected_);
    if (later == FRAME_PROGRESS_COMPLETE)
      return this->deliver_(this->later_);
    if (later == FRAME_PROGRESS_BROKEN)
      this->later_ = 0;
  }
  // An F4 that is not doubled, followed by F5, inside the frame: from a sender that doubles its
  // data bytes this can only be the start of another frame, so the one in front was cut short.
  // It is remembered and not acted on, in case the frame in front completes after all. The newest
  // one wins: whatever an older one was, it is cut short too.
  if (after_bare_f4 && b == FRAME_STX2 && this->len_ > 2) {
    this->later_ = this->len_ - 2;
    this->later_expected_ = 0;
  }

  if (front != FRAME_PROGRESS_BROKEN)
    return 0;
  if (this->later_ == 0) {
    // The byte that broke the frame may itself open the next one.
    this->start_(b);
    return 0;
  }
  // The frame in front was not one. The later start takes its place with what it has so far.
  this->len_ -= this->later_;
  std::memmove(this->buf_, this->buf_ + this->later_, this->len_);
  this->expected_ = this->later_expected_;
  this->later_ = 0;
  this->later_expected_ = 0;
  return 0;
}

}  // namespace esphome::hisense_ac
