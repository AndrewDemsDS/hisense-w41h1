#include "hisense_select.h"
#include "esphome/core/log.h"

#ifdef USE_SELECT

namespace esphome::hisense_ac {

ESPHOME_LOG_TAG(TAG, "hisense_ac.select");

void HisenseSleepSelect::setup() { this->parent_->add_status_listener(this); }

void HisenseSleepSelect::control(size_t index) {
  // Option order IS profile order (0 = Off). Paced through the hub's special-mode queue. A
  // request the hub did not take is not shown: the select keeps the unit's profile.
  if (!this->parent_->enqueue_special({SPECIAL_OP_SLEEP, (uint8_t) index}))
    return;
  this->published_profile_ = (uint8_t) index;
  this->publish_state(index);
}

void HisenseSleepSelect::on_status(const AcState &state) {
  if (!special_entity_follows_status(this->parent_->in_command_holdoff(), this->parent_->special_busy()))
    return;
  uint8_t profile;
  if (!sleep_profile_from_status(state.sleep_raw, &profile) || profile == this->published_profile_)
    return;
  if (!this->has_index(profile))
    return;
  this->published_profile_ = profile;
  this->publish_state((size_t) profile);
}

void HisenseSleepSelect::dump_config() { LOG_SELECT("", "Hisense A/C sleep profile", this); }

}  // namespace esphome::hisense_ac
#endif  // USE_SELECT
