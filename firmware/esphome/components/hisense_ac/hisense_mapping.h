#pragma once
// Pure Hisense <-> ESPHome translation: climate enums, mode, swing, the six-speed fan ladder, the
// setpoint, and the rules that keep the command shadow in step with the unit.
//
// Plain integers only, no ESPHome types, so this file can be compiled and tested on a host without
// ESPHome. The climate constants below mirror esphome::climate's enums and hisense_climate.cpp
// static_asserts each one it uses against the real enum.

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

static constexpr uint8_t CLIMATE_PRESET_NONE_VALUE = 0;
static constexpr uint8_t CLIMATE_PRESET_ECO_VALUE = 5;

// ---- Mode --------------------------------------------------------------------------------------
// Hisense AUTO is CLIMATE_MODE_AUTO: the unit picks heating or cooling itself. HEAT_COOL means
// "heat or cool to the target", which is not what this mode does. OFF is not a Mode (power has
// its own frame).
inline bool climate_mode_to_hisense(uint8_t climate_mode, Mode *out) {
  switch (climate_mode) {
    case CLIMATE_MODE_AUTO_VALUE:
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
      return CLIMATE_MODE_AUTO_VALUE;
    case MODE_COOL:
    default:
      return CLIMATE_MODE_COOL_VALUE;
  }
}

// The mode a status frame reports, checked before it reaches the command shadow or the entity.
// parse_status() takes the three mode bits of byte 18 (bits 4 to 6, the vendor module's
// t_work_mode) and folds 5 and 6 into AUTO, so the one value it can hand over that is no mode is
// 7: false, and the caller keeps the mode it had. Copied raw, 7 would be shown as cool and re-sent
// by the next combined frame as whatever (mode * 2 + 1) << 4 wraps to.
inline bool mode_from_status(Mode reported, Mode *out) {
  if (reported > MODE_AUTO)
    return false;
  *out = reported;
  return true;
}

// What the unit is doing. The compressor frequency decides whether a mode that uses the
// compressor is working or idle.
// VERIFY: in auto with the compressor running this says cooling, also when the unit heats. No
// capture or RE doc shows which status byte tells the two apart in auto.
inline uint8_t climate_action(bool power_on, Mode mode, uint8_t comp_freq) {
  if (!power_on)
    return CLIMATE_ACTION_OFF_VALUE;
  if (mode == MODE_FAN)
    return CLIMATE_ACTION_FAN_VALUE;
  if (comp_freq == 0)
    return CLIMATE_ACTION_IDLE_VALUE;
  if (mode == MODE_DRY)
    return CLIMATE_ACTION_DRYING_VALUE;
  return mode == MODE_HEAT ? CLIMATE_ACTION_HEATING_VALUE : CLIMATE_ACTION_COOLING_VALUE;
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
// The six discrete W41H1 speeds. raw = status byte 16, speed = 1..6, cmd = command enum. Auto
// (raw 0x01) is not a row: the A/C picks the speed.
struct FanRow {
  uint8_t raw;
  uint8_t speed;
  FanSpeed cmd;
};
inline constexpr FanRow FAN_TABLE[] = {
    {0x02, 1, FAN_SPEED_QUIET}, {0x0A, 2, FAN_SPEED_LOW},      {0x0C, 3, FAN_SPEED_MED_LOW},
    {0x0E, 4, FAN_SPEED_MID},   {0x10, 5, FAN_SPEED_MED_HIGH}, {0x12, 6, FAN_SPEED_HIGH},
};
static constexpr size_t FAN_TABLE_LEN = sizeof(FAN_TABLE) / sizeof(FAN_TABLE[0]);
static constexpr uint8_t FAN_RAW_AUTO = 0x01;

// Fan index as the climate entity sees it: 0 = auto, 1..6 = the ladder's speeds.
static constexpr uint8_t FAN_INDEX_AUTO = 0;
static constexpr uint8_t FAN_INDEX_QUIET = 1;  // the mute flag showing through, not a speed to command
static constexpr uint8_t FAN_INDEX_LOW = 2;
static constexpr uint8_t FAN_INDEX_MEDIUM_LOW = 3;
static constexpr uint8_t FAN_INDEX_MEDIUM = 4;
static constexpr uint8_t FAN_INDEX_MEDIUM_HIGH = 5;
static constexpr uint8_t FAN_INDEX_HIGH = 6;
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
// frame cannot clobber the user's fan setting.
inline FanSpeed fan_raw_to_cmd(uint8_t raw) {
  if (raw == FAN_RAW_AUTO)
    return FAN_SPEED_AUTO;
  const FanRow *r = fan_row_by_raw(raw);
  return r != nullptr ? r->cmd : FAN_SPEED_NOCHANGE;
}

// The fan the command shadow takes from a status frame, or NOCHANGE to keep what it holds. The quiet
// step (raw 0x02) is the mute flag showing through, not a speed that can be commanded: its command
// value 0x03 puts the unit on high (see the quiet note in hisense_climate.cpp). Left in the shadow,
// the next combined frame, for example a setpoint change while quiet is on, would send it.
inline FanSpeed shadow_fan_from_status(uint8_t raw) {
  FanSpeed fan = fan_raw_to_cmd(raw);
  return fan == FAN_SPEED_QUIET ? FAN_SPEED_NOCHANGE : fan;
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
      return FAN_INDEX_QUIET;
    case CLIMATE_FAN_LOW_VALUE:
      return FAN_INDEX_LOW;
    case CLIMATE_FAN_MIDDLE_VALUE:
      return FAN_INDEX_MEDIUM_LOW;
    case CLIMATE_FAN_MEDIUM_VALUE:
      return FAN_INDEX_MEDIUM;
    case CLIMATE_FAN_FOCUS_VALUE:
      return FAN_INDEX_MEDIUM_HIGH;
    case CLIMATE_FAN_HIGH_VALUE:
    case CLIMATE_FAN_ON_VALUE:
      return FAN_INDEX_HIGH;
    default:
      return FAN_INDEX_AUTO;
  }
}

