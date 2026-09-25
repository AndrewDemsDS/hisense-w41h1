#include "hisense_ac.h"
#include "hisense_climate.h"
#include "esphome/core/log.h"

namespace esphome::hisense_ac {

static const char *const TAG = "hisense_ac";

// ---- Bus results ------------------------------------------------------------------------------
// With the uart transport these run inside bus_.poll() on the main loop. The legacy transport
// calls them from its own FreeRTOS task, so entities are never touched here: the frame is parked
// under the lock and loop() publishes it.
void HisenseAC::on_bus_status(const AcState &state) {
  LockGuard guard(this->lock_);
  this->pending_ = state;
  this->pending_valid_ = true;
}

void HisenseAC::on_bus_features(const AcFeatures &features) {}  // polled in publish_diagnostics_

void HisenseAC::on_bus_link(bool up) {
  if (!up) {
    // Drop any undrained pre-loss frame, or loop() would publish those stale values (and flip
    // link_up_ back to true) until real data returns. Port of the Matter fix 0d3b9ff.
    LockGuard guard(this->lock_);
    this->pending_valid_ = false;
  }
  this->link_up_ = up;
  this->link_dirty_ = true;
}

void HisenseAC::setup() {
  if (!this->transport_start_()) {
    ESP_LOGE(TAG, "bus transport failed to start: no A/C control");
    this->mark_failed();
  }
}

void HisenseAC::loop() {
#ifndef USE_HISENSE_AC_LEGACY_DRIVER
  this->bus_.poll(millis());
  // DE settle and drain are a few ms each; the default ~16 ms loop would stretch them, so ask for
  // a tight loop only while they run.
  if (this->bus_.wants_fast_loop()) {
    this->fast_loop_.start();
  } else {
    this->fast_loop_.stop();
  }
#endif

  AcState state;
  bool have_state = false;
  {
    LockGuard guard(this->lock_);
    if (this->pending_valid_) {
      state = this->pending_;
      this->pending_valid_ = false;
      have_state = true;
    }
  }

  this->drain_special_queue_();

  if (this->link_dirty_) {
    this->link_dirty_ = false;
    ESP_LOGW(TAG, "A/C bus link %s", this->link_up_ ? LOG_STR_LITERAL("restored") : LOG_STR_LITERAL("LOST"));
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
  // false forever, reporting a working bus as down. Seen on a real A/C 2026-08-19.
  this->link_up_ = true;
  ESP_LOGD(TAG, "RX status: power=%d mode=%d setpoint=%d indoor=%d fan_raw=0x%02X holdoff=%d", state.power_on,
           (int) state.mode, state.setpoint_c, state.indoor_temp_c, state.fan_raw, this->in_command_holdoff());
  // Edge-logged at INFO because these two are how the A/C answers a mute or sleep command, and
  // a command that is accepted-then-ignored looks identical to one that was never sent unless
  // you can see the raw byte move. Silent in steady state.
  if (this->last_.valid && state.sleep_raw != this->last_.sleep_raw) {
    ESP_LOGI(TAG, "A/C sleep_raw %u -> %u", this->last_.sleep_raw, state.sleep_raw);
  }
  if (this->last_.valid && state.mute_on != this->last_.mute_on) {
    ESP_LOGI(TAG, "A/C mute %d -> %d (fan_raw 0x%02X)", this->last_.mute_on, state.mute_on, state.fan_raw);
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
    // Validated in the wire unit: a raw copy dropped the F flag and let an out-of-range report
    // poison the shadow, which silently killed every later Cool/Heat/Auto frame (#117).
    if (!sync_shadow_setpoint(state.setpoint_c, state.temp_unit_f, &this->cmd_)) {
      ESP_LOGD(TAG, "status setpoint %d C out of command range, shadow kept", state.setpoint_c);
    }
    this->cmd_.vswing = state.vswing_on ? SWING_MODE_SWING : SWING_MODE_OFF;
    this->cmd_.hswing = state.hswing_on ? SWING_MODE_SWING : SWING_MODE_OFF;
    // Without this a Turbo/Eco set from HA re-asserted on every later combined frame, even
    // after the remote cleared it. Ported from the esp32 Matter sync.
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

void HisenseAC::publish_telemetry_(const AcState &state) {
#ifdef USE_SENSOR
  if (this->indoor_temperature_sensor_ != nullptr)
    this->indoor_temperature_sensor_->publish_state(state.indoor_temp_c);
  if (this->outdoor_temperature_sensor_ != nullptr)
    this->outdoor_temperature_sensor_->publish_state(state.outdoor_temp_c);
  if (this->coil_temperature_sensor_ != nullptr)
    this->coil_temperature_sensor_->publish_state(state.coil_temp_c);
  if (this->compressor_frequency_sensor_ != nullptr)
    this->compressor_frequency_sensor_->publish_state(state.compressor_freq);
  // The bus carries a current PROXY, not amps: active power is 4.15 * raw^2, calibrated against
  // a panel meter. hisense_map.h owns that maths and works in milli-units, so scale back here.
  if (this->power_sensor_ != nullptr)
    this->power_sensor_->publish_state(active_power_mw(state.current_raw) / 1000.0f);
  if (this->voltage_sensor_ != nullptr)
    this->voltage_sensor_->publish_state(voltage_mv(state.voltage_raw) / 1000.0f);
  if (this->current_sensor_ != nullptr)
    this->current_sensor_->publish_state(active_current_ma(state.current_raw, state.voltage_raw) / 1000.0f);
  if (this->checksum_errors_sensor_ != nullptr)
    this->checksum_errors_sensor_->publish_state(this->transport_checksum_mismatches_());
#endif
#ifdef USE_BINARY_SENSOR
  if (this->aux_heat_binary_sensor_ != nullptr)
    this->aux_heat_binary_sensor_->publish_state(state.heat_relay_on);
  if (this->bus_link_binary_sensor_ != nullptr)
    this->bus_link_binary_sensor_->publish_state(this->link_up_);
#endif
}

void HisenseAC::publish_diagnostics_() {
#ifdef USE_BINARY_SENSOR
  AcFaults faults;
  if (this->transport_faults_(&faults)) {
    uint32_t bitmap = faults_to_bitmap32(faults);
    if (this->problem_binary_sensor_ != nullptr)
      this->problem_binary_sensor_->publish_state(faults.any);
    for (auto &entry : this->fault_sensors_)
      entry.second->publish_state((bitmap >> entry.first) & 1u);
  }
#endif

  // Capabilities answer once, after the 0x66/40 ProductType exchange, and never change.
  AcFeatures features;
  if (this->features_published_ || !this->transport_features_(&features) || !features.valid)
    return;
  this->features_published_ = true;
#ifdef USE_BINARY_SENSOR
  uint32_t bitmap = features_to_bitmap32(features);
  for (auto &entry : this->capability_sensors_)
    entry.second->publish_state((bitmap >> entry.first) & 1u);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->link_token_text_sensor_ != nullptr) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    this->transport_link_token_(&hi, &lo);
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
    ESP_LOGW(TAG, "command frame build failed");
    return;
  }
  if (!this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "command frame dropped: TX queue full");
  }
}

void HisenseAC::tx_override(int offset, int value) {
  uint8_t frame[CMD_FRAME_MAX];
  this->cmd_.display = this->display_pref_;
  size_t len = build_command_override(this->cmd_, frame, sizeof(frame), offset, (uint8_t) value);
  if (len == 0) {
    ESP_LOGW(TAG, "tx_override rejected: offset %d out of the payload range", offset);
    return;
  }
  ESP_LOGI(TAG, "tx_override: byte %d = 0x%02X", offset, (unsigned) value);
  if (!this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "tx_override frame dropped: TX queue full");
  }
}

