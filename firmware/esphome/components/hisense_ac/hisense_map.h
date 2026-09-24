#pragma once
// Pure Hisense <-> ESPHome translation: climate enums, the six-speed fan ladder, special-mode
// presets and the power estimate.
//
// ESPHome-style port of firmware/src/rs485-driver/esphome_aircon_map.h plus the parts of
// matter_aircon_map.h and power_estimate.h that it uses. Kept equal to them by
// firmware/test/test_esphome_codec_parity.cpp. Plain integers only, no ESPHome types, so the host
// tests compile it without ESPHome; the climate constants below mirror esphome::climate's enums and
// hisense_climate.cpp static_asserts each one against the real enum.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hisense_protocol.h"

namespace esphome::hisense_ac {

// ---- Mirrors of esphome::climate enums ---------------------------------------------------------
static constexpr uint8_t CLIMATE_MODE_OFF_VALUE = 0;
static constexpr uint8_t CLIMATE_MODE_HEAT_COOL_VALUE = 1;
static constexpr uint8_t CLIMATE_MODE_COOL_VALUE = 2;
static constexpr uint8_t CLIMATE_MODE_HEAT_VALUE = 3;
static constexpr uint8_t CLIMATE_MODE_FAN_ONLY_VALUE = 4;
static constexpr uint8_t CLIMATE_MODE_DRY_VALUE = 5;
static constexpr uint8_t CLIMATE_MODE_AUTO_VALUE = 6;

static constexpr uint8_t CLIMATE_ACTION_OFF_VALUE = 0;
static constexpr uint8_t CLIMATE_ACTION_COOLING_VALUE = 2;
static constexpr uint8_t CLIMATE_ACTION_HEATING_VALUE = 3;
static constexpr uint8_t CLIMATE_ACTION_IDLE_VALUE = 4;
static constexpr uint8_t CLIMATE_ACTION_DRYING_VALUE = 5;
static constexpr uint8_t CLIMATE_ACTION_FAN_VALUE = 6;

static constexpr uint8_t CLIMATE_SWING_OFF_VALUE = 0;
static constexpr uint8_t CLIMATE_SWING_BOTH_VALUE = 1;
static constexpr uint8_t CLIMATE_SWING_VERTICAL_VALUE = 2;
static constexpr uint8_t CLIMATE_SWING_HORIZONTAL_VALUE = 3;

// ClimateCall::set_fan_mode(const char *) resolves built-in names to this enum before the custom
// list, so "auto", "low", "medium" and "high" never arrive as custom modes.
static constexpr uint8_t CLIMATE_FAN_ON_VALUE = 0;
static constexpr uint8_t CLIMATE_FAN_OFF_VALUE = 1;
static constexpr uint8_t CLIMATE_FAN_AUTO_VALUE = 2;
static constexpr uint8_t CLIMATE_FAN_LOW_VALUE = 3;
static constexpr uint8_t CLIMATE_FAN_MEDIUM_VALUE = 4;
static constexpr uint8_t CLIMATE_FAN_HIGH_VALUE = 5;
static constexpr uint8_t CLIMATE_FAN_MIDDLE_VALUE = 6;
static constexpr uint8_t CLIMATE_FAN_FOCUS_VALUE = 7;
static constexpr uint8_t CLIMATE_FAN_DIFFUSE_VALUE = 8;
static constexpr uint8_t CLIMATE_FAN_QUIET_VALUE = 9;

// ---- Mode --------------------------------------------------------------------------------------
// Hisense AUTO is the A/C's own auto mode, so it is HEAT_COOL; ESPHome's AUTO means "a schedule
// decides", which this unit has no notion of. OFF is not a Mode (power has its own frame).
inline bool climate_mode_to_hisense(uint8_t climate_mode, Mode *out) {
  switch (climate_mode) {
    case CLIMATE_MODE_HEAT_COOL_VALUE:
      *out = MODE_AUTO;
      return true;
    case CLIMATE_MODE_COOL_VALUE:
      *out = MODE_COOL;
      return true;
    case CLIMATE_MODE_HEAT_VALUE:
      *out = MODE_HEAT;
      return true;
    case CLIMATE_MODE_FAN_ONLY_VALUE:
      *out = MODE_FAN;
      return true;
    case CLIMATE_MODE_DRY_VALUE:
      *out = MODE_DRY;
      return true;
    default:
      return false;
  }
}

inline uint8_t hisense_mode_to_climate(Mode m) {
  switch (m) {
    case MODE_FAN:
      return CLIMATE_MODE_FAN_ONLY_VALUE;
    case MODE_HEAT:
      return CLIMATE_MODE_HEAT_VALUE;
    case MODE_DRY:
      return CLIMATE_MODE_DRY_VALUE;
    case MODE_AUTO:
      return CLIMATE_MODE_HEAT_COOL_VALUE;
    case MODE_COOL:
    default:
      return CLIMATE_MODE_COOL_VALUE;
  }
}

// Matter ThermostatRunningState bitmap (Heat = 1, Cool = 2, Fan = 4). Compressor Hz decides whether a
// heat/cool mode is working or idle. Shared with the Matter builds so both agree on "active".
inline uint16_t running_state(bool power_on, Mode mode, uint8_t comp_freq) {
  if (!power_on)
    return 0;
  if (mode == MODE_FAN)
    return 4;
  if (comp_freq > 0)
    return (mode == MODE_HEAT) ? 1 : 2;
  return 0;
}

inline uint8_t climate_action(bool power_on, Mode mode, uint8_t comp_freq) {
  if (!power_on)
    return CLIMATE_ACTION_OFF_VALUE;
  if (mode == MODE_DRY)
    return comp_freq > 0 ? CLIMATE_ACTION_DRYING_VALUE : CLIMATE_ACTION_IDLE_VALUE;
  switch (running_state(power_on, mode, comp_freq)) {
    case 1:
      return CLIMATE_ACTION_HEATING_VALUE;
    case 2:
      return CLIMATE_ACTION_COOLING_VALUE;
    case 4:
      return CLIMATE_ACTION_FAN_VALUE;
    default:
      return CLIMATE_ACTION_IDLE_VALUE;
  }
}

// ---- Swing -------------------------------------------------------------------------------------
inline uint8_t climate_swing(bool vswing_on, bool hswing_on) {
  if (vswing_on && hswing_on)
    return CLIMATE_SWING_BOTH_VALUE;
  if (vswing_on)
    return CLIMATE_SWING_VERTICAL_VALUE;
  if (hswing_on)
    return CLIMATE_SWING_HORIZONTAL_VALUE;
  return CLIMATE_SWING_OFF_VALUE;
}

inline void climate_swing_to_hisense(uint8_t swing, SwingMode *vswing, SwingMode *hswing) {
  bool v = swing == CLIMATE_SWING_BOTH_VALUE || swing == CLIMATE_SWING_VERTICAL_VALUE;
  bool h = swing == CLIMATE_SWING_BOTH_VALUE || swing == CLIMATE_SWING_HORIZONTAL_VALUE;
  *vswing = v ? SWING_MODE_SWING : SWING_MODE_OFF;
  *hswing = h ? SWING_MODE_SWING : SWING_MODE_OFF;
}

// ---- Fan ladder --------------------------------------------------------------------------------
// The six discrete W41H1 speeds. raw = status byte 16, speed = 1..6, percent = Matter
// PercentCurrent, cmd = command enum. Auto (raw 0x01) is not a row: the A/C picks the speed.
struct FanRow {
  uint8_t raw;
  uint8_t speed;
  uint8_t percent;
  FanSpeed cmd;
};
static constexpr FanRow FAN_TABLE[] = {
    {0x02, 1, 10, FAN_SPEED_QUIET}, {0x0A, 2, 25, FAN_SPEED_LOW},      {0x0C, 3, 42, FAN_SPEED_MED_LOW},
    {0x0E, 4, 58, FAN_SPEED_MID},   {0x10, 5, 75, FAN_SPEED_MED_HIGH}, {0x12, 6, 100, FAN_SPEED_HIGH},
};
static constexpr size_t FAN_TABLE_LEN = sizeof(FAN_TABLE) / sizeof(FAN_TABLE[0]);
static constexpr uint8_t FAN_RAW_AUTO = 0x01;

// Fan index as the climate entity sees it: 0 = auto, 1..6 = the ladder's speeds.
static constexpr uint8_t FAN_INDEX_AUTO = 0;
static constexpr uint8_t FAN_INDEX_MAX = 6;

inline const FanRow *fan_row_by_raw(uint8_t raw) {
  for (const auto &row : FAN_TABLE) {
    if (row.raw == raw)
      return &row;
  }
  return nullptr;
}

inline const FanRow *fan_row_by_speed(uint8_t speed) {
  for (const auto &row : FAN_TABLE) {
    if (row.speed == speed)
      return &row;
  }
  return nullptr;
}

// Status byte 16 -> ladder index, 0 for auto or unknown (display only).
inline uint8_t fan_raw_to_index(uint8_t raw) {
  const FanRow *r = fan_row_by_raw(raw);
  return r != nullptr ? r->speed : 0;
}

// Status byte 16 -> command enum, for syncing the command shadow. Unknown -> NOCHANGE, so a garbled
// frame cannot clobber the user's fan setting (#59).
inline FanSpeed fan_raw_to_cmd(uint8_t raw) {
  if (raw == FAN_RAW_AUTO)
    return FAN_SPEED_AUTO;
  const FanRow *r = fan_row_by_raw(raw);
  return r != nullptr ? r->cmd : FAN_SPEED_NOCHANGE;
}

inline FanSpeed fan_index_to_hisense(uint8_t idx) {
  if (idx == FAN_INDEX_AUTO)
    return FAN_SPEED_AUTO;
  const FanRow *r = fan_row_by_speed(idx);
  return r != nullptr ? r->cmd : FAN_SPEED_AUTO;
}

// Built-in fan enum -> ladder index. MIDDLE and FOCUS take the two steps ESPHome has no name for.
// The fan cannot idle, so OFF and DIFFUSE fall back to auto.
inline uint8_t climate_fan_to_index(uint8_t fan_mode) {
  switch (fan_mode) {
    case CLIMATE_FAN_QUIET_VALUE:
      return 1;
    case CLIMATE_FAN_LOW_VALUE:
      return 2;
    case CLIMATE_FAN_MIDDLE_VALUE:
      return 3;
    case CLIMATE_FAN_MEDIUM_VALUE:
      return 4;
    case CLIMATE_FAN_FOCUS_VALUE:
      return 5;
    case CLIMATE_FAN_HIGH_VALUE:
    case CLIMATE_FAN_ON_VALUE:
      return 6;
    default:
      return FAN_INDEX_AUTO;
  }
}

// Quiet (index 1) is only reachable through the mute flag, i.e. the `quiet` preset, and the Matter
// path reads it back as low, so it is published as low.
inline uint8_t fan_published_index(uint8_t idx) { return idx == 1 ? 2 : idx; }

// ---- Setpoint ----------------------------------------------------------------------------------
inline int clamp_setpoint_c(int c) {
  if (c < SETPOINT_MIN_C)
    return SETPOINT_MIN_C;
  if (c > SETPOINT_MAX_C)
    return SETPOINT_MAX_C;
  return c;
}

// ESPHome speaks Celsius but the A/C reads the byte in its panel unit (#117). Clamp in C, convert,
// and tell the builder which unit it holds. Returns the clamped Celsius value to publish.
inline int setpoint_to_cmd(int wanted_c, bool panel_f, AcCommand *cmd) {
  auto c = static_cast<int8_t>(clamp_setpoint_c(wanted_c));
  cmd->fahrenheit = panel_f;
  cmd->setpoint = panel_f ? c_to_f(c) : c;
  return c;
}

// Status -> command shadow, validated in the wire unit. False leaves the shadow alone.
inline bool sync_shadow_setpoint(int8_t setpoint_c, bool temp_unit_f, AcCommand *cmd) {
  int8_t wire;
  if (!shadow_setpoint_from_status(setpoint_c, temp_unit_f, &wire))
    return false;
  cmd->setpoint = wire;
  cmd->fahrenheit = temp_unit_f;
  return true;
}

// ---- Eco / turbo -------------------------------------------------------------------------------
// The shadow's byte 33 rides every combined frame, so it tracks what the A/C reports. Eco wins.
inline Feature feature_from_status(bool eco_on, bool turbo_on) {
  if (eco_on)
    return FEATURE_ECO;
  if (turbo_on)
    return FEATURE_TURBO;
  return FEATURE_NONE;
}

// ECO_OFF is a one-shot clear; after it goes out the shadow returns to neutral.
inline Feature feature_after_send(Feature sent) { return sent == FEATURE_ECO_OFF ? FEATURE_NONE : sent; }

// ---- Special modes as presets ------------------------------------------------------------------
// Same names the hisense-unified-ac wrapper gives the Matter builds, so a climate group syncs them.
// Hardware (measured for the wrapper): eco + each sleep profile coexist; quiet + sleep do not;
// turbo combines with nothing; eco + quiet coexist. The first two rows are ESPHome built-ins
// (NONE, ECO); no custom name may equal a built-in, since set_preset(const char *) converts those.
struct SpecialState {
  bool eco{false};
  bool turbo{false};
  bool mute{false};
  uint8_t sleep{0};  // profile 0 = off, 1 General, 2 Old, 3 Young, 4 Kids (status byte 17 / 2)
};

struct PresetRow {
  const char *name;
  bool eco;
  bool turbo;
  bool mute;
  uint8_t sleep;
};

static constexpr PresetRow PRESETS[] = {
    {"none", false, false, false, 0},          {"eco", true, false, false, 0},
    {"quiet", false, false, true, 0},          {"turbo", false, true, false, 0},
    {"eco_quiet", true, false, true, 0},       {"sleep_general", false, false, false, 1},
    {"sleep_old", false, false, false, 2},     {"sleep_young", false, false, false, 3},
    {"sleep_kids", false, false, false, 4},    {"eco_sleep_general", true, false, false, 1},
    {"eco_sleep_old", true, false, false, 2},  {"eco_sleep_young", true, false, false, 3},
    {"eco_sleep_kids", true, false, false, 4},
};
static constexpr size_t PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);
static constexpr uint8_t PRESET_NONE = 0;
static constexpr uint8_t PRESET_ECO = 1;
static constexpr uint8_t PRESET_QUIET = 2;
static constexpr uint8_t PRESET_TURBO = 3;
static constexpr uint8_t PRESET_FIRST_SLEEP = 4;  // sleep_general is PRESET_FIRST_SLEEP + 1
static constexpr uint8_t PRESET_FIRST_CUSTOM = 2;

