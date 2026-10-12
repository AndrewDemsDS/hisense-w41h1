#pragma once
// Pure Hisense <-> ESPHome translation: climate enums, mode, swing, the six-speed fan ladder, the
// setpoint, and the rules that keep the command shadow in step with the unit.
//
// Plain integers only, no ESPHome types, so this file can be compiled and tested on a host without
// ESPHome. The climate constants below mirror esphome::climate's enums and hisense_climate.cpp
// static_asserts each one it uses against the real enum.

#include <cstddef>
#include <cstdint>

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
// parse_status() hands over the whole upper nibble of byte 18 with 5 and 6 folded into AUTO. The
// vendor module's descriptor for the field (t_work_mode, 1c030503) is three bits wide, bits 4 to 6,
// so bit 7 is not part of the mode and is dropped here. That leaves 7 as the one value with no
// meaning: false, and the caller keeps the mode it had. Copied raw, a value above AUTO would be
// shown as cool and re-sent by the next combined frame as whatever (mode * 2 + 1) << 4 wraps to.
inline bool mode_from_status(Mode reported, Mode *out) {
  auto v = static_cast<uint8_t>(reported);
  if (v > MODE_AUTO) {
    v &= 0x07;
    if (v == 5 || v == 6)
      v = MODE_AUTO;
  }
  if (v > MODE_AUTO)
    return false;
  *out = static_cast<Mode>(v);
  return true;
}

// Bitmap of what is running (Heat = 1, Cool = 2, Fan = 4). Compressor Hz decides whether a heat/cool
// mode is working or idle.
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
// The six discrete W41H1 speeds. raw = status byte 16, speed = 1..6, percent = the speed as a
// percentage, cmd = command enum. Auto (raw 0x01) is not a row: the A/C picks the speed.
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

// Quiet (index 1) is only reachable through the mute flag, i.e. the `quiet` preset, so as a fan mode
// it is published as low.
inline uint8_t fan_published_index(uint8_t idx) { return idx == 1 ? 2 : idx; }

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
