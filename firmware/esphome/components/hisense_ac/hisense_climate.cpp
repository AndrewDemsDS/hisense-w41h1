#include "hisense_climate.h"
#include "esphome/core/log.h"

#ifdef USE_CLIMATE

namespace esphome::hisense_ac {

ESPHOME_LOG_TAG(TAG, "hisense_ac.climate");

// hisense_mapping.h mirrors these enums as plain ints so it stays free of ESPHome types. If ESPHome
// ever renumbers one, this is where the build breaks, instead of the A/C silently switching to
// the wrong mode on the wire.
static_assert((int) climate::CLIMATE_MODE_OFF == CLIMATE_MODE_OFF_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_MODE_HEAT_COOL == CLIMATE_MODE_HEAT_COOL_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_MODE_COOL == CLIMATE_MODE_COOL_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_MODE_HEAT == CLIMATE_MODE_HEAT_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_MODE_FAN_ONLY == CLIMATE_MODE_FAN_ONLY_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_MODE_DRY == CLIMATE_MODE_DRY_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_MODE_AUTO == CLIMATE_MODE_AUTO_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_ACTION_OFF == CLIMATE_ACTION_OFF_VALUE, "ClimateAction drift");
static_assert((int) climate::CLIMATE_ACTION_COOLING == CLIMATE_ACTION_COOLING_VALUE, "ClimateAction drift");
static_assert((int) climate::CLIMATE_ACTION_HEATING == CLIMATE_ACTION_HEATING_VALUE, "ClimateAction drift");
static_assert((int) climate::CLIMATE_ACTION_IDLE == CLIMATE_ACTION_IDLE_VALUE, "ClimateAction drift");
static_assert((int) climate::CLIMATE_ACTION_DRYING == CLIMATE_ACTION_DRYING_VALUE, "ClimateAction drift");
static_assert((int) climate::CLIMATE_ACTION_FAN == CLIMATE_ACTION_FAN_VALUE, "ClimateAction drift");
static_assert((int) climate::CLIMATE_SWING_OFF == CLIMATE_SWING_OFF_VALUE, "ClimateSwingMode drift");
static_assert((int) climate::CLIMATE_SWING_BOTH == CLIMATE_SWING_BOTH_VALUE, "ClimateSwingMode drift");
static_assert((int) climate::CLIMATE_SWING_VERTICAL == CLIMATE_SWING_VERTICAL_VALUE, "ClimateSwingMode drift");
static_assert((int) climate::CLIMATE_SWING_HORIZONTAL == CLIMATE_SWING_HORIZONTAL_VALUE, "ClimateSwingMode drift");
static_assert(FAN_INDEX_MAX == (int) FAN_TABLE_LEN, "fan ladder drift");
static_assert((int) climate::CLIMATE_FAN_AUTO == CLIMATE_FAN_AUTO_VALUE, "ClimateFanMode drift");
static_assert((int) climate::CLIMATE_FAN_LOW == CLIMATE_FAN_LOW_VALUE, "ClimateFanMode drift");
static_assert((int) climate::CLIMATE_FAN_MEDIUM == CLIMATE_FAN_MEDIUM_VALUE, "ClimateFanMode drift");
static_assert((int) climate::CLIMATE_FAN_HIGH == CLIMATE_FAN_HIGH_VALUE, "ClimateFanMode drift");
static_assert((int) climate::CLIMATE_FAN_MIDDLE == CLIMATE_FAN_MIDDLE_VALUE, "ClimateFanMode drift");
static_assert((int) climate::CLIMATE_FAN_FOCUS == CLIMATE_FAN_FOCUS_VALUE, "ClimateFanMode drift");
static_assert((int) climate::CLIMATE_FAN_QUIET == CLIMATE_FAN_QUIET_VALUE, "ClimateFanMode drift");
static_assert((int) climate::CLIMATE_FAN_ON == CLIMATE_FAN_ON_VALUE, "ClimateFanMode drift");
static_assert((int) climate::CLIMATE_FAN_OFF == CLIMATE_FAN_OFF_VALUE, "ClimateFanMode drift");
static_assert((int) climate::CLIMATE_FAN_DIFFUSE == CLIMATE_FAN_DIFFUSE_VALUE, "ClimateFanMode drift");
static_assert((int) climate::CLIMATE_PRESET_NONE == CLIMATE_PRESET_NONE_VALUE, "ClimatePreset drift");
static_assert((int) climate::CLIMATE_PRESET_ECO == CLIMATE_PRESET_ECO_VALUE, "ClimatePreset drift");

void HisenseClimate::setup() {
  this->parent_->set_climate(this);
  // Custom list carries only the two intermediate steps; the other five ride the built-in enum
  // advertised in traits(). Lives on the entity because ClimateTraits only references the
  // entity's vector, and set_custom_fan_mode_() matches by pointer into it.
  this->set_supported_custom_fan_modes(FAN_CUSTOM_NAMES);
  for (size_t i = PRESET_FIRST_CUSTOM; i < PRESET_COUNT; i++) {
    if (preset_available((uint8_t) i, this->preset_support_)) {
      this->custom_presets_.push_back(PRESETS[i].name);
    }
  }
  if (!this->custom_presets_.empty())
    this->set_supported_custom_presets(this->custom_presets_);
  this->mode = climate::CLIMATE_MODE_OFF;
  this->action = climate::CLIMATE_ACTION_OFF;
  this->target_temperature = NAN;
  this->current_temperature = NAN;
}

climate::ClimateTraits HisenseClimate::traits() {
  auto traits = climate::ClimateTraits();

  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_COOL, climate::CLIMATE_MODE_DRY,
                              climate::CLIMATE_MODE_FAN_ONLY, climate::CLIMATE_MODE_AUTO});
  if (this->supports_heat_)
    traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);

  traits.set_supported_swing_modes({climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL});
  if (this->supports_hswing_) {
    traits.add_supported_swing_mode(climate::CLIMATE_SWING_HORIZONTAL);
    traits.add_supported_swing_mode(climate::CLIMATE_SWING_BOTH);
  }

  // Without this, validate_() silently resets any built-in fan mode and the command vanishes.
  traits.set_supported_fan_modes(
      {climate::CLIMATE_FAN_AUTO, climate::CLIMATE_FAN_LOW, climate::CLIMATE_FAN_MEDIUM, climate::CLIMATE_FAN_HIGH});

  // none + eco ride the built-in enum (see hisense_special.h); the rest are the custom
  // presets registered in setup(). No special modes at all means no preset control.
  if (this->preset_support_ != 0) {
    traits.add_supported_preset(climate::CLIMATE_PRESET_NONE);
    if (preset_available(PRESET_ECO, this->preset_support_))
      traits.add_supported_preset(climate::CLIMATE_PRESET_ECO);
  }

  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE | climate::CLIMATE_SUPPORTS_ACTION);
  traits.set_visual_min_temperature(this->visual_min_);
  traits.set_visual_max_temperature(this->visual_max_);
  traits.set_visual_target_temperature_step(1.0f);
  traits.set_visual_current_temperature_step(1.0f);
  return traits;
}

