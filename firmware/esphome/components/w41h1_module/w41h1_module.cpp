// The stock AEH-W41H1 module under LibreTiny: the flash layout check at boot, the serial port
// recovery for the A/C bus, and the two diagnostic entities. w41h1_module.h has the overview.
#include "w41h1_module.h"

#ifdef USE_LIBRETINY

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "w41h1_slots.h"

namespace esphome::w41h1_module {

ESPHOME_LOG_TAG(TAG, "w41h1_module");

// Long enough for w41h1_layout_report() with every number at its widest.
static const size_t LAYOUT_REPORT_LEN = 96;
// How often the hub's link state is looked at. w41h1_uart_restart_due() is written for about
// once a second.
static const uint32_t UART_WATCH_INTERVAL_MS = 1000;

void W41H1Module::setup() {
  char report[LAYOUT_REPORT_LEN];
  w41h1_layout_report(report, sizeof(report));
  if (w41h1_layout_ok()) {
    ESP_LOGI(TAG, "Flash layout: %s", report);
  } else {
    // The bootloader's partition table is not the one this image was built for. An update
    // would be written to the wrong place and the running image invalidated, so the update
    // server is switched off. Reflash with the layout this unit has.
    ESP_LOGE(TAG, "Flash layout: %s", report);
    // The schema requires ota_id, so the pointer is only null in a node put together by hand in
    // C++. Then nothing can be switched off here, and the log has to say so.
    if (this->ota_ != nullptr) {
      ESP_LOGE(TAG, "OTA is disabled: this image was built for another flash layout");
      this->ota_->mark_failed();
    } else {
      ESP_LOGE(TAG, "This image was built for another flash layout: do NOT update this unit over the air");
    }
  }
  // The first look at the port comes one interval after setup, not in the first loop.
  this->last_look_ms_ = millis();
}

void W41H1Module::update() {
  if (this->serial_port_restarts_sensor_ != nullptr)
    this->serial_port_restarts_sensor_->publish_state((float) this->uart_watch_.restarts);
  if (this->flash_layout_text_sensor_ != nullptr) {
    char report[LAYOUT_REPORT_LEN];
    w41h1_layout_report(report, sizeof(report));
    this->flash_layout_text_sensor_->publish_state(report);
  }
}

void W41H1Module::dump_config() {
  char report[LAYOUT_REPORT_LEN];
  w41h1_layout_report(report, sizeof(report));
  ESP_LOGCONFIG(TAG,
                "W41H1 module:\n"
                "  Flash layout: %s\n"
                "  Serial port restarts: %u",
                report, (unsigned) this->uart_watch_.restarts);
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Serial port restarts", this->serial_port_restarts_sensor_);
  LOG_TEXT_SENSOR("  ", "Flash layout", this->flash_layout_text_sensor_);
}

// Serial port recovery for the A/C bus (UART0).
//
// What was seen, on one module in an A/C, 2026-10-10: twice the receive side of the bus port
// stopped and stayed stopped until the unit was power cycled. Once within the first minute of the
// first boot, once right after the first command sent to a unit that had been idle for five
// minutes. Transmit kept working both times: the unit obeyed commands while the node reported
// the link as lost.
//
// A cause that gives exactly this, found on a second module the same day: LibreTiny 1.13.0 keeps
// received bytes in the Arduino RingBufferN. Its reader does `_numElems--` with the receive
// interrupt enabled, and the interrupt does `_numElems++`. When the interrupt lands between the
// load and the store of that decrement, the byte it stored is never counted. The byte stays in
// the buffer, the count says one less, and from then on every frame is read one byte late and
// one byte short. The frame assembler never completes a frame again: reply timeouts, no checksum
// error, transmit fine.
// Measured with the UART in internal loopback (the real interrupt and buffer, nothing on the bus):
//   - 115200 baud, single reads at random times, no lock: the count fell behind 21 times in
//     142558 bytes (first after 0.8 s) and stayed behind. With interrupts masked around the read
//     (PRIMASK, or ESPHome's InterruptLock): 0 times in 142551 and 142485 bytes.
//   - 9600 baud, reads in groups every 2 to 10 ms as the main loop does, no lock: 5 times in
//     233034 bytes, one a minute. With InterruptLock: 0 times in 233287 bytes.
// No byte was lost or out of order in any run. The component now reads under InterruptLock on
// LibreTiny (HisenseAC::bus_read), which removes the cause. Reopening the port also clears a
// buffer that has fallen behind, because begin() makes a new one: that is why the restart below
// brought the link back.
//
// The restart stays as a fallback for a stall with any other cause. It cannot help a unit that
// does not answer at all: on the second module the bus went silent at the pin itself (no low
// level on RX in 2.3 million samples after our frames), and neither about 100 port restarts,
// eight software resets nor a power-on reset brought a reply.
//
// So: when the hub reports the link lost, and every 15 s for as long as it stays down, never
// while the hub is transmitting (w41h1_uart_policy.h). This lives in this component and not in
// the hub because the port is LibreTiny's: ESPHome's uart component can only reload a port's
// settings on ESP32 and ESP8266.
void W41H1Module::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  if (now - this->last_look_ms_ < UART_WATCH_INTERVAL_MS)
    return;
  this->last_look_ms_ = now;
  if (!w41h1_uart_restart_due(&this->uart_watch_, this->parent_->link_up(), this->parent_->bus_transmitting(), now))
    return;
  this->restart_serial_port_();
  ESP_LOGW(TAG, "A/C bus silent: serial port restarted (%u so far)", (unsigned) this->uart_watch_.restarts);
}

// Tear the port down and bring it up again: pin mux, interrupt hook and FIFO all start fresh.
// The object is the one ESPHome's uart component holds (Serial0 for PA14 / PA13), opened again
// with the settings that component opened it with (9600 8N1, which the hub's validation
// requires), so the hub keeps reading from the same receive buffer afterwards.
void W41H1Module::restart_serial_port_() {
  HardwareSerial *serial = this->uart_->get_hw_serial();
  serial->end();
  serial->begin(this->uart_->get_baud_rate(), this->uart_->get_config());
}

}  // namespace esphome::w41h1_module

#ifdef USE_LIBRETINY_VARIANT_RTL8720C
// The other half of --wrap=lt_ota_dual_switch_flag (__init__.py has why). LibreTiny's own scan
// of the bootloader decides: 2 is its LT_OTA_SWITCH_BIT_FLAG, and only that bootloader reads the
// flag, so on any other the flag stays as it is.
extern "C" uint8_t lt_ota_check_switch_type();
extern "C" bool __real_lt_ota_dual_switch_flag();
extern "C" bool __wrap_lt_ota_dual_switch_flag() {
  if (lt_ota_check_switch_type() == 2)
    return __real_lt_ota_dual_switch_flag();
  return true;
}
#endif

#endif  // USE_LIBRETINY
