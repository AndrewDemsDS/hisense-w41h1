#include "hisense_ac.h"
#include "hisense_climate.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cmath>

namespace esphome::hisense_ac {

static const char *const TAG = "hisense_ac";

// What each log level shows:
//   WARN          the bus link dropping, a frame that could not be sent, a bad checksum, a fault,
//                 a command that got no reply, was sent again, or was given up on
//   INFO          the bus link coming back, a fault clearing, a command taken on a re-send
//   DEBUG         a status change, every command sent, the unit's capabilities
//   VERBOSE       every decoded status frame, every poll that got no reply
//   VERY_VERBOSE  the raw bytes of every frame sent and received

// ---- Bus results ------------------------------------------------------------------------------
// These run inside bus_.poll(). Entities are not touched here: the frame is parked and loop()
// publishes it after the special-mode queue has had its turn.
void HisenseAC::on_bus_status(const AcState &state) {
  this->pending_ = state;
  this->pending_valid_ = true;
}

void HisenseAC::on_bus_features(const AcFeatures &features) {}  // polled in publish_diagnostics_

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
// A status frame is about 160 bytes, more than one log line holds, so frames go out in rows.
static void log_frame(const char *direction, const uint8_t *frame, size_t len) {
  static const size_t ROW = 32;
  char hex[format_hex_pretty_size(ROW)];
  for (size_t at = 0; at < len; at += ROW) {
    const size_t n = len - at < ROW ? len - at : ROW;
    ESP_LOGVV(TAG, "%s %3u: %s", direction, (unsigned) at, format_hex_pretty_to(hex, frame + at, n, ' '));
  }
}
#endif

void HisenseAC::on_bus_frame(const uint8_t *frame, size_t len) {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
  log_frame("RX", frame, len);
#endif
}

void HisenseAC::on_bus_checksum_error(const uint8_t *frame, size_t len) {
  ESP_LOGW(TAG, "Status frame with a bad checksum dropped (%" PRIu32 " since boot)", this->bus_.checksum_mismatches());
}

void HisenseAC::on_bus_timeout(uint8_t expect_class) {
  ESP_LOGV(TAG, "No reply within the window (expected class 0x%02X)", expect_class);
}

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
static const LogString *command_reply_to_string(uint8_t reply) {
  switch (reply) {
    case COMMAND_REPLY_ACK:
      return LOG_STR("echo, ack");
    case COMMAND_REPLY_NAK:
      return LOG_STR("echo, no ack");
    default:
      return LOG_STR("not an echo");
  }
}
#endif

void HisenseAC::on_bus_command(bool answered, uint8_t reply_class) {
  // The unit answers every command it hears. Silence means the frame was lost on the way or
  // arrived damaged.
  if (answered) {
    ESP_LOGD(TAG, "Command answered (class 0x%02X, %s)", reply_class,
             LOG_STR_ARG(command_reply_to_string(this->bus_.command_reply())));
  } else if (this->bus_.command_resend_pending()) {
    ESP_LOGW(TAG, "Command frame got no reply from the unit, sending it again");
  } else {
    ESP_LOGW(TAG, "Command frame got no reply from the unit, not sent again");
  }
  // The frame left the wire now, whichever way it ended: the unit's settle time, the hold-off and
  // the special-mode pacing all count from here and not from the moment it was queued.
  const uint32_t now = millis();
  if (this->main_pending_.active || this->special_pending_.active)
    this->note_user_command();
  pending_on_wire(&this->main_pending_, now);
  pending_on_wire(&this->special_pending_, now);
  if (!answered && this->special_pending_.active)
    this->last_special_ms_ = now;
}

void HisenseAC::on_bus_link(bool up) {
  if (!up) {
    // Drop any undrained pre-loss frame, or loop() would publish those stale values (and flip
    // link_up_ back to true) until real data returns.
    this->pending_valid_ = false;
    // A silent unit may be one that lost power, and it comes back with the placeholder.
    outdoor_temp_gate_reset(&this->outdoor_gate_);
  }
  this->link_up_ = up;
  this->link_dirty_ = true;
}

void HisenseAC::setup() {
  if (this->de_pin_ != nullptr) {
    this->de_pin_->setup();
    this->de_pin_->digital_write(false);  // idle low = receive, before the UART speaks
  }
  this->bus_.setup(this, this, this->de_pin_ != nullptr);
}

void HisenseAC::loop() {
  this->bus_.poll(millis());
  // DE settle and drain are a few ms each; the default ~16 ms loop would stretch them, so ask for
  // a tight loop only while they run.
  if (this->bus_.wants_fast_loop()) {
    this->fast_loop_.start();
  } else {
    this->fast_loop_.stop();
  }

  AcState state;
  bool have_state = false;
  if (this->pending_valid_) {
    state = this->pending_;
    this->pending_valid_ = false;
    have_state = true;
  }

  this->drain_special_queue_();

  if (this->link_dirty_) {
    this->link_dirty_ = false;
    if (this->link_up_) {
      ESP_LOGI(TAG, "A/C bus link restored");
    } else {
      ESP_LOGW(TAG, "A/C bus link lost: the unit is not answering");
    }
#ifdef USE_BINARY_SENSOR
    // publish_telemetry_() only runs on a decoded frame, so it can only ever publish true; the
    // loss edge has to be published here or the sensor never shows the link down.
    if (this->bus_link_binary_sensor_ != nullptr)
      this->bus_link_binary_sensor_->publish_state(this->link_up_);
#endif
    // Nothing is re-sent into a dead bus, and nothing still waiting is sent when it comes back.
    if (!this->link_up_)
      this->drop_pending_commands_();
    // No status frame arrives while the link is down, so the edge is the one moment the counters
    // can still go out.
    this->publish_bus_counters_(false);
  }

  if (have_state)
    this->process_status_(state);
}

void HisenseAC::process_status_(const AcState &state) {
  // A decoded frame IS the link being up. The link callback only fires on EDGES, so a link that
  // is healthy from boot never produces a "restored" edge and link_up_ would sit at its initial
  // false forever, reporting a working bus as down.
  this->link_up_ = true;
  ESP_LOGV(TAG, "Status: power=%d mode=%d setpoint=%d indoor=%d fan_raw=0x%02X holdoff=%d", state.power_on,
           (int) state.mode, state.setpoint_c, state.indoor_temp_c, state.fan_raw, this->in_command_holdoff());
  // A frame arrives about once a second, so DEBUG gets it only when something a user would
  // recognise moved. The sleep and mute bytes are in the list because they are how the unit
  // answers those two commands: one that is accepted and then ignored looks the same as one that
  // was never sent unless the raw value is seen to move.
  const AcState &was = this->last_;
  if (!was.valid || was.power_on != state.power_on || was.mode != state.mode || was.setpoint_c != state.setpoint_c ||
      was.indoor_temp_c != state.indoor_temp_c || was.fan_raw != state.fan_raw || was.sleep_raw != state.sleep_raw ||
      was.mute_on != state.mute_on || was.eco_on != state.eco_on || was.turbo_on != state.turbo_on) {
    ESP_LOGD(
        TAG,
        "Status changed: power=%d mode=%d setpoint=%d indoor=%d fan_raw=0x%02X eco=%d turbo=%d mute=%d sleep_raw=%u",
        state.power_on, (int) state.mode, state.setpoint_c, state.indoor_temp_c, state.fan_raw, state.eco_on,
        state.turbo_on, state.mute_on, state.sleep_raw);
  }
  this->last_ = state;

  // First, so that a command sent again re-arms the hold-off before anything below reads it.
  this->confirm_commands_(state);

  power_intent_status(&this->power_intent_, this->in_command_holdoff());

  // Keep the command shadow tracking reality, so a later single-field write rebuilds the
  // combined frame from what the A/C is actually doing instead of a stale shadow. Skipped
  // during the hold-off, when `state` may predate the user's own command.
  if (!this->in_command_holdoff()) {
    FanSpeed fan = shadow_fan_from_status(state.fan_raw);
    if (fan != FAN_SPEED_NOCHANGE)
      this->cmd_.fan = fan;
    if (!mode_from_status(state.mode, &this->cmd_.mode)) {
      ESP_LOGV(TAG, "Status mode %d is not a mode, shadow kept", (int) state.mode);
    }
    // Validated in the wire unit: a raw copy would drop the F flag and let an out-of-range report
    // poison the shadow, which silently kills every later Cool/Heat/Auto frame.
    if (!sync_shadow_setpoint(state.setpoint_c, state.temp_unit_f, &this->cmd_)) {
      ESP_LOGV(TAG, "Status setpoint %d C is outside the command range, shadow kept", state.setpoint_c);
    }
    this->cmd_.vswing = state.vswing_on ? SWING_MODE_SWING : SWING_MODE_OFF;
    this->cmd_.hswing = state.hswing_on ? SWING_MODE_SWING : SWING_MODE_OFF;
    // Without this a Turbo/Eco set from HA is re-asserted on every later combined frame, even
    // after the remote cleared it.
    this->cmd_.feature = feature_from_status(state.eco_on, state.turbo_on);
  }
  // The projection only follows the A/C while nothing of ours is queued or settling; otherwise
  // a frame from before our last op would make the next plan re-send what is already on its way.
  if (!this->special_busy() && !this->in_command_holdoff())
    this->projected_ = special_from_status(state.eco_on, state.turbo_on, state.mute_on, state.sleep_raw);

  if (this->climate_ != nullptr)
    this->climate_->update_from_bus(state, this->in_command_holdoff());
  for (auto *listener : this->listeners_)
    listener->on_status(state);

  this->publish_telemetry_(state);
  this->publish_diagnostics_();
}

#ifdef USE_SENSOR
void HisenseAC::publish_sensor_(sensor::Sensor *sensor, float value, bool refresh_due) {
  if (sensor == nullptr)
    return;
  if (telemetry_publish_due(sensor->has_state(), sensor->get_raw_state(), value, refresh_due))
    sensor->publish_state(value);
}
#endif

void HisenseAC::publish_telemetry_(const AcState &state) {
#ifdef USE_SENSOR
  // An unchanged value is still republished once per refresh period: integrating sensors such as
  // total_daily_energy only advance when their source publishes.
  const uint32_t now = millis();
  const bool refresh = !this->telemetry_refreshed_ || now - this->last_refresh_ms_ >= TELEMETRY_REFRESH_MS;
  if (refresh) {
    this->last_refresh_ms_ = now;
    this->telemetry_refreshed_ = true;
  }
  this->publish_sensor_(this->indoor_temperature_sensor_, state.indoor_temp_c, refresh);
  // Both read -20 C for a while after mains power returns: the unit's "not measured yet" value.
  // Those frames go out as unknown (see outdoor_temps_measured in hisense_map.h).
  const bool outdoor_measured = outdoor_temps_measured(&this->outdoor_gate_, state.outdoor_temp_c, state.coil_temp_c);
  this->publish_sensor_(this->outdoor_temperature_sensor_, outdoor_measured ? state.outdoor_temp_c : NAN, refresh);
  this->publish_sensor_(this->coil_temperature_sensor_, outdoor_measured ? state.coil_temp_c : NAN, refresh);
  this->publish_sensor_(this->compressor_frequency_sensor_, state.compressor_freq, refresh);
  // The bus carries a current PROXY, not amps: active power is 4.15 * raw^2, calibrated against
  // a panel meter. hisense_map.h owns that maths and works in milli-units, so scale back here.
  this->publish_sensor_(this->power_sensor_, active_power_mw(state.current_raw) / 1000.0f, refresh);
  this->publish_sensor_(this->voltage_sensor_, voltage_mv(state.voltage_raw) / 1000.0f, refresh);
  this->publish_sensor_(this->current_sensor_, active_current_ma(state.current_raw, state.voltage_raw) / 1000.0f,
                        refresh);
  this->publish_bus_counters_(refresh);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->aux_heat_binary_sensor_ != nullptr)
    this->aux_heat_binary_sensor_->publish_state(state.heat_relay_on);
  if (this->bus_link_binary_sensor_ != nullptr)
    this->bus_link_binary_sensor_->publish_state(this->link_up_);
#endif
}

