// test_reference_map.cpp -- the reference driver's protocol tables (aircon_map_common.h) and
// power estimate (power_estimate.h), tied to the validated wire bytes: a speed step or a status
// byte must rebuild to the command byte the captures show.
//
// These cases come from the retired Matter builds' map test. The cluster-specific cases (attribute
// conversions, echo ledgers, capability gates) went with those builds; what is left is protocol
// logic that the reference ESPHome map and the codec parity test build on.
#include "hal_stub.h"
#include "aircon_map_common.h"
#include "power_estimate.h"
#include "hisense_rs485.h"
#include "test_common.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

// build a command from a shadow and return byte @off
static uint8_t cmd_byte(HisenseCommand c, int off) {
  uint8_t f[64];
  hisense_build_command(&c, f, sizeof(f));
  return f[off];
}

int main() {
  printf("== reference map: fan ladder, setpoint clamp, running state, power estimate ==\n");
  HisenseCommand base = {
      HISENSE_MODE_COOL,       24, false, HISENSE_FAN_AUTO, HISENSE_SWING_OFF, HISENSE_SWING_OFF, HISENSE_FEATURE_NONE,
      HISENSE_DISPLAY_NOCHANGE};

  // ---- fan: speed step -> command enum -> wire byte16 ----
  printf("[fan speed step -> command byte16]\n");
  struct {
    uint8_t sp;
    uint8_t b;
  } speeds[] = {{1, 0x03}, {2, 0x0B}, {3, 0x0D}, {4, 0x0F}, {5, 0x11}, {6, 0x13}};
  for (auto &s : speeds) {
    HisenseCommand c = base;
    c.fan = speed_to_hisense_fan(s.sp);
    CHECK(cmd_byte(c, 16) == s.b, "SpeedSetting %d -> byte16 0x%02X exp 0x%02X", s.sp, cmd_byte(c, 16), s.b);
  }

  CHECK(speed_to_hisense_fan(0) == HISENSE_FAN_AUTO && speed_to_hisense_fan(7) == HISENSE_FAN_AUTO,
        "step 0 and an out-of-range step -> auto");

  // ---- fan status -> speed step ----
  printf("[fan status -> speed step]\n");
  CHECK(hisense_fan_raw_to_speed(0x0A) == 2, "low raw->speed 2");
  CHECK(hisense_fan_raw_to_speed(0x12) == 6, "high raw->speed 6");
  CHECK(hisense_fan_raw_to_speed(0x01) == 0 && hisense_fan_raw_to_speed(0xFF) == 0, "auto and unknown raw -> 0");

  // ---- fan status raw -> command-shadow enum (shadow-sync, docs/07 Tier-1 #2) ----
  printf("[fan status raw -> command enum]\n");
  CHECK(hisense_fan_raw_to_cmd(0x01) == HISENSE_FAN_AUTO, "raw 0x01 -> AUTO");
  CHECK(hisense_fan_raw_to_cmd(0x02) == HISENSE_FAN_QUIET, "raw 0x02 -> QUIET");
  CHECK(hisense_fan_raw_to_cmd(0x0A) == HISENSE_FAN_LOW, "raw 0x0A -> LOW");
  CHECK(hisense_fan_raw_to_cmd(0x0C) == HISENSE_FAN_MED_LOW, "raw 0x0C -> MED_LOW");
  CHECK(hisense_fan_raw_to_cmd(0x0E) == HISENSE_FAN_MID, "raw 0x0E -> MID");
  CHECK(hisense_fan_raw_to_cmd(0x10) == HISENSE_FAN_MED_HIGH, "raw 0x10 -> MED_HIGH");
  CHECK(hisense_fan_raw_to_cmd(0x12) == HISENSE_FAN_HIGH, "raw 0x12 -> HIGH");
  CHECK(hisense_fan_raw_to_cmd(0xFF) == HISENSE_FAN_NOCHANGE, "raw unknown -> NOCHANGE (no clobber, #59)");
  // a synced fan must rebuild to the golden command byte16 (0x0A->0x0B, 0x12->0x13)
  {
    HisenseCommand c = base;
    c.fan = hisense_fan_raw_to_cmd(0x0A);
    CHECK(cmd_byte(c, 16) == 0x0B, "raw 0x0A synced -> byte16 0x%02X exp 0x0B", cmd_byte(c, 16));
  }
  {
    HisenseCommand c = base;
    c.fan = hisense_fan_raw_to_cmd(0x12);
    CHECK(cmd_byte(c, 16) == 0x13, "raw 0x12 synced -> byte16 0x%02X exp 0x13", cmd_byte(c, 16));
  }
  // the NOCHANGE sentinel must never reach the wire -- it packs the 0x01 filler (#59)
  {
    HisenseCommand c = base;
    c.fan = HISENSE_FAN_NOCHANGE;
    CHECK(cmd_byte(c, 16) == 0x01, "NOCHANGE fan -> byte16 0x%02X exp 0x01 (filler)", cmd_byte(c, 16));
  }

  // ---- setpoint clamp -> wire byte19 ----
  printf("[setpoint clamp -> command byte19]\n");
  CHECK(hisense_clamp_setpoint_c(10) == 16, "below range clamps to 16");
  CHECK(hisense_clamp_setpoint_c(40) == 32, "above range clamps to 32");
  {
    HisenseCommand c = base;
    c.setpoint = (int8_t) hisense_clamp_setpoint_c(22);
    CHECK(cmd_byte(c, 19) == 0x2D, "setpt 22 -> byte19 0x%02X exp 0x2D", cmd_byte(c, 19));
  }

  // ---- Dry/Fan-only lockout: strip setpoint (both), strip fan (Dry only) (#53) ----
  printf("[dry/fan-only frame lockout]\n");
  {
    HisenseCommand c = base;
    c.mode = HISENSE_MODE_DRY;
    c.setpoint = 24;
    c.fan = HISENSE_FAN_HIGH;
    CHECK(cmd_byte(c, 19) == 0x00, "DRY strips setpoint byte19=0x%02X exp 0x00", cmd_byte(c, 19));
    CHECK(cmd_byte(c, 16) == 0x01, "DRY strips fan byte16=0x%02X exp 0x01", cmd_byte(c, 16));
  }
  {
    HisenseCommand c = base;
    c.mode = HISENSE_MODE_FAN;
    c.setpoint = 24;
    c.fan = HISENSE_FAN_HIGH;
    CHECK(cmd_byte(c, 19) == 0x00, "FAN-only strips setpoint byte19=0x%02X exp 0x00", cmd_byte(c, 19));
    CHECK(cmd_byte(c, 16) == 0x13, "FAN-only keeps fan byte16=0x%02X exp 0x13", cmd_byte(c, 16));
  }
  // an out-of-range setpoint must NOT drop the whole Dry/Fan frame (range check gated)
  {
    HisenseCommand c = base;
    c.mode = HISENSE_MODE_FAN;
    c.setpoint = 99;
    uint8_t f[64];
    CHECK(hisense_build_command(&c, f, sizeof(f)) > 0, "FAN-only builds despite out-of-range setpoint");
  }

  // ---- running state: the compressor decides between working and idle ----
  printf("[running state -> hvac_action bits]\n");
  CHECK(hisense_to_running_state(false, HISENSE_MODE_COOL, 50) == 0, "off -> 0");
  CHECK(hisense_to_running_state(true, HISENSE_MODE_COOL, 50) == 2, "cool+comp -> Cool(2)");
  CHECK(hisense_to_running_state(true, HISENSE_MODE_HEAT, 50) == 1, "heat+comp -> Heat(1)");
  CHECK(hisense_to_running_state(true, HISENSE_MODE_COOL, 0) == 0, "cool idle(comp0) -> 0");
  CHECK(hisense_to_running_state(true, HISENSE_MODE_FAN, 0) == 4, "fan-only -> Fan(4)");

  // ---- power estimate (calibrated 2026-07-07, docs/09 §4b) ----
  printf("[power: P = 4.15 * @55^2 W, calibrated 3-point]\n");
  CHECK(hisense_active_power_mw(0) == 0, "@55=0  -> 0 W");
  // meter points: @55=8 -> 268W(+-3%), @55=15 -> 934W(+-3%)
  {
    int64_t w = hisense_active_power_mw(8) / 1000;
    CHECK(w >= 255 && w <= 280, "@55=8  -> %lldW exp ~268", (long long) w);
  }
  {
    int64_t w = hisense_active_power_mw(15) / 1000;
    CHECK(w >= 905 && w <= 965, "@55=15 -> %lldW exp ~934", (long long) w);
  }
  CHECK(hisense_active_power_mw(16) > hisense_active_power_mw(15), "monotonic in @55");
  // glitchy/reserved @55 byte must be clamped, not report ~270 kW into HA's dashboard
  CHECK(hisense_active_power_mw(255) == HISENSE_POWER_MW_MAX, "@55=255 clamped to %lldmW",
        (long long) HISENSE_POWER_MW_MAX);
  CHECK(hisense_active_power_mw(255) <= 50000000LL, "@55=255 <= 50kW ceiling");
  // derived current vs meter: @55=15 ~4.47A, @55=8 ~1.27A (meter 4.2/1.2)
  {
    int64_t ma = hisense_active_current_ma(15, 220);
    CHECK(ma >= 4200 && ma <= 4700, "@55=15,220V -> %lldmA exp ~4470", (long long) ma);
  }
  CHECK(hisense_active_current_ma(15, 0) == 0, "unknown voltage -> 0 mA");
  CHECK(hisense_voltage_mv(220) == 220000, "@50=220 -> 220000 mV");
  // energy: 1000 W for 3600 s (3.6e6 ms) = 1000 mWh... 1000W=1e6mW; 1e6mW*3.6e6ms=3.6e12 mW-ms /3.6e6 = 1e6 mWh = 1 kWh
  {
    uint64_t acc = 0;
    hisense_energy_add(&acc, 1000000, 3600000);
    CHECK(hisense_energy_mwh(acc) == 1000000, "1kW*1h -> 1e6 mWh (1 kWh)");
  }
  {
    uint64_t acc = 0;
    hisense_energy_add(&acc, -5, 3600000);
    CHECK(acc == 0, "negative power clamped (import-only)");
  }

  printf("== %d passed, %d failed ==\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