// Every decision here is climate_plan() in hisense_entities.h, with a host test. This translates
// the call into plain values, carries the plan out and shows what the hub took.
void HisenseClimate::control(const climate::ClimateCall &call) {
  ClimateRequest request;
  if (call.get_mode().has_value()) {
    request.has_mode = true;
    request.mode = (uint8_t) *call.get_mode();
  }
  if (call.get_target_temperature().has_value()) {
    request.has_target = true;
    request.target_c = *call.get_target_temperature();
  }
  if (call.has_custom_fan_mode()) {
    request.custom_fan = call.get_custom_fan_mode().c_str();
  } else if (call.get_fan_mode().has_value()) {
    request.has_fan = true;
    request.fan = (uint8_t) *call.get_fan_mode();
  }
  if (call.get_swing_mode().has_value()) {
    request.has_swing = true;
    request.swing = (uint8_t) *call.get_swing_mode();
  }
  if (call.has_custom_preset()) {
    request.custom_preset = call.get_custom_preset().c_str();
  } else if (call.get_preset().has_value()) {
    request.has_preset = true;
    request.preset = (uint8_t) *call.get_preset();
  }
  const AcState &status = this->parent_->last_state();
  ClimateContext context;
  context.have_status = status.valid;
  context.power_expected = this->parent_->power_expected();
  context.panel_f = status.valid && status.temp_unit_f;
  context.shadow = this->parent_->cmd();
  context.projected = this->parent_->projected_special();
  context.preset_support = this->preset_support_;

  const ClimatePlan plan = climate_plan(request, context);
  if ((plan.refused & CLIMATE_REFUSED_NO_STATUS) != 0) {
    ESP_LOGW(TAG, "Command refused: no status from the unit yet");
  }
  if ((plan.refused & CLIMATE_REFUSED_MODE) != 0) {
    ESP_LOGW(TAG, "Unsupported climate mode %d", (int) request.mode);
  }
  if ((plan.refused & CLIMATE_REFUSED_SETPOINT) != 0) {
    ESP_LOGW(TAG, "Target temperature refused: the unit only takes one in cool and heat");
  }
  if ((plan.refused & CLIMATE_REFUSED_FAN) != 0) {
    ESP_LOGW(TAG, "Fan change refused: turbo, quiet or sleep is holding the fan speed");
  }
  if ((plan.refused & CLIMATE_REFUSED_PRESET) != 0) {
    ESP_LOGW(TAG, "Unsupported preset requested");
  }

  // A frame the hub could not queue changes nothing here: the entity keeps showing the unit.
  if (plan.power_off) {
    if (!this->parent_->send_user_power_off())
      return;
    this->mode = climate::CLIMATE_MODE_OFF;
    this->publish_state();
    return;
  }
  bool publish = false;
  if (plan.mute_on && this->parent_->enqueue_special({SPECIAL_OP_MUTE, 1}))
    this->publish_fan_index((uint8_t) plan.fan_index);
  if (plan.preset >= 0 && this->parent_->request_preset((uint8_t) plan.preset)) {
    this->publish_preset_index((uint8_t) plan.preset);
    publish = true;
  }
  if (plan.send_combined && this->parent_->send_user_command(plan.cmd, plan.fields, plan.power_on)) {
    if (plan.show_mode)
      this->mode = (climate::ClimateMode) plan.mode;
    if (plan.show_target)
      this->target_temperature = plan.target_c;
    if (plan.show_swing)
      this->swing_mode = (climate::ClimateSwingMode) plan.swing;
    if (plan.fan_index >= 0 && !plan.mute_on)
      this->publish_fan_index((uint8_t) plan.fan_index);
    publish = true;
  }
  if (publish)
    this->publish_state();
}