void HisenseAC::publish_bus_counters_(bool refresh_due) {
#ifdef USE_SENSOR
  this->publish_sensor_(this->checksum_errors_sensor_, this->bus_.checksum_mismatches(), refresh_due);
  this->publish_sensor_(this->reply_timeouts_sensor_, this->bus_.reply_timeouts(), refresh_due);
  this->publish_sensor_(this->unanswered_commands_sensor_, this->bus_.unanswered_commands(), refresh_due);
  this->publish_sensor_(this->command_retries_sensor_, this->bus_.command_resends() + this->command_retries_,
                        refresh_due);
  this->publish_sensor_(this->failed_commands_sensor_, this->failed_commands_, refresh_due);
  this->publish_sensor_(this->link_losses_sensor_, this->bus_.link_losses(), refresh_due);
#endif
}

void HisenseAC::publish_diagnostics_() {
  AcFaults faults;
  if (this->bus_.faults(&faults)) {
    uint32_t bitmap = faults_to_bitmap32(faults);
    if (bitmap != this->logged_faults_) {
      if (bitmap != 0) {
        ESP_LOGW(TAG, "A/C reports a fault: bitmap 0x%08" PRIX32, bitmap);
      } else {
        ESP_LOGI(TAG, "A/C fault cleared");
      }
      this->logged_faults_ = bitmap;
    }
#ifdef USE_BINARY_SENSOR
    if (this->problem_binary_sensor_ != nullptr)
      this->problem_binary_sensor_->publish_state(faults.any);
    for (auto &entry : this->fault_sensors_)
      entry.second->publish_state((bitmap >> entry.first) & 1u);
#endif
  }

  // Capabilities answer once, after the 0x66/40 ProductType exchange, and never change.
  AcFeatures features;
  if (this->features_published_ || !this->bus_.features(&features) || !features.valid)
    return;
  this->features_published_ = true;
  uint32_t bitmap = features_to_bitmap32(features);
  ESP_LOGD(TAG, "A/C capabilities: bitmap 0x%08" PRIX32, bitmap);
#ifdef USE_BINARY_SENSOR
  for (auto &entry : this->capability_sensors_)
    entry.second->publish_state((bitmap >> entry.first) & 1u);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->link_token_text_sensor_ != nullptr) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    this->bus_.link_token(&hi, &lo);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02X %02X", hi, lo);
    this->link_token_text_sensor_->publish_state(buf);
  }
