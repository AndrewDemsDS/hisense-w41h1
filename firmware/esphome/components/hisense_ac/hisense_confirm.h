#pragma once
// Confirm and retry: what a command asks of the unit, what the unit can be expected to show for
// it, and whether a status frame says it took, should be sent again, was overridden from
// elsewhere or is given up. Pure, no ESPHome types. CommandTracker (hisense_commands.h) carries
// these decisions out.

#include <cstddef>
#include <cstdint>

#include "hisense_mapping.h"
#include "hisense_protocol.h"
#include "hisense_special.h"

namespace esphome::hisense_ac {

// ---- Confirm and retry ---------------------------------------------------------------------------
// A command frame used to be sent once and never looked at again. Two things can go wrong with it.
// The unit may not hear it: no reply in the 500 ms window. Or it answers and still does not apply
// it, which is how a special-mode command inside the unit's debounce ends. The stock module checks
// the reply of every command (RE docs/10 section 4.6). Here the scheduler re-sends a frame that got
// no reply, and the hub compares what the unit reports a few seconds later with what was asked.

// The scheduler's half, the re-send rule (COMMAND_SENDS_MAX, command_resend_allowed) and the verdict
// on a command's reply (command_reply_verdict), is in hisense_bus.h with the scheduler that applies
// it. The hub's half follows.

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

}  // namespace esphome::hisense_ac