// Special modes the unit has, from YAML (supports_eco / _quiet / _turbo / _sleep).
static constexpr uint8_t SUPPORT_ECO = 0x01;
static constexpr uint8_t SUPPORT_QUIET = 0x02;
static constexpr uint8_t SUPPORT_TURBO = 0x04;
static constexpr uint8_t SUPPORT_SLEEP = 0x08;

inline bool preset_available(size_t idx, uint8_t support) {
  if (idx >= PRESET_COUNT)
    return false;
  const PresetRow &r = PRESETS[idx];
  if (r.eco && (support & SUPPORT_ECO) == 0)
    return false;
  if (r.mute && (support & SUPPORT_QUIET) == 0)
    return false;
  if (r.turbo && (support & SUPPORT_TURBO) == 0)
    return false;
  if (r.sleep != 0 && (support & SUPPORT_SLEEP) == 0)
    return false;
  return true;
}

// Name -> row index, or -1. HA passes the advertised string back verbatim.
inline int preset_index(const char *name) {
  if (name == nullptr)
    return -1;
  for (size_t i = 0; i < PRESET_COUNT; i++) {
    if (std::strcmp(PRESETS[i].name, name) == 0)
      return static_cast<int>(i);
  }
  return -1;
}

inline SpecialState special_from_status(bool eco_on, bool turbo_on, bool mute_on, uint8_t sleep_raw) {
  SpecialState s;
  s.eco = eco_on;
  s.turbo = turbo_on;
  s.mute = mute_on;
  s.sleep = static_cast<uint8_t>(sleep_raw / 2);  // status carries profile * 2 (RE docs/03, byte 17)
  return s;
}

