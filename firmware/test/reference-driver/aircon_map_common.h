/********************************************************************************
 * aircon_map_common.h
 *
 * The protocol tables the reference ESPHome map (esphome_aircon_map.h) builds on:
 * the fan ladder, the setpoint clamp and the running-state derivation. Pure
 * functions over plain integers, host-tested by firmware/test/test_reference_map.cpp.
 *
 * Frozen reference, see README.md in this directory. This is what is left of the
 * retired Matter builds' mapping header: the cluster-specific half (attribute
 * conversions, echo ledgers, capability gates) went with those builds and is in
 * the matter-final tag. The values below are hardware-confirmed.
 ********************************************************************************/
#pragma once
#include "hisense_rs485.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Fan --------------------------------------------------------------------
 * One table for the six discrete speeds (#58):
 *   raw     = status wind_status byte
 *   speed   = 1..6, quiet to high
 *   percent = the same step as a percentage
 *   cmd     = command-side HisenseFanSpeed enum
 * (auto = raw 0x01 -> speed 0 / HISENSE_FAN_AUTO; not a table row.) */
typedef struct {
  uint8_t raw;
  uint8_t speed;
  uint8_t percent;
  HisenseFanSpeed cmd;
} HisenseFanRow;

static const HisenseFanRow k_hisense_fan_table[] = {
    {0x02, 1, 10, HISENSE_FAN_QUIET}, {0x0A, 2, 25, HISENSE_FAN_LOW},      {0x0C, 3, 42, HISENSE_FAN_MED_LOW},
    {0x0E, 4, 58, HISENSE_FAN_MID},   {0x10, 5, 75, HISENSE_FAN_MED_HIGH}, {0x12, 6, 100, HISENSE_FAN_HIGH},
};
#define HISENSE_FAN_TABLE_LEN (sizeof(k_hisense_fan_table) / sizeof(k_hisense_fan_table[0]))

/* AUTO is raw 0x01, not a table row (the A/C picks the speed itself). */
#define HISENSE_FAN_RAW_AUTO 0x01

static inline const HisenseFanRow *hisense_fan_row_by_raw(uint8_t raw) {
  for (unsigned i = 0; i < HISENSE_FAN_TABLE_LEN; i++) {
    if (k_hisense_fan_table[i].raw == raw)
      return &k_hisense_fan_table[i];
  }
  return 0;
}

static inline const HisenseFanRow *hisense_fan_row_by_speed(uint8_t speed) {
  for (unsigned i = 0; i < HISENSE_FAN_TABLE_LEN; i++) {
    if (k_hisense_fan_table[i].speed == speed)
      return &k_hisense_fan_table[i];
  }
  return 0;
}

/* Speed step (1..6) -> the six discrete speeds (0/invalid -> auto).
 * 1=quiet 2=low 3=med-low 4=mid 5=med-high 6=high. */
static inline HisenseFanSpeed speed_to_hisense_fan(uint8_t speed) {
  const HisenseFanRow *r = hisense_fan_row_by_speed(speed);
  return r ? r->cmd : HISENSE_FAN_AUTO;
}

/* status wind_status byte -> speed step (1..6), 0 = auto/unknown. */
static inline uint8_t hisense_fan_raw_to_speed(uint8_t raw) {
  const HisenseFanRow *r = hisense_fan_row_by_raw(raw);
  return r ? r->speed : 0;  // auto/unknown
}

/* status wind_status byte -> command-side HisenseFanSpeed enum. Used to sync the
 * uplink command shadow to the A/C's ACTUAL fan on each downlink, so a later
 * single-attribute write rebuilds the combined frame from reality instead of a
 * stale shadow (which would clobber an out-of-band fan change). Inverse of the
 * confirmed status encoding (0x01 auto, 0x02 quiet, 0x0A..0x12 = low..high). */
static inline HisenseFanSpeed hisense_fan_raw_to_cmd(uint8_t raw) {
  if (raw == HISENSE_FAN_RAW_AUTO)
    return HISENSE_FAN_AUTO;  // 0x01 = genuine AUTO
  const HisenseFanRow *r = hisense_fan_row_by_raw(raw);
  return r ? r->cmd : HISENSE_FAN_NOCHANGE;  // unknown -> keep previous, don't force AUTO (#59)
}

/* power/mode/compressor-Hz -> running-state bitmap (Heat=1 Cool=2 Fan=4). Compressor idle
 * (Hz==0) in a heat/cool mode -> 0. */
static inline uint16_t hisense_to_running_state(bool power_on, HisenseMode mode, uint8_t comp_freq) {
  if (!power_on)
    return 0;
  if (mode == HISENSE_MODE_FAN)
    return 4;  // Fan
  if (comp_freq > 0)
    return (mode == HISENSE_MODE_HEAT) ? 1 : 2;  // Heat : Cool
  return 0;                                      // on but idle
}

/* ---- Setpoint ---------------------------------------------------------------
 * Whole degrees C, clamped to the W41H1's 16..32 range. */
static inline int hisense_clamp_setpoint_c(int c) {
  if (c < 16)
    c = 16;
  if (c > 32)
    c = 32;
  return c;
}

#ifdef __cplusplus
}
#endif
