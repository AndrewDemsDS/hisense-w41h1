#pragma once
// The stock AEH-W41H1 module's own MCU (Realtek RTL8710C, AmebaZ2) under LibreTiny: what the
// module needs that no other board does.
//
//   - The flash layout check at boot (w41h1_slots.h). On a mismatch ESPHome's update server is
//     switched off, because the first update would brick the unit.
//   - The serial port recovery for the A/C bus (w41h1_module.cpp has what was measured, the
//     timing rule is in w41h1_uart_policy.h).
//   - Two diagnostic entities: how often the recovery has fired, and the layout report.
//
// The flash layout itself, the build-time check's flag and the reboot workaround are set by the
// codegen (__init__.py).
#ifdef USE_LIBRETINY

#include "esphome/core/component.h"
#include "esphome/components/hisense_ac/hisense_ac.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart_component_libretiny.h"

#include "w41h1_uart_policy.h"

namespace esphome::w41h1_module {

class W41H1Module : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  // After everything else, the update server included: the layout check switches that one off,
  // and the port is only watched once the hub and the UART are up.
  float get_setup_priority() const override { return -100.0f; }

  void set_hub(hisense_ac::HisenseAC *hub) { this->hub_ = hub; }
  void set_uart(uart::LibreTinyUARTComponent *uart) { this->uart_ = uart; }
  void set_ota(Component *ota) { this->ota_ = ota; }
  void set_serial_port_restarts_sensor(sensor::Sensor *sensor) { this->serial_port_restarts_sensor_ = sensor; }
  void set_flash_layout_text_sensor(text_sensor::TextSensor *sensor) { this->flash_layout_text_sensor_ = sensor; }

 protected:
  void watch_serial_port_();
  void restart_serial_port_();

  hisense_ac::HisenseAC *hub_{nullptr};
  uart::LibreTinyUARTComponent *uart_{nullptr};
  Component *ota_{nullptr};
  sensor::Sensor *serial_port_restarts_sensor_{nullptr};
  text_sensor::TextSensor *flash_layout_text_sensor_{nullptr};
  W41h1UartWatch uart_watch_;
};

}  // namespace esphome::w41h1_module

#endif  // USE_LIBRETINY
