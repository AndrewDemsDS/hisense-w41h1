#include "hisense_switch.h"
#include "esphome/core/log.h"

#ifdef USE_SWITCH

namespace esphome::hisense_ac {

static const char *const TAG = "hisense_ac.switch";

void HisenseSwitch::setup() {
  if (this->parent_ == nullptr)
    return;
  this->parent_->add_status_listener(this);
  // Boot the display switch to the firmware's standing preference (on), so the UI and what
  // the frames assert agree from the first poll rather than after the first toggle.
  if (this->kind_ == SWITCH_DISPLAY)
    this->publish_state(this->parent_->display_pref_on());
  // The beeper is a setting of this node, not of the A/C, so it comes back from flash.
  if (this->kind_ == SWITCH_BEEPER) {
    bool on = this->get_initial_state_with_restore_mode().value_or(true);
    this->parent_->set_beeper(on);
    this->publish_state(on);
  }
}

void HisenseSwitch::write_state(bool state) {
  if (this->parent_ == nullptr)
    return;
  if (this->kind_ == SWITCH_BEEPER) {
    // Rides the next command frame. Nothing to send now, and no hold-off to start.
    this->parent_->set_beeper(state);
    this->publish_state(state);
    return;
  }
  switch (this->kind_) {
    // Eco, turbo and quiet are special modes: they go through the hub's paced queue, shared
    // with the climate presets, so a switch flipped right after a preset change is not
    // swallowed by the A/C's debounce.
    case SWITCH_ECO:
      // Eco and turbo share one command byte and are mutually exclusive on this bus, so
      // clearing eco sends the explicit eco-off value rather than the neutral one (which
      // clears turbo instead).
      this->parent_->enqueue_special({SPECIAL_OP_FEATURE, (uint8_t) (state ? FEATURE_ECO : FEATURE_ECO_OFF)});
      break;
    case SWITCH_TURBO:
      this->parent_->enqueue_special({SPECIAL_OP_FEATURE, (uint8_t) (state ? FEATURE_TURBO : FEATURE_NONE)});
      break;
    case SWITCH_QUIET:
      this->parent_->enqueue_special({SPECIAL_OP_MUTE, (uint8_t) (state ? 1 : 0)});
      break;
    case SWITCH_DISPLAY:
      // Standing preference, re-asserted on every later frame. See the note on
      // HisenseAC::set_display_pref: byte 36 rides every command and 0x00 lights the panel.
      this->parent_->set_display_pref(state);
      this->parent_->send_command();
      break;
    case SWITCH_BEEPER:
      break;
  }

  this->parent_->note_user_command();
  this->publish_state(state);
}

void HisenseSwitch::on_status(const AcState &state) {
  if (this->parent_->in_command_holdoff() || this->parent_->special_busy())
    return;
  switch (this->kind_) {
    case SWITCH_ECO:
      this->publish_state(state.eco_on);
      break;
    case SWITCH_TURBO:
      this->publish_state(state.turbo_on);
      break;
    case SWITCH_QUIET:
      this->publish_state(state.mute_on);
      break;
    case SWITCH_DISPLAY:
      // The status frame carries no display bit, so this one is write-only. Publishing the
      // standing preference at least keeps the switch honest about what the firmware is
      // asserting, which is what the panel will be showing unless the remote changed it.
      this->publish_state(this->parent_->display_pref_on());
      break;
    case SWITCH_BEEPER:
      break;
  }
}

void HisenseSwitch::dump_config() { LOG_SWITCH("", "Hisense A/C switch", this); }

}  // namespace esphome::hisense_ac
#endif  // USE_SWITCH
