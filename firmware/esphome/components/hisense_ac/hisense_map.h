#pragma once
// Pure Hisense <-> ESPHome translation: climate enums, the six-speed fan ladder, special-mode
// presets and the power estimate.
//
// Plain integers only, no ESPHome types, so this file can be compiled and tested on a host without
// ESPHome. The climate constants below mirror esphome::climate's enums and hisense_climate.cpp
// static_asserts each one against the real enum.

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

// ---- Special modes as presets ------------------------------------------------------------------
// Measured on hardware: eco + each sleep profile coexist; quiet + sleep do not; turbo combines with
// nothing; eco + quiet coexist. The first two rows are ESPHome built-ins (NONE, ECO); no custom
// name may equal a built-in, since set_preset(const char *) converts those.
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
  s.sleep = static_cast<uint8_t>(sleep_raw / 2);  // status byte 17 carries profile * 2
  return s;
}

// An exact row wins. Otherwise an arbitration is in flight (the A/C drops one of an illegal pair a
// second or two later), so report the strongest mode really on: turbo, eco, quiet, then sleep. A
// preset the unit does not offer degrades to none.
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

// The A/C swallows a special-mode command that arrives too soon after the previous one. Measured:
// 6 s failed repeatedly, 8 s and 12 s engaged. 10 s is 8 with margin.
static constexpr uint32_t SPECIAL_SETTLE_MS = 10000;

// Ordered writes taking the unit from `now` to the special-mode state `want`; only writes that
// change something.
//   1. sleep off first when the target has none (quiet and sleep drop whichever came first).
//   2. clears: byte 33 to neutral (ECO_OFF if eco was on), then mute off.
//   3. sets: byte 33 to ECO or TURBO (one write switches between them), then mute on.
//   4. sleep profile last, and re-sent after any byte 33 write: the combined frame re-asserts the
//      fan while a profile owns it (measured: eco then sleep keeps both, sleep then eco does not).
inline size_t special_plan(const SpecialState &now, const SpecialState &want, SpecialOp out[PRESET_PLAN_MAX]) {
  size_t n = 0;
  bool feature_change = (want.eco != now.eco) || (want.turbo != now.turbo);
  bool feature_set = feature_change && (want.eco || want.turbo);

  if (want.sleep == 0 && now.sleep != 0)
    out[n++] = {SPECIAL_OP_SLEEP, 0};
  if (feature_change && !feature_set)
    out[n++] = {SPECIAL_OP_FEATURE, static_cast<uint8_t>(now.eco ? FEATURE_ECO_OFF : FEATURE_NONE)};
  if (now.mute && !want.mute)
    out[n++] = {SPECIAL_OP_MUTE, 0};
  if (feature_set)
    out[n++] = {SPECIAL_OP_FEATURE, static_cast<uint8_t>(want.eco ? FEATURE_ECO : FEATURE_TURBO)};
  if (want.mute && !now.mute)
    out[n++] = {SPECIAL_OP_MUTE, 1};
  if (want.sleep != 0 && (want.sleep != now.sleep || feature_change))
    out[n++] = {SPECIAL_OP_SLEEP, want.sleep};
  return n;
}

