#pragma once
// Sleep profile as an ESPHome select.
// Profiles are Off / General / Old / Young / Kids, confirmed on hardware as status byte 17
// = profile * 2 and command byte 17 is profile * 2 + 1.
#include "esphome/core/defines.h"
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#include "hisense_ac.h"

namespace esphome::hisense_ac {

class HisenseSleepSelect final : public select::Select,
                                 public Component,
                                 public StatusListener,
                                 public Parented<HisenseAC> {
 public:
  void setup() override;
  void dump_config() override;
  void on_status(const AcState &state) override;

 protected:
  void control(size_t index) override;

  /// Profile last published, so an unchanged status frame does not republish it.
  uint8_t published_profile_{0xFF};
};

}  // namespace esphome::hisense_ac
#endif  // USE_SELECT
