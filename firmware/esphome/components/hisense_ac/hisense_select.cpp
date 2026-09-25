#include "hisense_select.h"
#include "esphome/core/log.h"

#ifdef USE_SELECT

namespace esphome::hisense_ac {

static const char *const TAG = "hisense_ac.select";

void HisenseSleepSelect::setup() {
  if (this->parent_ != nullptr)
    this->parent_->add_status_listener(this);
}

void HisenseSleepSelect::control(const std::string &value) {
  if (this->parent_ == nullptr)
    return;
  auto index = this->index_of(value);
  if (!index.has_value())
    return;
  // Option order IS profile order (0 = Off). Paced through the hub's special-mode queue.
  this->parent_->enqueue_special({SPECIAL_OP_SLEEP, (uint8_t) *index});
  this->parent_->note_user_command();
  this->publish_state(value);
}

void HisenseSleepSelect::on_status(const AcState &state) {
  if (this->parent_ != nullptr && (this->parent_->in_command_holdoff() || this->parent_->special_busy()))
    return;
  // Status carries profile * 2 (0x00 off, 0x02 General, 0x04 Old, 0x06 Young, 0x08 Kids).
  uint8_t profile = (uint8_t) (state.sleep_raw / 2);
  auto option = this->at(profile);
  if (option.has_value())
    this->publish_state(*option);
}

void HisenseSleepSelect::dump_config() { LOG_SELECT("", "Hisense A/C sleep profile", this); }

}  // namespace esphome::hisense_ac
#endif  // USE_SELECT