// An exact row wins. Otherwise an arbitration is in flight (the A/C drops one of an illegal pair a
// second or two later), so report the strongest mode really on: turbo, eco, quiet, then sleep. Same
// order as the wrapper's _detect_preset. A preset the unit does not offer degrades to none.
inline uint8_t preset_detect(const SpecialState &s, uint8_t support) {
  for (size_t i = 0; i < PRESET_COUNT; i++) {
    const PresetRow &r = PRESETS[i];
    if (r.eco == s.eco && r.turbo == s.turbo && r.mute == s.mute && r.sleep == s.sleep)
      return preset_available(i, support) ? static_cast<uint8_t>(i) : PRESET_NONE;
  }
  uint8_t guess = PRESET_NONE;
  if (s.turbo) {
    guess = PRESET_TURBO;
  } else if (s.eco) {
    guess = PRESET_ECO;
  } else if (s.mute) {
    guess = PRESET_QUIET;
  } else if (s.sleep >= 1 && s.sleep <= 4) {
    guess = static_cast<uint8_t>(PRESET_FIRST_SLEEP + s.sleep);
  }
  return preset_available(guess, support) ? guess : PRESET_NONE;
}

// One bus write that changes a special mode. FEATURE carries a Feature (byte 33, combined frame);
// MUTE is 0/1 and SLEEP the profile, each on its own single-field frame.
enum SpecialOpKind : uint8_t {
  SPECIAL_OP_FEATURE = 0,
  SPECIAL_OP_MUTE = 1,
  SPECIAL_OP_SLEEP = 2,
};
struct SpecialOp {
  SpecialOpKind kind;
  uint8_t value;
};
static constexpr size_t PRESET_PLAN_MAX = 5;

