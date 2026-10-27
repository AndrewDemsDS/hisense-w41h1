#include "wifi_setup_mode.h"

#include <cstdio>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/components/wifi/wifi_component.h"

namespace esphome::wifi_setup_mode {

ESPHOME_LOG_TAG(TAG, "wifi_setup_mode");

// What the journal holds while a session is on: the network to go back to. It is written when
// setup mode starts and cleared (overwritten with zeroes) when it ends, whichever way. One found
// armed at boot means the node went down in between.
struct Journal {
  uint32_t build;  // App.get_config_version_hash() of the build that wrote it
  uint8_t armed;   // JOURNAL_ARMED, anything else is "empty"
  Credentials previous;
};
static constexpr uint8_t JOURNAL_ARMED = 0xA5;

static void copy_text(char *to, size_t size, const char *from) {
  strncpy(to, from == nullptr ? "" : from, size - 1);
  to[size - 1] = '\0';
}

static uint32_t fingerprint_of(const Credentials &credentials) {
  return credentials_fingerprint(credentials.ssid, credentials.password);
}

// The station's current entry. Empty while Wi-Fi scans.
static void read_station(Credentials *out) {
  const wifi::WiFiAP station = wifi::global_wifi_component->get_sta();
  copy_text(out->ssid, sizeof(out->ssid), station.get_ssid().c_str());
  copy_text(out->password, sizeof(out->password), station.get_password().c_str());
}

void WiFiSetupMode::set_configured_network(const char *ssid, const char *password) {
  copy_text(this->configured_.ssid, sizeof(this->configured_.ssid), ssid);
  copy_text(this->configured_.password, sizeof(this->configured_.password), password);
}

void WiFiSetupMode::setup() {
  this->journal_pref_ = global_preferences->make_preference<Journal>(fnv1_hash("wifi_setup_mode"), true);

  // Wi-Fi has started: when it loaded a network from flash, that is the station's entry now.
  Credentials boot{};
  read_station(&boot);

  Journal journal{};
  const bool loaded = this->journal_pref_.load(&journal);
  journal.previous.ssid[sizeof(journal.previous.ssid) - 1] = '\0';
  journal.previous.password[sizeof(journal.previous.password) - 1] = '\0';
  switch (boot_repair(loaded && journal.armed == JOURNAL_ARMED, journal.build == App.get_config_version_hash(),
                      fingerprint_of(journal.previous), fingerprint_of(boot))) {
    case BootRepair::RESTORE:
      ESP_LOGW(TAG,
               "The node went down in setup mode with a network that had not connected: restoring the previous one");
      wifi::global_wifi_component->save_wifi_sta(journal.previous.ssid, journal.previous.password);
      boot = journal.previous;
      this->disarm_();
      break;
    case BootRepair::DISARM:
      this->disarm_();
      break;
    case BootRepair::NOTHING:
      break;
  }
  this->remembered_ = boot;
  this->refresh_previous_();

  // The placeholder: a network name nobody has, with a key nobody knows, so nothing can answer.
  snprintf(this->parked_.ssid, sizeof(this->parked_.ssid), "setup-%08x", (unsigned) random_uint32());
  snprintf(this->parked_.password, sizeof(this->parked_.password), "%08x%08x", (unsigned) random_uint32(),
           (unsigned) random_uint32());
  this->parked_fingerprint_ = fingerprint_of(this->parked_);
}

void WiFiSetupMode::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Wi-Fi setup mode:\n"
                "  Timeout: %" PRIu32 " s\n"
                "  Hotspot on demand: %s",
                this->config_.timeout_ms / 1000, YESNO(this->config_.on_demand));
  if (!this->config_.on_demand) {
    ESP_LOGCONFIG(TAG, "    Not available: %s", this->unavailable_reason_);
  }
  if (!this->config_.single_network) {
    ESP_LOGCONFIG(TAG, "  Several networks configured: a failed entry is replaced by one of them only");
  }
}

Display WiFiSetupMode::status_display() const {
  return wifi_setup_mode::status_display(this->state_.phase, wifi::global_wifi_component->is_ap_active(), this->down_);
}

void WiFiSetupMode::request_(Event event) {
  // One at a time. A second request within one look (250 ms) is the same press.
  if (this->pending_ == Event::NONE)
    this->pending_ = event;
}

Inputs WiFiSetupMode::read_inputs_(uint32_t now) const {
  auto *net = wifi::global_wifi_component;
  Inputs in;
  in.now = now;
  in.ap_active = net->is_ap_active();
  in.connected = net->is_connected();
  in.parked = this->parked_fingerprint_;
  in.previous = this->previous_fingerprint_;
  // Only looked at in setup mode: get_sta() returns a copy.
  if (mode_active(this->state_.phase)) {
    Credentials station{};
    read_station(&station);
    in.station = fingerprint_of(station);
  }
  return in;
}

void WiFiSetupMode::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  if (now - this->last_second_ms_ >= 1000) {
    this->last_second_ms_ = now;
    count_down_seconds(&this->down_, wifi::global_wifi_component->is_connected(), this->parent_->link_up());
  }
  if (this->pending_ == Event::NONE && now - this->last_look_ms_ < LOOK_INTERVAL_MS)
    return;
  this->last_look_ms_ = now;

  Inputs in = this->read_inputs_(now);
  this->apply_(step(this->state_, observe(this->state_, in), in, this->config_));
  if (this->pending_ != Event::NONE) {
    const Event event = this->pending_;
    this->pending_ = Event::NONE;
    in = this->read_inputs_(now);
    this->apply_(step(this->state_, event, in, this->config_));
  }
}