#endif
}

// ---- Commands ---------------------------------------------------------------------------------
bool HisenseAC::send_frame_(const uint8_t *frame, size_t len) {
  // Builders write the buzzer bit set, as the stock module does by default.
  if (this->beeper_)
    return this->bus_.enqueue(frame, len);
  uint8_t quiet[CMD_FRAME_MAX];
  size_t n = stamp_beep(frame, len, false, quiet, sizeof(quiet));
  return n != 0 && this->bus_.enqueue(quiet, n);
}

bool HisenseAC::send_command() {
  if (!combined_frame_allowed(this->last_.valid)) {
    ESP_LOGW(TAG, "Command not sent: no status from the unit yet, so the frame would carry defaults");
    return false;
  }
  uint8_t frame[CMD_FRAME_MAX];
  ESP_LOGD(TAG, "TX combined: power_on=%d mode=%d setpoint=%d fan=0x%02X vswing=%d hswing=%d feature=%d",
           this->cmd_.power_on, (int) this->cmd_.mode, (int) this->cmd_.setpoint, (unsigned) this->cmd_.fan,
           (int) this->cmd_.vswing, (int) this->cmd_.hswing, (int) this->cmd_.feature);
  // Stamp the user's standing display preference on every combined frame. Leaving it at
  // NOCHANGE writes 0x00, which real hardware treats as "on".
  this->cmd_.display = this->display_pref_;
  size_t len = build_command(this->cmd_, frame, sizeof(frame));
  if (len == 0) {
    ESP_LOGW(TAG, "Command frame could not be built");
    return false;
  }
  if (!this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "Command frame dropped: TX queue full");
    return false;
  }
  return true;
}