// The A/C swallows a special-mode command that arrives too soon after the previous one. Measured for
// the wrapper: 6 s failed repeatedly, 8 s and 12 s engaged. 10 s is 8 with margin.
static constexpr uint32_t SPECIAL_SETTLE_MS = 10000;

// Ordered writes taking the unit from `now` to preset `target`; only writes that change something.
//   1. sleep off first when the target has none (quiet and sleep drop whichever came first).
//   2. clears: byte 33 to neutral (ECO_OFF if eco was on), then mute off.
//   3. sets: byte 33 to ECO or TURBO (one write switches between them), then mute on.
//   4. sleep profile last, and re-sent after any byte 33 write: the combined frame re-asserts the
//      fan while a profile owns it (measured: eco then sleep keeps both, sleep then eco does not).
inline size_t preset_plan(const SpecialState &now, uint8_t target, SpecialOp out[PRESET_PLAN_MAX]) {
  if (target >= PRESET_COUNT)
    return 0;
  const PresetRow &t = PRESETS[target];
  size_t n = 0;
  bool feature_change = (t.eco != now.eco) || (t.turbo != now.turbo);
  bool feature_set = feature_change && (t.eco || t.turbo);

  if (t.sleep == 0 && now.sleep != 0)
    out[n++] = {SPECIAL_OP_SLEEP, 0};
  if (feature_change && !feature_set)
    out[n++] = {SPECIAL_OP_FEATURE, static_cast<uint8_t>(now.eco ? FEATURE_ECO_OFF : FEATURE_NONE)};
  if (now.mute && !t.mute)
    out[n++] = {SPECIAL_OP_MUTE, 0};
  if (feature_set)
    out[n++] = {SPECIAL_OP_FEATURE, static_cast<uint8_t>(t.eco ? FEATURE_ECO : FEATURE_TURBO)};
  if (t.mute && !now.mute)
    out[n++] = {SPECIAL_OP_MUTE, 1};
  if (t.sleep != 0 && (t.sleep != now.sleep || feature_change))
    out[n++] = {SPECIAL_OP_SLEEP, t.sleep};
  return n;
}

