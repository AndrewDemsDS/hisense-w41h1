// LEGACY transport: the shared driver's own FreeRTOS bus task (firmware/src/rs485-driver) over its
// ESP-IDF HAL, as the ESPHome build ran before the uart transport existed. Compiled only when the
// YAML uses tx_pin / rx_pin (USE_HISENSE_AC_LEGACY_DRIVER); this is the one file that includes the
// driver's C header. Frames are built by the port (hisense_protocol.*), which the parity test
// holds byte-for-byte equal to the driver's builders, so only the transport differs.
#include "esphome/core/defines.h"
#ifdef USE_HISENSE_AC_LEGACY_DRIVER

#include "hisense_ac.h"
#include "esphome/core/log.h"

extern "C" {
#include "hisense_rs485.h"
}
#include <FreeRTOS.h>  // pdPASS

namespace esphome::hisense_ac {

static const char *const TAG = "hisense_ac.legacy";

// The driver's callbacks are plain C function pointers with no user context. One A/C per node,
// which is the only topology the bus supports anyway.
static HisenseAC *legacy_hub = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

static AcState to_state(const HisenseState &s) {
  AcState o;
  o.valid = s.valid;
  o.power_on = s.power_on;
  o.mode = static_cast<Mode>(s.mode);
  o.temp_unit_f = s.temp_unit_f;
  o.indoor_temp_c = s.indoor_temp_c;
  o.setpoint_c = s.setpoint_c;
  o.fan_raw = s.fan_raw;
  o.vswing_on = s.vswing_on;
  o.turbo_on = s.turbo_on;
  o.eco_on = s.eco_on;
  o.hswing_on = s.hswing_on;
  o.heat_relay_on = s.heat_relay_on;
  o.mute_on = s.mute_on;
  o.sleep_on = s.sleep_on;
  o.sleep_raw = s.sleep_raw;
  o.purify_on = s.purify_on;
  o.outdoor_temp_c = s.outdoor_temp_c;
  o.coil_temp_c = s.coil_temp_c;
  o.compressor_freq = s.compressor_freq;
  o.current_raw = s.current_raw;
  o.voltage_raw = s.voltage_raw;
  return o;
}

static void status_trampoline(const HisenseState *state) {
  if (legacy_hub != nullptr && state != nullptr)
    legacy_hub->on_bus_status(to_state(*state));
}

static void link_trampoline(bool link_up) {
  if (legacy_hub != nullptr)
    legacy_hub->on_bus_link(link_up);
}

bool HisenseAC::transport_start_() {
  legacy_hub = this;
  hisense_set_link_cb(&link_trampoline);
  if (hisense_init(&status_trampoline) != pdPASS) {
    ESP_LOGE(TAG, "hisense_init() failed: no bus task");
    return false;
  }
  ESP_LOGCONFIG(TAG, "RS-485 bus task started (TX=%d RX=%d DE=%d)", PA_14, PA_13, PA_17);
  return true;
}

bool HisenseAC::send_frame_(const uint8_t *frame, size_t len) { return hisense_send_frame(frame, len); }

bool HisenseAC::transport_faults_(AcFaults *out) {
  HisenseFaults f;
  if (!hisense_get_faults(&f))
    return false;
  AcFaults o;
  o.valid = f.valid;
  o.any = f.any;
  o.raw_indoor = f.raw_indoor;
  o.raw_module = f.raw_module;
  o.raw_outdoor = f.raw_outdoor;
  o.raw_protect = f.raw_protect;
  o.in_temp = f.in_temp;
  o.in_coil_temp = f.in_coil_temp;
  o.in_humidity = f.in_humidity;
  o.water_full = f.water_full;
  o.in_fan_motor = f.in_fan_motor;
  o.grille = f.grille;
  o.in_vzero = f.in_vzero;
  o.in_com = f.in_com;
  o.in_display = f.in_display;
  o.in_keys = f.in_keys;
  o.in_wifi = f.in_wifi;
  o.in_ele = f.in_ele;
  o.in_eeprom = f.in_eeprom;
  o.out_eeprom = f.out_eeprom;
  o.out_coil_temp = f.out_coil_temp;
  o.out_gas_temp = f.out_gas_temp;
  o.out_temp = f.out_temp;
  o.over_temp = f.over_temp;
  *out = o;
  return true;
}

bool HisenseAC::transport_features_(AcFeatures *out) {
  HisenseFeatures f;
  if (!hisense_get_features(&f))
    return false;
  // The bitmap round trip is exact for every field but the diagnostic reply length.
  *out = features_from_bitmap32(hisense_features_to_bitmap32(&f));
  out->reply_len = f.reply_len;
  return true;
}

void HisenseAC::transport_link_token_(uint8_t *hi, uint8_t *lo) { hisense_get_link_token(hi, lo); }

uint32_t HisenseAC::transport_checksum_mismatches_() { return hisense_checksum_mismatch_count(); }

}  // namespace esphome::hisense_ac

#endif  // USE_HISENSE_AC_LEGACY_DRIVER
