#pragma once
// Hub component: owns the shared RS-485 driver and hands its bus-task state to loop().
//
// The driver (../../../src/rs485-driver, registered as a local IDF component) is reused
// UNCHANGED, exactly as the esp-matter build reuses it. This file is the ESPHome analogue of
// esp32-matter's app_main.cpp:
// it only moves decoded values into entities and user intent back onto the bus.
//
// Threading: hisense_init() spawns its own FreeRTOS bus task and fires the status callback from
// it. ESPHome entities must only be published from the main loop task, so the callback copies
// state into a mutex-guarded snapshot and loop() drains it. That is the whole hand-off; no queues.
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

extern "C" {
#include "hisense_rs485.h"
}
#include "esphome_aircon_map.h"
#include "power_estimate.h"

namespace esphome {
namespace hisense_ac {

class HisenseClimate;

/// Anything that wants a copy of each decoded status frame.
class StatusListener {
 public:
  virtual void on_status(const HisenseState &state) = 0;
};

class HisenseAC : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_climate(HisenseClimate *climate) { this->climate_ = climate; }
  void add_status_listener(StatusListener *listener) { this->listeners_.push_back(listener); }

  /// Build and send the combined command frame from the current shadow.
  void send_command();
  /// Power is a separate, byte-for-byte ported frame, never part of the combined command.
  void send_power(bool on);
  void send_mute(bool on);
  void send_sleep(uint8_t profile);

  /// Panel display is STICKY, not one-shot. Byte 36 rides every frame, and 0x00 ("no change")
  /// turns the panel ON on real hardware, so a command that does not state a preference
  /// re-lights a display the user switched off. Confirmed on an A/C 2026-08-19; only 0x40
  /// (off) and 0xC0 (on) were ever bench-confirmed, 0x00 was an assumption.
  void set_display_pref(bool on) { this->display_pref_ = on ? HISENSE_DISPLAY_ON : HISENSE_DISPLAY_OFF; }
  bool display_pref_on() const { return this->display_pref_ != HISENSE_DISPLAY_OFF; }

  /// BENCH PROBE. Send the current combined frame with exactly ONE pre-checksum byte replaced.
  /// Every other byte stays at the shadow's known-good state, which is what makes an offset
  /// sweep safe to run against a live A/C: a failed probe is a no-op rather than a surprise
  /// mode or setpoint change. The ESPHome analogue of the Matter build's `tx` diag command
  /// (#52). Offsets outside the payload are rejected by the driver.
  void tx_override(int offset, int value);
  /// Two-byte variant: some controls are not a single field write (sleep + mute together).
  void tx_override2(int off1, int val1, int off2, int val2);

  /// The command shadow. Entities mutate this, then call send_command().
  HisenseCommand &cmd() { return this->cmd_; }

  /// A user command just went out, so ignore the A/C's echo of the PREVIOUS state for a
  /// moment. Without this, a poll already in flight lands after the write and visibly
  /// reverts the control in Home Assistant before the next poll corrects it.
  void note_user_command() { this->holdoff_until_ = millis() + COMMAND_HOLDOFF_MS; }
  bool in_command_holdoff() const { return int32_t(millis() - this->holdoff_until_) < 0; }

  bool link_up() const { return this->link_up_; }
  bool has_state() const { return this->last_.valid; }
  const HisenseState &last_state() const { return this->last_; }

#ifdef USE_SENSOR
  SUB_SENSOR(indoor_temperature)
  SUB_SENSOR(outdoor_temperature)
  SUB_SENSOR(coil_temperature)
  SUB_SENSOR(compressor_frequency)
  SUB_SENSOR(power)
  SUB_SENSOR(voltage)
  SUB_SENSOR(current)
  SUB_SENSOR(checksum_errors)
#endif
#ifdef USE_BINARY_SENSOR
  SUB_BINARY_SENSOR(aux_heat)
  SUB_BINARY_SENSOR(bus_link)
  SUB_BINARY_SENSOR(problem)
  /// One entry per named f_e_* fault bit, keyed by its HISENSE_FAULT1_* index. The Matter
  /// builds pack these into a bitmap because a manufacturer cluster cannot be rendered in
  /// Home Assistant without upstream changes; here each bit is its own entity.
  void add_fault_binary_sensor(uint8_t bit, binary_sensor::BinarySensor *sensor) {
    this->fault_sensors_.push_back({bit, sensor});
  }
  void add_capability_binary_sensor(uint8_t bit, binary_sensor::BinarySensor *sensor) {
    this->capability_sensors_.push_back({bit, sensor});
  }
#endif
#ifdef USE_TEXT_SENSOR
  SUB_TEXT_SENSOR(link_token)
#endif

 protected:
  // The driver's callbacks are plain C function pointers with no user context, so the
  // instance is reached through a file-static. One A/C per node, which is the only
  // topology the bus supports anyway.
  static void status_trampoline(const HisenseState *state);
  static void link_trampoline(bool link_up);
  static HisenseAC *instance_;

  /// Called on the BUS TASK. Must not touch entities.
  void on_status_isr_(const HisenseState *state);

  Mutex lock_;
  HisenseState pending_{};
  bool pending_valid_{false};
  volatile bool link_up_{false};
  volatile bool link_dirty_{false};

  void publish_telemetry_(const HisenseState &state);
  void publish_diagnostics_();

  HisenseState last_{};
  HisenseCommand cmd_{};
  HisenseClimate *climate_{nullptr};
  std::vector<StatusListener *> listeners_;
  uint32_t holdoff_until_{0};
  bool features_published_{false};
  // Defaults to ON to match the switch's boot state; the A/C ships with the panel lit.
  HisenseDisplay display_pref_{HISENSE_DISPLAY_ON};
#ifdef USE_BINARY_SENSOR
  std::vector<std::pair<uint8_t, binary_sensor::BinarySensor *>> fault_sensors_;
  std::vector<std::pair<uint8_t, binary_sensor::BinarySensor *>> capability_sensors_;
#endif

  static constexpr uint32_t COMMAND_HOLDOFF_MS = 4000;
};

}  // namespace hisense_ac
}  // namespace esphome