bool HisenseAC::send_power(bool on) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_power_frame(on, frame, sizeof(frame));
  if (len == 0 || !this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "Power %s frame not sent", on ? LOG_STR_LITERAL("on") : LOG_STR_LITERAL("off"));
    return false;
  }
  ESP_LOGD(TAG, "TX power %s", on ? LOG_STR_LITERAL("on") : LOG_STR_LITERAL("off"));
  power_intent_sent(&this->power_intent_, on);
  return true;
}

void HisenseAC::send_user_command(uint16_t fields, bool power_on) {
  // Power and mode in one frame, as the stock module sends them: the unit takes both or neither.
  this->cmd_.power_on = power_on;
  const bool sent = this->send_command();
  this->cmd_.power_on = false;  // one frame only, never part of the shadow
  if (!sent)
    return;
  if (power_on) {
    power_intent_sent(&this->power_intent_, true);
    fields |= CONFIRM_POWER;
  }
  this->track_command_(intent_from_command(this->cmd_, fields));
}

void HisenseAC::send_user_power_off() {
  if (this->send_power(false))
    this->track_command_(intent_power_off());
}

void HisenseAC::track_command_(const CommandIntent &intent) {
  pending_begin(&this->main_pending_, intent, unit_view_from_status(this->last_), millis());
  this->note_user_command();
}

