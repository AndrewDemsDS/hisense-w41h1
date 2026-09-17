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
#include <string.h>
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
 * names first, so "auto", "low", "medium" and "high" arrive as enum values and never as custom
 * modes. Only "medium_low" and "medium_high" stay custom. Glue that handles just the
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

/* Ladder index -> the index the climate entity publishes. Quiet (1) is not offered as a fan mode:
 * the A/C only reaches it through the mute flag, which is the `quiet` preset, and the Matter path
 * (hisense-unified-ac) reads that step back as low. Publishing it as low keeps both firmwares
 * showing the same fan mode for the same unit state, and never shows a mode outside the advertised
 * list. */
static inline uint8_t esphome_fan_published_index(uint8_t idx)
{
    return idx == 1 ? 2 : idx;
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

/* ---- Setpoint unit (#117) ---------------------------------------------------
 * ESPHome always speaks Celsius, but the A/C reads and reports the setpoint byte in its
 * PANEL's unit. The Matter builds learned this on hardware 2026-07-19 (Celsius 23 sent to an
 * F panel made it target 23 F); the ESPHome glue shipped without it and hit the same thing.
 *
 * Clamp in Celsius, then convert, and tell the builder which unit it holds so it applies the
 * matching range check. Returns the clamped Celsius value for the entity to publish. */
static inline int esphome_setpoint_to_cmd(int wanted_c, bool panel_f, HisenseCommand *cmd)
{
    int8_t c = (int8_t) matter_clamp_setpoint_c(wanted_c);
    cmd->fahrenheit = panel_f;
    cmd->setpoint   = panel_f ? hisense_c_to_f(c) : c;
    return c;
}

/* Status -> command shadow, validated in the wire unit. Copying setpoint_c verbatim both
 * dropped the unit and let an out-of-range report (a 24 F panel decodes to -4 C) poison the
 * shadow, after which hisense_build_command() rejected every Cool/Heat/Auto frame while
 * Fan-only, which strips the setpoint, still went through. Returns false and leaves the
 * shadow alone when the report is outside what the builder accepts. */
static inline bool esphome_sync_shadow_setpoint(int8_t setpoint_c, bool temp_unit_f, HisenseCommand *cmd)
{
    int8_t wire;
    if (!hisense_shadow_setpoint_from_status(setpoint_c, temp_unit_f, &wire)) return false;
    cmd->setpoint   = wire;
    cmd->fahrenheit = temp_unit_f;
    return true;
}

/* ---- Eco / turbo shadow --------------------------------------------------------
 * Ported from the esp32 Matter sync (app_main.cpp). The shadow's feature byte rides every
 * combined frame, so it must track what the A/C reports; otherwise a Turbo set from HA is
 * re-asserted on each later mode/setpoint/fan change after the remote cleared it. Eco wins
 * when both flags read set, matching that sync. */
static inline HisenseFeature hisense_feature_from_status(bool eco_on, bool turbo_on)
{
    if (eco_on)   return HISENSE_FEATURE_ECO;
    if (turbo_on) return HISENSE_FEATURE_TURBO;
    return HISENSE_FEATURE_NONE;
}

/* ECO_OFF (byte33 0x10) is a one-shot clear, not a state: after it is sent the shadow goes
 * back to neutral, as the Matter apply_eco() does. Left in place it rode every later frame. */
static inline HisenseFeature esphome_feature_after_send(HisenseFeature sent)
{
    return sent == HISENSE_FEATURE_ECO_OFF ? HISENSE_FEATURE_NONE : sent;
}

/* ---- Special modes as climate presets ------------------------------------------
 * Eco, quiet, turbo and the sleep profile exposed as ONE preset value on the climate entity,
 * with exactly the names the hisense-unified-ac wrapper gives the Matter builds (its
 * const.py and _preset_plan). Same names on both paths is the point: a climate group only
 * syncs attributes its members agree on, so a unit that spells a preset differently drops
 * out of preset sync.
 *
 * Which combinations exist is hardware, measured on a live A/C for the wrapper:
 *   eco + each sleep profile   coexist, so each pair is its own preset
 *   quiet + sleep              do NOT: whichever is commanded second drops the first
 *   turbo                      combines with nothing (shares byte33 with eco, and maxes
 *                              power while quiet and sleep reduce it)
 *   eco + quiet                coexist (mute is independent of byte33)
 *
 * The first two names ride ESPHome's built-in preset enum (NONE, ECO); every other name is a
 * custom preset. None of the custom names may equal a built-in ("sleep", "boost", ...),
 * because ClimateCall::set_preset(const char *) converts a built-in name to the enum before
 * the custom list is consulted, the same trap the fan modes hit. */
typedef struct {
    bool    eco;
    bool    turbo;
    bool    mute;
    uint8_t sleep;   /* profile 0 = off, 1 General, 2 Old, 3 Young, 4 Kids (status byte17 / 2) */
} HisenseSpecialState;

typedef struct {
    const char *name;
    bool        eco;
    bool        turbo;
    bool        mute;
    uint8_t     sleep;
} EsphomePresetRow;

#define ESPHOME_PRESET_NONE 0
#define ESPHOME_PRESET_ECO  1
static const EsphomePresetRow k_esphome_presets[] = {
    { "none",              false, false, false, 0 },
    { "eco",               true,  false, false, 0 },
    { "quiet",             false, false, true,  0 },
    { "turbo",             false, true,  false, 0 },
    { "eco_quiet",         true,  false, true,  0 },
    { "sleep_general",     false, false, false, 1 },
    { "sleep_old",         false, false, false, 2 },
    { "sleep_young",       false, false, false, 3 },
    { "sleep_kids",        false, false, false, 4 },
    { "eco_sleep_general", true,  false, false, 1 },
    { "eco_sleep_old",     true,  false, false, 2 },
    { "eco_sleep_young",   true,  false, false, 3 },
    { "eco_sleep_kids",    true,  false, false, 4 },
};
#define ESPHOME_PRESET_COUNT (sizeof(k_esphome_presets)/sizeof(k_esphome_presets[0]))
/* Rows from here on are custom presets; below it they are ESPHome built-ins. */
#define ESPHOME_PRESET_FIRST_CUSTOM 2

/* Which special modes the unit has, from YAML (supports_eco / _quiet / _turbo / _sleep). A
 * preset is offered only when every mode it needs is supported. */
#define ESPHOME_SUPPORT_ECO   0x01
#define ESPHOME_SUPPORT_QUIET 0x02
#define ESPHOME_SUPPORT_TURBO 0x04
#define ESPHOME_SUPPORT_SLEEP 0x08

static inline bool esphome_preset_available(uint8_t idx, uint8_t support)
{
    const EsphomePresetRow *r;
    if (idx >= ESPHOME_PRESET_COUNT) return false;
    r = &k_esphome_presets[idx];
    if (r->eco   && !(support & ESPHOME_SUPPORT_ECO))   return false;
    if (r->mute  && !(support & ESPHOME_SUPPORT_QUIET)) return false;
    if (r->turbo && !(support & ESPHOME_SUPPORT_TURBO)) return false;
    if (r->sleep && !(support & ESPHOME_SUPPORT_SLEEP)) return false;
    return true;
}

/* Name -> row index, or -1. Exact match (HA passes the advertised string back verbatim). */
static inline int esphome_preset_index(const char *name)
{
    unsigned i;
    if (!name) return -1;
    for (i = 0; i < ESPHOME_PRESET_COUNT; i++)
        if (strcmp(k_esphome_presets[i].name, name) == 0) return (int) i;
    return -1;
}

static inline HisenseSpecialState hisense_special_from_status(bool eco_on, bool turbo_on,
                                                              bool mute_on, uint8_t sleep_raw)
{
    HisenseSpecialState s;
    s.eco   = eco_on;
    s.turbo = turbo_on;
    s.mute  = mute_on;
    s.sleep = (uint8_t) (sleep_raw / 2);   /* status carries profile * 2 (RE doc 03, byte 17) */
    return s;
}

/* Name the preset the unit is in. An exact row wins. With no exact match an arbitration is in
 * flight (the A/C drops one of an illegal pair a second or two after the command), so report
 * the strongest thing really on: turbo, eco, quiet, then the sleep profile. Same order as the
 * wrapper's _detect_preset, so both paths name a transient state identically. A result the
 * unit does not offer degrades to none, since HA rejects a preset outside the advertised list. */
static inline uint8_t esphome_preset_detect(const HisenseSpecialState *s, uint8_t support)
{
    unsigned i;
    uint8_t guess = ESPHOME_PRESET_NONE;
    for (i = 0; i < ESPHOME_PRESET_COUNT; i++) {
        const EsphomePresetRow *r = &k_esphome_presets[i];
        if (r->eco == s->eco && r->turbo == s->turbo && r->mute == s->mute && r->sleep == s->sleep)
            return esphome_preset_available((uint8_t) i, support) ? (uint8_t) i : ESPHOME_PRESET_NONE;
    }
    if (s->turbo)               guess = 3;                          /* turbo */
    else if (s->eco)            guess = ESPHOME_PRESET_ECO;
    else if (s->mute)           guess = 2;                          /* quiet */
    else if (s->sleep >= 1 && s->sleep <= 4) guess = (uint8_t) (4 + s->sleep);   /* sleep_* */
    return esphome_preset_available(guess, support) ? guess : ESPHOME_PRESET_NONE;
}

/* One bus write that changes a special mode. FEATURE carries a HisenseFeature for the byte33
 * enum (rides the combined frame), MUTE is 0/1 (its own frame), SLEEP is the profile (its own
 * frame). */
#define HISENSE_SPECIAL_OP_FEATURE 0
#define HISENSE_SPECIAL_OP_MUTE    1
#define HISENSE_SPECIAL_OP_SLEEP   2
typedef struct {
    uint8_t kind;
    uint8_t value;
} HisenseSpecialOp;
#define ESPHOME_PRESET_PLAN_MAX 5

/* The A/C debounces special-mode commands: one that arrives too soon after the previous is
 * swallowed outright. Measured on a live A/C for the wrapper by commanding eco then quiet at
 * varying gaps: 6 s failed repeatedly, 8 s and 12 s both engaged. 10 s is 8 with margin. The
 * hub spaces every op in a plan (and every special switch/select write) by this much. */
#define HISENSE_SPECIAL_SETTLE_MS 10000

/* Ordered list of writes taking the unit from `now` to preset `target`. Only writes that change
 * something are emitted, since each one after the first costs a settle wait. Returns the count.
 *
 * Order, and why:
 *   1. sleep OFF first when the target has no profile. Quiet and sleep drop whichever came first,
 *      so a sleep clear sent after mute-on could take quiet down with it.
 *   2. clears: byte33 back to neutral (ECO_OFF when eco was on, since NONE clears turbo but not
 *      eco), then mute off.
 *   3. sets: byte33 to ECO or TURBO (one enum, so switching eco <-> turbo is a single write),
 *      then mute on.
 *   4. sleep profile LAST. Eco rides the combined frame, which re-asserts the fan while a sleep
 *      profile owns it: measured, eco then sleep keeps both, sleep then eco loses the profile.
 *      So any byte33 write with a profile wanted re-sends the profile after it, even when the
 *      profile itself is unchanged. */
static inline uint8_t esphome_preset_plan(const HisenseSpecialState *now, uint8_t target,
                                          HisenseSpecialOp out[ESPHOME_PRESET_PLAN_MAX])
{
    const EsphomePresetRow *t;
    uint8_t n = 0;
    bool feature_change, feature_set;
    if (target >= ESPHOME_PRESET_COUNT) return 0;
    t = &k_esphome_presets[target];
    feature_change = (t->eco != now->eco) || (t->turbo != now->turbo);
    feature_set = feature_change && (t->eco || t->turbo);

    if (t->sleep == 0 && now->sleep != 0) {
        out[n].kind = HISENSE_SPECIAL_OP_SLEEP; out[n].value = 0; n++;
    }
    if (feature_change && !feature_set) {
        out[n].kind = HISENSE_SPECIAL_OP_FEATURE;
        out[n].value = (uint8_t) (now->eco ? HISENSE_FEATURE_ECO_OFF : HISENSE_FEATURE_NONE);
        n++;
    }
    if (now->mute && !t->mute) {
        out[n].kind = HISENSE_SPECIAL_OP_MUTE; out[n].value = 0; n++;
    }
    if (feature_set) {
        out[n].kind = HISENSE_SPECIAL_OP_FEATURE;
        out[n].value = (uint8_t) (t->eco ? HISENSE_FEATURE_ECO : HISENSE_FEATURE_TURBO);
        n++;
    }
    if (t->mute && !now->mute) {
        out[n].kind = HISENSE_SPECIAL_OP_MUTE; out[n].value = 1; n++;
    }
    if (t->sleep != 0 && (t->sleep != now->sleep || feature_change)) {
        out[n].kind = HISENSE_SPECIAL_OP_SLEEP; out[n].value = t->sleep; n++;
    }
    return n;
}

/* What the unit is expected to report once `op` lands. Encoding facts only (byte33 is one
 * enum, so ECO clears turbo and TURBO clears eco); the A/C's own cross-mode drops are left to
 * the next status frame to reveal. Used to plan a second preset change before the first one's
 * readback has arrived. */
static inline void hisense_special_apply(HisenseSpecialState *s, const HisenseSpecialOp *op)
{
    switch (op->kind) {
    case HISENSE_SPECIAL_OP_FEATURE:
        switch ((HisenseFeature) op->value) {
        case HISENSE_FEATURE_ECO:     s->eco = true;  s->turbo = false; break;
        case HISENSE_FEATURE_TURBO:   s->turbo = true; s->eco = false;  break;
        case HISENSE_FEATURE_ECO_OFF: s->eco = false; break;
        default:                      s->turbo = false; break;   /* NONE: turbo-off */
        }
        break;
    case HISENSE_SPECIAL_OP_MUTE:  s->mute = op->value != 0; break;
    case HISENSE_SPECIAL_OP_SLEEP: s->sleep = op->value; break;
    default: break;
    }
}

/* The fan index (see ESPHOME_FAN_INDEX_*) a special mode pins the A/C to, or -1 when the fan is
 * free. Turbo forces high, quiet and sleep force their low profile (firmware/docs/05 "Special
 * functions"); the A/C's order is turbo, then quiet, then sleep. While pinned, any other speed
 * is overwritten by the next status frame about a second later, so the glue refuses it instead
 * of acknowledging a change that undoes itself (the wrapper does the same). Quiet reads back as
 * the ladder's lowest real step on the Matter path, so it is reported as low (index 2) here. */
static inline int esphome_forced_fan_index(const HisenseSpecialState *s)
{
    if (s->turbo) return 6;
    if (s->mute)  return 2;
    if (s->sleep) return 2;
    return -1;
}

/* Whether a fan request can stick. Quiet and sleep pin the A/C's quiet/low profile, which the
 * fan byte reports as either of the two bottom ladder steps, so both count as agreeing. */
static inline bool esphome_fan_request_allowed(const HisenseSpecialState *s, uint8_t wanted_idx)
{
    int pin = esphome_forced_fan_index(s);
    if (pin < 0) return true;
    if (pin == 2) return wanted_idx == 1 || wanted_idx == 2;
    return wanted_idx == (uint8_t) pin;
}

#ifdef __cplusplus
}
#endif