// Quiet is only reachable through the mute flag, i.e. the `quiet` preset, so as a fan mode it is
// published as low.
inline uint8_t fan_published_index(uint8_t idx) { return idx == FAN_INDEX_QUIET ? FAN_INDEX_LOW : idx; }

// How the climate entity names a ladder step. Five of the seven are ESPHome's BUILT-IN fan enum
// and only the two it has no name for are custom. Mixing is not cosmetic: ClimateCall converts a
// name matching a built-in into the enum, and its validation then DISCARDS that enum unless the
// traits advertise it, so publishing everything as custom would make "High" unsettable.
// ESPHome keeps a pointer to the custom name it is given, so these strings are the one definition.
inline constexpr const char *const FAN_CUSTOM_NAMES[2] = {"medium_low", "medium_high"};

inline bool fan_index_is_custom(uint8_t idx) { return idx == FAN_INDEX_MEDIUM_LOW || idx == FAN_INDEX_MEDIUM_HIGH; }

// The custom name of a step that has one (fan_index_is_custom).
inline const char *fan_custom_name(uint8_t idx) { return FAN_CUSTOM_NAMES[idx == FAN_INDEX_MEDIUM_LOW ? 0 : 1]; }

// Custom name -> ladder index, or -1.
inline int fan_custom_index(const char *name) {
  if (name == nullptr)
    return -1;
  if (std::strcmp(name, FAN_CUSTOM_NAMES[0]) == 0)
    return FAN_INDEX_MEDIUM_LOW;
  if (std::strcmp(name, FAN_CUSTOM_NAMES[1]) == 0)
    return FAN_INDEX_MEDIUM_HIGH;
  return -1;
}

// The built-in fan enum value a step without a custom name is published as. The quiet step is not
// a fan mode and shows as low.
inline uint8_t fan_index_to_climate_fan(uint8_t idx) {
  switch (idx) {
    case FAN_INDEX_AUTO:
      return CLIMATE_FAN_AUTO_VALUE;
    case FAN_INDEX_QUIET:
    case FAN_INDEX_LOW:
      return CLIMATE_FAN_LOW_VALUE;
    case FAN_INDEX_MEDIUM:
      return CLIMATE_FAN_MEDIUM_VALUE;
    default:
      return CLIMATE_FAN_HIGH_VALUE;
  }
}

