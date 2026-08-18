#include "hisense_switch.h"
#include "esphome/core/log.h"

#ifdef USE_SWITCH

namespace esphome {
namespace hisense_ac {

static const char *const TAG = "hisense_ac.switch";

void HisenseSwitch::setup() {
  if (this->parent_ != nullptr)
    this->parent_->add_status_listener(this);
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
      // One-shot: send_command() resets it so ordinary traffic stops re-asserting the panel.
      cmd.display = state ? HISENSE_DISPLAY_ON : HISENSE_DISPLAY_OFF;
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
      // The status frame carries no display bit, so this one is write-only and keeps whatever
      // was last commanded. The Matter build hit the same wall (its ep9 switch could never
      // self-correct); nothing to publish here rather than publishing a guess.
      break;
  }
}

void HisenseSwitch::dump_config() { LOG_SWITCH("", "Hisense A/C switch", this); }

}  // namespace hisense_ac
}  // namespace esphome
#endif  // USE_SWITCH
