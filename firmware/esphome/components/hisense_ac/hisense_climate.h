#pragma once
// The A/C as a single ESPHome climate entity: power, mode, setpoint, 6-speed fan, swing,
// current temperature and action.
//
// The decisions are in hisense_entities.h (climate_plan, climate_view_from_status), with host
// tests. control() is called by a user request only, never by this entity's own publish_state(),
// so what the unit reports is never sent back to it. The one timing defence is the hub's command
// hold-off, which stops a poll already in flight from reverting a control the user just moved.
#include "esphome/core/defines.h"
#ifdef USE_CLIMATE
#include "esphome/components/climate/climate.h"
#include "hisense_ac.h"

namespace esphome::hisense_ac {

class HisenseClimate final : public climate::Climate, public Component, public Parented<HisenseAC> {
 public:
  void setup() override;
  void dump_config() override;

  void set_visual_min(float v) { this->visual_min_ = v; }
  void set_visual_max(float v) { this->visual_max_ = v; }
  void set_supports_heat(bool v) { this->supports_heat_ = v; }
  void set_supports_horizontal_swing(bool v) { this->supports_hswing_ = v; }
  /// SUPPORT_* bits: which special modes this unit offers as presets.
  void set_preset_support(uint8_t v) { this->preset_support_ = v; }

  /// Called from the hub on the main loop with a freshly decoded status frame.
  void update_from_bus(const AcState &state, bool holdoff);

  /// Set the fan fields for a ladder index (as published, see fan_published_index): built-in enum
  /// for the five steps ESPHome names, custom string for the two it does not.
  void publish_fan_index(uint8_t idx);

  /// Publish a preset row using the right representation: built-in enum for none/eco,
  /// custom string (pointer into PRESETS) for the rest.
  void publish_preset_index(uint8_t idx);

 protected:
  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;
  ClimateShown shown_() const;

  float visual_min_{16.0f};
  float visual_max_{32.0f};
  bool supports_heat_{true};
  bool supports_hswing_{false};
  uint8_t preset_support_{0};
  // What publish_fan_index() / publish_preset_index() last set. The entity holds each as either
  // an enum or a custom string, so the index is the one value that compares across both.
  uint8_t fan_index_{0xFF};
  uint8_t preset_index_{0xFF};
  bool bus_state_published_{false};
  /// The custom preset names this unit offers, pointers into PRESETS. ESPHome matches
  /// custom presets by pointer into this vector, so it must outlive the entity (it is a member).
  std::vector<const char *> custom_presets_;
};

}  // namespace esphome::hisense_ac

#endif  // USE_CLIMATE
