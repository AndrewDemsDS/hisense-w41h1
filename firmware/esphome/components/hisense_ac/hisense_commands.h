#pragma once
// The command tracker: everything the hub knows about commands it has sent or still has to send.
//
// It holds the command shadow, the last user write to power, mode, setpoint, fan or swing until the
// unit's status confirms it, the same for the last special-mode write, the paced special-mode queue,
// the special-mode state the unit is expected to reach, the power intent, the hold-off and the
// counters. Every decision it takes is a function in hisense_map.h. Frames leave through
// CommandSink, which the hub implements over the bus scheduler and a host test implements over a
// model of the unit, so the scenarios in firmware/test/test_esphome_confirm.cpp run this code and
// not a copy of it.
//
// No ESPHome includes and no clock: the caller passes the time in.

#include <cstddef>
#include <cstdint>

#include "hisense_bus.h"
#include "hisense_map.h"
#include "hisense_protocol.h"

namespace esphome::hisense_ac {

// What the tracker reports, so its owner can log at the level it likes. `fields` and `count` mean
// what the comment on each value says and are 0 otherwise.
enum class CommandEvent : uint8_t {
  COMBINED_REFUSED_NO_STATUS,  // a combined frame was asked for before the first status
  MAIN_CONFIRMED,              // count = re-sends it took
  MAIN_YIELDED,                // fields = what the unit shows differently, changed from elsewhere
  MAIN_GIVEN_UP,               // fields = unmet, count = re-sends
  MAIN_RESENT,                 // fields = unmet, count = this re-send's number
  SPECIAL_CONFIRMED,           // count = re-sends it took
  SPECIAL_YIELDED,             // fields = what the unit shows differently
  SPECIAL_GIVEN_UP,            // fields = unmet, count = re-sends
  SPECIAL_REPLANNED,           // fields = unmet, count = writes queued again
  SPECIAL_SENDING,             // fields = the op's kind, count = its value, just before the frame
  SPECIAL_QUEUE_FULL,          // fields = the kind of the op that was refused
  SPECIAL_NOT_SENT,            // fields = the op's kind, count = its value: the frame was not queued
  PRESET_QUEUED,               // fields = preset row, count = writes queued
  DROPPED_ON_LINK_LOSS,        // count = commands unconfirmed or unsent
  SHADOW_MODE_KEPT,            // fields = the status mode value that is not a mode
  SHADOW_SETPOINT_KEPT,        // fields = the status setpoint (as uint8_t) outside the command range
};

// Where the tracker's frames go. Each send returns the scheduler's ticket for the frame
// (BusScheduler::enqueue), or BUS_TICKET_NONE when it was not queued. The ticket comes back in
// on_command_done(), which is how the tracker knows which of its commands a finished frame was.
class CommandSink {
 public:
  virtual ~CommandSink() = default;
  // The combined 0x65 frame built from `cmd`.
  virtual BusTicket send_combined(const AcCommand &cmd) = 0;
  // The literal power frames.
  virtual BusTicket send_power(bool on) = 0;
  // The single-field mute and sleep frames.
  virtual BusTicket send_mute(bool on) = 0;
  virtual BusTicket send_sleep(uint8_t profile) = 0;
  virtual void on_command_event(CommandEvent event, uint16_t fields, uint16_t count) {}
};

class CommandTracker {
 public:
  void set_sink(CommandSink *sink) { this->sink_ = sink; }

  /// The command shadow: what the next combined frame states. An entity copies it, changes what
  /// the user asked for and hands the copy to send_user_command().
  const AcCommand &cmd() const { return this->cmd_; }
  const AcState &last_state() const { return this->last_; }
  bool has_state() const { return this->last_.valid; }

  /// A user command just went out, so the unit's echo of the PREVIOUS state is ignored for a
  /// moment. Without this, a poll already in flight lands after the write and visibly reverts the
  /// control in Home Assistant before the next poll corrects it.
  /// Elapsed-time form, so it stays correct across the 49.7-day clock wrap: a stored deadline
  /// compared with a signed difference reads as "in hold-off" again ~24.8 days after the last command.
  void note_user_command(uint32_t now) {
    this->holdoff_start_ = now;
    this->holdoff_armed_ = true;
  }
  bool in_holdoff(uint32_t now) const {
    if (this->holdoff_armed_ && now - this->holdoff_start_ >= COMMAND_HOLDOFF_MS)
      this->holdoff_armed_ = false;  // or it re-arms for 4 s at every wrap
    return this->holdoff_armed_;
  }

