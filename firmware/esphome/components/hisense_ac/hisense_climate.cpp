#include "hisense_climate.h"
#include "esphome/core/log.h"

namespace esphome::hisense_ac {

static const char *const TAG = "hisense_ac.climate";

// Five of the seven steps are advertised as ESPHome's BUILT-IN fan enum, and only the two it
// has no name for stay custom. Mixing is not cosmetic: ClimateCall converts a name matching a
// built-in into the enum, and validate_() then DISCARDS that enum unless traits advertise it.
// Publishing everything as custom therefore made "High" unsettable, which is what shipped.
// Ladder index -> representation: 0 auto, 2 low, 3 "medium_low", 4 medium, 5 "medium_high",
// 6 high. Index 1 (quiet) is the `quiet` preset, not a fan mode, and publishes as low. The names
// match the hisense-unified-ac wrapper's fan_modes so a climate group syncs fan speed across
// firmwares.
const char *const FAN_CUSTOM_MEDIUM_LOW = "medium_low";
const char *const FAN_CUSTOM_MEDIUM_HIGH = "medium_high";
const char *const FAN_CUSTOM_NAMES[2] = {FAN_CUSTOM_MEDIUM_LOW, FAN_CUSTOM_MEDIUM_HIGH};

static bool fan_index_is_custom(uint8_t idx) { return idx == 3 || idx == 5; }

static climate::ClimateFanMode fan_index_to_enum(uint8_t idx) {
  switch (idx) {
    case 0:
      return climate::CLIMATE_FAN_AUTO;
    case 1:  // quiet step: not a fan mode, shows as low
    case 2:
      return climate::CLIMATE_FAN_LOW;
    case 4:
      return climate::CLIMATE_FAN_MEDIUM;
    default:
      return climate::CLIMATE_FAN_HIGH;
  }
}

// esphome_aircon_map.h mirrors these enums as plain ints so it stays host-testable. If upstream
// ever renumbers ClimateMode, this is where the build breaks, instead of the A/C silently
// switching to the wrong mode on the wire.
static_assert((int) climate::CLIMATE_MODE_OFF == CLIMATE_MODE_OFF_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_MODE_HEAT_COOL == CLIMATE_MODE_HEAT_COOL_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_MODE_COOL == CLIMATE_MODE_COOL_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_MODE_HEAT == CLIMATE_MODE_HEAT_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_MODE_FAN_ONLY == CLIMATE_MODE_FAN_ONLY_VALUE, "ClimateMode drift");
static_assert((int) climate::CLIMATE_MODE_DRY == CLIMATE_MODE_DRY_VALUE, "ClimateMode drift");
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

void HisenseClimate::setup() {
  if (this->parent_ != nullptr)
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
                              climate::CLIMATE_MODE_FAN_ONLY, climate::CLIMATE_MODE_HEAT_COOL});
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

  // none + eco ride the built-in enum (see esphome_aircon_map.h); the rest are the custom
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

void HisenseClimate::control(const climate::ClimateCall &call) {
  if (this->parent_ == nullptr)
    return;
  AcCommand &cmd = this->parent_->cmd();
  bool send_combined = false;
  bool powering_on = false;

  if (call.get_mode().has_value()) {
    climate::ClimateMode mode = *call.get_mode();
    if (mode == climate::CLIMATE_MODE_OFF) {
      this->parent_->send_power(false);
      this->mode = mode;
      this->parent_->note_user_command();
      this->publish_state();
      return;
    }
    Mode hmode;
    if (climate_mode_to_hisense((uint8_t) mode, &hmode)) {
      cmd.mode = hmode;
      this->mode = mode;
      send_combined = true;
      // The A/C ignores a mode change while powered down, so power on first and let the
      // combined frame carry the mode.
      if (!this->parent_->last_state().power_on)
        powering_on = true;
    } else {
      ESP_LOGW(TAG, "unsupported climate mode %d", (int) mode);
    }
  }

  if (call.get_target_temperature().has_value()) {
    // The A/C reads byte 19 in its panel's unit, so an F panel needs the value in F (#117).
    const AcState &st = this->parent_->last_state();
    int wanted = setpoint_to_cmd((int) lroundf(*call.get_target_temperature()), st.valid && st.temp_unit_f, &cmd);
    this->target_temperature = (float) wanted;
    send_combined = true;
  }

  // A fan request can arrive by EITHER route, and the trap is that it is not our choice which.
  // ClimateCall::set_fan_mode(const char *) case-insensitively matches the built-in enum names
  // FIRST, so "auto", "low", "medium" and "high" are converted to ClimateFanMode and never reach
  // has_custom_fan_mode(); only "medium_low" and "medium_high" stay custom. Handling just the
  // custom path silently dropped most of the speeds.
  int8_t wanted_index = -1;
  if (call.has_custom_fan_mode()) {
    StringRef wanted = call.get_custom_fan_mode();
    if (wanted == FAN_CUSTOM_MEDIUM_LOW) {
      wanted_index = 3;
    } else if (wanted == FAN_CUSTOM_MEDIUM_HIGH) {
      wanted_index = 5;
    }
  } else if (call.get_fan_mode().has_value()) {
    wanted_index = (int8_t) climate_fan_to_index((uint8_t) *call.get_fan_mode());
  }
  // A mode that owns the fan (turbo, quiet, sleep) overwrites any other speed about a second
  // later. Refuse instead of acknowledging a change that undoes itself, and keep showing the
  // pinned speed. Same rule as the wrapper's fan_mode_forced_by_preset.
  if (wanted_index >= 0 && !fan_request_allowed(this->parent_->projected_special(), (uint8_t) wanted_index)) {
    ESP_LOGW(TAG, "fan change refused: an active special mode (turbo/quiet/sleep) owns the fan");
    wanted_index = -1;
  }
  if (wanted_index >= 0) {
    if (wanted_index == 1) {
      // Unreachable from Home Assistant now that quiet is not an advertised fan mode (validate_()
      // drops it), kept so a direct API client asking for QUIET still gets the mute path.
      // "Quiet" is not reachable through the fan byte on this unit. Commanding fan 0x03 (the
      // 3-speed reference's "mute" value, flagged // VERIFY in the driver and never confirmed
      // on a W41H1) put the A/C on HIGH. The A/C reaches quiet via the MUTE flag instead:
      // toggling mute drove fan_raw to 0x02 on hardware 2026-08-19, which is what the driver
      // documents ("also sets fan_raw=0x02 quiet").
      this->parent_->enqueue_special({SPECIAL_OP_MUTE, 1});
    } else {
      cmd.fan = fan_index_to_hisense((uint8_t) wanted_index);
      send_combined = true;
    }
    this->publish_fan_index((uint8_t) wanted_index);
  }

  if (call.get_swing_mode().has_value()) {
    climate::ClimateSwingMode swing = *call.get_swing_mode();
    climate_swing_to_hisense((uint8_t) swing, &cmd.vswing, &cmd.hswing);
    this->swing_mode = swing;
    send_combined = true;
  }

  int preset = this->preset_request_index_(call);
  if (preset >= 0) {
    this->parent_->request_preset((uint8_t) preset);
    this->publish_preset_index((uint8_t) preset);
    this->publish_state();
  }

  if (powering_on)
    this->parent_->send_power(true);
  if (send_combined)
    this->parent_->send_command();
  if (powering_on || send_combined) {
    this->parent_->note_user_command();
    this->publish_state();
  }
}

