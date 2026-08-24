/********************************************************************************
 * esphome_aircon_map.h
 *
 * PURE ESPHome <-> Hisense translation for the ESPHome climate glue, the sibling
 * of matter_aircon_map.h. Kept free of any ESPHome types (plain uint8_t) so it is
 * host-unit-testable without ESPHome installed -- see firmware/test/test_esphome_map.cpp.
 *
 * Only the ENUM mapping lives here. Everything else (the six-speed fan ladder, the
 * setpoint clamp, the running-state derivation) is protocol logic, not Matter logic,
 * so it is reused from matter_aircon_map.h rather than duplicated. Duplicating the
 * fan ladder is exactly the drift this project already designed sync-files.sh to avoid.
 *
 * The ESPHOME_CLIMATE_* constants mirror esphome::climate::ClimateMode / ClimateAction /
 * ClimateSwingMode. hisense_climate.cpp static_asserts every one of them against the real
 * enum, so an upstream renumber becomes a compile error here instead of a silently wrong
 * mode on the wire.
 ********************************************************************************/
#pragma once
#include "hisense_rs485.h"
#include "matter_aircon_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ClimateMode (mirrors esphome::climate::ClimateMode) --------------------- */
#define ESPHOME_CLIMATE_MODE_OFF       0
#define ESPHOME_CLIMATE_MODE_HEAT_COOL 1
#define ESPHOME_CLIMATE_MODE_COOL      2
#define ESPHOME_CLIMATE_MODE_HEAT      3
#define ESPHOME_CLIMATE_MODE_FAN_ONLY  4
#define ESPHOME_CLIMATE_MODE_DRY       5
#define ESPHOME_CLIMATE_MODE_AUTO      6

/* ---- ClimateAction (mirrors esphome::climate::ClimateAction) ----------------- */
#define ESPHOME_CLIMATE_ACTION_OFF     0
#define ESPHOME_CLIMATE_ACTION_COOLING 2
#define ESPHOME_CLIMATE_ACTION_HEATING 3
#define ESPHOME_CLIMATE_ACTION_IDLE    4
#define ESPHOME_CLIMATE_ACTION_DRYING  5
#define ESPHOME_CLIMATE_ACTION_FAN     6

/* ---- ClimateSwingMode (mirrors esphome::climate::ClimateSwingMode) ----------- */
#define ESPHOME_CLIMATE_SWING_OFF        0
#define ESPHOME_CLIMATE_SWING_BOTH       1
#define ESPHOME_CLIMATE_SWING_VERTICAL   2
#define ESPHOME_CLIMATE_SWING_HORIZONTAL 3

/* Hisense AUTO is the A/C's own auto mode, so it maps to HEAT_COOL. ESPHome's
 * CLIMATE_MODE_AUTO means "a schedule decides", which this A/C has no concept of.
 * OFF is not a HisenseMode (it rides the separate power frame), so it returns false. */
static inline bool esphome_mode_to_hisense(uint8_t climate_mode, HisenseMode *out)
{
    switch (climate_mode) {
    case ESPHOME_CLIMATE_MODE_HEAT_COOL: *out = HISENSE_MODE_AUTO; return true;
    case ESPHOME_CLIMATE_MODE_COOL:      *out = HISENSE_MODE_COOL; return true;
    case ESPHOME_CLIMATE_MODE_HEAT:      *out = HISENSE_MODE_HEAT; return true;
    case ESPHOME_CLIMATE_MODE_FAN_ONLY:  *out = HISENSE_MODE_FAN;  return true;
    case ESPHOME_CLIMATE_MODE_DRY:       *out = HISENSE_MODE_DRY;  return true;
    default: return false;   /* OFF / AUTO */
    }
}

static inline uint8_t hisense_mode_to_esphome(HisenseMode m)
{
    switch (m) {
    case HISENSE_MODE_FAN:  return ESPHOME_CLIMATE_MODE_FAN_ONLY;
    case HISENSE_MODE_HEAT: return ESPHOME_CLIMATE_MODE_HEAT;
    case HISENSE_MODE_COOL: return ESPHOME_CLIMATE_MODE_COOL;
    case HISENSE_MODE_DRY:  return ESPHOME_CLIMATE_MODE_DRY;
    case HISENSE_MODE_AUTO: return ESPHOME_CLIMATE_MODE_HEAT_COOL;
    default:                return ESPHOME_CLIMATE_MODE_COOL;
    }
}

/* hvac_action. Reuses the Matter running-state derivation (compressor Hz decides whether
 * a heat/cool mode is actually working or merely idle) and re-expresses its bitmap as the
 * ESPHome enum, so both firmwares agree on when the A/C counts as active. Dry has no
 * Matter running-state bit, so it is derived here from the mode. */
static inline uint8_t hisense_to_climate_action(bool power_on, HisenseMode mode, uint8_t comp_freq)
{
    uint16_t rs;
    if (!power_on) return ESPHOME_CLIMATE_ACTION_OFF;
    if (mode == HISENSE_MODE_DRY) return comp_freq > 0 ? ESPHOME_CLIMATE_ACTION_DRYING
                                                       : ESPHOME_CLIMATE_ACTION_IDLE;
    rs = hisense_to_running_state(power_on, mode, comp_freq);
    switch (rs) {
    case 1:  return ESPHOME_CLIMATE_ACTION_HEATING;
    case 2:  return ESPHOME_CLIMATE_ACTION_COOLING;
    case 4:  return ESPHOME_CLIMATE_ACTION_FAN;
    default: return ESPHOME_CLIMATE_ACTION_IDLE;   /* powered but not working */
    }
}

