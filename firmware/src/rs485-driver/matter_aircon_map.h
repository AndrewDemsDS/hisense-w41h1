/********************************************************************************
 * matter_aircon_map.h
 *
 * PURE Matter <-> Hisense translation for the room_air_conditioner glue. Kept
 * free of any CHIP/SDK types (plain uint8_t/int16_t) so it is host-unit-testable
 * without the Matter stack -- this is the "Matter side" QA surface (see
 * firmware/test/test_matter_map.cpp). matter_drivers.cpp includes this and does
 * only the CHIP attribute I/O around these functions.
 *
 * All Hisense encodings are hardware-confirmed (see INTEGRATION.md).
 ********************************************************************************/
#pragma once
#include "hisense_rs485.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Mode -------------------------------------------------------------------
 * Matter Thermostat::SystemModeEnum: Off=0, Auto=1, Cool=3, Heat=4, FanOnly=7, Dry=8
 * Off is not a HisenseMode (handled via the power frame), so returns false. */
static inline bool matter_mode_to_hisense(uint8_t matter_mode, HisenseMode *out)
{
    switch (matter_mode) {
    case 1: *out = HISENSE_MODE_AUTO; return true;
    case 3: *out = HISENSE_MODE_COOL; return true;
    case 4: *out = HISENSE_MODE_HEAT; return true;
    case 7: *out = HISENSE_MODE_FAN;  return true;
    case 8: *out = HISENSE_MODE_DRY;  return true;
    default: return false;
    }
}

static inline uint8_t hisense_mode_to_matter(HisenseMode m)
{
    switch (m) {
    case HISENSE_MODE_FAN:  return 7;
    case HISENSE_MODE_HEAT: return 4;
    case HISENSE_MODE_COOL: return 3;
    case HISENSE_MODE_DRY:  return 8;
    case HISENSE_MODE_AUTO: return 1;
    default:                return 3;
    }
}

/* ---- Fan --------------------------------------------------------------------
 * Single source of truth for the six discrete W41H1 fan speeds. Every raw<->X
 * conversion below indexes THIS table instead of duplicating the ladder:
 *   raw     = status wind_status byte (confirmed encoding)
 *   speed   = Matter SpeedCurrent/SpeedSetting (1..6)
 *   percent = Matter PercentCurrent (0..100)
 *   cmd     = command-side HisenseFanSpeed enum
 * (auto = raw 0x01 -> speed 0 / percent 0 / HISENSE_FAN_AUTO; not a table row.) */
typedef struct {
    uint8_t         raw;
    uint8_t         speed;
    uint8_t         percent;
    HisenseFanSpeed cmd;
} HisenseFanRow;

static const HisenseFanRow k_hisense_fan_table[] = {
    { 0x02, 1,  10, HISENSE_FAN_QUIET    },
    { 0x0A, 2,  25, HISENSE_FAN_LOW      },
    { 0x0C, 3,  42, HISENSE_FAN_MED_LOW  },
    { 0x0E, 4,  58, HISENSE_FAN_MID      },
    { 0x10, 5,  75, HISENSE_FAN_MED_HIGH },
    { 0x12, 6, 100, HISENSE_FAN_HIGH     },
};
#define HISENSE_FAN_TABLE_LEN (sizeof(k_hisense_fan_table)/sizeof(k_hisense_fan_table[0]))

/* AUTO is raw 0x01 -- NOT a table row (the A/C picks the speed itself). We still
 * report a non-zero PercentCurrent for it so HA renders the fan as "Auto / on"
 * instead of off (FanMode=Auto is authoritative; the percent is a UX placeholder,
 * HIL-tunable). Unknown raws stay 0% so they remain distinct from AUTO (#58). */
#define HISENSE_FAN_RAW_AUTO     0x01
#define HISENSE_FAN_AUTO_PERCENT 50

static inline const HisenseFanRow *hisense_fan_row_by_raw(uint8_t raw)
{
    for (unsigned i = 0; i < HISENSE_FAN_TABLE_LEN; i++)
        if (k_hisense_fan_table[i].raw == raw) return &k_hisense_fan_table[i];
    return 0;
}

static inline const HisenseFanRow *hisense_fan_row_by_speed(uint8_t speed)
{
    for (unsigned i = 0; i < HISENSE_FAN_TABLE_LEN; i++)
        if (k_hisense_fan_table[i].speed == speed) return &k_hisense_fan_table[i];
    return 0;
}

/* PercentSetting (0..100) -> the six W41H1 speeds (+auto/quiet). */
static inline HisenseFanSpeed percent_to_hisense_fan(uint8_t pct)
{
    if (pct == 0)  return HISENSE_FAN_AUTO;
    if (pct <= 16) return HISENSE_FAN_QUIET;
    if (pct <= 33) return HISENSE_FAN_LOW;
    if (pct <= 50) return HISENSE_FAN_MED_LOW;
    if (pct <= 67) return HISENSE_FAN_MID;
    if (pct <= 83) return HISENSE_FAN_MED_HIGH;
    return HISENSE_FAN_HIGH;
}