  /// The unit's power once every power frame already sent has landed. Status alone lags a power
  /// frame by a poll or two.
  bool power_expected() const { return hisense_ac::power_expected(this->power_intent_, this->last_.power_on); }

  /// Queue not yet drained, or the last write not yet confirmed: preset and switch readbacks would
  /// show intermediate states.
  bool special_busy() const { return this->special_len_ > 0 || this->special_pending_.active; }
  /// The special-mode state the unit should report once everything sent so far lands.
  const SpecialState &projected_special() const { return this->projected_; }

  /// Counters since boot: commands sent again after a status that did not match, and commands the
  /// unit never took.
  uint32_t command_retries() const { return this->command_retries_; }
  uint32_t failed_commands() const { return this->failed_commands_; }

  // ---- Sending ----------------------------------------------------------------------------------
  /// The combined frame from the current shadow, unchecked. For frames that only carry something
  /// the status does not report (the display preference).
  bool send_combined() { return this->send_combined_() != BUS_TICKET_NONE; }

  /// A user write of `fields` (CONFIRM_* bits): `wanted` is the shadow with those fields changed.
  /// Sends the combined frame, with the power-on bits in it when `power_on`, and keeps the request
  /// until the unit's status agrees with it: a frame the unit did not take is sent again.
  /// False when the frame was not queued. Then nothing has changed: the shadow is what it was, so
  /// the refused values cannot ride out on a later frame, nothing is tracked, and the caller must
  /// not show the request.
  bool send_user_command(const AcCommand &wanted, uint16_t fields, bool power_on, uint32_t now) {
    const AcCommand before = this->cmd_;
    this->cmd_ = wanted;
    // Power and mode in one frame, as the stock module sends them: the unit takes both or neither.
    this->cmd_.power_on = power_on;
    const BusTicket ticket = this->send_combined_();
    this->cmd_.power_on = false;  // one frame only, never part of the shadow
    if (ticket == BUS_TICKET_NONE) {
      this->cmd_ = before;
      return false;
    }
    this->main_ticket_ = ticket;
    if (power_on) {
      power_intent_sent(&this->power_intent_, true);
      fields |= CONFIRM_POWER;
    }
    this->track_(intent_from_command(this->cmd_, fields), now);
    return true;
  }

  /// Switch the unit off, checked and re-sent the same way. False when the frame was not queued.
  bool send_user_power_off(uint32_t now) {
    const BusTicket ticket = this->send_power_(false);
    if (ticket == BUS_TICKET_NONE)
      return false;
    this->main_ticket_ = ticket;
    this->track_(intent_power_off(), now);
    return true;
  }

  // ---- Special modes ----------------------------------------------------------------------------
  /// Special modes (eco/turbo byte 33, mute, sleep) go through a paced queue, never straight to the
  /// bus: the A/C swallows a special-mode command that lands within ~8 s of the previous one, so
  /// every op waits SPECIAL_SETTLE_MS after the last. Presets, switches and the sleep select all
  /// share it, so they can never race each other on the wire.
  /// False when the op was not taken (the queue is full): the caller must not show the request.
  bool enqueue_special(const SpecialOp &op, uint32_t now) {
    this->special_resends_ = 0;  // a new request from the user: a fresh allowance of re-sends
    return this->push_special_(op, now);
  }

  /// Replace whatever is still queued with the plan for preset `target` (a row index), computed
  /// against what the unit is expected to be once already-sent ops land.
  void request_preset(uint8_t target, uint32_t now) {
    // Plan from where the unit will be once everything already SENT lands. Queued-but-unsent ops
    // are discarded: the new preset fully determines all four modes, so they would only add
    // settle waits (or undo each other). A write already sent and not yet confirmed stays under
    // watch: if the unit did not take it, the plan is made again from what it reports.
    SpecialOp ops[PRESET_PLAN_MAX];
    size_t n = preset_plan(this->projected_, target, ops);
    this->special_len_ = 0;
    this->special_resends_ = 0;
    this->event_(CommandEvent::PRESET_QUEUED, target, static_cast<uint16_t>(n));
    for (size_t i = 0; i < n; i++)
      this->push_special_(ops[i], now);
    this->note_user_command(now);
  }