/* Mute and sleep use the MINIMAL single-field frame, which is what the vendor module's generic
 * attribute setter sends: one field set, every other byte left at 0x00, "leave alone".
 *
 * The A/C ignores that frame unless it also carries frame[31] = 0x01, the marker every combined
 * command writes and a zeroed buffer omits. With the marker added in build_single_field(), both
 * attributes work: all four sleep profiles select (sleep_raw 2/4/6/8) and mute engages with
 * fan_raw 0x02.
 *
 * The minimal frame is preferable to patching the combined one, because it leaves mode,
 * setpoint, fan and swing alone instead of re-asserting the shadow on every mute or sleep. */
bool HisenseAC::send_mute(bool on) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_mute_frame(on, frame, sizeof(frame));
  if (len == 0 || !this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "Mute frame not sent");
    return false;
  }
  return true;
}

bool HisenseAC::send_sleep(uint8_t profile) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_sleep_frame(profile, frame, sizeof(frame));
  if (len == 0 || !this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "Sleep frame not sent");
    return false;
  }
  return true;
}

// ---- Confirm and retry ------------------------------------------------------------------------
// Every decision here is a function in hisense_map.h with a host test (test_esphome_confirm.cpp).
// This code only carries the decisions out.
void HisenseAC::confirm_commands_(const AcState &state) {
  const uint32_t now = millis();
  const UnitView view = unit_view_from_status(state);
  const SpecialState wanted = special_wanted(this->projected_, this->special_queue_, this->special_len_);
  this->confirm_main_(view, wanted, now, state.temp_unit_f);
  this->confirm_special_(view, wanted, now);
}

void HisenseAC::confirm_main_(const UnitView &view, const SpecialState &wanted, uint32_t now, bool panel_f) {
  PendingCommand &p = this->main_pending_;
  if (!p.active)
    return;
  uint16_t unmet = 0;
  switch (confirm_decision(p, view, wanted, now, COMMAND_RESEND_MAX, &unmet)) {
    case CONFIRM_WAIT:
      return;
    case CONFIRM_DONE:
      if (p.resends > 0) {
        ESP_LOGI(TAG, "Command taken by the unit after %u re-send(s)", p.resends);
      } else {
        ESP_LOGD(TAG, "Command confirmed by the unit's status");
      }
      p.active = false;
      return;
    case CONFIRM_YIELD:
      ESP_LOGD(TAG, "Unit was changed from elsewhere (fields 0x%03X), command not sent again", unmet);
      p.active = false;
      return;
    case CONFIRM_GIVE_UP:
      ESP_LOGW(TAG, "Command not taken by the unit after %u re-send(s) (fields 0x%03X), showing what it reports",
               p.resends, unmet);
      this->failed_commands_++;
      p.active = false;
      return;
    case CONFIRM_RESEND:
      break;
  }
  p.resends++;
  this->command_retries_++;
  ESP_LOGW(TAG, "Command not taken by the unit (fields 0x%03X), sending it again (%u of %u)", unmet, p.resends,
           COMMAND_RESEND_MAX);
  intent_to_command(p.intent, panel_f, &this->cmd_);
  switch (resend_frames(p.intent, unmet)) {
    case RESEND_POWER_OFF:
      this->send_power(false);
      break;
    case RESEND_POWER_ON_ALONE:
      this->send_power(true);
      break;
    case RESEND_POWER_ON_PAIR:
      this->send_power(true);
      this->send_command();
      break;
    case RESEND_COMBINED:
      this->send_command();
      break;
  }
  p.sent_ms = now;
  this->note_user_command();
}

