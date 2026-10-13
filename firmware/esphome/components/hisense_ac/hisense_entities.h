#pragma once
// What the entities ask of the unit and what they show of it, as pure functions: the climate
// entity's request turned into a plan, a status frame turned into what the climate entity, the
// switches and the sleep select show. Plain values only, no ESPHome types: the entity classes
// translate their ESPHome call into these structs and carry the result out, and the host tests
// (firmware/test/test_esphome_entities.cpp) cover every branch.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hisense_confirm.h"
#include "hisense_mapping.h"
#include "hisense_protocol.h"
#include "hisense_special.h"

namespace esphome::hisense_ac {

// ---- Climate: a request ------------------------------------------------------------------------
// One ClimateCall in plain values. A fan request can arrive by EITHER route, and it is not our
// choice which: ClimateCall::set_fan_mode(const char *) matches the built-in enum names FIRST, so
// "auto", "low", "medium" and "high" arrive as the enum and only "medium_low" and "medium_high"
// stay custom. Presets are the same: "none" and "eco" arrive as the enum, every other name as a
// custom preset.
struct ClimateRequest {
  bool has_mode{false};
  uint8_t mode{CLIMATE_MODE_OFF_VALUE};  // CLIMATE_MODE_*_VALUE
  bool has_target{false};
  float target_c{0.0f};
  bool has_fan{false};
  uint8_t fan{CLIMATE_FAN_AUTO_VALUE};  // CLIMATE_FAN_*_VALUE
  const char *custom_fan{nullptr};
  bool has_swing{false};
  uint8_t swing{CLIMATE_SWING_OFF_VALUE};  // CLIMATE_SWING_*_VALUE
  bool has_preset{false};
  uint8_t preset{CLIMATE_PRESET_NONE_VALUE};  // CLIMATE_PRESET_*_VALUE
  const char *custom_preset{nullptr};
};

// What the plan is made against.
struct ClimateContext {
  bool have_status{false};     // a status frame has filled the shadow
  bool power_expected{false};  // the unit's power once every power frame already sent has landed
  bool panel_f{false};         // the panel, and so the setpoint byte, is in Fahrenheit
  AcCommand shadow{};
  SpecialState projected{};   // the special modes once everything sent so far lands
  uint8_t preset_support{0};  // SUPPORT_* bits
};

// Parts of a request that are not carried out, for the log.
static constexpr uint8_t CLIMATE_REFUSED_NO_STATUS = 0x01;  // nothing but "off" before the first status
static constexpr uint8_t CLIMATE_REFUSED_MODE = 0x02;       // not a mode of this unit
static constexpr uint8_t CLIMATE_REFUSED_SETPOINT = 0x04;   // the unit only takes one in cool and heat
static constexpr uint8_t CLIMATE_REFUSED_FAN = 0x08;        // turbo, quiet or sleep holds the fan
static constexpr uint8_t CLIMATE_REFUSED_PRESET = 0x10;     // not a preset this unit offers

// What to send for a request, and what the entity shows once it has been sent.
struct ClimatePlan {
  uint8_t refused{0};
  // The literal power-off frame, and nothing else of the request.
  bool power_off{false};
  // One combined frame: `cmd` is the shadow with the request in it, `fields` the CONFIRM_* bits
  // the user asked for, `power_on` whether the frame also switches the unit on.
  bool send_combined{false};
  AcCommand cmd{};
  uint16_t fields{0};
  bool power_on{false};
  // Quiet asked for as a fan speed: reached through the mute flag, not the fan byte.
  bool mute_on{false};
  // The preset row to request, or -1.
  int8_t preset{-1};
  // What the entity shows when the frame has been queued.
  bool show_mode{false};
  uint8_t mode{CLIMATE_MODE_OFF_VALUE};
  bool show_target{false};
  int8_t target_c{0};
  int8_t fan_index{-1};  // ladder index as published (quiet shows as low), or -1
  bool show_swing{false};
  uint8_t swing{CLIMATE_SWING_OFF_VALUE};
};

// The preset row a request names, -1 for none, -2 for one this unit does not offer.
inline int climate_preset_request(const ClimateRequest &rq, uint8_t support) {
  if (support == 0)
    return -1;  // no preset control at all: nothing to refuse
  int idx = -1;
  if (rq.custom_preset != nullptr) {
    idx = preset_index(rq.custom_preset);
    if (idx >= 0 && idx < PRESET_FIRST_CUSTOM)
      idx = -1;  // "none" and "eco" are never custom names
  } else if (rq.has_preset) {
    if (rq.preset == CLIMATE_PRESET_NONE_VALUE) {
      idx = PRESET_NONE;
    } else if (rq.preset == CLIMATE_PRESET_ECO_VALUE) {
      idx = PRESET_ECO;
    }
  } else {
    return -1;
  }
  return (idx >= 0 && preset_available(static_cast<size_t>(idx), support)) ? idx : -2;
}

// The plan for one request.
//
// Switching off is the literal power frame and drops every other field of the same call: a
// combined frame does nothing on a unit that is off, and switching on again restates them.
// Everything else rides one combined frame, which is built from the shadow, so before the first
// status frame only "off" is possible (combined_frame_allowed).
inline ClimatePlan climate_plan(const ClimateRequest &rq, const ClimateContext &ctx) {
  ClimatePlan p;
  p.cmd = ctx.shadow;
  const bool off = rq.has_mode && rq.mode == CLIMATE_MODE_OFF_VALUE;
  if (!off && !combined_frame_allowed(ctx.have_status)) {
    p.refused = CLIMATE_REFUSED_NO_STATUS;
    return p;
  }
  if (off) {
    p.power_off = true;
    p.show_mode = true;
    p.mode = CLIMATE_MODE_OFF_VALUE;
    return p;
  }

  if (rq.has_mode) {
    Mode mode;
    if (climate_mode_to_hisense(rq.mode, &mode)) {
      p.cmd.mode = mode;
      p.show_mode = true;
      p.mode = rq.mode;
      p.send_combined = true;
      p.fields |= CONFIRM_MODE;
      // The A/C ignores a mode change while powered down, so the combined frame that carries the
      // mode also switches the unit on. The expected power, not the last status: that still reads
      // on for a poll or two after an off.
      p.power_on = !ctx.power_expected;
    } else {
      p.refused |= CLIMATE_REFUSED_MODE;
    }
  }

  if (rq.has_target) {
    // p.cmd.mode is the mode the combined frame will carry: the one just requested, else the
    // shadow's.
    if (!setpoint_request_allowed(ctx.power_expected || p.power_on, p.cmd.mode)) {
      p.refused |= CLIMATE_REFUSED_SETPOINT;
    } else {
      // The A/C reads byte 19 in its panel's unit, so an F panel needs the value in F.
      p.target_c = static_cast<int8_t>(setpoint_to_cmd(static_cast<int>(lroundf(rq.target_c)), ctx.panel_f, &p.cmd));
      p.show_target = true;
      p.send_combined = true;
      p.fields |= CONFIRM_SETPOINT;
    }
  }

  int wanted = -1;
  if (rq.custom_fan != nullptr) {
    wanted = fan_custom_index(rq.custom_fan);
  } else if (rq.has_fan) {
    wanted = climate_fan_to_index(rq.fan);
  }
  // A mode that owns the fan (turbo, quiet, sleep) overwrites any other speed about a second
  // later. Refuse instead of acknowledging a change that undoes itself, and keep showing the
  // pinned speed.
  if (wanted >= 0 && !fan_request_allowed(ctx.projected, static_cast<uint8_t>(wanted))) {
    p.refused |= CLIMATE_REFUSED_FAN;
    wanted = -1;
  }
  if (wanted >= 0) {
    if (wanted == FAN_INDEX_QUIET) {
      // Unreachable from Home Assistant, where quiet is not an advertised fan mode, and kept for a
      // direct API client that asks for QUIET. "Quiet" is not reachable through the fan byte on
      // this unit: commanding fan 0x03 (FAN_SPEED_QUIET, still marked VERIFY) puts the A/C on
      // HIGH. It reaches quiet through the MUTE flag, which drives fan_raw to 0x02.
      p.mute_on = true;
    } else {
      p.cmd.fan = fan_index_to_hisense(static_cast<uint8_t>(wanted));
      p.send_combined = true;
      p.fields |= CONFIRM_FAN;
    }
    p.fan_index = static_cast<int8_t>(fan_published_index(static_cast<uint8_t>(wanted)));
  }

  if (rq.has_swing) {
    climate_swing_to_hisense(rq.swing, &p.cmd.vswing, &p.cmd.hswing);
    p.show_swing = true;
    p.swing = rq.swing;
    p.send_combined = true;
    p.fields |= CONFIRM_SWING;
  }

  const int preset = climate_preset_request(rq, ctx.preset_support);
  if (preset == -2) {
    p.refused |= CLIMATE_REFUSED_PRESET;
  } else {
    p.preset = static_cast<int8_t>(preset);
  }
  return p;
}

// ---- Climate: a status frame -------------------------------------------------------------------
// What a status frame gives the climate entity to show.
struct ClimateView {
  // Telemetry, always taken.
  int8_t current_c{0};
  uint8_t action{CLIMATE_ACTION_OFF_VALUE};
  // Everything the user can move. Not taken during the hold-off, when the frame may predate the
  // user's command: the entity is left showing what they asked for.
  bool controls{false};
  bool mode_known{false};  // false for a mode value with no meaning: the mode shown is kept
  uint8_t mode{CLIMATE_MODE_OFF_VALUE};
  int8_t target_c{0};
  uint8_t swing{CLIMATE_SWING_OFF_VALUE};
  uint8_t fan_index{FAN_INDEX_AUTO};  // as published: quiet shows as low
  // A preset can take several paced frames (~10 s apart). Until the last one has landed and been
  // held off, the requested preset stays shown: a climate group mirroring this entity would
  // otherwise copy every intermediate state to the other rooms.
  bool preset_known{false};
  uint8_t preset{PRESET_NONE};
};

inline ClimateView climate_view_from_status(const AcState &st, bool holdoff, bool special_busy,
                                            uint8_t preset_support) {
  ClimateView v;
  Mode mode = st.mode;
  const bool mode_valid = mode_from_status(st.mode, &mode);
  v.current_c = st.indoor_temp_c;
  v.action = climate_action(st.power_on, mode, st.compressor_freq);
  if (!holdoff) {
    v.controls = true;
    if (!st.power_on) {
      v.mode_known = true;
      v.mode = CLIMATE_MODE_OFF_VALUE;
    } else if (mode_valid) {
      v.mode_known = true;
      v.mode = hisense_mode_to_climate(mode);
    }
    v.target_c = st.setpoint_c;
    v.swing = climate_swing(st.vswing_on, st.hswing_on);
    v.fan_index = fan_published_index(fan_raw_to_index(st.fan_raw));
  }
  if (preset_support != 0 && !holdoff && !special_busy) {
    v.preset_known = true;
    v.preset = preset_detect(special_from_status(st.eco_on, st.turbo_on, st.mute_on, st.sleep_raw), preset_support);
  }
  return v;
}

// Everything the climate entity shows, to tell whether a status frame moved any of it. A frame
// arrives about once a second and is nearly always identical to the last.
struct ClimateShown {
  float current_c{0.0f};
  float target_c{0.0f};
  uint8_t action{0};
  uint8_t mode{0};
  uint8_t swing{0};
  uint8_t fan_index{0};
  uint8_t preset_index{0};
};

// Negated equalities, so that a temperature that is still NaN (before the first frame) counts as
// changed when a number arrives.
inline bool climate_shown_changed(const ClimateShown &a, const ClimateShown &b) {
  return !(a.current_c == b.current_c) || !(a.target_c == b.target_c) || a.action != b.action || a.mode != b.mode ||
         a.swing != b.swing || a.fan_index != b.fan_index || a.preset_index != b.preset_index;
}

// ---- Switches and the sleep select -------------------------------------------------------------
enum SwitchKind : uint8_t {
  SWITCH_ECO = 0,
  SWITCH_TURBO,
  SWITCH_QUIET,
  SWITCH_DISPLAY,
  SWITCH_BEEPER,
};

// The special-mode write behind a switch, false for a switch that is not a special mode. Eco and
// turbo share one command byte and are mutually exclusive on this bus, so clearing eco sends the
// explicit eco-off value and not the neutral one (which clears turbo instead).
inline bool switch_special_op(SwitchKind kind, bool on, SpecialOp *op) {
  switch (kind) {
    case SWITCH_ECO:
      *op = {SPECIAL_OP_FEATURE, static_cast<uint8_t>(on ? FEATURE_ECO : FEATURE_ECO_OFF)};
      return true;
    case SWITCH_TURBO:
      *op = {SPECIAL_OP_FEATURE, static_cast<uint8_t>(on ? FEATURE_TURBO : FEATURE_NONE)};
      return true;
    case SWITCH_QUIET:
      *op = {SPECIAL_OP_MUTE, static_cast<uint8_t>(on ? 1 : 0)};
      return true;
    default:
      return false;
  }
}

// A switch or the select follows the unit's status only while nothing of ours is on its way:
// inside the hold-off the frame may predate the request, and while a special-mode write is queued
// or unconfirmed the unit passes through states nobody asked for.
inline bool special_entity_follows_status(bool holdoff, bool special_busy) { return !holdoff && !special_busy; }

// What a switch shows for a status frame, false when the frame says nothing about it. The status
// carries no display bit, so that switch shows the standing preference: what the firmware asserts
// on every frame, which is what the panel shows unless the remote changed it. The beeper is a
// setting of this node and no status moves it.
inline bool switch_state_from_status(SwitchKind kind, const AcState &st, bool display_pref_on, bool *on) {
  switch (kind) {
    case SWITCH_ECO:
      *on = st.eco_on;
      return true;
    case SWITCH_TURBO:
      *on = st.turbo_on;
      return true;
    case SWITCH_QUIET:
      *on = st.mute_on;
      return true;
    case SWITCH_DISPLAY:
      *on = display_pref_on;
      return true;
    default:
      return false;
  }
}

// The sleep select's options are in wire order: the option index IS the profile number the
// command carries, and the status reports profile * 2 (0x00 off, 0x02 General, 0x04 Old, 0x06
// Young, 0x08 Kids). False for a value that is no profile: the select keeps what it shows.
static constexpr uint8_t SLEEP_PROFILE_MAX = 4;

inline bool sleep_profile_from_status(uint8_t sleep_raw, uint8_t *profile) {
  const auto p = static_cast<uint8_t>(sleep_raw / 2);
  if (p > SLEEP_PROFILE_MAX)
    return false;
  *profile = p;
  return true;
}

}  // namespace esphome::hisense_ac