// The same, to preset row `target`.
inline size_t preset_plan(const SpecialState &now, uint8_t target, SpecialOp out[PRESET_PLAN_MAX]) {
  if (target >= PRESET_COUNT)
    return 0;
  const PresetRow &t = PRESETS[target];
  SpecialState want;
  want.eco = t.eco;
  want.turbo = t.turbo;
  want.mute = t.mute;
  want.sleep = t.sleep;
  return special_plan(now, want, out);
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
// force their low profile. A request for another speed would be overwritten by the next status
// frame, so the climate entity refuses it instead.
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

// ---- Confirm and retry ---------------------------------------------------------------------------
// A command frame used to be sent once and never looked at again. Two things can go wrong with it.
// The unit may not hear it: no reply in the 500 ms window. Or it answers and still does not apply
// it, which is how a special-mode command inside the unit's debounce ends. The stock module checks
// the reply of every command (RE docs/10 section 4.6). Here the scheduler re-sends a frame that got
// no reply, and the hub compares what the unit reports a few seconds later with what was asked.

// A frame with no reply goes out again in the next cycle's command slot, in front of anything queued
// behind it, so a newer command can never be overtaken by an older one. Three sends in all. Only
// while the unit is answering its status poll: a unit that answers nothing is a dead bus, and frames
// re-sent into it would pile up and all land when it comes back.
static constexpr uint8_t COMMAND_SENDS_MAX = 3;

inline bool command_resend_allowed(uint8_t sends, bool unit_answering) {
  return unit_answering && sends < COMMAND_SENDS_MAX;
}

// What the reply to a command frame says. The stock transaction primitive accepts a reply as verified
// when its payload repeats the first two payload bytes of the command and carries 1 in the third
// (RE docs/10 section 4.2 step 6). Payload byte N is frame byte 13 + N, so that is frame bytes 13
// and 14 echoed, and the value 1 at frame offset 15.
// VERIFY: the layout is read from the stock image and has not been captured on this bus. Until a
// capture backs it the verdict is logged and nothing is decided on it: the status check below is
// what tells a command that took from one that did not.
static constexpr size_t CMD_REPLY_ACK_OFFSET = 15;
static constexpr uint8_t CMD_REPLY_ACK_VALUE = 0x01;

enum CommandReply : uint8_t {
  COMMAND_REPLY_NONE = 0,   // the window closed empty
  COMMAND_REPLY_ACK = 1,    // echo of the command, ack byte 1
  COMMAND_REPLY_NAK = 2,    // echo of the command, ack byte something else
  COMMAND_REPLY_OTHER = 3,  // a frame of another class (a status or link frame) ended the window
};

inline CommandReply command_reply_verdict(const uint8_t *tx, size_t tx_len, const uint8_t *reply, size_t reply_len) {
  if (reply == nullptr || reply_len == 0)
    return COMMAND_REPLY_NONE;
  if (tx == nullptr || tx_len <= FRAME_CLASS_OFFSET + 1 || reply_len <= CMD_REPLY_ACK_OFFSET)
    return COMMAND_REPLY_OTHER;
  if (reply[FRAME_CLASS_OFFSET] != tx[FRAME_CLASS_OFFSET] ||
      reply[FRAME_CLASS_OFFSET + 1] != tx[FRAME_CLASS_OFFSET + 1])
    return COMMAND_REPLY_OTHER;
  return reply[CMD_REPLY_ACK_OFFSET] == CMD_REPLY_ACK_VALUE ? COMMAND_REPLY_ACK : COMMAND_REPLY_NAK;
}

// The things a command can ask for, one bit each.
static constexpr uint16_t CONFIRM_POWER = 0x001;
static constexpr uint16_t CONFIRM_MODE = 0x002;
static constexpr uint16_t CONFIRM_SETPOINT = 0x004;
static constexpr uint16_t CONFIRM_FAN = 0x008;
static constexpr uint16_t CONFIRM_SWING = 0x010;
static constexpr uint16_t CONFIRM_ECO = 0x020;
static constexpr uint16_t CONFIRM_TURBO = 0x040;
static constexpr uint16_t CONFIRM_MUTE = 0x080;
static constexpr uint16_t CONFIRM_SLEEP = 0x100;

// The unit in the terms a command is checked in, as a status frame reports it or as a command
// wants it.
struct UnitView {
  bool power_on{false};
  bool mode_valid{false};  // false when the status mode field holds no mode (see mode_from_status)
  Mode mode{MODE_COOL};
  int8_t setpoint_c{0};
  uint8_t fan_raw{0};  // status byte 16
  bool vswing{false};
  bool hswing{false};
  SpecialState special{};
};

inline UnitView unit_view_from_status(const AcState &st) {
  UnitView v;
  v.power_on = st.power_on;
  v.mode_valid = mode_from_status(st.mode, &v.mode);
  v.setpoint_c = st.setpoint_c;
  v.fan_raw = st.fan_raw;
  v.vswing = st.vswing_on;
  v.hswing = st.hswing_on;
  v.special = special_from_status(st.eco_on, st.turbo_on, st.mute_on, st.sleep_raw);
  return v;
}

// Which of `fields` differ between two views.
inline uint16_t view_diff(const UnitView &a, const UnitView &b, uint16_t fields) {
  uint16_t d = 0;
  if (a.power_on != b.power_on)
    d |= CONFIRM_POWER;
  if (a.mode_valid && b.mode_valid && a.mode != b.mode)
    d |= CONFIRM_MODE;
  if (a.setpoint_c != b.setpoint_c)
    d |= CONFIRM_SETPOINT;
  if (a.fan_raw != b.fan_raw)
    d |= CONFIRM_FAN;
  if (a.vswing != b.vswing || a.hswing != b.hswing)
    d |= CONFIRM_SWING;
  if (a.special.eco != b.special.eco)
    d |= CONFIRM_ECO;
  if (a.special.turbo != b.special.turbo)
    d |= CONFIRM_TURBO;
  if (a.special.mute != b.special.mute)
    d |= CONFIRM_MUTE;
  if (a.special.sleep != b.special.sleep)
    d |= CONFIRM_SLEEP;
  return d & fields;
}

// What one command asks of the unit. `fields` are the things the user asked for. `known` are the
// things `want` holds a value for, which is more: a combined frame states the whole shadow.
struct CommandIntent {
  uint16_t fields{0};
  uint16_t known{0};
  UnitView want{};
};

// The intent of a combined frame built from `cmd`. `fields` are the user's (CONFIRM_POWER means the
// frame also switches the unit on). A shadow fan with no status value (NOCHANGE, or the quiet step,
// whose command byte is not what the status reports) cannot be checked and is left out.
inline CommandIntent intent_from_command(const AcCommand &cmd, uint16_t fields) {
  CommandIntent i;
  i.known = CONFIRM_MODE | CONFIRM_SETPOINT | CONFIRM_SWING;
  i.want.mode = cmd.mode;
  i.want.mode_valid = true;
  // As the status will report it: an F panel's byte comes back through f_to_c().
  i.want.setpoint_c = cmd.fahrenheit ? f_to_c(cmd.setpoint) : cmd.setpoint;
  i.want.vswing = cmd.vswing == SWING_MODE_SWING;
  i.want.hswing = cmd.hswing == SWING_MODE_SWING;
  if (cmd.fan == FAN_SPEED_AUTO) {
    i.want.fan_raw = FAN_RAW_AUTO;
    i.known |= CONFIRM_FAN;
  } else if (cmd.fan != FAN_SPEED_QUIET) {
    for (const auto &row : FAN_TABLE) {
      if (row.cmd == cmd.fan) {
        i.want.fan_raw = row.raw;
        i.known |= CONFIRM_FAN;
      }
    }
  }
  if ((fields & CONFIRM_POWER) != 0) {
    i.want.power_on = true;
    i.known |= CONFIRM_POWER;
  }
  i.fields = fields & i.known;
  return i;
}

inline CommandIntent intent_power_off() {
  CommandIntent i;
  i.fields = i.known = CONFIRM_POWER;
  i.want.power_on = false;
  return i;
}

// The intent of one special-mode write: the one flag it moves. What the unit drops on its own in
// answer (sleep when quiet engages, eco under turbo) is not part of it.
inline CommandIntent intent_from_special(const SpecialOp &op) {
  CommandIntent i;
  switch (op.kind) {
    case SPECIAL_OP_FEATURE:
      switch (static_cast<Feature>(op.value)) {
        case FEATURE_ECO:
          i.fields = CONFIRM_ECO;
          i.want.special.eco = true;
          break;
        case FEATURE_TURBO:
          i.fields = CONFIRM_TURBO;
          i.want.special.turbo = true;
          break;
        case FEATURE_ECO_OFF:
          i.fields = CONFIRM_ECO;
          break;
        default:
          i.fields = CONFIRM_TURBO;  // NONE: turbo off
          break;
      }
      break;
    case SPECIAL_OP_MUTE:
      i.fields = CONFIRM_MUTE;
      i.want.special.mute = op.value != 0;
      break;
    case SPECIAL_OP_SLEEP:
      i.fields = CONFIRM_SLEEP;
      i.want.special.sleep = op.value;
      break;
    default:
      break;
  }
  i.known = i.fields;
  return i;
}

// Put an intent's values back into the command shadow, for the frame that re-sends it.
inline void intent_to_command(const CommandIntent &i, bool panel_f, AcCommand *cmd) {
  if ((i.fields & CONFIRM_MODE) != 0)
    cmd->mode = i.want.mode;
  if ((i.fields & CONFIRM_SETPOINT) != 0)
    setpoint_to_cmd(i.want.setpoint_c, panel_f, cmd);
  if ((i.fields & CONFIRM_FAN) != 0) {
    FanSpeed fan = fan_raw_to_cmd(i.want.fan_raw);
    if (fan != FAN_SPEED_NOCHANGE)
      cmd->fan = fan;
  }
  if ((i.fields & CONFIRM_SWING) != 0) {
    cmd->vswing = i.want.vswing ? SWING_MODE_SWING : SWING_MODE_OFF;
    cmd->hswing = i.want.hswing ? SWING_MODE_SWING : SWING_MODE_OFF;
  }
}

// Which of the fields a command asked for the unit can be expected to show. The rest is the unit's
// own behaviour and must not be read as a lost command:
//   - powered down, it ignores everything except power;
//   - turbo forces cool at 16 C on high fan, and the unit stays in cool afterwards;
//   - it keeps its own setpoint outside cool and heat (auto measured 2026-10-09, dry and fan-only
//     carry none on the wire, see setpoint_request_allowed);
//   - quiet and a sleep profile pin the fan, and dry carries no fan change on the wire;
//   - a status mode field that holds no mode says nothing about the mode.
// `wanted` is the special-mode state the unit is on its way to (sent and queued writes applied): a
// turbo that is about to engage excuses the same fields as one that has.
inline uint16_t intent_expected_fields(const CommandIntent &intent, const UnitView &now, const SpecialState &wanted) {
  uint16_t f = intent.fields;
  if (!now.power_on)
    return f & CONFIRM_POWER;
  const bool turbo = now.special.turbo || wanted.turbo;
  const bool fan_pinned = turbo || now.special.mute || wanted.mute || now.special.sleep != 0 || wanted.sleep != 0;
  if (turbo || !now.mode_valid)
    f &= static_cast<uint16_t>(~CONFIRM_MODE);
  if (turbo || !now.mode_valid || (now.mode != MODE_COOL && now.mode != MODE_HEAT))
    f &= static_cast<uint16_t>(~CONFIRM_SETPOINT);
  if (fan_pinned || !now.mode_valid || now.mode == MODE_DRY)
    f &= static_cast<uint16_t>(~CONFIRM_FAN);
  return f;
}

// Timing. The check runs on the first status frame at least CONFIRM_SETTLE_MS after the frame left
// the wire. That is the hub's 4 s hold-off less 100 ms, so the verdict is always in before a status
// frame may reach the entity: a re-send keeps the entity on the request with no flicker in between.
// Every status the hardware test reads 7 s after a write has passed through the same 4 s.
// A command older than CONFIRM_STALE_MS is not sent again: status has been missing for so long that
// the user may have moved on.
static constexpr uint32_t CONFIRM_SETTLE_MS = 3900;
static constexpr uint32_t CONFIRM_STALE_MS = 20000;
// Re-sends after a status that does not match. Each re-send of a special-mode write costs the 10 s
// pacing, so those get one.
static constexpr uint8_t COMMAND_RESEND_MAX = 2;
static constexpr uint8_t SPECIAL_RESEND_MAX = 1;

// A command waiting for the unit's status to agree with it.
struct PendingCommand {
  bool active{false};
  CommandIntent intent{};
  UnitView before{};         // what the unit reported when each field was first asked for
  uint16_t before_known{0};  // fields `before` is still good for
  uint32_t sent_ms{0};       // when the frame was queued, then when it left the wire
  uint8_t resends{0};
};

// A new user command. It takes over from the one still waiting: the newest value of every field is
// the only one checked, so an older request is never re-sent against a newer one. Switching off drops
// everything else that was asked. A field asked for again loses its `before`: the unit may be
// anywhere between the two requests, so a third value no longer proves someone else moved it.
inline void pending_begin(PendingCommand *p, const CommandIntent &next, const UnitView &now, uint32_t now_ms) {
  const bool off = (next.fields & CONFIRM_POWER) != 0 && !next.want.power_on;
  const uint16_t carried = (p->active && !off) ? p->intent.fields : 0;
  const bool keep_power = (carried & CONFIRM_POWER) != 0 && (next.fields & CONFIRM_POWER) == 0;
  const bool old_power = p->intent.want.power_on;
  const uint16_t repeated = carried & next.fields;
  const uint16_t fresh = next.fields & static_cast<uint16_t>(~carried);
  const uint16_t kept_before = carried != 0 ? p->before_known : 0;

  const UnitView old_before = p->before;
  p->before = now;
  if ((kept_before & CONFIRM_MODE) != 0) {
    p->before.mode = old_before.mode;
    p->before.mode_valid = old_before.mode_valid;
  }
  if ((kept_before & CONFIRM_SETPOINT) != 0)
    p->before.setpoint_c = old_before.setpoint_c;
  if ((kept_before & CONFIRM_FAN) != 0)
    p->before.fan_raw = old_before.fan_raw;
  if ((kept_before & CONFIRM_SLEEP) != 0)
    p->before.special.sleep = old_before.special.sleep;
  p->before_known = static_cast<uint16_t>((kept_before & ~repeated) | fresh);

  p->intent.want = next.want;
  if (keep_power)
    p->intent.want.power_on = old_power;
  p->intent.known = next.known | (keep_power ? CONFIRM_POWER : 0);
  p->intent.fields = static_cast<uint16_t>((carried | next.fields) & p->intent.known);
  p->sent_ms = now_ms;
  p->resends = 0;
  p->active = p->intent.fields != 0;
}

// A command frame just finished its transaction: the settle time counts from here.
inline void pending_on_wire(PendingCommand *p, uint32_t now_ms) {
  if (p->active)
    p->sent_ms = now_ms;
}

enum ConfirmAction : uint8_t {
  CONFIRM_WAIT = 0,     // too early to tell
  CONFIRM_DONE = 1,     // the unit shows what was asked, or nothing of it can be expected to show
  CONFIRM_RESEND = 2,   // it does not: send again
  CONFIRM_YIELD = 3,    // it shows a third value: someone else (the remote) moved it, leave it
  CONFIRM_GIVE_UP = 4,  // it does not, and the re-sends are used up or the command is too old
};

// The fields where "neither what it was nor what was asked" is evidence of another hand. A
// two-valued field that is not what was asked is still what it was. The fan and the sleep profile
// are left out: the unit moves both on its own (the fan when quiet or a profile lets go of it, the
// profile when eco or quiet arrives), so a third value there may still be a lost command.
static constexpr uint16_t CONFIRM_YIELD_FIELDS = CONFIRM_MODE | CONFIRM_SETPOINT;

// Judge a waiting command against a status frame. `unmet` (optional) receives the fields that were
// expected and are not there.
inline ConfirmAction confirm_decision(const PendingCommand &p, const UnitView &now, const SpecialState &wanted,
                                      uint32_t now_ms, uint8_t resend_max, uint16_t *unmet) {
  if (unmet != nullptr)
    *unmet = 0;
  if (!p.active)
    return CONFIRM_DONE;
  const uint32_t age = now_ms - p.sent_ms;
  if (age < CONFIRM_SETTLE_MS)
    return CONFIRM_WAIT;
  const uint16_t missing = view_diff(p.intent.want, now, intent_expected_fields(p.intent, now, wanted));
  if (unmet != nullptr)
    *unmet = missing;
  if (missing == 0)
    return CONFIRM_DONE;
  if (view_diff(p.before, now, missing & p.before_known & CONFIRM_YIELD_FIELDS) != 0)
    return CONFIRM_YIELD;
  if (age >= CONFIRM_STALE_MS || p.resends >= resend_max)
    return CONFIRM_GIVE_UP;
  return CONFIRM_RESEND;
}

// The frames that re-send a command the unit did not take.
//
// Power and mode: a mode request on a powered-down unit used to be two frames, the literal power-on
// and then the combined frame with the mode. Each was sent once, and losing the second left the unit
// running in its last mode. The stock module packs both into byte 18 of one frame (RE docs/10
// section 5b-2), so the unit takes both or neither, and that is what the first send does (AcCommand
// power_on). Only cool (0x5C) and heat (0x3C) are bytes the stock image contains, so if the unit is
// still off when the command is checked, the re-send goes back to the pair of frames this bus has
// carried on hardware since the first release.
enum ResendFrames : uint8_t {
  RESEND_COMBINED = 0,        // the plain combined frame
  RESEND_POWER_OFF = 1,       // the literal power-off frame
  RESEND_POWER_ON_PAIR = 2,   // the literal power-on frame, then the plain combined frame
  RESEND_POWER_ON_ALONE = 3,  // the literal power-on frame (nothing else was asked)
};

inline ResendFrames resend_frames(const CommandIntent &intent, uint16_t unmet) {
  if ((unmet & CONFIRM_POWER) == 0)
    return RESEND_COMBINED;
  if (!intent.want.power_on)
    return RESEND_POWER_OFF;
  if ((intent.fields & static_cast<uint16_t>(~CONFIRM_POWER)) == 0)
    return RESEND_POWER_ON_ALONE;
  return RESEND_POWER_ON_PAIR;
}

// The special-mode state the unit is being taken to: what it should report once every write already
// sent (`projected`) and every write still queued has landed.
inline SpecialState special_wanted(const SpecialState &projected, const SpecialOp *queue, size_t len) {
  SpecialState s = projected;
  for (size_t i = 0; i < len; i++)
    special_apply(&s, queue[i]);
  return s;
}

// ---- Power estimate ----------------------------------------------------------------------------
// Calibrated against a panel meter: status byte 55 tracks sqrt(power), P[W] = 4.15 * raw^2. Byte 50
// is supply voltage in whole volts, about 6% low.
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

// ---- Telemetry pacing ---------------------------------------------------------------------------
// A status frame arrives about once a second and rarely differs from the previous one. A sensor
// value goes out when it is the first, when it changed, or when the periodic refresh is due. The
// refresh keeps integrating sensors (total_daily_energy) advancing through a steady load.
static constexpr uint32_t TELEMETRY_REFRESH_MS = 60000;

inline bool telemetry_publish_due(bool has_state, float last, float value, bool refresh_due) {
  // Negated equality, not !=, so a NaN on either side counts as a change.
  return !has_state || refresh_due || !(last == value);
}

}  // namespace esphome::hisense_ac