void HisenseAC::confirm_special_(const UnitView &view, const SpecialState &wanted, uint32_t now) {
  PendingCommand &p = this->special_pending_;
  if (!p.active)
    return;
  uint16_t unmet = 0;
  switch (confirm_decision(p, view, wanted, now, SPECIAL_RESEND_MAX, &unmet)) {
    case CONFIRM_WAIT:
      return;
    case CONFIRM_DONE:
      if (p.resends > 0) {
        ESP_LOGI(TAG, "Special-mode command taken by the unit after %u re-send(s)", p.resends);
      }
      p.active = false;
      return;
    case CONFIRM_YIELD:
      ESP_LOGD(TAG, "Special mode was changed from elsewhere (fields 0x%03X), command not sent again", unmet);
      p.active = false;
      return;
    case CONFIRM_GIVE_UP:
      ESP_LOGW(TAG, "Special-mode command not taken by the unit after %u re-send(s) (fields 0x%03X)", p.resends, unmet);
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
  ESP_LOGW(TAG, "Special-mode command not taken by the unit (fields 0x%03X), %u command(s) queued again", unmet,
           (unsigned) n);
  this->projected_ = view.special;
  this->special_len_ = 0;
  for (size_t i = 0; i < n; i++)
    this->push_special_(ops[i]);
  this->note_user_command();
}

void HisenseAC::drop_pending_commands_() {
  unsigned dropped = (this->main_pending_.active ? 1 : 0) + (this->special_pending_.active ? 1 : 0);
  if (this->special_len_ > 0)
    dropped++;
  this->main_pending_.active = false;
  this->special_pending_.active = false;
  this->special_len_ = 0;
  if (dropped == 0)
    return;
  this->failed_commands_ += dropped;
  ESP_LOGW(TAG, "%u command(s) unconfirmed or unsent when the link dropped, not sent again", dropped);
}

// ---- Special-mode queue -----------------------------------------------------------------------
void HisenseAC::push_special_(const SpecialOp &op) {
  if (this->special_len_ >= SPECIAL_QUEUE_CAP) {
    ESP_LOGW(TAG, "Special-mode queue full, op %u dropped", op.kind);
    return;
  }
  this->special_queue_[this->special_len_++] = op;
  // Held from the moment of the request, not the send, so a readback that predates it cannot
  // flip the entity back while the op waits out the settle.
  this->note_user_command();
}

void HisenseAC::enqueue_special(const SpecialOp &op) {
  this->special_resends_ = 0;  // a new request from the user: a fresh allowance of re-sends
  this->push_special_(op);
}

void HisenseAC::request_preset(uint8_t target) {
  // Plan from where the unit will be once everything already SENT lands. Queued-but-unsent ops
  // are discarded: the new preset fully determines all four modes, so they would only add
  // settle waits (or undo each other). A write already sent and not yet confirmed stays under
  // watch: if the unit did not take it, the plan is made again from what it reports.
  SpecialOp ops[PRESET_PLAN_MAX];
  size_t n = preset_plan(this->projected_, target, ops);
  this->special_len_ = 0;
  this->special_resends_ = 0;
  ESP_LOGD(TAG, "Preset %s: %u command(s) queued", PRESETS[target].name, (unsigned) n);
  for (size_t i = 0; i < n; i++)
    this->push_special_(ops[i]);
  this->note_user_command();
}

void HisenseAC::drain_special_queue_() {
  if (this->special_len_ == 0)
    return;
  // One write at a time: the next waits until the unit's status has answered for the last.
  if (this->special_pending_.active)
    return;
  if (this->special_sent_ && millis() - this->last_special_ms_ < SPECIAL_SETTLE_MS)
    return;
  SpecialOp op = this->special_queue_[0];
  for (uint8_t i = 1; i < this->special_len_; i++)
    this->special_queue_[i - 1] = this->special_queue_[i];
  this->special_len_--;
  const bool sent = this->execute_special_(op);
  special_apply(&this->projected_, op);
  this->last_special_ms_ = millis();
  this->special_sent_ = true;
  this->note_user_command();
  if (sent) {
    pending_begin(&this->special_pending_, intent_from_special(op), unit_view_from_status(this->last_),
                  this->last_special_ms_);
    this->special_pending_.resends = this->special_resends_;
  }
}

bool HisenseAC::execute_special_(const SpecialOp &op) {
  switch (op.kind) {
    case SPECIAL_OP_FEATURE: {
      ESP_LOGD(TAG, "TX special: byte33 feature %u", op.value);
      this->cmd_.feature = (Feature) op.value;
      const bool sent = this->send_command();
      this->cmd_.feature = feature_after_send(this->cmd_.feature);  // ECO_OFF is one-shot
      return sent;
    }
    case SPECIAL_OP_MUTE:
      ESP_LOGD(TAG, "TX special: mute %u", op.value);
      return this->send_mute(op.value != 0);
    case SPECIAL_OP_SLEEP:
      ESP_LOGD(TAG, "TX special: sleep profile %u", op.value);
      return this->send_sleep(op.value);
    default:
      return false;
  }
}

void HisenseAC::dump_config() {
  ESP_LOGCONFIG(TAG, "Hisense A/C (RS-485 9600 8N1)");
  LOG_PIN("  DE pin: ", this->de_pin_);
  if (this->de_pin_ == nullptr) {
    ESP_LOGCONFIG(TAG, "  DE: owned by the UART (flow_control_pin) or the transceiver");
  }
  this->check_uart_settings(BUS_BAUD_RATE, 1, uart::UART_CONFIG_PARITY_NONE, 8);
  ESP_LOGCONFIG(TAG, "  link: %s", this->link_up_ ? LOG_STR_LITERAL("up") : LOG_STR_LITERAL("down"));
  if (this->last_.valid) {
    ESP_LOGCONFIG(TAG, "  last status: power=%d mode=%d setpoint=%dC indoor=%dC", this->last_.power_on,
                  (int) this->last_.mode, this->last_.setpoint_c, this->last_.indoor_temp_c);
  } else {
    ESP_LOGCONFIG(TAG, "  no status frame decoded yet");
  }
}

// ---- BusIO ------------------------------------------------------------------------------------
void HisenseAC::bus_set_de(bool high) {
  if (this->de_pin_ != nullptr)
    this->de_pin_->digital_write(high);
}
void HisenseAC::bus_write(const uint8_t *data, size_t len) {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERY_VERBOSE
  log_frame("TX", data, len);
#endif
  this->write_array(data, len);
}
void HisenseAC::bus_flush() { this->flush(); }
int HisenseAC::bus_read() {
#ifdef USE_LIBRETINY
  // LibreTiny's Serial keeps received bytes in the Arduino RingBufferN, whose reader decrements
  // its element count with the receive interrupt enabled. An interrupt that lands inside that
  // decrement stores a byte and the count loses it: from then on the byte sits in the buffer
  // uncounted and every frame arrives short by one byte, late by one byte, which the assembler
  // can only time out on. Measured on an RTL8710C with the UART in loopback at 9600 baud, read
  // in groups as this loop reads: 5 such losses in 233034 bytes without this lock, none in
  // 233287 with it (firmware/docs/15-esphome-path.md has the runs). The lock covers one buffered
  // byte, a few microseconds.
  InterruptLock lock;
#endif
  uint8_t b;
  if (this->available() > 0 && this->read_byte(&b))
    return b;
  return -1;
}

}  // namespace esphome::hisense_ac
