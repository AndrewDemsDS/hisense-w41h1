#pragma once
// Hub component: owns the A/C bus and moves decoded values into entities and user intent back
// onto the bus. The protocol lives in hisense_protocol.* and hisense_map.h (the codec) and
// hisense_bus.* (the transaction scheduler).
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

  /// A user write of `fields` (CONFIRM_* bits in hisense_map.h), already placed in the shadow.
  /// Sends the combined frame, with the power-on bits in it when `power_on`, and keeps the request
  /// until the unit's status agrees with it: a frame the unit did not take is sent again.
  void send_user_command(uint16_t fields, bool power_on);
  /// Switch the unit off, checked and re-sent the same way.
  void send_user_power_off();
  /// Build and send the combined command frame from the current shadow, unchecked. For frames
  /// that only carry something the status does not report (the display preference).
  bool send_command();
  /// The literal power frames. Off is always this frame; on is the fallback for a unit that did
  /// not take power and mode in one frame.
  bool send_power(bool on);
  bool send_mute(bool on);
  bool send_sleep(uint8_t profile);

  /// Special modes (eco/turbo byte33, mute, sleep) go through a paced queue, never straight to
  /// the bus: the A/C swallows a special-mode command that lands within ~8 s of the previous
  /// one, so every op waits SPECIAL_SETTLE_MS after the last. Presets, switches and the
  /// sleep select all share it, so they can never race each other on the wire.
  void enqueue_special(const SpecialOp &op);
  /// Replace whatever is still queued with the plan for preset `target` (a row index), computed
  /// against what the unit is expected to be once already-sent ops land.
  void request_preset(uint8_t target);
  /// Queue not yet drained, or the last write not yet confirmed: preset/switch readbacks would
  /// show intermediate states.
  bool special_busy() const { return this->special_len_ > 0 || this->special_pending_.active; }
  /// The special-mode state the unit should report once everything sent so far lands.
  const SpecialState &projected_special() const { return this->projected_; }

  /// Panel display is STICKY, not one-shot. Byte 36 rides every frame, and 0x00 ("no change")
  /// turns the panel ON on real hardware, so a command that does not state a preference
  /// re-lights a display the user switched off. Only 0x40 (off) and 0xC0 (on) are confirmed on
  /// hardware to do what they are named for.
  void set_display_pref(bool on) { this->display_pref_ = on ? DISPLAY_ON : DISPLAY_OFF; }
  bool display_pref_on() const { return this->display_pref_ != DISPLAY_OFF; }
  /// Whether command frames ask the unit to beep. Applied to every command frame from then on;
  /// nothing is sent when it changes.
  void set_beeper(bool on) { this->beeper_ = on; }
  bool beeper() const { return this->beeper_; }

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

  /// The unit's power once every power frame already sent has landed. Status alone lags a power
  /// frame by a poll or two.
  bool power_expected() const { return hisense_ac::power_expected(this->power_intent_, this->last_.power_on); }

  bool link_up() const { return this->link_up_; }
  /// True while the transceiver is held in transmit (DE high around a frame). Board code that
  /// touches the serial port from outside the component must leave it alone during that time.
  bool bus_transmitting() const { return this->bus_.wants_fast_loop(); }
  bool has_state() const { return this->last_.valid; }
  const AcState &last_state() const { return this->last_; }

  // BusListener. These arrive from poll() on the main loop.
  void on_bus_status(const AcState &state) override;
  void on_bus_features(const AcFeatures &features) override;
  void on_bus_link(bool up) override;
  void on_bus_frame(const uint8_t *frame, size_t len) override;
  void on_bus_checksum_error(const uint8_t *frame, size_t len) override;
  void on_bus_timeout(uint8_t expect_class) override;
  void on_bus_command(bool answered, uint8_t reply_class) override;

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
  SUB_SENSOR(reply_timeouts)
  SUB_SENSOR(unanswered_commands)
  SUB_SENSOR(command_retries)
  SUB_SENSOR(failed_commands)
  SUB_SENSOR(link_losses)
#endif
#ifdef USE_BINARY_SENSOR
  SUB_BINARY_SENSOR(aux_heat)
  SUB_BINARY_SENSOR(bus_link)
  SUB_BINARY_SENSOR(problem)
  /// One entry per named f_e_* fault bit, keyed by its FAULT1_* index. Each bit is its own entity.
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
  bool send_frame_(const uint8_t *frame, size_t len);

  void process_status_(const AcState &state);
  void publish_telemetry_(const AcState &state);
  void publish_bus_counters_(bool refresh_due);
#ifdef USE_SENSOR
  void publish_sensor_(sensor::Sensor *sensor, float value, bool refresh_due);
#endif
  void drain_special_queue_();
  bool execute_special_(const SpecialOp &op);
  void push_special_(const SpecialOp &op);
  void track_command_(const CommandIntent &intent);
  void confirm_commands_(const AcState &state);
  void confirm_main_(const UnitView &view, const SpecialState &wanted, uint32_t now, bool panel_f);
  void confirm_special_(const UnitView &view, const SpecialState &wanted, uint32_t now);
  void drop_pending_commands_();
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
  PowerIntent power_intent_{};
  // The last user write to power, mode, setpoint, fan or swing, until the unit's status confirms
  // it, and the same for the last special-mode write sent.
  PendingCommand main_pending_{};
  PendingCommand special_pending_{};
  uint8_t special_resends_{0};
  // Counters since boot: commands sent again after a status that did not match, and commands the
  // unit never took.
  uint32_t command_retries_{0};
  uint32_t failed_commands_{0};
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
  // Last fault bitmap logged, so a fault is reported when it appears and when it clears, not on
  // every status frame that still carries it.
  uint32_t logged_faults_{0};
  // Status frames arrive about once a second and almost never differ, so telemetry goes out on
  // change plus one refresh per TELEMETRY_REFRESH_MS.
  uint32_t last_refresh_ms_{0};
  bool telemetry_refreshed_{false};
  // Whether the outdoor and coil bytes have held a real reading since the link came up.
  OutdoorTempGate outdoor_gate_{};
  // Defaults to ON to match the switch's boot state; the A/C ships with the panel lit.
  Display display_pref_{DISPLAY_ON};
  bool beeper_{true};
#ifdef USE_BINARY_SENSOR
  std::vector<std::pair<uint8_t, binary_sensor::BinarySensor *>> fault_sensors_;
  std::vector<std::pair<uint8_t, binary_sensor::BinarySensor *>> capability_sensors_;
#endif

  static constexpr uint32_t COMMAND_HOLDOFF_MS = 4000;
};

// The verdict on a command has to be in before the hold-off lets a status frame reach the entities.
static_assert(CONFIRM_SETTLE_MS < 4000, "confirm settle must end inside the command hold-off");

}  // namespace esphome::hisense_ac
