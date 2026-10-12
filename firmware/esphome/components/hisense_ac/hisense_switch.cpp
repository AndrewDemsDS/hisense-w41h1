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
  SpecialOp op;
  if (this->kind_ == SWITCH_BEEPER) {
    // Rides the next command frame. Nothing to send now, and no hold-off to start.
    this->parent_->set_beeper(state);
  } else if (switch_special_op(this->kind_, state, &op)) {
    // Eco, turbo and quiet are special modes: they go through the hub's paced queue, shared
    // with the climate presets, so a switch flipped right after a preset change is not
    // swallowed by the A/C's debounce. A request the hub did not take is not shown.
    if (!this->parent_->enqueue_special(op))
      return;
  } else {
    // The display: a standing preference, re-asserted on every later frame (see
    // HisenseAC::set_display_pref). The frame sent now only carries it early, so the switch
    // shows the preference whether or not that frame could be queued.
    this->parent_->set_display_pref(state);
    this->parent_->send_command();
    this->parent_->note_user_command();
  }
  this->publish_state(state);
}

void HisenseSwitch::on_status(const AcState &state) {
  if (!special_entity_follows_status(this->parent_->in_command_holdoff(), this->parent_->special_busy()))
    return;
  bool on;
  if (switch_state_from_status(this->kind_, state, this->parent_->display_pref_on(), &on))
    this->publish_state(on);
}

void HisenseSwitch::dump_config() { LOG_SWITCH("", "Hisense A/C switch", this); }

}  // namespace esphome::hisense_ac
#endif  // USE_SWITCH