void HisenseAC::tx_override2(int off1, int val1, int off2, int val2) {
  uint8_t frame[CMD_FRAME_MAX];
  this->cmd_.display = this->display_pref_;
  size_t len = build_command_override(this->cmd_, frame, sizeof(frame), off1, (uint8_t) val1, off2, (uint8_t) val2);
  if (len == 0 || off2 < 0) {
    ESP_LOGW(TAG, "tx_override2 rejected: offsets %d/%d out of range", off1, off2);
    return;
  }
  ESP_LOGI(TAG, "tx_override2: byte %d = 0x%02X, byte %d = 0x%02X", off1, (unsigned) val1, off2, (unsigned) val2);
  if (!this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "tx_override2 frame dropped: TX queue full");
  }
}

void HisenseAC::tx_single(int offset, int value) {
  // Deliberately NOT the combined frame: this is the shape a generic attribute setter would
  // send, one field set and every other byte left at 0x00 ("leave alone").
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = (offset == 17) ? build_sleep_frame((uint8_t) ((value - 1) / 2), frame, sizeof(frame))
                              : build_mute_frame(value == 0x30, frame, sizeof(frame));
  ESP_LOGI(TAG, "tx_single: byte %d = 0x%02X (len %u)", offset, (unsigned) value, (unsigned) len);
  if (len == 0 || !this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "tx_single frame not sent");
  }
}