  /// Send the next queued special-mode write when its turn has come. Call on every loop pass.
  void drain_special_queue(uint32_t now) {
    if (this->special_len_ == 0)
      return;
    // One write at a time: the next waits until the unit's status has answered for the last.
    if (this->special_pending_.active)
      return;
    if (this->special_sent_ && now - this->last_special_ms_ < SPECIAL_SETTLE_MS)
      return;
    SpecialOp op = this->special_queue_[0];
    for (uint8_t i = 1; i < this->special_len_; i++)
      this->special_queue_[i - 1] = this->special_queue_[i];
    this->special_len_--;
    const BusTicket ticket = this->execute_special_(op);
    if (ticket == BUS_TICKET_NONE) {
      // Nothing left for the unit: it is not expected to change, the pacing has nothing to wait
      // out, and the next status frame puts the entities back on what the unit reports.
      this->failed_commands_++;
      this->event_(CommandEvent::SPECIAL_NOT_SENT, op.kind, op.value);
      return;
    }
    special_apply(&this->projected_, op);
    this->last_special_ms_ = now;
    this->special_sent_ = true;
    this->note_user_command(now);
    this->special_ticket_ = ticket;
    pending_begin(&this->special_pending_, intent_from_special(op), unit_view_from_status(this->last_), now);
    this->special_pending_.resends = this->special_resends_;
  }

  // ---- From the bus -----------------------------------------------------------------------------
  /// The command frame with this ticket finished its transaction, answered or not. The frame left
  /// the wire now, whichever way it ended: the unit's settle time and the hold-off count from here
  /// and not from the moment it was queued, and so does the special-mode pacing for a frame that
  /// got no answer. Only the command the frame belongs to is touched: a frame of another command,
  /// or one nobody tracks (the display preference), says nothing about the ones still waiting.
  void on_command_done(BusTicket ticket, bool answered, uint32_t now) {
    if (ticket == BUS_TICKET_NONE)
      return;
    if (this->main_pending_.active && ticket == this->main_ticket_) {
      pending_on_wire(&this->main_pending_, now);
      this->note_user_command(now);
    }
    if (this->special_pending_.active && ticket == this->special_ticket_) {
      pending_on_wire(&this->special_pending_, now);
      this->note_user_command(now);
      if (!answered)
        this->last_special_ms_ = now;
    }
  }

  /// The link dropped. Nothing is re-sent into a dead bus, and nothing still waiting is sent when
  /// it comes back.
  void on_link_lost() {
    unsigned dropped = (this->main_pending_.active ? 1 : 0) + (this->special_pending_.active ? 1 : 0);
    if (this->special_len_ > 0)
      dropped++;
    this->main_pending_.active = false;
    this->special_pending_.active = false;
    this->special_len_ = 0;
    if (dropped == 0)
      return;
    this->failed_commands_ += dropped;
    this->event_(CommandEvent::DROPPED_ON_LINK_LOSS, 0, static_cast<uint16_t>(dropped));
  }

  /// A decoded status frame: judge what is waiting, then let the shadow and the projection follow
  /// the unit where no command of ours is in the way.
  void on_status(const AcState &state, uint32_t now) {
    this->last_ = state;

    // First, so that a command sent again re-arms the hold-off before anything below reads it.
    const UnitView view = unit_view_from_status(state);
    const SpecialState wanted = special_wanted(this->projected_, this->special_queue_, this->special_len_);
    this->confirm_main_(view, wanted, now, state.temp_unit_f);
    this->confirm_special_(view, wanted, now);

    power_intent_status(&this->power_intent_, this->in_holdoff(now));

    // Keep the command shadow tracking reality, so a later single-field write rebuilds the
    // combined frame from what the A/C is actually doing instead of a stale shadow. Skipped
    // during the hold-off, when `state` may predate the user's own command.
    if (!this->in_holdoff(now)) {
      FanSpeed fan = shadow_fan_from_status(state.fan_raw);
      if (fan != FAN_SPEED_NOCHANGE)
        this->cmd_.fan = fan;
      if (!mode_from_status(state.mode, &this->cmd_.mode))
        this->event_(CommandEvent::SHADOW_MODE_KEPT, static_cast<uint16_t>(state.mode));
      // Validated in the wire unit: a raw copy would drop the F flag and let an out-of-range report
      // poison the shadow, which silently kills every later Cool/Heat/Auto frame.
      if (!sync_shadow_setpoint(state.setpoint_c, state.temp_unit_f, &this->cmd_))
        this->event_(CommandEvent::SHADOW_SETPOINT_KEPT, static_cast<uint8_t>(state.setpoint_c));
      this->cmd_.vswing = state.vswing_on ? SWING_MODE_SWING : SWING_MODE_OFF;
      this->cmd_.hswing = state.hswing_on ? SWING_MODE_SWING : SWING_MODE_OFF;
      // Without this a Turbo/Eco set from HA is re-asserted on every later combined frame, even
      // after the remote cleared it.
      this->cmd_.feature = feature_from_status(state.eco_on, state.turbo_on);
    }
    // The projection only follows the A/C while nothing of ours is queued or settling; otherwise
    // a frame from before our last op would make the next plan re-send what is already on its way.
    if (!this->special_busy() && !this->in_holdoff(now))
      this->projected_ = special_from_status(state.eco_on, state.turbo_on, state.mute_on, state.sleep_raw);
  }

