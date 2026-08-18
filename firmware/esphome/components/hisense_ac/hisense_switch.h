#pragma once
// Eco / Turbo / Quiet / panel-display switches: the ep3/ep4/ep5/ep9 OnOff endpoints of the
// Matter builds, as ordinary ESPHome switches.
#include "esphome/core/defines.h"
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#include "hisense_ac.h"

namespace esphome {
namespace hisense_ac {

enum SwitchKind : uint8_t {
  SWITCH_ECO = 0,
  SWITCH_TURBO,
  SWITCH_QUIET,
  SWITCH_DISPLAY,
};

class HisenseSwitch : public switch_::Switch, public Component, public StatusListener {
 public:
  void setup() override;
  void dump_config() override;
  void set_parent(HisenseAC *parent) { this->parent_ = parent; }
  void set_kind(SwitchKind kind) { this->kind_ = kind; }
  void on_status(const HisenseState &state) override;

 protected:
  void write_state(bool state) override;

  HisenseAC *parent_{nullptr};
  SwitchKind kind_{SWITCH_ECO};
};

}  // namespace hisense_ac
}  // namespace esphome
#endif  // USE_SWITCH