void HisenseAC::send_power(bool on) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_power_frame(on, frame, sizeof(frame));
  if (len == 0 || !this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "power %s frame not sent", on ? LOG_STR_LITERAL("on") : LOG_STR_LITERAL("off"));
  }
}

/* Mute and sleep use the MINIMAL single-field frame, which is what the stock module's generic
 * attribute setter sends: one field set, every other byte left at 0x00, "leave alone".
 *
 * That frame was ignored by the A/C until 2026-08-19, when the cause turned out to be a single
 * missing byte: frame[31] = 0x01, which every combined command writes and the zeroed buffer
 * omitted. With the marker added in build_single_field(), both attributes work: all four sleep
 * profiles select (sleep_raw 2/4/6/8) and mute engages with fan_raw 0x02.
 *
 * The minimal frame is preferable to patching the combined one, because it leaves mode,
 * setpoint, fan and swing alone instead of re-asserting the shadow on every mute or sleep. */
void HisenseAC::send_mute(bool on) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_mute_frame(on, frame, sizeof(frame));
  if (len == 0 || !this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "mute frame not sent");
  }
}

void HisenseAC::send_sleep(uint8_t profile) {
  uint8_t frame[CMD_FRAME_MAX];
  size_t len = build_sleep_frame(profile, frame, sizeof(frame));
  if (len == 0 || !this->send_frame_(frame, len)) {
    ESP_LOGW(TAG, "sleep frame not sent");
  }
}

// ---- Special-mode queue -----------------------------------------------------------------------
void HisenseAC::enqueue_special(const SpecialOp &op) {
  if (this->special_len_ >= SPECIAL_QUEUE_CAP) {
    ESP_LOGW(TAG, "special-mode queue full, op %u dropped", op.kind);
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
  ESP_LOGD(TAG, "preset -> %s: %u op(s)", PRESETS[target].name, (unsigned) n);
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
#ifdef USE_HISENSE_AC_LEGACY_DRIVER
  ESP_LOGCONFIG(TAG, "  transport: legacy driver task (pins in the boot log)");
#else
  ESP_LOGCONFIG(TAG, "  transport: uart, scheduler in loop()");
  LOG_PIN("  DE pin: ", this->de_pin_);
  if (this->de_pin_ == nullptr) {
    ESP_LOGCONFIG(TAG, "  DE: owned by the UART (flow_control_pin) or the transceiver");
  }
  this->check_uart_settings(BUS_BAUD_RATE, 1, uart::UART_CONFIG_PARITY_NONE, 8);
#endif
  ESP_LOGCONFIG(TAG, "  link: %s", this->link_up_ ? LOG_STR_LITERAL("up") : LOG_STR_LITERAL("down"));
  if (this->last_.valid) {
    ESP_LOGCONFIG(TAG, "  last status: power=%d mode=%d setpoint=%dC indoor=%dC", this->last_.power_on,
                  (int) this->last_.mode, this->last_.setpoint_c, this->last_.indoor_temp_c);
  } else {
    ESP_LOGCONFIG(TAG, "  no status frame decoded yet");
  }
}

// ---- uart transport ---------------------------------------------------------------------------
#ifndef USE_HISENSE_AC_LEGACY_DRIVER
bool HisenseAC::transport_start_() {
  if (this->de_pin_ != nullptr) {
    this->de_pin_->setup();
    this->de_pin_->digital_write(false);  // idle low = receive, before the UART speaks
  }
  this->bus_.setup(this, this, this->de_pin_ != nullptr);
  return true;
}

bool HisenseAC::send_frame_(const uint8_t *frame, size_t len) { return this->bus_.enqueue(frame, len); }
bool HisenseAC::transport_faults_(AcFaults *out) { return this->bus_.faults(out); }
bool HisenseAC::transport_features_(AcFeatures *out) { return this->bus_.features(out); }
void HisenseAC::transport_link_token_(uint8_t *hi, uint8_t *lo) { this->bus_.link_token(hi, lo); }
uint32_t HisenseAC::transport_checksum_mismatches_() { return this->bus_.checksum_mismatches(); }

void HisenseAC::bus_set_de(bool high) {
  if (this->de_pin_ != nullptr)
    this->de_pin_->digital_write(high);
}
void HisenseAC::bus_write(const uint8_t *data, size_t len) { this->write_array(data, len); }
void HisenseAC::bus_flush() { this->flush(); }
int HisenseAC::bus_read() {
  uint8_t b;
  if (this->available() > 0 && this->read_byte(&b))
    return b;
  return -1;
}
#endif

}  // namespace esphome::hisense_ac