void HisenseClimate::update_from_bus(const AcState &state, bool holdoff) {
  this->current_temperature = state.indoor_temp_c;
  this->action = (climate::ClimateAction) climate_action(state.power_on, state.mode, state.compressor_freq);
  // During the hold-off the frame may predate the user's command, so only telemetry is taken
  // from it. Everything the user can move is left showing what they asked for.
  if (!holdoff) {
    this->mode =
        state.power_on ? (climate::ClimateMode) hisense_mode_to_climate(state.mode) : climate::CLIMATE_MODE_OFF;
    this->target_temperature = state.setpoint_c;
    this->swing_mode = (climate::ClimateSwingMode) climate_swing(state.vswing_on, state.hswing_on);
    uint8_t idx = fan_raw_to_index(state.fan_raw);
    if (idx <= FAN_INDEX_MAX)
      this->publish_fan_index(idx);
  }
  // A preset can take several paced frames (~10 s apart). Until the last one has landed and been
  // held off, keep showing the requested preset: a climate group mirroring this entity would
  // otherwise copy every intermediate state to the other rooms.
  if (this->preset_support_ != 0 && !holdoff && !this->parent_->special_busy()) {
    SpecialState special = special_from_status(state.eco_on, state.turbo_on, state.mute_on, state.sleep_raw);
    this->publish_preset_index(preset_detect(special, this->preset_support_));
  }
  this->publish_state();
}

void HisenseClimate::publish_fan_index(uint8_t idx) {
  idx = fan_published_index(idx);  // quiet (1) shows as low; see esphome_aircon_map.h
  if (fan_index_is_custom(idx)) {
    this->set_custom_fan_mode_(idx == 3 ? FAN_CUSTOM_MEDIUM_LOW : FAN_CUSTOM_MEDIUM_HIGH);
  } else {
    this->clear_custom_fan_mode_();
    this->fan_mode = fan_index_to_enum(idx);
  }
}

int HisenseClimate::preset_request_index_(const climate::ClimateCall &call) const {
  if (this->preset_support_ == 0)
    return -1;
  int idx = -1;
  // Same two routes as the fan: "none" and "eco" match ESPHome's built-in names and arrive as
  // the enum; every other name arrives as a custom preset.
  if (call.has_custom_preset()) {
    StringRef wanted = call.get_custom_preset();
    for (const char *name : this->custom_presets_) {
      if (wanted == name) {
        idx = preset_index(name);
        break;
      }
    }
  } else if (call.get_preset().has_value()) {
    switch (*call.get_preset()) {
      case climate::CLIMATE_PRESET_NONE:
        idx = PRESET_NONE;
        break;
      case climate::CLIMATE_PRESET_ECO:
        idx = PRESET_ECO;
        break;
      default:
        break;
    }
  }
  if (idx < 0 || !preset_available((uint8_t) idx, this->preset_support_)) {
    if (call.has_custom_preset() || call.get_preset().has_value()) {
      ESP_LOGW(TAG, "unsupported preset requested");
    }
    return -1;
  }
  return idx;
}

void HisenseClimate::publish_preset_index(uint8_t idx) {
  if (idx >= PRESET_FIRST_CUSTOM) {
    this->set_custom_preset_(PRESETS[idx].name);
  } else {
    this->clear_custom_preset_();
    this->preset = idx == PRESET_ECO ? climate::CLIMATE_PRESET_ECO : climate::CLIMATE_PRESET_NONE;
  }
}

void HisenseClimate::dump_config() { LOG_CLIMATE("", "Hisense A/C climate", this); }

}  // namespace esphome::hisense_ac