/* SpeedSetting (1..SpeedMax=6) -> the six discrete speeds (0/invalid -> auto).
 * 1=quiet 2=low 3=med-low 4=mid 5=med-high 6=high. */
static inline HisenseFanSpeed speed_to_hisense_fan(uint8_t speed)
{
    const HisenseFanRow *r = hisense_fan_row_by_speed(speed);
    return r ? r->cmd : HISENSE_FAN_AUTO;
}

/* FanControl FanMode preset (Matter FanModeEnum: Off=0 Low=1 Medium=2 High=3 On=4
 * Auto=5 Smart=6) -> a representative W41H1 speed. HA writes THIS attribute when the
 * user picks a preset from the fan card; without a handler for it the preset was
 * silently dropped (the six fine speeds still live on the % / SpeedSetting slider).
 * Coarse by necessity: Matter's enum has only Low/Medium/High/Auto, so the presets
 * fold onto low/mid/high while Auto/Off/Smart -> the A/C's auto fan. */
static inline HisenseFanSpeed fanmode_to_hisense_fan(uint8_t fanmode)
{
    switch (fanmode) {
    case 1: return HISENSE_FAN_LOW;    // Low
    case 2: return HISENSE_FAN_MID;    // Medium
    case 3: return HISENSE_FAN_HIGH;   // High
    case 4: return HISENSE_FAN_HIGH;   // On -> full speed
    default: return HISENSE_FAN_AUTO;  // Off/Auto/Smart -> auto (A/C fan can't idle off)
    }
}

/* status wind_status byte -> SpeedCurrent (1..6), 0 = auto/unknown. */
static inline uint8_t hisense_fan_raw_to_speed(uint8_t raw)
{
    const HisenseFanRow *r = hisense_fan_row_by_raw(raw);
    return r ? r->speed : 0;  // auto/unknown
}

/* status wind_status byte -> PercentCurrent (0..100). AUTO (0x01) reports a non-zero
 * placeholder so HA doesn't render the fan as off; unknown raws stay 0 (#58). */
static inline uint8_t hisense_fan_raw_to_percent(uint8_t raw)
{
    if (raw == HISENSE_FAN_RAW_AUTO) return HISENSE_FAN_AUTO_PERCENT;  // AUTO -> non-zero, not off
    const HisenseFanRow *r = hisense_fan_row_by_raw(raw);
    return r ? r->percent : 0;  // unknown -> 0
}

/* status wind_status byte -> command-side HisenseFanSpeed enum. Used to sync the
 * uplink command shadow to the A/C's ACTUAL fan on each downlink, so a later
 * single-attribute write rebuilds the combined frame from reality instead of a
 * stale shadow (which would clobber an out-of-band fan change). Inverse of the
 * confirmed status encoding (0x01 auto, 0x02 quiet, 0x0A..0x12 = low..high). */
static inline HisenseFanSpeed hisense_fan_raw_to_cmd(uint8_t raw)
{
    if (raw == HISENSE_FAN_RAW_AUTO) return HISENSE_FAN_AUTO;  // 0x01 = genuine AUTO
    const HisenseFanRow *r = hisense_fan_row_by_raw(raw);
    return r ? r->cmd : HISENSE_FAN_NOCHANGE;  // unknown -> keep previous, don't force AUTO (#59)
}

/* status wind_status byte + power -> FanControl FanMode preset (Matter FanModeEnum:
 * Off=0 Low=1 Medium=2 High=3 On=4 Auto=5 Smart=6). HA's fan entity shows the fan OFF
 * unless FanMode is set -- it reads FanMode, not PercentCurrent (docs/08). Off when the
 * unit is powered down; otherwise the six speeds fold into Auto/Low/Medium/High. */
static inline uint8_t hisense_fan_raw_to_fanmode(uint8_t raw, bool power_on)
{
    if (!power_on) return 0;                     // Off
    const HisenseFanRow *r = hisense_fan_row_by_raw(raw);
    if (!r) return 5;                            // 0x01 auto / unknown-but-running -> Auto
    return (uint8_t)((r->speed + 1) / 2);        // speeds 1..6 fold to Low/Medium/High (1/2/3)
}

/* power/mode/compressor-Hz -> Matter ThermostatRunningState bitmap (Heat=1 Cool=2 Fan=4).
 * HA maps this to hvac_action (heating/cooling/fan). Compressor idle (Hz==0) in a
 * heat/cool mode -> 0 (no action badge, which HA renders as None). (docs/08) */
