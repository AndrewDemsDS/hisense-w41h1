#include "hisense_protocol.h"

#include <cstring>

namespace esphome::hisense_ac {

// Literal power frames. Several bytes of `off` differ from every other single-purpose command and
// are not understood field by field, only that this exact sequence powers the unit off. Neither
// checksum low byte is 0xF4, so no stuffing.
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

AcFeatures features_from_bitmap32(uint32_t b) {
  AcFeatures f;
  f.cool_heat = (b >> FEAT1_COOL_HEAT) & 1u;
  f.ai = (b >> FEAT1_AI) & 1u;
  f.infinite_fan = (b >> FEAT1_INFINITE_FAN) & 1u;
  f.power_save = (b >> FEAT1_POWER_SAVE) & 1u;
  f.fan_mute = (b >> FEAT1_FAN_MUTE) & 1u;
  f.swing_dir_8 = (b >> FEAT1_SWING_DIR_8) & 1u;
  f.swing_follow = (b >> FEAT1_SWING_FOLLOW) & 1u;
  f.humidity = (b >> FEAT1_HUMIDITY) & 1u;
  f.heat_8c = (b >> FEAT1_HEAT_8C) & 1u;
  f.purify = (b >> FEAT1_PURIFY) & 1u;
  f.power_display = static_cast<uint8_t>((b >> FEAT1_POWER_DISPLAY_SHIFT) & 0x3u);
  f.demand_resp = static_cast<uint8_t>((b >> FEAT1_DEMAND_RESP_SHIFT) & 0x3u);
  f.ext_valid = (b >> FEAT1_EXT_VALID) & 1u;
  f.q_display = (b >> FEAT1_Q_DISPLAY) & 1u;
  f.enable_8heat = (b >> FEAT1_ENABLE_8HEAT) & 1u;
  f.trans_102_64 = (b >> FEAT1_TRANS_102_64) & 1u;
  f.reply_len = 0;  // not encoded in the bitmap
  f.valid = (b >> FEAT1_VALID) & 1u;
  return f;
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

// Write the checksum over [2, chk_offset) at chk_offset (hi, lo), then F4 FB at end_offset.
static void finalize_frame(uint8_t *frame, size_t chk_offset, size_t end_offset) {
  uint16_t chk = checksum_range(frame, 2, chk_offset);
  frame[chk_offset] = static_cast<uint8_t>(chk >> 8);
  frame[chk_offset + 1] = static_cast<uint8_t>(chk & 0xFF);
  frame[end_offset] = FRAME_ETX1;
  frame[end_offset + 1] = FRAME_ETX2;
}

// 0xF4 inside a frame (only ever a checksum byte) is doubled so the receiver does not take it for a
// marker. For example, checksum 0x01F4 goes out as 01 F4 F4, then F4 FB.
static size_t stuff_checksum(const uint8_t *frame, size_t len, uint8_t *out, size_t out_cap) {
  if (len < 4 || out_cap < len + 2)
    return 0;
  size_t body = len - 4;
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

// Both stamps take a finished frame, as a builder returned it, and may get one back that already
// went through the other stamp. Such a frame can be one byte longer than LEN + 9 says, because a
// 0xF4 checksum low byte is doubled. The high byte never is: the sum of a transmit frame cannot
// reach 0xF400, which is what lets a stamp take the first LEN + 9 bytes and rewrite the trailer.
static_assert(TX_FRAME_MAX * 255 < 0xF400, "a transmit frame's checksum high byte must not be able to reach 0xF4");

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
  if (unstuffed == 0 || out == nullptr)
    return 0;
  f[7] = hi;
  f[8] = lo;
  finalize_frame(f, unstuffed - 4, unstuffed - 2);
  return stuff_checksum(f, unstuffed, out, out_cap);
}

size_t stamp_beep(const uint8_t *in, size_t len, bool beep, uint8_t *out, size_t out_cap) {
  uint8_t f[TX_FRAME_MAX];
  const size_t unstuffed = copy_for_stamp(in, len, f);
  if (unstuffed == 0 || out == nullptr)
    return 0;
  // The length first: a frame of 13 bytes has no byte 13 to look at.
  if (unstuffed >= CMD_FRAME_LEN && f[FRAME_CLASS_OFFSET] == CLASS_COMMAND) {
    if (beep) {
      f[CMD_BEEP_BYTE] |= CMD_BEEP_BIT;
    } else {
      f[CMD_BEEP_BYTE] &= static_cast<uint8_t>(~CMD_BEEP_BIT);
    }
    finalize_frame(f, unstuffed - 4, unstuffed - 2);
  }
  return stuff_checksum(f, unstuffed, out, out_cap);
}

// ---- Builders ----------------------------------------------------------------------------------

size_t build_producttype_request(uint8_t *out, size_t out_cap) {
  if (out == nullptr || out_cap < STATUS_REQUEST_LEN)
    return 0;
  std::memcpy(out, STATUS_REQUEST, STATUS_REQUEST_LEN);
  out[14] = SUBTYPE_PRODUCT_TYPE;  // body `66 40 00 00`
  finalize_frame(out, 17, 19);
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
  if (out == nullptr || out_cap < CMD_FRAME_MAX || offset >= CMD_FRAME_LEN)
    return 0;
  uint8_t f[CMD_FRAME_LEN] = {};
  std::memcpy(f, CMD_HEADER, CMD_HEADER_LEN);
  f[23] = 0x04;
  f[31] = 0x01;
  f[offset] = value;
  finalize_frame(f, CMD_CHK_OFFSET, CMD_END_OFFSET);
  return stuff_checksum(f, CMD_FRAME_LEN, out, out_cap);
}

size_t build_mute_frame(bool on, uint8_t *out, size_t out_cap) {
  return build_single_field(35, on ? 0x30 : 0x10, out, out_cap);
}

size_t build_sleep_frame(uint8_t profile, uint8_t *out, size_t out_cap) {
  // 0 = off (0x01); 1..4 = General / Old / Young / Kids. Unknown profiles are treated as off rather
  // than emitting an undefined wire byte.
  if (profile > 4)
    profile = 0;
  uint8_t v = (profile == 0) ? 0x01 : static_cast<uint8_t>(profile * 2 + 1);
  return build_single_field(17, v, out, out_cap);
}

size_t build_command(const AcCommand &cmd, uint8_t *out, size_t out_cap) {
  if (out == nullptr || out_cap < CMD_FRAME_MAX)
    return 0;

  // Dry and Fan-only strip the setpoint from the wire, so a stale or out-of-range one must not
  // drop the whole frame there.
  if (cmd.mode != MODE_DRY && cmd.mode != MODE_FAN && !setpoint_in_range(cmd.setpoint, cmd.fahrenheit))
    return 0;

  uint8_t frame[CMD_FRAME_LEN] = {};
  std::memcpy(frame, CMD_HEADER, CMD_HEADER_LEN);

  // 16: fan, index * 2 + 1. 0x01 (auto) is also the "unchanged" filler and stands in for NOCHANGE.
  frame[16] = (cmd.fan == FAN_SPEED_NOCHANGE) ? 0x01 : static_cast<uint8_t>(cmd.fan * 2 + 1);
  // 17: sleep, 0x00 = leave alone (sleep has its own frame).
  frame[17] = 0x00;
  // 18: mode in bits 4-7, (mode * 2 + 1) << 4.
  frame[CMD_MODE_BYTE] = static_cast<uint8_t>((cmd.mode * 2 + 1) << 4);
  // 18 bits 2-3: power-on in the same frame, as the stock module packs it (0x5C cool, 0x3C heat).
  if (cmd.power_on)
    frame[CMD_MODE_BYTE] |= CMD_POWER_ON_BITS;
  // 19: setpoint, value * 2 + 1 in whichever unit the panel uses.
  frame[19] = static_cast<uint8_t>(cmd.setpoint * 2 + 1);
  // Lockouts, matching the vendor app: Dry and Fan-only carry no setpoint, Dry no fan change.
  if (cmd.mode == MODE_DRY || cmd.mode == MODE_FAN)
    frame[19] = 0x00;
  if (cmd.mode == MODE_DRY)
    frame[16] = 0x01;
  // 23: baseline 0x04 in every non-unit-switch sample.
  frame[23] = 0x04;

  // 31/32/37: swing. Vertical is always commanded: "off" means hold the louvre (0x40), since
  // emitting 0x00 left the unit swinging. Combined byte 32 is non-overlapping bits. VERIFY combined.
  frame[31] = 0x01;
  uint8_t vswing_bits = (cmd.vswing == SWING_MODE_SWING) ? 0x3 : 0x1;
  uint8_t hswing_bits = 0x0;
  if (cmd.hswing == SWING_MODE_SWING) {
    hswing_bits = 0x3;
  } else if (cmd.hswing == SWING_MODE_DIRECTION) {
    hswing_bits = 0x1;
  }
  frame[32] = static_cast<uint8_t>((vswing_bits << 6) | (hswing_bits << 4));
  if (cmd.hswing != SWING_MODE_OFF)
    frame[37] = 0x14;

  // 33: eco / turbo.
  switch (cmd.feature) {
    case FEATURE_TURBO:
      frame[33] = 0x0C;
      break;
    case FEATURE_ECO:
      frame[33] = 0x30;
      break;
    case FEATURE_ECO_OFF:
      frame[33] = 0x10;
      break;
    case FEATURE_NONE:
    default:
      frame[33] = 0x04;
      break;
  }

  // 35: baseline 0x00. 36: display.
  frame[35] = 0x00;
  switch (cmd.display) {
    case DISPLAY_ON:
      frame[36] = 0xC0;
      break;
    case DISPLAY_OFF:
      frame[36] = 0x40;
      break;
    case DISPLAY_NOCHANGE:
    default:
      frame[36] = 0x00;
      break;
  }

  finalize_frame(frame, CMD_CHK_OFFSET, CMD_END_OFFSET);
  return stuff_checksum(frame, CMD_FRAME_LEN, out, out_cap);
}

size_t build_link_heartbeat(bool heard_ac, bool provisioning, uint8_t *out, size_t out_cap) {
  if (out == nullptr || out_cap < LINK_HEARTBEAT_LEN + 2)
    return 0;
  uint8_t f[LINK_HEARTBEAT_LEN];
  std::memcpy(f, LINK_HEARTBEAT, LINK_HEARTBEAT_LEN);
  f[16] = heard_ac ? 0xB0 : 0xF0;  // bit 6 = "not yet heard the A/C's 0x1E"
  // Byte 17 is 0x80 | selftest << 5 | scan << 4 | prov << 3 in the vendor module's builder. With
  // prov set the panel shows "77" (bench, 2026-07-09).
  if (provisioning)
    f[LINK_FLAGS_OFFSET] |= LINK_FLAG_PROVISIONING;
  finalize_frame(f, 24, 26);
  return stuff_checksum(f, LINK_HEARTBEAT_LEN, out, out_cap);
}

size_t build_link_heartbeat(bool heard_ac, uint8_t *out, size_t out_cap) {
  return build_link_heartbeat(heard_ac, false, out, out_cap);
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

uint8_t setup_request_step(SetupRequestTracker *tracker, uint8_t req_bits, bool indicating, uint32_t now_ms) {
  uint8_t req = req_bits & LINK_REQ_MASK;
  // Bit 3 while the module's own flag is on the wire, or before a reply without it, is not a
  // request anyone made.
  if (indicating) {
    tracker->reconfig_quiet = false;
  } else if ((req & LINK_REQ_RECONFIG) == 0) {
    tracker->reconfig_quiet = true;
  }
  if (!tracker->reconfig_quiet)
    req &= static_cast<uint8_t>(~LINK_REQ_RECONFIG);
  // A request that changes kind without a quiet reply in between starts over, so the second kind
  // is not swallowed by the first one's latch.
  if (req != 0 && req != tracker->last_req) {
    tracker->streak = 0;
    tracker->latched = false;
  }
  tracker->last_req = req;
  // An expired lockout is disarmed here, or it would read as running again after a clock wrap.
  if (tracker->lockout_armed && now_ms - tracker->lockout_since >= SETUP_REQUEST_LOCKOUT_MS)
    tracker->lockout_armed = false;
  const uint8_t gated = smartcfg_lockout_mask(req, now_ms, tracker->lockout_armed, tracker->lockout_since);
  // Only bit 3 can be handed back, so only a request that includes it has to be held.
  const uint8_t hold = gated == LINK_REQ_SMARTCFG ? 1 : SETUP_REQUEST_HOLD_FRAMES;
  if (setup_request_debounce(gated, &tracker->streak, &tracker->latched, hold) <= 0)
    return 0;
  setup_request_lockout(tracker, now_ms);
  return gated;
}

// ---- Parsers -----------------------------------------------------------------------------------
bool parse_status(const uint8_t *buf, size_t len, AcState *out) {
  if (buf == nullptr || out == nullptr)
    return false;
  // Byte 44 (outdoor temperature) is the last one read without a length test of its own.
  if (!frame_has_payload_byte(len, 44) || len != static_cast<size_t>(buf[4]) + FRAME_LEN_OVERHEAD)
    return false;
  if (buf[0] != FRAME_STX1 || buf[1] != FRAME_STX2)
    return false;
  if (buf[len - 2] != FRAME_ETX1 || buf[len - 1] != FRAME_ETX2)
    return false;
  uint16_t computed = checksum_range(buf, 2, len - 4);
  uint16_t received = static_cast<uint16_t>((buf[len - 4] << 8) | buf[len - 3]);
  if (computed != received)
    return false;

  *out = AcState{};
  uint8_t packed = buf[18];  // direction:2, run:2, mode:3, bit 7 not part of the mode
  uint8_t run_status = (packed >> 2) & 0x3;
  // 3-bit field, bits 4-6 (stock t_work_mode record, RE docs/10 7.4a). Kept identical to
  // hisense_rs485.cpp: the codec parity test compares the two parsers.
  uint8_t mode_status = (packed >> 4) & 0x7;
  // AUTO is status nibble 5 or 6 (the vendor module's AUTO command lands on 6).
  if (mode_status == 5 || mode_status == 6)
    mode_status = MODE_AUTO;
  uint8_t flags1 = buf[35];
  uint8_t flags2 = buf[36];

  out->valid = true;
  out->power_on = run_status != 0;
  out->mode = static_cast<Mode>(mode_status);
  out->fan_raw = buf[16];
  out->temp_unit_f = (buf[26] & 0x02) != 0;
  // Setpoint (19) and indoor (20) follow the panel unit; outdoor (44) and coil (45) stay Celsius.
  if (out->temp_unit_f) {
    out->setpoint_c = f_to_c(buf[19]);
    out->indoor_temp_c = f_to_c(buf[20]);
  } else {
    out->setpoint_c = static_cast<int8_t>(buf[19]);
    out->indoor_temp_c = static_cast<int8_t>(buf[20]);
  }
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