  // For tests and logs.
  const PendingCommand &main_pending() const { return this->main_pending_; }
  const PendingCommand &special_pending() const { return this->special_pending_; }
  size_t special_queued() const { return this->special_len_; }

  static constexpr uint32_t COMMAND_HOLDOFF_MS = 4000;
  // Paced special-mode queue. Fixed size: a preset plan is at most PRESET_PLAN_MAX ops, and
  // anything that would overflow is dropped with a warning rather than allocated.
  static constexpr uint8_t SPECIAL_QUEUE_CAP = 8;

 protected:
  void event_(CommandEvent event, uint16_t fields = 0, uint16_t count = 0) {
    this->sink_->on_command_event(event, fields, count);
  }

  BusTicket send_combined_() {
    if (!combined_frame_allowed(this->last_.valid)) {
      this->event_(CommandEvent::COMBINED_REFUSED_NO_STATUS);
      return BUS_TICKET_NONE;
    }
    return this->sink_->send_combined(this->cmd_);
  }

  BusTicket send_power_(bool on) {
    const BusTicket ticket = this->sink_->send_power(on);
    if (ticket != BUS_TICKET_NONE)
      power_intent_sent(&this->power_intent_, on);
    return ticket;
  }

  // The frames of a re-send. The settle time counts from the last one that was queued.
  void resend_main_(ResendFrames frames) {
    BusTicket first = BUS_TICKET_NONE;
    BusTicket last = BUS_TICKET_NONE;
    switch (frames) {
      case RESEND_POWER_OFF:
        last = this->send_power_(false);
        break;
      case RESEND_POWER_ON_ALONE:
        last = this->send_power_(true);
        break;
      case RESEND_POWER_ON_PAIR:
        first = this->send_power_(true);
        last = this->send_combined_();
        break;
      case RESEND_COMBINED:
        last = this->send_combined_();
        break;
    }
    if (last == BUS_TICKET_NONE)
      last = first;
    if (last != BUS_TICKET_NONE)
      this->main_ticket_ = last;
  }

  void track_(const CommandIntent &intent, uint32_t now) {
    pending_begin(&this->main_pending_, intent, unit_view_from_status(this->last_), now);
    this->note_user_command(now);
  }

  void confirm_main_(const UnitView &view, const SpecialState &wanted, uint32_t now, bool panel_f) {
    PendingCommand &p = this->main_pending_;
    if (!p.active)
      return;
    uint16_t unmet = 0;
    switch (confirm_decision(p, view, wanted, now, COMMAND_RESEND_MAX, &unmet)) {
      case CONFIRM_WAIT:
        return;
      case CONFIRM_DONE:
        this->event_(CommandEvent::MAIN_CONFIRMED, 0, p.resends);
        p.active = false;
        return;
      case CONFIRM_YIELD:
        this->event_(CommandEvent::MAIN_YIELDED, unmet);
        p.active = false;
        return;
      case CONFIRM_GIVE_UP:
        this->event_(CommandEvent::MAIN_GIVEN_UP, unmet, p.resends);
        this->failed_commands_++;
        p.active = false;
        return;
      case CONFIRM_RESEND:
        break;
    }
    p.resends++;
    this->command_retries_++;
    this->event_(CommandEvent::MAIN_RESENT, unmet, p.resends);
    intent_to_command(p.intent, panel_f, &this->cmd_);
    this->resend_main_(resend_frames(p.intent, unmet));
    p.sent_ms = now;
    this->note_user_command(now);
  }