// ---- Setpoint ----------------------------------------------------------------------------------
inline int clamp_setpoint_c(int c) {
  if (c < SETPOINT_MIN_C)
    return SETPOINT_MIN_C;
  if (c > SETPOINT_MAX_C)
    return SETPOINT_MAX_C;
  return c;
}

// ESPHome speaks Celsius but the A/C reads the byte in its panel unit. Clamp in C, convert,
// and tell the builder which unit it holds. Returns the clamped Celsius value to publish.
inline int setpoint_to_cmd(int wanted_c, bool panel_f, AcCommand *cmd) {
  auto c = static_cast<int8_t>(clamp_setpoint_c(wanted_c));
  cmd->fahrenheit = panel_f;
  cmd->setpoint = panel_f ? c_to_f(c) : c;
  return c;
}

// The A/C takes a setpoint only in cool and heat. In dry and fan-only the command builder strips it
// from the wire. In auto the frame carries it and the unit keeps its own: measured 2026-10-09, writes
// of 19 C and 25 C in auto both read back as the previous 22 C once the hold-off expired. A write
// the unit drops would show a target that reverts about four seconds later, so the climate entity
// refuses it. `running` is false for a powered-down unit that the same call does not switch on:
// there the value still goes into the shadow, for the mode change that may follow.
inline bool setpoint_request_allowed(bool running, Mode frame_mode) {
  if (!running)
    return true;
  return frame_mode == MODE_COOL || frame_mode == MODE_HEAT;
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

// The same from a decoded status frame, and the one the command tracker uses. On a Fahrenheit panel
// the shadow takes the frame's own byte (setpoint_raw). Going through setpoint_c would turn whole
// F into whole C and back, and whole Celsius is the coarser of the two: 13 of the 30 values an F
// panel can show came back one degree off (62 F as 63, 65 as 64), and since the shadow rides every
// combined frame, the next fan, swing or mode change from Home Assistant moved a setpoint chosen
// on the remote.
inline bool sync_shadow_setpoint(const AcState &st, AcCommand *cmd) {
  if (!st.temp_unit_f)
    return sync_shadow_setpoint(st.setpoint_c, false, cmd);
  // As int8_t a byte above 127 is negative, and so out of range like any other bad value.
  if (!setpoint_in_range(static_cast<int8_t>(st.setpoint_raw), true))
    return false;
  cmd->setpoint = static_cast<int8_t>(st.setpoint_raw);
  cmd->fahrenheit = true;
  return true;
}

// ---- Power intent -------------------------------------------------------------------------------
// The status frame lags a power frame by a poll or two, and the hold-off keeps the entity showing the
// request meanwhile. A decision that needs the unit's power state in that window (does this mode
// change need the power-on frame first) has to use what was last commanded, not the lagging status:
// off followed by a mode within the window otherwise skips the power-on frame, and the unit, which
// ignores a mode change while powered down, stays off.
struct PowerIntent {
  bool pending{false};
  bool on{false};
};

inline void power_intent_sent(PowerIntent *p, bool on) {
  p->pending = true;
  p->on = on;
}

// A status frame from outside the hold-off is newer than every command sent: it is the truth again.
inline void power_intent_status(PowerIntent *p, bool holdoff) {
  if (!holdoff)
    p->pending = false;
}

inline bool power_expected(const PowerIntent &p, bool status_on) { return p.pending ? p.on : status_on; }

// ---- Shadow readiness ---------------------------------------------------------------------------
// A combined frame states mode, setpoint, fan and swing together and has no "leave alone" value for
// the mode or the swing. Until one status frame has filled the shadow those fields are the struct's
// defaults (cool, 24, auto fan, no swing), so a single write, say swing, would also switch a heating
// unit to cool. No combined frame goes out before the first status.
inline bool combined_frame_allowed(bool have_status) { return have_status; }

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

}  // namespace esphome::hisense_ac
