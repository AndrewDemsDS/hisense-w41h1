#pragma once
// Sleep profile: the ep6 ModeSelect endpoint of the Matter builds, as an ESPHome select.
// Profiles are Off / General / Old / Young / Kids, confirmed on hardware as status byte 17
// = profile * 2 and command byte 17 = profile * 2 + 1.
#include "esphome/core/defines.h"
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#include "hisense_ac.h"

namespace esphome {
namespace hisense_ac {

class HisenseSleepSelect : public select::Select, public Component, public StatusListener {
 public:
  void setup() override;
  void dump_config() override;
  void set_parent(HisenseAC *parent) { this->parent_ = parent; }
  void on_status(const HisenseState &state) override;

 protected:
  void control(const std::string &value) override;

  HisenseAC *parent_{nullptr};
};

}  // namespace hisense_ac
}  // namespace esphome
#endif  // USE_SELECT