void WiFiSetupMode::on_shutdown() {
  // Runs for App.reboot() and App.safe_reboot() alike, so also for a restart this component did
  // not ask for (the API's reboot timeout, an update, a restart button).
  const Inputs in = this->read_inputs_(App.get_loop_component_start_time());
  this->apply_(step(this->state_, Event::SHUTDOWN, in, this->config_));
}

void WiFiSetupMode::apply_(const Result &result) {
  // First, because the restart at the end comes back in through on_shutdown().
  this->state_ = result.state;
  this->log_(result.note, (result.actions & ACT_RESTART) != 0);
  if (result.actions & ACT_REMEMBER)
    this->remember_();
  if (result.actions & ACT_ARM)
    this->arm_();
  if (result.actions & ACT_INDICATOR_ON)
    this->parent_->set_setup_indicator(true);
  if (result.actions & ACT_PARK)
    this->park_();
  if (result.actions & ACT_INDICATOR_OFF)
    this->parent_->set_setup_indicator(false);
  if (result.actions & ACT_RESTORE)
    this->restore_();
  if (result.actions & ACT_DISARM)
    this->disarm_();
  if (result.actions & ACT_RESTART)
    App.safe_reboot();
}

void WiFiSetupMode::log_(Note note, bool restart) const {
  const char *const then = restart ? "restarting" : "the node stays up";
  switch (note) {
    case Note::NONE:
      break;
    case Note::ENTERING:
      ESP_LOGW(TAG, "Wi-Fi setup mode: leaving the network and opening the hotspot");
      break;
    case Note::ATTENDING:
      ESP_LOGW(TAG, "Wi-Fi setup mode: the hotspot is already open");
      break;
    case Note::UNAVAILABLE:
      ESP_LOGE(TAG,
               "Wi-Fi setup mode cannot open the hotspot on demand: %s. The hotspot still opens by itself when "
               "the network has been out of reach for a minute",
               this->unavailable_reason_);
      break;
    case Note::ALREADY_ACTIVE:
      ESP_LOGD(TAG, "Wi-Fi setup mode is already on");
      break;
    case Note::HOTSPOT_OPEN:
      ESP_LOGW(TAG, "Wi-Fi setup mode: hotspot open, http://192.168.4.1");
      break;
    case Note::HOTSPOT_FAILED:
      ESP_LOGE(TAG, "Wi-Fi setup mode: the hotspot did not open. Restarting, ask again after that");
      break;
    case Note::PORTAL_SAVED:
      ESP_LOGW(TAG, "Wi-Fi setup mode: a network was entered, waiting for it to connect");
      break;
    case Note::CONNECTED:
      ESP_LOGW(TAG, "Wi-Fi setup mode: connected, restarting");
      break;
    case Note::CONNECTED_NO_RESTART:
      ESP_LOGW(TAG, "Wi-Fi setup mode: connected, over");
      break;
    case Note::UNEXPECTED_CONNECT:
      ESP_LOGW(TAG, "Wi-Fi setup mode: the network came back by itself, restarting");
      break;
    case Note::STOPPED:
      ESP_LOGW(TAG, "Wi-Fi setup mode: stopped, %s", then);
      break;
    case Note::TIMED_OUT:
      ESP_LOGW(TAG, "Wi-Fi setup mode: over, %s", then);
      break;
    case Note::STOPPED_RESTORING:
    case Note::TIMED_OUT_RESTORING:
      ESP_LOGW(TAG, "Wi-Fi setup mode: the network entered did not connect, restoring the previous one, %s", then);
      break;
    case Note::SHUTDOWN_RESTORING:
      ESP_LOGW(TAG, "Wi-Fi setup mode: restart with a network that has not connected, restoring the previous one");
      break;
  }
}

void WiFiSetupMode::remember_() {
  Credentials station{};
  read_station(&station);
  if (station.ssid[0] == '\0')
    return;
  this->remembered_ = station;
  this->refresh_previous_();
}

void WiFiSetupMode::refresh_previous_() {
  switch (previous_source(this->remembered_.ssid[0] != '\0', this->configured_.ssid[0] != '\0')) {
    case PreviousSource::REMEMBERED:
      this->previous_ = this->remembered_;
      break;
    case PreviousSource::FROM_BUILD:
      this->previous_ = this->configured_;
      break;
    case PreviousSource::NONE:
      this->previous_ = Credentials{};
      break;
  }
  this->previous_fingerprint_ = fingerprint_of(this->previous_);
}

void WiFiSetupMode::arm_() {
  Journal journal{};
  journal.build = App.get_config_version_hash();
  journal.armed = JOURNAL_ARMED;
  journal.previous = this->previous_;
  this->journal_pref_.save(&journal);
  global_preferences->sync();
}

void WiFiSetupMode::disarm_() {
  // Zeroes, so the credentials do not stay in flash a second time.
  Journal journal{};
  this->journal_pref_.save(&journal);
  global_preferences->sync();
}

void WiFiSetupMode::park_() {
  // ESPHome opens its hotspot when no network has been connected for `ap_timeout`. So Wi-Fi is
  // switched off and on again with that timeout at 1 ms, and the station is pointed, in memory
  // only, at a network that does not exist. disable(), set_sta(), enable() is also what
  // ESPHome's own wifi.configure action does.
  auto *net = wifi::global_wifi_component;
  wifi::WiFiAP parked;
  parked.set_ssid(this->parked_.ssid);
  parked.set_password(this->parked_.password);
  net->disable();
  net->set_ap_timeout(1);
  net->enable();
  // After enable(), which reloads credentials saved by an earlier use of the portal.
  net->set_sta(parked);
}

void WiFiSetupMode::restore_() {
  // What was entered in the portal is in flash and did not connect. Without this the node would
  // come back up on it and stay off the network.
  wifi::global_wifi_component->save_wifi_sta(this->previous_.ssid, this->previous_.password);
}

}  // namespace esphome::wifi_setup_mode