ClimateShown HisenseClimate::shown_() const {
  ClimateShown shown;
  shown.current_c = this->current_temperature;
  shown.target_c = this->target_temperature;
  shown.action = (uint8_t) this->action;
  shown.mode = (uint8_t) this->mode;
  shown.swing = (uint8_t) this->swing_mode;
  shown.fan_index = this->fan_index_;
  shown.preset_index = this->preset_index_;
  return shown;
}

// What a frame shows is climate_view_from_status() in hisense_entities.h, with a host test.
void HisenseClimate::update_from_bus(const AcState &state, bool holdoff) {
  const ClimateShown before = this->shown_();
  const ClimateView view =
      climate_view_from_status(state, holdoff, this->parent_->special_busy(), this->preset_support_);
  this->current_temperature = view.current_c;
  this->action = (climate::ClimateAction) view.action;
  if (view.controls) {
    if (view.mode_known)
      this->mode = (climate::ClimateMode) view.mode;
    this->target_temperature = view.target_c;
    this->swing_mode = (climate::ClimateSwingMode) view.swing;
    this->publish_fan_index(view.fan_index);
  }
  if (view.preset_known)
    this->publish_preset_index(view.preset);
  // A status frame arrives about once a second and is nearly always identical to the last, so
  // the entity is published only when something it shows moved.
  if (this->bus_state_published_ && !climate_shown_changed(before, this->shown_()))
    return;
  this->bus_state_published_ = true;
  this->publish_state();
}

// Sets the entity's fan fields for a ladder index as it is published (quiet already shown as low):
// the built-in enum for the steps ESPHome names, the custom string for the two it does not.
void HisenseClimate::publish_fan_index(uint8_t idx) {
  this->fan_index_ = idx;
  if (fan_index_is_custom(idx)) {
    this->set_custom_fan_mode_(fan_custom_name(idx));
  } else {
    this->clear_custom_fan_mode_();
    this->fan_mode = (climate::ClimateFanMode) fan_index_to_climate_fan(idx);
  }
}

void HisenseClimate::publish_preset_index(uint8_t idx) {
  if (idx >= PRESET_COUNT)
    return;
  this->preset_index_ = idx;
  if (idx >= PRESET_FIRST_CUSTOM) {
    this->set_custom_preset_(PRESETS[idx].name);
  } else {
    this->clear_custom_preset_();
    this->preset = idx == PRESET_ECO ? climate::CLIMATE_PRESET_ECO : climate::CLIMATE_PRESET_NONE;
  }
}

void HisenseClimate::dump_config() { LOG_CLIMATE("", "Hisense A/C climate", this); }

}  // namespace esphome::hisense_ac

#endif  // USE_CLIMATE