// What the unit should report once `op` lands. Encoding facts only (byte 33 is one enum, so ECO
// clears turbo and TURBO clears eco); the A/C's own cross-mode drops show up in the next status.
inline void special_apply(SpecialState *s, const SpecialOp &op) {
  switch (op.kind) {
    case SPECIAL_OP_FEATURE:
      switch (static_cast<Feature>(op.value)) {
        case FEATURE_ECO:
          s->eco = true;
          s->turbo = false;
          break;
        case FEATURE_TURBO:
          s->turbo = true;
          s->eco = false;
          break;
        case FEATURE_ECO_OFF:
          s->eco = false;
          break;
        default:
          s->turbo = false;  // NONE: turbo off
          break;
      }
      break;
    case SPECIAL_OP_MUTE:
      s->mute = op.value != 0;
      break;
    case SPECIAL_OP_SLEEP:
      s->sleep = op.value;
      break;
    default:
      break;
  }
}

// The fan index a special mode pins the A/C to, or -1 when free. Turbo forces high; quiet and sleep
// force their low profile (firmware/docs/05). A request for another speed would be overwritten by
// the next status frame, so the glue refuses it instead.
inline int forced_fan_index(const SpecialState &s) {
  if (s.turbo)
    return 6;
  if (s.mute || s.sleep != 0)
    return 2;
  return -1;
}

