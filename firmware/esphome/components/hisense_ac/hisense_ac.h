#pragma once
// Hub component: owns the A/C bus and moves decoded values into entities and user intent back
// onto the bus. The protocol lives in hisense_protocol.* and hisense_map.h (the codec, held equal
// to the shared driver by a parity test) and hisense_bus.* (the transaction scheduler).
//
// ESPHome's uart component carries the bytes and the scheduler runs in loop(). No task, no
// global state.
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
#include "esphome/components/uart/uart.h"

#include "hisense_bus.h"
#include "hisense_map.h"
#include "hisense_protocol.h"

namespace esphome::hisense_ac {

class HisenseClimate;

/// Anything that wants a copy of each decoded status frame.
class StatusListener {
 public:
  virtual void on_status(const AcState &state) = 0;
};

class HisenseAC : public Component, public uart::UARTDevice, public BusIO, public BusListener {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /// Optional: without it the UART's flow_control_pin (or the transceiver) owns DE.
  void set_de_pin(GPIOPin *pin) { this->de_pin_ = pin; }

  void set_climate(HisenseClimate *climate) { this->climate_ = climate; }
  void add_status_listener(StatusListener *listener) { this->listeners_.push_back(listener); }

  /// Build and send the combined command frame from the current shadow.
  void send_command();
  /// Power is a separate, byte-for-byte ported frame, never part of the combined command.
  void send_power(bool on);
  void send_mute(bool on);
  void send_sleep(uint8_t profile);

  /// Special modes (eco/turbo byte33, mute, sleep) go through a paced queue, never straight to
  /// the bus: the A/C swallows a special-mode command that lands within ~8 s of the previous
  /// one, so every op waits SPECIAL_SETTLE_MS after the last. Presets, switches and the
  /// sleep select all share it, so they can never race each other on the wire.
  void enqueue_special(const SpecialOp &op);
  /// Replace whatever is still queued with the plan for preset `target` (a row index), computed
  /// against what the unit is expected to be once already-sent ops land.
  void request_preset(uint8_t target);
  /// Queue not yet drained: preset/switch readbacks would show intermediate states.
  bool special_busy() const { return this->special_len_ > 0; }
  /// The special-mode state the unit should report once everything sent so far lands.
  const SpecialState &projected_special() const { return this->projected_; }

  /// Panel display is STICKY, not one-shot. Byte 36 rides every frame, and 0x00 ("no change")
  /// turns the panel ON on real hardware, so a command that does not state a preference
  /// re-lights a display the user switched off. Confirmed on an A/C 2026-08-19; only 0x40
  /// (off) and 0xC0 (on) were ever bench-confirmed, 0x00 was an assumption.
  void set_display_pref(bool on) { this->display_pref_ = on ? DISPLAY_ON : DISPLAY_OFF; }
  bool display_pref_on() const { return this->display_pref_ != DISPLAY_OFF; }

  /// The command shadow. Entities mutate this, then call send_command().
  AcCommand &cmd() { return this->cmd_; }

  /// A user command just went out, so ignore the A/C's echo of the PREVIOUS state for a
  /// moment. Without this, a poll already in flight lands after the write and visibly
  /// reverts the control in Home Assistant before the next poll corrects it.
  /// Elapsed-time form, so it stays correct across the 49.7-day millis() wrap: a stored deadline
  /// compared with a signed difference reads as "in holdoff" again ~24.8 days after the last command.
  void note_user_command() {
    this->holdoff_start_ = millis();
    this->holdoff_armed_ = true;
  }
  bool in_command_holdoff() const {
    if (this->holdoff_armed_ && millis() - this->holdoff_start_ >= COMMAND_HOLDOFF_MS)
      this->holdoff_armed_ = false;  // or it re-arms for 4 s at every wrap
    return this->holdoff_armed_;
  }

  bool link_up() const { return this->link_up_; }
  bool has_state() const { return this->last_.valid; }
  const AcState &last_state() const { return this->last_; }

  // BusListener. These arrive from poll() on the main loop.
  void on_bus_status(const AcState &state) override;
  void on_bus_features(const AcFeatures &features) override;
  void on_bus_link(bool up) override;

  // BusIO, over uart::UARTDevice and the DE pin.
  void bus_set_de(bool high) override;
  void bus_write(const uint8_t *data, size_t len) override;
  void bus_flush() override;
  int bus_read() override;

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
  /// One entry per named f_e_* fault bit, keyed by its FAULT1_* index. The Matter builds pack
  /// these into a bitmap because a manufacturer cluster cannot be rendered in Home Assistant
  /// without upstream changes; here each bit is its own entity.
  void add_fault_binary_sensor(uint8_t bit, binary_sensor::BinarySensor *sensor) {
    this->fault_sensors_.emplace_back(bit, sensor);
  }
  void add_capability_binary_sensor(uint8_t bit, binary_sensor::BinarySensor *sensor) {
    this->capability_sensors_.emplace_back(bit, sensor);
  }
#endif
#ifdef USE_TEXT_SENSOR
  SUB_TEXT_SENSOR(link_token)
#endif

 protected:
  bool send_frame_(const uint8_t *frame, size_t len) { return this->bus_.enqueue(frame, len); }

  void process_status_(const AcState &state);
  void publish_telemetry_(const AcState &state);
  void drain_special_queue_();
  void execute_special_(const SpecialOp &op);
  void publish_diagnostics_();

  // A decoded frame is parked here by poll() and published by loop() once the queue has drained.
  AcState pending_{};
  bool pending_valid_{false};
  bool link_up_{false};
  bool link_dirty_{false};

  BusScheduler bus_;
  GPIOPin *de_pin_{nullptr};
  HighFrequencyLoopRequester fast_loop_;

  AcState last_{};
  AcCommand cmd_{};
  HisenseClimate *climate_{nullptr};
  std::vector<StatusListener *> listeners_;
  uint32_t holdoff_start_{0};
  mutable bool holdoff_armed_{false};
  // Paced special-mode queue. Fixed size: a preset plan is at most PRESET_PLAN_MAX ops, and
  // anything that would overflow is dropped with a warning rather than allocated.
  static constexpr uint8_t SPECIAL_QUEUE_CAP = 8;
  SpecialOp special_queue_[SPECIAL_QUEUE_CAP]{};
  uint8_t special_len_{0};
  uint32_t last_special_ms_{0};
  bool special_sent_{false};
  SpecialState projected_{};
  bool features_published_{false};
  // Defaults to ON to match the switch's boot state; the A/C ships with the panel lit.
  Display display_pref_{DISPLAY_ON};
#ifdef USE_BINARY_SENSOR
  std::vector<std::pair<uint8_t, binary_sensor::BinarySensor *>> fault_sensors_;
  std::vector<std::pair<uint8_t, binary_sensor::BinarySensor *>> capability_sensors_;
#endif

  static constexpr uint32_t COMMAND_HOLDOFF_MS = 4000;
};

}  // namespace esphome::hisense_ac
