#pragma once
// Wi-Fi setup mode: the node's hotspot and captive portal on demand, while the configured network
// is still reachable. The remote's "77" gesture (through the hub's on_setup_request) or a button
// starts it, the A/C panel shows "77" while it is on.
//
// Every decision is in setup_mode_logic.h, which has no ESPHome in it and is tested on a host
// (firmware/test/test_esphome_setup_mode.cpp). This class reads the inputs, asks, and does what
// it is told: the Wi-Fi calls, the panel's "77" through the hub, the restart, and a journal in
// flash that lets the next boot undo a network that was entered and never connected.
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/hisense_ac/hisense_ac.h"

#include "setup_mode_logic.h"

namespace esphome::wifi_setup_mode {

// An SSID and its password, with the lengths ESPHome's own saved Wi-Fi settings have
// (SavedWifiSettings, esphome/components/wifi/wifi_component.h).
struct Credentials {
  char ssid[33];
  char password[65];
};

// The parent is the hub: it carries "77" to the panel and reports the bus link for the status
// display.
class WiFiSetupMode final : public Component, public Parented<hisense_ac::HisenseAC> {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  // After Wi-Fi: setup() reads what Wi-Fi loaded from flash and may write the previous network
  // back over it.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_timeout(uint32_t timeout_ms) { this->config_.timeout_ms = timeout_ms; }
  void set_single_network(bool single) { this->config_.single_network = single; }
  // The first network of the build, the one to go back to when the node has not been connected
  // in this boot and nothing was loaded from flash.
  void set_configured_network(const char *ssid, const char *password);
  // The build cannot open the hotspot on demand, and why (for the log).
  void set_on_demand_unavailable(const char *reason) {
    this->config_.on_demand = false;
    this->unavailable_reason_ = reason;
  }

  // The gesture: the way in and the way out.
  void toggle() { this->request_(Event::GESTURE); }
  // The button: the way in only.
  void start() { this->request_(Event::START); }
  void stop() { this->request_(Event::STOP); }
  // Setup mode is on: "77" is on the panel.
  bool is_active() const { return mode_active(this->state_.phase); }
  // What a status LED or pixel shows. One rule for every package.
  Display status_display() const;

 protected:
  void request_(Event event);
  Inputs read_inputs_(uint32_t now) const;
  void apply_(const Result &result);
  void log_(Note note, bool restart) const;
  void remember_();
  void refresh_previous_();
  void arm_();
  void disarm_();
  void park_();
  void restore_();

  // Requests wait here for loop(), so they are handled after the look at Wi-Fi of the same pass.
  Event pending_{Event::NONE};
  State state_;
  Config config_;
  DownSeconds down_;
  uint32_t last_look_ms_{0};
  uint32_t last_second_ms_{0};
  const char *unavailable_reason_{nullptr};
  // Never logged. `previous_` is what a session started now would restore.
  Credentials configured_{};
  Credentials remembered_{};
  Credentials previous_{};
  Credentials parked_{};
  uint32_t previous_fingerprint_{0};
  uint32_t parked_fingerprint_{0};
  ESPPreferenceObject journal_pref_;

  // The inputs are looked at this often. A request is handled in the next pass of loop().
  static constexpr uint32_t LOOK_INTERVAL_MS = 250;
};

template<typename... Ts> class ToggleAction final : public Action<Ts...>, public Parented<WiFiSetupMode> {
 public:
  void play(const Ts &...x) override { this->parent_->toggle(); }
};

template<typename... Ts> class StartAction final : public Action<Ts...>, public Parented<WiFiSetupMode> {
 public:
  void play(const Ts &...x) override { this->parent_->start(); }
};

template<typename... Ts> class StopAction final : public Action<Ts...>, public Parented<WiFiSetupMode> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};

template<typename... Ts> class ActiveCondition final : public Condition<Ts...>, public Parented<WiFiSetupMode> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_active(); }
};

}  // namespace esphome::wifi_setup_mode
