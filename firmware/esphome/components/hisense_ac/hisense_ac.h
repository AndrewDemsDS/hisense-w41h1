#pragma once
// Hub component: owns the A/C bus and moves decoded values into entities and user intent back
// onto the bus. The protocol lives in hisense_protocol.* and hisense_map.h (the codec) and
// hisense_bus.* (the transaction scheduler).
//
// ESPHome's uart component carries the bytes and the scheduler runs in loop(). No task, no
// global state.
#include "esphome/core/automation.h"
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
#include "hisense_commands.h"
#include "hisense_map.h"
#include "hisense_protocol.h"

namespace esphome::hisense_ac {

class HisenseClimate;

/// Anything that wants a copy of each decoded status frame.
class StatusListener {
 public:
  virtual void on_status(const AcState &state) = 0;
};

class HisenseAC : public Component, public uart::UARTDevice, public BusIO, public BusListener, public CommandSink {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  /// Optional: without it the UART's flow_control_pin (or the transceiver) owns DE.
  void set_de_pin(GPIOPin *pin) { this->de_pin_ = pin; }

  void set_climate(HisenseClimate *climate) { this->climate_ = climate; }
  void add_status_listener(StatusListener *listener) { this->listeners_.push_back(listener); }

  /// A user write of `fields` (CONFIRM_* bits in hisense_map.h): `wanted` is cmd() with those
  /// fields changed. Sends the combined frame, with the power-on bits in it when `power_on`, and
  /// keeps the request until the unit's status agrees with it: a frame the unit did not take is
  /// sent again. False when the frame could not be queued: show nothing of the request then.
  bool send_user_command(const AcCommand &wanted, uint16_t fields, bool power_on) {
    return this->commands_.send_user_command(wanted, fields, power_on, millis());
  }
  /// Switch the unit off, checked and re-sent the same way. False when the frame was not queued.
  bool send_user_power_off() { return this->commands_.send_user_power_off(millis()); }
  /// Build and send the combined command frame from the current shadow, unchecked. For frames
  /// that only carry something the status does not report (the display preference).
  bool send_command() { return this->commands_.send_combined(); }

  /// Special modes (eco/turbo byte33, mute, sleep) go through the tracker's paced queue, never
  /// straight to the bus (see CommandTracker::enqueue_special).
  /// False when the op was not taken: show nothing of the request then.
  bool enqueue_special(const SpecialOp &op) { return this->commands_.enqueue_special(op, millis()); }
  /// Replace whatever is still queued with the plan for preset `target` (a row index).
  bool request_preset(uint8_t target) { return this->commands_.request_preset(target, millis()); }
  /// Queue not yet drained, or the last write not yet confirmed: preset/switch readbacks would
  /// show intermediate states.
  bool special_busy() const { return this->commands_.special_busy(); }
  /// The special-mode state the unit should report once everything sent so far lands.
  const SpecialState &projected_special() const { return this->commands_.projected_special(); }

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

  /// The command shadow: what the next combined frame states.
  const AcCommand &cmd() const { return this->commands_.cmd(); }

  /// A user command just went out, so ignore the A/C's echo of the PREVIOUS state for a moment
  /// (see CommandTracker::note_user_command).
  void note_user_command() { this->commands_.note_user_command(millis()); }
  bool in_command_holdoff() const { return this->commands_.in_holdoff(millis()); }

  /// The unit's power once every power frame already sent has landed. Status alone lags a power
  /// frame by a poll or two.
  bool power_expected() const { return this->commands_.power_expected(); }

  /// Show "77" on the unit's panel, or take it off again. While on, the heartbeat tells the unit
  /// that the module is being set up, as the stock module does while it is pairing. It changes
  /// nothing else: the unit keeps running and keeps taking commands.
  void set_setup_indicator(bool on);
  bool setup_indicator() const { return this->bus_.setup_indicator(); }
  /// Called from loop() when the user asks for setup from the remote (horizontal airflow six
  /// times, the "77" gesture), once per request. The argument is the request bits of the unit's
  /// 0x1E reply: 0x20, 0x08 or both (LINK_REQ_* in hisense_protocol.h).
  template<typename F> void add_on_setup_request_callback(F &&callback) {
    this->setup_request_callback_.add(std::forward<F>(callback));
  }

  bool link_up() const { return this->link_up_; }
  /// True while the transceiver is held in transmit (DE high around a frame). Board code that
  /// touches the serial port from outside the component must leave it alone during that time.
  bool bus_transmitting() const { return this->bus_.transmitting(); }
  bool has_state() const { return this->commands_.has_state(); }
  const AcState &last_state() const { return this->commands_.last_state(); }

  // BusListener. These arrive from poll() on the main loop.
  void on_bus_status(const AcState &state) override;
  void on_bus_features(const AcFeatures &features) override;
  void on_bus_link(bool up) override;
  void on_bus_frame(const uint8_t *frame, size_t len) override;
  void on_bus_checksum_error(const uint8_t *frame, size_t len) override;
  void on_bus_timeout(uint8_t expect_class) override;
  void on_bus_command(bool answered, uint8_t reply_class) override;
  void on_bus_setup_request(uint8_t request) override;

  // CommandSink: the tracker's frames, built and handed to the scheduler.
  BusTicket send_combined(const AcCommand &cmd) override;
  BusTicket send_power(bool on) override;
  BusTicket send_mute(bool on) override;
  BusTicket send_sleep(uint8_t profile) override;
  void on_command_event(CommandEvent event, uint16_t fields, uint16_t count) override;

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
  BusTicket send_frame_(const uint8_t *frame, size_t len);

  void log_status_(const AcState &state);
  void process_status_(const AcState &state);
  void publish_telemetry_(const AcState &state);
  void publish_bus_counters_(bool refresh_due);
#ifdef USE_SENSOR
  void publish_sensor_(sensor::Sensor *sensor, float value, bool refresh_due);
#endif
  void publish_diagnostics_();

  // A decoded frame is parked here by poll() and published by loop() once the queue has drained.
  AcState pending_{};
  bool pending_valid_{false};
  bool link_up_{false};
  bool link_dirty_{false};
  // A setup request is parked by poll() too, and its automations run from loop().
  uint8_t setup_request_pending_{0};
  uint8_t logged_link_request_{0};
  CallbackManager<void(uint8_t)> setup_request_callback_;

  BusScheduler bus_;
  GPIOPin *de_pin_{nullptr};
  HighFrequencyLoopRequester fast_loop_;

  // The command shadow, what is waiting for the unit's status to confirm it, the special-mode
  // queue, the hold-off and the command counters.
  CommandTracker commands_;
  HisenseClimate *climate_{nullptr};
  std::vector<StatusListener *> listeners_;
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
};

template<typename... Ts> class SetSetupIndicatorAction final : public Action<Ts...>, public Parented<HisenseAC> {
 public:
  TEMPLATABLE_VALUE(bool, state)

  void play(const Ts &...x) override { this->parent_->set_setup_indicator(this->state_.value(x...)); }
};

}  // namespace esphome::hisense_ac
