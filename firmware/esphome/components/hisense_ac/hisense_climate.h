#pragma once
// The A/C as a single ESPHome climate entity: power, mode, setpoint, 6-speed fan, swing,
// current temperature and action.
//
// Unlike the Matter glue there are no per-field echo guards here, and they are not an
// oversight: control() is only ever called by Home Assistant, never by our own publish_state(),
// so the downlink -> readback -> uplink feedback loop that the Matter builds have to defend
// against cannot form. The only timing defence needed is the hub's command hold-off, which stops
// a poll already in flight from visibly reverting a control the user just moved.
#include "esphome/components/climate/climate.h"
#include "hisense_ac.h"

namespace esphome {
namespace hisense_ac {

/// The two ladder steps ESPHome has no built-in fan-mode name for (indices 3 and 5).
/// The other five ride the built-in enum; see the note in hisense_climate.cpp. ESPHome matches
/// custom modes by pointer into the entity vector, so these strings are the one definition.
extern const char *const FAN_CUSTOM_NAMES[2];

class HisenseClimate : public climate::Climate, public Component {
 public:
  void setup() override;
  void dump_config() override;
  climate::ClimateTraits traits() override;

  void set_parent(HisenseAC *parent) { this->parent_ = parent; }
  void set_visual_min(float v) { this->visual_min_ = v; }
  void set_visual_max(float v) { this->visual_max_ = v; }
  void set_supports_heat(bool v) { this->supports_heat_ = v; }
  void set_supports_horizontal_swing(bool v) { this->supports_hswing_ = v; }

  /// Called from the hub on the main loop with a freshly decoded status frame.
  void update_from_bus(const HisenseState &state, bool holdoff);

  /// Publish a ladder index using the right representation: built-in enum for the five steps
  /// ESPHome names, custom string for the two it does not.
  void publish_fan_index(uint8_t idx);

 protected:
  void control(const climate::ClimateCall &call) override;

  HisenseAC *parent_{nullptr};
  float visual_min_{16.0f};
  float visual_max_{32.0f};
  bool supports_heat_{true};
  bool supports_hswing_{false};
};

}  // namespace hisense_ac
}  // namespace esphome
