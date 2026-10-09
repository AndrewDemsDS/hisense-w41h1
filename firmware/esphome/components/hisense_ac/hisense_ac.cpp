#include "hisense_ac.h"
#include "hisense_climate.h"
#include "esphome/core/log.h"

#include <cinttypes>

namespace esphome::hisense_ac {

static const char *const TAG = "hisense_ac";

// What each log level shows:
//   WARN          the bus link dropping, a frame that could not be sent, a bad checksum, a fault
//   INFO          the bus link coming back, a fault clearing
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

void HisenseAC::on_bus_link(bool up) {
  if (!up) {
    // Drop any undrained pre-loss frame, or loop() would publish those stale values (and flip
    // link_up_ back to true) until real data returns.
    this->pending_valid_ = false;
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

  // Keep the command shadow tracking reality, so a later single-field write rebuilds the
  // combined frame from what the A/C is actually doing instead of a stale shadow. Skipped
  // during the hold-off, when `state` may predate the user's own command.
  if (!this->in_command_holdoff()) {
    FanSpeed fan = fan_raw_to_cmd(state.fan_raw);
    if (fan != FAN_SPEED_NOCHANGE)
      this->cmd_.fan = fan;
    this->cmd_.mode = state.mode;
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
  this->publish_sensor_(this->outdoor_temperature_sensor_, state.outdoor_temp_c, refresh);
  this->publish_sensor_(this->coil_temperature_sensor_, state.coil_temp_c, refresh);
  this->publish_sensor_(this->compressor_frequency_sensor_, state.compressor_freq, refresh);
  // The bus carries a current PROXY, not amps: active power is 4.15 * raw^2, calibrated against
  // a panel meter. hisense_map.h owns that maths and works in milli-units, so scale back here.
  this->publish_sensor_(this->power_sensor_, active_power_mw(state.current_raw) / 1000.0f, refresh);
  this->publish_sensor_(this->voltage_sensor_, voltage_mv(state.voltage_raw) / 1000.0f, refresh);
  this->publish_sensor_(this->current_sensor_, active_current_ma(state.current_raw, state.voltage_raw) / 1000.0f,
                        refresh);
  this->publish_sensor_(this->checksum_errors_sensor_, this->bus_.checksum_mismatches(), refresh);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->aux_heat_binary_sensor_ != nullptr)
    this->aux_heat_binary_sensor_->publish_state(state.heat_relay_on);
  if (this->bus_link_binary_sensor_ != nullptr)
    this->bus_link_binary_sensor_->publish_state(this->link_up_);
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
void HisenseAC::send_command() {
  uint8_t frame[CMD_FRAME_MAX];
  ESP_LOGD(TAG, "TX combined: mode=%d setpoint=%d fan=0x%02X vswing=%d hswing=%d feature=%d", (int) this->cmd_.mode,
           (int) this->cmd_.setpoint, (unsigned) this->cmd_.fan, (int) this->cmd_.vswing, (int) this->cmd_.hswing,
           (int) this->cmd_.feature);
  // Stamp the user's standing display preference on every combined frame. Leaving it at
  // NOCHANGE writes 0x00, which real hardware treats as "on".
  this->cmd_.display = this->display_pref_;
  size_t len = build_command(this->cmd_, frame, sizeof(frame));
  if (len == 0) {
    ESP_LOGW(TAG, "Command frame could not be built");
    return;
  }
  if (!this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "Command frame dropped: TX queue full");
  }
}

void HisenseAC::send_power(bool on) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_power_frame(on, frame, sizeof(frame));
  if (len == 0 || !this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "Power %s frame not sent", on ? LOG_STR_LITERAL("on") : LOG_STR_LITERAL("off"));
  }
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
void HisenseAC::send_mute(bool on) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_mute_frame(on, frame, sizeof(frame));
  if (len == 0 || !this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "Mute frame not sent");
  }
}

void HisenseAC::send_sleep(uint8_t profile) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_sleep_frame(profile, frame, sizeof(frame));
  if (len == 0 || !this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "Sleep frame not sent");
  }
}

// ---- Special-mode queue -----------------------------------------------------------------------
void HisenseAC::enqueue_special(const SpecialOp &op) {
  if (this->special_len_ >= SPECIAL_QUEUE_CAP) {
    ESP_LOGW(TAG, "Special-mode queue full, op %u dropped", op.kind);
    return;
  }
  this->special_queue_[this->special_len_++] = op;
  // Held from the moment of the request, not the send, so a readback that predates it cannot
  // flip the entity back while the op waits out the settle.
  this->note_user_command();
}

void HisenseAC::request_preset(uint8_t target) {
  // Plan from where the unit will be once everything already SENT lands. Queued-but-unsent ops
  // are discarded: the new preset fully determines all four modes, so they would only add
  // settle waits (or undo each other).
  SpecialOp ops[PRESET_PLAN_MAX];
  size_t n = preset_plan(this->projected_, target, ops);
  this->special_len_ = 0;
  ESP_LOGD(TAG, "Preset %s: %u command(s) queued", PRESETS[target].name, (unsigned) n);
  for (size_t i = 0; i < n; i++)
    this->enqueue_special(ops[i]);
  this->note_user_command();
}

void HisenseAC::drain_special_queue_() {
  if (this->special_len_ == 0)
    return;
  if (this->special_sent_ && millis() - this->last_special_ms_ < SPECIAL_SETTLE_MS)
    return;
  SpecialOp op = this->special_queue_[0];
  for (uint8_t i = 1; i < this->special_len_; i++)
    this->special_queue_[i - 1] = this->special_queue_[i];
  this->special_len_--;
  this->execute_special_(op);
  special_apply(&this->projected_, op);
  this->last_special_ms_ = millis();
  this->special_sent_ = true;
  this->note_user_command();
}

void HisenseAC::execute_special_(const SpecialOp &op) {
  switch (op.kind) {
    case SPECIAL_OP_FEATURE:
      ESP_LOGD(TAG, "TX special: byte33 feature %u", op.value);
      this->cmd_.feature = (Feature) op.value;
      this->send_command();
      this->cmd_.feature = feature_after_send(this->cmd_.feature);  // ECO_OFF is one-shot
      break;
    case SPECIAL_OP_MUTE:
      ESP_LOGD(TAG, "TX special: mute %u", op.value);
      this->send_mute(op.value != 0);
      break;
    case SPECIAL_OP_SLEEP:
      ESP_LOGD(TAG, "TX special: sleep profile %u", op.value);
      this->send_sleep(op.value);
      break;
    default:
      break;
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
  uint8_t b;
  if (this->available() > 0 && this->read_byte(&b))
    return b;
  return -1;
}

}  // namespace esphome::hisense_ac
