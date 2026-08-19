#include "hisense_switch.h"
#include "esphome/core/log.h"

#ifdef USE_SWITCH

namespace esphome {
namespace hisense_ac {

static const char *const TAG = "hisense_ac.switch";

void HisenseSwitch::setup() {
  if (this->parent_ == nullptr)
    return;
  this->parent_->add_status_listener(this);
  // Boot the display switch to the firmware's standing preference (on), so the UI and what
  // the frames assert agree from the first poll rather than after the first toggle.
  if (this->kind_ == SWITCH_DISPLAY)
    this->publish_state(this->parent_->display_pref_on());
}

void HisenseSwitch::write_state(bool state) {
  if (this->parent_ == nullptr)
    return;
  HisenseCommand &cmd = this->parent_->cmd();

  switch (this->kind_) {
    case SWITCH_ECO:
      // Eco and turbo share one command byte and are mutually exclusive on this bus, so
      // clearing eco sends the explicit eco-off value rather than the neutral one (which
      // clears turbo instead).
      cmd.feature = state ? HISENSE_FEATURE_ECO : HISENSE_FEATURE_ECO_OFF;
      this->parent_->send_command();
      break;
    case SWITCH_TURBO:
      cmd.feature = state ? HISENSE_FEATURE_TURBO : HISENSE_FEATURE_NONE;
      this->parent_->send_command();
      break;
    case SWITCH_QUIET:
      this->parent_->send_mute(state);
      break;
    case SWITCH_DISPLAY:
      // Standing preference, re-asserted on every later frame. See the note on
      // HisenseAC::set_display_pref: byte 36 rides every command and 0x00 lights the panel.
      this->parent_->set_display_pref(state);
      this->parent_->send_command();
      break;
  }

  this->parent_->note_user_command();
  this->publish_state(state);
}

void HisenseSwitch::on_status(const HisenseState &state) {
  if (this->parent_ != nullptr && this->parent_->in_command_holdoff())
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
  }
}

void HisenseSwitch::dump_config() { LOG_SWITCH("", "Hisense A/C switch", this); }

}  // namespace hisense_ac
}  // namespace esphome
#endif  // USE_SWITCH