static inline uint8_t hisense_to_climate_swing(bool vswing_on, bool hswing_on)
{
    if (vswing_on && hswing_on) return ESPHOME_CLIMATE_SWING_BOTH;
    if (vswing_on)              return ESPHOME_CLIMATE_SWING_VERTICAL;
    if (hswing_on)              return ESPHOME_CLIMATE_SWING_HORIZONTAL;
    return ESPHOME_CLIMATE_SWING_OFF;
}

static inline void climate_swing_to_hisense(uint8_t swing, HisenseSwingMode *vswing,
                                            HisenseSwingMode *hswing)
{
    bool v = (swing == ESPHOME_CLIMATE_SWING_BOTH) || (swing == ESPHOME_CLIMATE_SWING_VERTICAL);
    bool h = (swing == ESPHOME_CLIMATE_SWING_BOTH) || (swing == ESPHOME_CLIMATE_SWING_HORIZONTAL);
    *vswing = v ? HISENSE_SWING_SWING : HISENSE_SWING_OFF;
    *hswing = h ? HISENSE_SWING_SWING : HISENSE_SWING_OFF;
}

/* ---- Fan -------------------------------------------------------------------
 * ESPHome addresses fan speeds by NAME, so the ladder is exposed as an index into
 * k_hisense_fan_table (speed 1..6) plus AUTO. Index 0 is AUTO; 1..6 are the table's
 * `speed` values in order, which keeps this a view of the shared table rather than a
 * second copy of it. The name strings live in the component (ESPHome wants
 * `const char *`), keyed by this index. */
#define ESPHOME_FAN_INDEX_AUTO 0
#define ESPHOME_FAN_INDEX_MAX  6   /* == HISENSE_FAN_TABLE_LEN */

/* ---- ClimateFanMode (mirrors esphome::climate::ClimateFanMode) ---------------
 * These matter because ESPHome resolves a fan-mode NAME against this enum before the custom
 * list: ClimateCall::set_fan_mode(const char *) does a case-insensitive match on the built-in
 * names first, so "Auto", "Quiet", "Low", "Medium" and "High" arrive as enum values and never
 * as custom modes. Only "Medium-low" and "Medium-high" stay custom. Glue that handles just the
 * custom path drops five of the seven speeds, which is exactly what shipped and was caught on
 * the bench. */
#define ESPHOME_CLIMATE_FAN_ON      0
#define ESPHOME_CLIMATE_FAN_OFF     1
#define ESPHOME_CLIMATE_FAN_AUTO    2
#define ESPHOME_CLIMATE_FAN_LOW     3
#define ESPHOME_CLIMATE_FAN_MEDIUM  4
#define ESPHOME_CLIMATE_FAN_HIGH    5
#define ESPHOME_CLIMATE_FAN_MIDDLE  6
#define ESPHOME_CLIMATE_FAN_FOCUS   7
#define ESPHOME_CLIMATE_FAN_DIFFUSE 8
#define ESPHOME_CLIMATE_FAN_QUIET   9

/* Built-in fan enum -> ladder index (0 = auto, 1..6 = the six speeds). MIDDLE and FOCUS take
 * the two intermediate steps that ESPHome's enum has no name for. The A/C's fan cannot idle,
 * so OFF and DIFFUSE fall back to auto rather than inventing a speed. */
static inline uint8_t esphome_fan_enum_to_index(uint8_t fan_mode)
{
    switch (fan_mode) {
    case ESPHOME_CLIMATE_FAN_QUIET:  return 1;
    case ESPHOME_CLIMATE_FAN_LOW:    return 2;
    case ESPHOME_CLIMATE_FAN_MIDDLE: return 3;
    case ESPHOME_CLIMATE_FAN_MEDIUM: return 4;
    case ESPHOME_CLIMATE_FAN_FOCUS:  return 5;
    case ESPHOME_CLIMATE_FAN_HIGH:
    case ESPHOME_CLIMATE_FAN_ON:     return 6;
    default:                         return ESPHOME_FAN_INDEX_AUTO;
    }
}

static inline HisenseFanSpeed esphome_fan_index_to_hisense(uint8_t idx)
{
    if (idx == ESPHOME_FAN_INDEX_AUTO) return HISENSE_FAN_AUTO;
    return speed_to_hisense_fan(idx);   /* speed 1..6 -> cmd enum; invalid -> AUTO */
}

/* status wind_status byte -> fan index (0 = auto, 1..6 = the ladder). An unknown raw
 * reports AUTO for display purposes only; the command shadow uses
 * hisense_fan_raw_to_cmd(), which returns NOCHANGE so a garbled frame cannot clobber
 * the user's fan setting (#59). */
static inline uint8_t hisense_fan_raw_to_esphome_index(uint8_t raw)
{
    return hisense_fan_raw_to_speed(raw);   /* 0 for auto/unknown, else 1..6 */
}

#ifdef __cplusplus
}
#endif