static inline uint16_t hisense_to_running_state(bool power_on, HisenseMode mode, uint8_t comp_freq)
{
    if (!power_on) return 0;
    if (mode == HISENSE_MODE_FAN) return 4;             // Fan
    if (comp_freq > 0) return (mode == HISENSE_MODE_HEAT) ? 1 : 2;  // Heat : Cool
    return 0;                                            // on but idle
}

/* ---- Swing ------------------------------------------------------------------
 * FanControl RockSetting/RockSupport bitmap: bit0 = RockLeftRight (horizontal),
 * bit1 = RockUpDown (vertical), bit2 = RockRound. */
#define HISENSE_ROCK_LEFTRIGHT 0x01
#define HISENSE_ROCK_UPDOWN    0x02

static inline void rock_to_swing(uint8_t rock, bool *vswing, bool *hswing)
{
    *vswing = (rock & HISENSE_ROCK_UPDOWN)    != 0;
    *hswing = (rock & HISENSE_ROCK_LEFTRIGHT) != 0;
}

static inline uint8_t swing_to_rock(bool vswing, bool hswing)
{
    return (uint8_t)((vswing ? HISENSE_ROCK_UPDOWN : 0) | (hswing ? HISENSE_ROCK_LEFTRIGHT : 0));
}

/* ---- Setpoint ---------------------------------------------------------------
 * Matter int16 hundredths-of-a-degree-C -> whole degrees C, clamped 16..32.
 * Split into two reusable pieces (matter_drivers.cpp reuses them):
 *   matter_round_setpoint_c -> nearest whole degree, UNCLAMPED
 *   matter_clamp_setpoint_c -> clamp to the W41H1's 16..32 range */
static inline int matter_round_setpoint_c(int16_t centi)
{
    return (int)((centi + (centi >= 0 ? 50 : -50)) / 100);
}

static inline int matter_clamp_setpoint_c(int c)
{
    if (c < 16) c = 16;
    if (c > 32) c = 32;
    return c;
}

static inline int8_t matter_setpoint_to_c(int16_t hundredths)
{
    return (int8_t)matter_clamp_setpoint_c(matter_round_setpoint_c(hundredths));
}

/* ---- #72 runtime capability gating: pure decision layer ----------------------
 * The A/C reports its per-unit capabilities in the 0x66/ProductType reply
 * (HisenseFeatures). These predicates are the SINGLE SOURCE OF TRUTH for "should
 * this Matter surface be exposed on THIS unit?", shared by the ESP32 and AmebaZ2
 * wirings so the two paths cannot drift. Pure + host-testable; they encode only the
 * DECISION, never touch a Matter API.
 *
 * Design rule (docs/11 §5.1): gate on the VALID tier only, and be PERMISSIVE when
 * capabilities are not yet known. An absent or not-yet-parsed reply (`valid == false`,
 * e.g. the head unit is briefly silent or the bus has not answered yet) means
 * "unknown", NOT "unsupported", so we keep the surface rather than hide a real
 * capability. All four gated flags (cool_heat, power_save, fan_mute, power_display)
 * live in the valid tier, so NONE of these consults ext_valid (bit30). */

/* Eco switch (ep3) <- ac_power_save. Returns true = expose on this unit. */
static inline bool matter_gate_eco(const HisenseFeatures *f)
{
    return (f == NULL) || !f->valid || f->power_save;
}

/* Quiet switch (ep4) <- ac_fan_mute. */
static inline bool matter_gate_quiet(const HisenseFeatures *f)
{
    return (f == NULL) || !f->valid || f->fan_mute;
}

/* Display switch (ep9) <- ac_power_display (2-bit code; any non-zero = present). */
static inline bool matter_gate_display(const HisenseFeatures *f)
{
    return (f == NULL) || !f->valid || (f->power_display != 0);
}

/* Thermostat FeatureMap for THIS unit: a heat-pump unit gets Heat+Cool+Auto (35);
 * a cooling-only unit (cool_heat absent) gets Cooling-only (2) so HA never offers a
 * Heat/Auto mode it cannot do. Permissive on unknown (valid==false) -> keep the full
 * 35 default. Matter Thermostat FeatureMap bits: Heating=0x01, Cooling=0x02,
 * AutoMode=0x20 (0x23 = 35). */
#define MATTER_THERMOSTAT_FEATUREMAP_FULL  0x23u   /* 35: Heating + Cooling + AutoMode */
#define MATTER_THERMOSTAT_FEATUREMAP_COOL  0x02u   /* 2:  Cooling only */
static inline uint32_t matter_thermostat_featuremap(const HisenseFeatures *f)
{
    if (f == NULL || !f->valid) return MATTER_THERMOSTAT_FEATUREMAP_FULL;
    return f->cool_heat ? MATTER_THERMOSTAT_FEATUREMAP_FULL
                        : MATTER_THERMOSTAT_FEATUREMAP_COOL;
}

#ifdef __cplusplus
}
#endif