  void confirm_special_(const UnitView &view, const SpecialState &wanted, uint32_t now) {
    PendingCommand &p = this->special_pending_;
    if (!p.active)
      return;
    uint16_t unmet = 0;
    switch (confirm_decision(p, view, wanted, now, SPECIAL_RESEND_MAX, &unmet)) {
      case CONFIRM_WAIT:
        return;
      case CONFIRM_DONE:
        this->event_(CommandEvent::SPECIAL_CONFIRMED, 0, p.resends);
        p.active = false;
        return;
      case CONFIRM_YIELD:
        this->event_(CommandEvent::SPECIAL_YIELDED, unmet);
        p.active = false;
        return;
      case CONFIRM_GIVE_UP:
        this->event_(CommandEvent::SPECIAL_GIVEN_UP, unmet, p.resends);
        this->failed_commands_++;
        p.active = false;
        return;
      case CONFIRM_RESEND:
        break;
    }
    // Nothing else is in flight (the queue waits for this verdict), so the status is the truth. Plan
    // again from it to where the user wants the unit: the lost write, plus whatever was still queued.
    p.active = false;
    this->special_resends_++;
    this->command_retries_++;
    SpecialOp ops[PRESET_PLAN_MAX];
    size_t n = special_plan(view.special, wanted, ops);
    this->event_(CommandEvent::SPECIAL_REPLANNED, unmet, static_cast<uint16_t>(n));
    this->projected_ = view.special;
    this->special_len_ = 0;
    for (size_t i = 0; i < n; i++)
      this->push_special_(ops[i], now);
    this->note_user_command(now);
  }

  bool push_special_(const SpecialOp &op, uint32_t now) {
    if (this->special_len_ >= SPECIAL_QUEUE_CAP) {
      this->event_(CommandEvent::SPECIAL_QUEUE_FULL, op.kind);
      return false;
    }
    this->special_queue_[this->special_len_++] = op;
    // Held from the moment of the request, not the send, so a readback that predates it cannot
    // flip the entity back while the op waits out the settle.
    this->note_user_command(now);
    return true;
  }

  BusTicket execute_special_(const SpecialOp &op) {
    this->event_(CommandEvent::SPECIAL_SENDING, op.kind, op.value);
    switch (op.kind) {
      case SPECIAL_OP_FEATURE: {
        this->cmd_.feature = static_cast<Feature>(op.value);
        const BusTicket ticket = this->send_combined_();
        this->cmd_.feature = feature_after_send(this->cmd_.feature);  // ECO_OFF is one-shot
        return ticket;
      }
      case SPECIAL_OP_MUTE:
        return this->sink_->send_mute(op.value != 0);
      case SPECIAL_OP_SLEEP:
        return this->sink_->send_sleep(op.value);
      default:
        return BUS_TICKET_NONE;
    }
  }

  CommandSink *sink_{nullptr};
  AcState last_{};
  AcCommand cmd_{};
  PowerIntent power_intent_{};
  PendingCommand main_pending_{};
  PendingCommand special_pending_{};
  // The scheduler's ticket for the newest frame of each, so a finished frame is credited to the
  // command it belongs to and to no other.
  BusTicket main_ticket_{BUS_TICKET_NONE};
  BusTicket special_ticket_{BUS_TICKET_NONE};
  uint8_t special_resends_{0};
  uint32_t command_retries_{0};
  uint32_t failed_commands_{0};
  uint32_t holdoff_start_{0};
  mutable bool holdoff_armed_{false};
  SpecialOp special_queue_[SPECIAL_QUEUE_CAP]{};
  uint8_t special_len_{0};
  uint32_t last_special_ms_{0};
  bool special_sent_{false};
  SpecialState projected_{};
};

// The verdict on a command has to be in before the hold-off lets a status frame reach the entities.
static_assert(CONFIRM_SETTLE_MS < CommandTracker::COMMAND_HOLDOFF_MS,
              "confirm settle must end inside the command hold-off");

}  // namespace esphome::hisense_ac