// Quiet and sleep report as either of the two bottom ladder steps, so both count as agreeing.
inline bool fan_request_allowed(const SpecialState &s, uint8_t wanted_idx) {
  int pin = forced_fan_index(s);
  if (pin < 0)
    return true;
  if (pin == 2)
    return wanted_idx == 1 || wanted_idx == 2;
  return wanted_idx == static_cast<uint8_t>(pin);
}

// ---- Power estimate ----------------------------------------------------------------------------
// Calibrated 2026-07-07 against a panel meter (firmware/docs/09): status byte 55 tracks sqrt(power),
// P[W] = 4.15 * raw^2. Byte 50 is supply voltage in whole volts, about 6% low.
static constexpr uint64_t POWER_MW_PER_COUNT2 = 4150;  // milliwatts per byte-55 count squared
static constexpr int64_t POWER_PF_PERMIL = 950;        // assumed inverter power factor * 1000
static constexpr int64_t POWER_MW_MAX = 50000000;      // 50 kW; a glitched byte must not reach HA

inline int64_t active_power_mw(uint8_t raw55) {
  auto p = static_cast<int64_t>(POWER_MW_PER_COUNT2 * raw55 * raw55);
  return p > POWER_MW_MAX ? POWER_MW_MAX : p;
}

inline int64_t voltage_mv(uint8_t raw50) { return static_cast<int64_t>(raw50) * 1000; }

// I = P / (V * PF), back-computed since byte 55 is a power proxy. 0 when the voltage is unknown.
inline int64_t active_current_ma(uint8_t raw55, uint8_t raw50) {
  if (raw50 == 0)
    return 0;
  return (active_power_mw(raw55) * 1000) / (static_cast<int64_t>(raw50) * POWER_PF_PERMIL);
}

// Energy accumulates losslessly in mW*ms per status poll; import only, so non-positive power adds 0.
inline void energy_add(uint64_t *acc_mw_ms, int64_t power_mw, uint32_t dt_ms) {
  if (power_mw > 0)
    *acc_mw_ms += static_cast<uint64_t>(power_mw) * dt_ms;
}

inline uint64_t energy_mwh(uint64_t acc_mw_ms) { return acc_mw_ms / 3600000ULL; }

}  // namespace esphome::hisense_ac
