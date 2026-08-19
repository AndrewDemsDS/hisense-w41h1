#include "hisense_ac.h"
#include <FreeRTOS.h>  // pdPASS
#include "hisense_climate.h"
#include "esphome/core/log.h"

namespace esphome {
namespace hisense_ac {

static const char *const TAG = "hisense_ac";

HisenseAC *HisenseAC::instance_ = nullptr;

void HisenseAC::status_trampoline(const HisenseState *state) {
  if (HisenseAC::instance_ != nullptr)
    HisenseAC::instance_->on_status_isr_(state);
}

void HisenseAC::link_trampoline(bool link_up) {
  if (HisenseAC::instance_ == nullptr)
    return;
  HisenseAC::instance_->link_up_ = link_up;
  HisenseAC::instance_->link_dirty_ = true;
}

void HisenseAC::on_status_isr_(const HisenseState *state) {
  if (state == nullptr)
    return;
  LockGuard guard(this->lock_);
  this->pending_ = *state;
  this->pending_valid_ = true;
}

void HisenseAC::setup() {
  HisenseAC::instance_ = this;

  // Shadow defaults mirror the esp-matter build's: a combined frame always carries every
  // field, so the shadow must start somewhere sane rather than at all-zero (mode FAN, 0 C).
  this->cmd_.mode = HISENSE_MODE_COOL;
  this->cmd_.setpoint = 24;
  this->cmd_.fahrenheit = false;
  this->cmd_.fan = HISENSE_FAN_AUTO;
  this->cmd_.vswing = HISENSE_SWING_OFF;
  this->cmd_.hswing = HISENSE_SWING_OFF;
  this->cmd_.feature = HISENSE_FEATURE_NONE;
  this->cmd_.display = HISENSE_DISPLAY_NOCHANGE;

  hisense_set_link_cb(&HisenseAC::link_trampoline);
  if (hisense_init(&HisenseAC::status_trampoline) != pdPASS) {
    ESP_LOGE(TAG, "hisense_init() failed: no bus task, no A/C control");
    this->mark_failed();
    return;
  }
  ESP_LOGCONFIG(TAG, "RS-485 bus task started (TX=%d RX=%d DE=%d)", PA_14, PA_13, PA_17);
}

void HisenseAC::loop() {
  HisenseState state;
  bool have_state = false;
  {
    LockGuard guard(this->lock_);
    if (this->pending_valid_) {
      state = this->pending_;
      this->pending_valid_ = false;
      have_state = true;
    }
  }

  if (this->link_dirty_) {
    this->link_dirty_ = false;
    ESP_LOGW(TAG, "A/C bus link %s", this->link_up_ ? "restored" : "LOST");
  }

  if (!have_state)
    return;
  // A decoded frame IS the link being up. The driver's link callback only fires on EDGES, so a
  // link that is healthy from boot never produces a "restored" edge and link_up_ would sit at
  // its initial false forever, reporting a working bus as down. Seen on a real A/C 2026-08-19.
  this->link_up_ = true;
  ESP_LOGD(TAG, "RX status: power=%d mode=%d setpoint=%d indoor=%d fan_raw=0x%02X holdoff=%d",
           state.power_on, (int) state.mode, state.setpoint_c, state.indoor_temp_c, state.fan_raw,
           this->in_command_holdoff());
  this->last_ = state;

  // Keep the command shadow tracking reality, so a later single-field write rebuilds the
  // combined frame from what the A/C is actually doing instead of a stale shadow. Skipped
  // during the hold-off, when `state` may predate the user's own command.
  if (!this->in_command_holdoff()) {
    HisenseFanSpeed fan = hisense_fan_raw_to_cmd(state.fan_raw);
    if (fan != HISENSE_FAN_NOCHANGE)
      this->cmd_.fan = fan;
    this->cmd_.mode = state.mode;
    this->cmd_.setpoint = state.setpoint_c;
    this->cmd_.vswing = state.vswing_on ? HISENSE_SWING_SWING : HISENSE_SWING_OFF;
    this->cmd_.hswing = state.hswing_on ? HISENSE_SWING_SWING : HISENSE_SWING_OFF;
  }

  if (this->climate_ != nullptr)
    this->climate_->update_from_bus(state, this->in_command_holdoff());
  for (auto *listener : this->listeners_)
    listener->on_status(state);

  this->publish_telemetry_(state);
  this->publish_diagnostics_();
}

void HisenseAC::publish_telemetry_(const HisenseState &state) {
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
  // a panel meter. power_estimate.h owns that maths and works in milli-units, so scale back here.
  if (this->power_sensor_ != nullptr)
    this->power_sensor_->publish_state(hisense_active_power_mw(state.current_raw) / 1000.0f);
  if (this->voltage_sensor_ != nullptr)
    this->voltage_sensor_->publish_state(hisense_voltage_mv(state.voltage_raw) / 1000.0f);
  if (this->current_sensor_ != nullptr)
    this->current_sensor_->publish_state(
        hisense_active_current_ma(state.current_raw, state.voltage_raw) / 1000.0f);
  if (this->checksum_errors_sensor_ != nullptr)
    this->checksum_errors_sensor_->publish_state(hisense_checksum_mismatch_count());
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
  HisenseFaults faults;
  if (hisense_get_faults(&faults)) {
    uint32_t bitmap = hisense_faults_to_bitmap32(&faults);
    if (this->problem_binary_sensor_ != nullptr)
      this->problem_binary_sensor_->publish_state(faults.any);
    for (auto &entry : this->fault_sensors_)
      entry.second->publish_state((bitmap >> entry.first) & 1u);
  }
#endif

  // Capabilities answer once, after the 0x66/40 ProductType exchange, and never change.
  HisenseFeatures features;
  if (this->features_published_ || !hisense_get_features(&features) || !features.valid)
    return;
  this->features_published_ = true;
#ifdef USE_BINARY_SENSOR
  uint32_t bitmap = hisense_features_to_bitmap32(&features);
  for (auto &entry : this->capability_sensors_)
    entry.second->publish_state((bitmap >> entry.first) & 1u);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->link_token_text_sensor_ != nullptr) {
    uint8_t hi = 0, lo = 0;
    hisense_get_link_token(&hi, &lo);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02X %02X", hi, lo);
    this->link_token_text_sensor_->publish_state(buf);
  }
#endif
}

void HisenseAC::send_command() {
  uint8_t frame[HISENSE_CMD_FRAME_LEN + 2];
  ESP_LOGD(TAG, "TX combined: mode=%d setpoint=%d fan=0x%02X vswing=%d hswing=%d feature=%d",
           (int) this->cmd_.mode, (int) this->cmd_.setpoint, (unsigned) this->cmd_.fan,
           (int) this->cmd_.vswing, (int) this->cmd_.hswing, (int) this->cmd_.feature);
  // Stamp the user's standing display preference on every combined frame. Leaving it at
  // NOCHANGE writes 0x00, which real hardware treats as "on".
  this->cmd_.display = this->display_pref_;
  size_t len = hisense_build_command(&this->cmd_, frame, sizeof(frame));
  if (len == 0) {
    ESP_LOGW(TAG, "command frame build failed");
    return;
  }
  if (!hisense_send_frame(frame, len))
    ESP_LOGW(TAG, "command frame dropped: TX queue full");
}

/// The single-field frames (mute, sleep) are built from a zeroed buffer, so they also write
/// byte 36 = 0x00 and re-light the panel. Re-assert only when the user wants it OFF: when the
/// preference is ON, the 0x00 those frames send already agrees and costs nothing.
void HisenseAC::reassert_display_if_off() {
  if (this->display_pref_ == HISENSE_DISPLAY_OFF)
    this->send_command();
}

void HisenseAC::send_power(bool on) {
  uint8_t frame[HISENSE_CMD_FRAME_LEN + 2];
  size_t len = hisense_build_power_frame(on, frame, sizeof(frame));
  if (len == 0 || !hisense_send_frame(frame, len))
    ESP_LOGW(TAG, "power %s frame not sent", on ? "on" : "off");
}

void HisenseAC::send_mute(bool on) {
  uint8_t frame[HISENSE_CMD_FRAME_LEN + 2];
  size_t len = hisense_build_mute_frame(on, frame, sizeof(frame));
  if (len == 0 || !hisense_send_frame(frame, len))
    ESP_LOGW(TAG, "mute frame not sent");
  this->reassert_display_if_off();
}

void HisenseAC::send_sleep(uint8_t profile) {
  uint8_t frame[HISENSE_CMD_FRAME_LEN + 2];
  size_t len = hisense_build_sleep_frame(profile, frame, sizeof(frame));
  if (len == 0 || !hisense_send_frame(frame, len))
    ESP_LOGW(TAG, "sleep frame not sent");
  this->reassert_display_if_off();
}

void HisenseAC::dump_config() {
  ESP_LOGCONFIG(TAG, "Hisense A/C (RS-485 9600 8N1)");
  ESP_LOGCONFIG(TAG, "  TX=%d  RX=%d  DE=%d", PA_14, PA_13, PA_17);
  ESP_LOGCONFIG(TAG, "  link: %s", this->link_up_ ? "up" : "down");
  if (this->last_.valid) {
    ESP_LOGCONFIG(TAG, "  last status: power=%d mode=%d setpoint=%dC indoor=%dC", this->last_.power_on,
                  (int) this->last_.mode, this->last_.setpoint_c, this->last_.indoor_temp_c);
  } else {
    ESP_LOGCONFIG(TAG, "  no status frame decoded yet");
  }
}

}  // namespace hisense_ac
}  // namespace esphome
