#include "hisense_protocol.h"

#include <cstring>

// Port of the codec half of firmware/src/rs485-driver/hisense_rs485.cpp. Kept equal to it by
// firmware/test/test_esphome_codec_parity.cpp; see hisense_protocol.h.

namespace esphome::hisense_ac {

// Literal power frames, byte for byte from messages.h on[] / off[]. Several bytes of `off` differ
// from every other single-purpose command and are not understood field by field, only that this
// exact sequence powers the unit off. Neither checksum low byte is 0xF4, so no stuffing.
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
// sent to an F panel made it target 23 F (hardware, 2026-07-19).
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
// Confirmed against the stock parser (a plain running byte sum, Thumb `ldrb; add` loop) and
// re-derived against all ~75 sample frames in esphome_airconintl messages.h.
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
// marker. messages.h temp_16_C proves it: checksum 0x01F4 goes out as 01 F4 F4, then F4 FB.
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

// Stock writes envelope bytes 7/8 before the checksum (0x9b6f09dc), and the checksum covers them, so
// a re-stamp is re-finalised. The unstuffed length comes from LEN + 9 (RE docs/10 3.2).
size_t stamp_link_token(const uint8_t *in, size_t len, uint8_t hi, uint8_t lo, uint8_t *out, size_t out_cap) {
  if (in == nullptr || out == nullptr || len < 13)
    return 0;
  if (in[0] != FRAME_STX1 || in[1] != FRAME_STX2)
    return 0;
  size_t unstuffed = static_cast<size_t>(in[4]) + FRAME_LEN_OVERHEAD;
  if (unstuffed < 13 || unstuffed > len || unstuffed > TX_FRAME_MAX)
    return 0;
  uint8_t f[TX_FRAME_MAX];
  std::memcpy(f, in, unstuffed);
  f[7] = hi;
  f[8] = lo;
  finalize_frame(f, unstuffed - 4, unstuffed - 2);
  return stuff_checksum(f, unstuffed, out, out_cap);
}

// ---- Builders ----------------------------------------------------------------------------------
size_t build_producttype_request(uint8_t *out, size_t out_cap) {
  if (out == nullptr || out_cap < STATUS_REQUEST_LEN)
    return 0;
  std::memcpy(out, STATUS_REQUEST, STATUS_REQUEST_LEN);
  out[14] = SUBTYPE_PRODUCT_TYPE;  // stock body `66 40 00 00`
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
// the rest is the minimal baseline. Byte 31 = 0x01 is the marker every combined frame writes; both
// single-field frames were accepted and ignored by a real A/C until it was added (2026-08-19).
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

static bool override_offset_ok(int off) {
  return off >= static_cast<int>(CMD_HEADER_LEN) && off < static_cast<int>(CMD_CHK_OFFSET);
}

// Shared body of build_command() and build_command_override(). Negative offsets mean no patch, which
// reproduces the plain command byte for byte.
static size_t build_command_impl(const AcCommand &cmd, uint8_t *out, size_t out_cap, int off1, uint8_t val1, int off2,
                                 uint8_t val2) {
  if (out == nullptr || out_cap < CMD_FRAME_MAX)
    return 0;

  // Dry and Fan-only strip the setpoint from the wire (#53), so a stale or out-of-range one must not
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
  frame[18] = static_cast<uint8_t>((cmd.mode * 2 + 1) << 4);
  // 19: setpoint, value * 2 + 1 in whichever unit the panel uses.
  frame[19] = static_cast<uint8_t>(cmd.setpoint * 2 + 1);
  // #53 lockouts, matching the stock app: Dry and Fan-only carry no setpoint, Dry no fan change.
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

  // Bench patches go after the packing and before the checksum, so the frame stays valid.
  if (off1 >= 0 && static_cast<size_t>(off1) < CMD_FRAME_LEN)
    frame[off1] = val1;
  if (off2 >= 0 && static_cast<size_t>(off2) < CMD_FRAME_LEN)
    frame[off2] = val2;

  finalize_frame(frame, CMD_CHK_OFFSET, CMD_END_OFFSET);
  return stuff_checksum(frame, CMD_FRAME_LEN, out, out_cap);
}

size_t build_command(const AcCommand &cmd, uint8_t *out, size_t out_cap) {
  return build_command_impl(cmd, out, out_cap, -1, 0, -1, 0);
}

size_t build_command_override(const AcCommand &cmd, uint8_t *out, size_t out_cap, int off1, uint8_t val1, int off2,
                              uint8_t val2) {
  // Patching the header or checksum gives a frame the A/C drops, which reads as "this offset does
  // nothing" and would poison a sweep, so those are refused.
  if (!override_offset_ok(off1) || (off2 >= 0 && !override_offset_ok(off2)))
    return 0;
  return build_command_impl(cmd, out, out_cap, off1, val1, off2, val2);
}

size_t build_link_heartbeat(bool heard_ac, uint8_t *out, size_t out_cap) {
  if (out == nullptr || out_cap < LINK_HEARTBEAT_LEN + 2)
    return 0;
  uint8_t f[LINK_HEARTBEAT_LEN];
  std::memcpy(f, LINK_HEARTBEAT, LINK_HEARTBEAT_LEN);
  f[16] = heard_ac ? 0xB0 : 0xF0;  // bit 6 = "not yet heard the A/C's 0x1E" (stock recv_num30_flag)
  finalize_frame(f, 24, 26);
  return stuff_checksum(f, LINK_HEARTBEAT_LEN, out, out_cap);
}

// ---- Parsers -----------------------------------------------------------------------------------
bool parse_status(const uint8_t *buf, size_t len, AcState *out) {
  if (buf == nullptr || out == nullptr)
    return false;
  if (len < 45 || len != static_cast<size_t>(buf[4]) + FRAME_LEN_OVERHEAD)
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
  uint8_t packed = buf[18];  // direction:2, run:2, mode:4
  uint8_t run_status = (packed >> 2) & 0x3;
  uint8_t mode_status = (packed >> 4) & 0xF;
  // AUTO is status nibble 5 or 6 (bus tap 2026-07-08: the stock AUTO command lands on 6).
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
  out->coil_temp_c = (len > 45) ? static_cast<int8_t>(buf[45]) : 0;
  out->compressor_freq = buf[42];
  out->voltage_raw = (len > 50) ? buf[50] : 0;
  out->current_raw = (len > 55) ? buf[55] : 0;
  return true;
}

// Offsets are frame[13 + N] per the stock parser (FUN_9b6f0c4c). The extended tier (frame 38/39) is
// gated on length, as stock does (RE docs/10 5a), instead of rejecting a shorter reply.
bool parse_features(const uint8_t *buf, size_t len, AcFeatures *out) {
  if (buf == nullptr || out == nullptr)
    return false;
  if (len <= 35 || buf[FRAME_CLASS_OFFSET] != CLASS_STATUS || buf[14] != SUBTYPE_PRODUCT_TYPE)
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
  out->ext_valid = len > 39;
  out->q_display = out->ext_valid && (buf[39] & 0x40) != 0;
  out->enable_8heat = out->ext_valid && (buf[39] & 0x04) != 0;
  out->trans_102_64 = out->ext_valid && (buf[38] & 0x08) != 0;
  out->reply_len = static_cast<uint8_t>(len > 255 ? 255 : len);
  out->valid = true;
  return true;
}

// Each byte group is gated on its own, since short frames occur and the outdoor/protection bytes
// sit well past the indoor pair. Validated against induced faults 2026-07-22 (RE docs/10 7.6).
bool parse_faults(const uint8_t *buf, size_t len, AcFaults *out) {
  if (buf == nullptr || out == nullptr)
    return false;
  *out = AcFaults{};
  if (len <= FAULT_BYTE_INDOOR)
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

  if (len > FAULT_BYTE_MODULE) {
    out->raw_module = buf[FAULT_BYTE_MODULE];
    out->in_display = (out->raw_module & 0x80) != 0;
    out->in_keys = (out->raw_module & 0x40) != 0;
    out->in_wifi = (out->raw_module & 0x20) != 0;
    out->in_ele = (out->raw_module & 0x10) != 0;
    out->in_eeprom = (out->raw_module & 0x08) != 0;
  }
  if (len > FAULT_BYTE_OUTDOOR) {
    out->raw_outdoor = buf[FAULT_BYTE_OUTDOOR];
    out->out_eeprom = (out->raw_outdoor & 0x40) != 0;
    out->out_coil_temp = (out->raw_outdoor & 0x20) != 0;
    out->out_gas_temp = (out->raw_outdoor & 0x10) != 0;
    out->out_temp = (out->raw_outdoor & 0x08) != 0;
  }
  if (len > FAULT_BYTE_PROTECT) {
    out->raw_protect = buf[FAULT_BYTE_PROTECT];
    out->over_temp = (out->raw_protect & 0x10) != 0;
  }
  out->any = (out->raw_indoor | out->raw_module | out->raw_outdoor |
              static_cast<uint8_t>(out->raw_protect & ~FAULT_NONFAULT_PROTECT)) != 0;
  out->valid = true;
  return true;
}

// Mirrors handle_devType_cmd_result (0x9b6f2194). NOT the envelope 9/10 "session token": stamping
// that killed the link on hardware (v10207).
bool devtype_from_reply(const uint8_t *reply, size_t n, uint8_t *hi, uint8_t *lo) {
  if (reply == nullptr || n <= 17 || reply[FRAME_CLASS_OFFSET] != CLASS_DEVTYPE)
    return false;
  if (hi != nullptr)
    *hi = reply[16];
  if (lo != nullptr)
    *lo = reply[17];
  return true;
}

// ---- FrameAssembler ----------------------------------------------------------------------------
void FrameAssembler::reset() {
  this->in_message_ = false;
  this->len_ = 0;
  this->prev_was_f4_ = false;
  this->expected_ = 0;
}

size_t FrameAssembler::feed(uint8_t b) {
  if (!this->in_message_) {
    if (b == FRAME_STX1) {
      this->buf_[0] = b;
      this->len_ = 1;
      this->in_message_ = true;
      this->prev_was_f4_ = true;
      this->expected_ = 0;
    }
    return 0;
  }
  // A doubled F4 is an escaped data byte: drop the second one.
  if (b == FRAME_STX1 && this->prev_was_f4_) {
    this->prev_was_f4_ = false;
    return 0;
  }
  this->prev_was_f4_ = b == FRAME_STX1;

  if (this->len_ >= RX_FRAME_MAX) {
    this->reset();
    return 0;
  }
  this->buf_[this->len_++] = b;
  size_t idx = this->len_ - 1;

  if (idx == 1 && this->buf_[1] != FRAME_STX2) {
    this->reset();
    return 0;
  }
  if (idx == 2 && this->buf_[2] != 0x01) {  // A/C -> module only
    this->reset();
    return 0;
  }
  if (idx == 4) {
    this->expected_ = static_cast<size_t>(this->buf_[4]) + FRAME_LEN_OVERHEAD;
    if (this->expected_ < 6 || this->expected_ > RX_FRAME_MAX) {
      this->reset();
      return 0;
    }
  }
  if (this->expected_ > 0 && idx == this->expected_ - 1) {
    size_t n = this->len_;
    bool tagged = this->buf_[n - 2] == FRAME_ETX1 && this->buf_[n - 1] == FRAME_ETX2;
    this->reset();
    return tagged ? n : 0;
  }
  return 0;
}

}  // namespace esphome::hisense_ac
