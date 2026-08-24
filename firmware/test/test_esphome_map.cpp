// test_esphome_map.cpp -- ESPHome-side QA (no chip, no ESPHome installed): asserts the
// ESPHome<->Hisense mapping and ties it end-to-end to the validated wire bytes, the same way
// test_matter_map.cpp does for the Matter path.
//
// esphome_aircon_map.h deliberately holds only the enum mapping; the fan ladder, setpoint
// clamp and running-state derivation are reused from matter_aircon_map.h. So the interesting
// cases here are the ones where ESPHome and Matter differ: Hisense AUTO maps to
// HEAT_COOL (not ESPHome's AUTO, which means "a schedule decides"), and hvac_action is an
// enum rather than a bitmap, with a Dry state Matter has no running-state bit for.
//
// The ESPHOME_CLIMATE_* constants mirror esphome::climate::ClimateMode / ClimateAction /
// ClimateSwingMode. This test pins the mapping; hisense_climate.cpp static_asserts the
// constants themselves against the real enums at firmware build time, so the two halves
// together make an upstream renumber impossible to miss.

#include "hal_stub.h"
#include "esphome_aircon_map.h"
#include "matter_aircon_map.h"
#include "hisense_rs485.h"
#include "test_common.h"
#include <cstdio>
#include <initializer_list>

// build a command from a shadow and return byte @off
static uint8_t cmd_byte(HisenseCommand c, int off) {
    uint8_t f[64]; hisense_build_command(&c, f, sizeof(f)); return f[off];
}

int main() {
    printf("== ESPHome <-> Hisense mapping tests ==\n");
    HisenseMode hm;
    HisenseSwingMode vs, hs;
    HisenseCommand base = { HISENSE_MODE_COOL, 24, false, HISENSE_FAN_AUTO,
                            HISENSE_SWING_OFF, HISENSE_SWING_OFF, HISENSE_FEATURE_NONE,
                            HISENSE_DISPLAY_NOCHANGE };

    // ---- mode mapping (both directions + round-trip) ----
    printf("[mode]\n");
    CHECK(esphome_mode_to_hisense(ESPHOME_CLIMATE_MODE_COOL, &hm) && hm == HISENSE_MODE_COOL,
          "COOL->COOL");
    CHECK(esphome_mode_to_hisense(ESPHOME_CLIMATE_MODE_HEAT, &hm) && hm == HISENSE_MODE_HEAT,
          "HEAT->HEAT");
    CHECK(esphome_mode_to_hisense(ESPHOME_CLIMATE_MODE_DRY, &hm) && hm == HISENSE_MODE_DRY,
          "DRY->DRY");
    CHECK(esphome_mode_to_hisense(ESPHOME_CLIMATE_MODE_FAN_ONLY, &hm) && hm == HISENSE_MODE_FAN,
          "FAN_ONLY->FAN");
    CHECK(esphome_mode_to_hisense(ESPHOME_CLIMATE_MODE_HEAT_COOL, &hm) && hm == HISENSE_MODE_AUTO,
          "HEAT_COOL->AUTO (the A/C's own auto)");
    // OFF rides the separate power frame, and ESPHome's AUTO means a schedule decides, which
    // this A/C has no concept of. Both must be rejected rather than silently mapped.
    CHECK(!esphome_mode_to_hisense(ESPHOME_CLIMATE_MODE_OFF, &hm), "OFF is not a HisenseMode");
    CHECK(!esphome_mode_to_hisense(ESPHOME_CLIMATE_MODE_AUTO, &hm), "AUTO(schedule) rejected");

    for (int m = 0; m <= 6; m++) {
        if (m == ESPHOME_CLIMATE_MODE_OFF || m == ESPHOME_CLIMATE_MODE_AUTO) continue;
        esphome_mode_to_hisense((uint8_t) m, &hm);
        CHECK(hisense_mode_to_esphome(hm) == m, "mode round-trip %d", m);
    }

    // ---- mode reaches the wire as the CONFIRMED command byte ----
    printf("[mode -> wire]\n");
    esphome_mode_to_hisense(ESPHOME_CLIMATE_MODE_HEAT_COOL, &hm);
    base.mode = hm;
    CHECK(cmd_byte(base, 18) == 0x90, "HEAT_COOL -> byte18=0x90 (confirmed AUTO command)");
    esphome_mode_to_hisense(ESPHOME_CLIMATE_MODE_COOL, &hm);
    base.mode = hm;
    CHECK(cmd_byte(base, 18) == 0x50, "COOL -> byte18=0x50");

    // ---- hvac_action ----
    printf("[action]\n");
    CHECK(hisense_to_climate_action(false, HISENSE_MODE_COOL, 0) == ESPHOME_CLIMATE_ACTION_OFF,
          "powered off -> OFF");
    CHECK(hisense_to_climate_action(true, HISENSE_MODE_COOL, 45) == ESPHOME_CLIMATE_ACTION_COOLING,
          "cool + compressor -> COOLING");
    CHECK(hisense_to_climate_action(true, HISENSE_MODE_HEAT, 45) == ESPHOME_CLIMATE_ACTION_HEATING,
          "heat + compressor -> HEATING");
    CHECK(hisense_to_climate_action(true, HISENSE_MODE_FAN, 0) == ESPHOME_CLIMATE_ACTION_FAN,
          "fan mode -> FAN");
    // The compressor is the difference between "on" and "actually working". Matter renders the
    // idle case as no badge; ESPHome has a real IDLE state for it.
    CHECK(hisense_to_climate_action(true, HISENSE_MODE_COOL, 0) == ESPHOME_CLIMATE_ACTION_IDLE,
          "cool, compressor stopped -> IDLE");
    CHECK(hisense_to_climate_action(true, HISENSE_MODE_DRY, 30) == ESPHOME_CLIMATE_ACTION_DRYING,
          "dry + compressor -> DRYING");
    CHECK(hisense_to_climate_action(true, HISENSE_MODE_DRY, 0) == ESPHOME_CLIMATE_ACTION_IDLE,
          "dry, compressor stopped -> IDLE");

    // ---- swing ----
    printf("[swing]\n");
    CHECK(hisense_to_climate_swing(false, false) == ESPHOME_CLIMATE_SWING_OFF, "none -> OFF");
    CHECK(hisense_to_climate_swing(true, false) == ESPHOME_CLIMATE_SWING_VERTICAL, "v -> VERTICAL");
    CHECK(hisense_to_climate_swing(false, true) == ESPHOME_CLIMATE_SWING_HORIZONTAL,
          "h -> HORIZONTAL");
    CHECK(hisense_to_climate_swing(true, true) == ESPHOME_CLIMATE_SWING_BOTH, "both -> BOTH");

    climate_swing_to_hisense(ESPHOME_CLIMATE_SWING_VERTICAL, &vs, &hs);
    CHECK(vs == HISENSE_SWING_SWING && hs == HISENSE_SWING_OFF, "VERTICAL -> vswing only");
    climate_swing_to_hisense(ESPHOME_CLIMATE_SWING_BOTH, &vs, &hs);
    CHECK(vs == HISENSE_SWING_SWING && hs == HISENSE_SWING_SWING, "BOTH -> both swings");
    climate_swing_to_hisense(ESPHOME_CLIMATE_SWING_OFF, &vs, &hs);
    CHECK(vs == HISENSE_SWING_OFF && hs == HISENSE_SWING_OFF, "OFF -> neither");

    for (int s = 0; s <= 3; s++) {
        climate_swing_to_hisense((uint8_t) s, &vs, &hs);
        CHECK(hisense_to_climate_swing(vs == HISENSE_SWING_SWING, hs == HISENSE_SWING_SWING) == s,
              "swing round-trip %d", s);
    }

    // ---- fan ladder: index view of the shared table, not a second copy ----
    printf("[fan]\n");
    CHECK(esphome_fan_index_to_hisense(ESPHOME_FAN_INDEX_AUTO) == HISENSE_FAN_AUTO, "index 0 -> AUTO");
    CHECK(esphome_fan_index_to_hisense(1) == HISENSE_FAN_QUIET, "index 1 -> QUIET");
    CHECK(esphome_fan_index_to_hisense(6) == HISENSE_FAN_HIGH, "index 6 -> HIGH");
    CHECK(esphome_fan_index_to_hisense(99) == HISENSE_FAN_AUTO, "out-of-range index -> AUTO");
    CHECK(hisense_fan_raw_to_esphome_index(HISENSE_FAN_RAW_AUTO) == ESPHOME_FAN_INDEX_AUTO,
          "raw 0x01 -> index 0");
    CHECK(hisense_fan_raw_to_esphome_index(0x0A) == 2, "raw 0x0A (low) -> index 2");
    CHECK(hisense_fan_raw_to_esphome_index(0x12) == 6, "raw 0x12 (high) -> index 6");

    // Every ladder index must survive index -> command -> wire -> status-raw -> index. This is
    // the loop that decides whether the fan control in Home Assistant sticks.
    for (uint8_t i = 1; i <= ESPHOME_FAN_INDEX_MAX; i++) {
        const HisenseFanRow *row = hisense_fan_row_by_speed(i);
        CHECK(row != 0, "fan table has speed %u", i);
        if (row == 0) continue;
        base.fan = esphome_fan_index_to_hisense(i);
        CHECK(cmd_byte(base, 16) == (uint8_t) (row->cmd * 2 + 1),
              "index %u -> command byte16 0x%02X", i, row->cmd * 2 + 1);
        CHECK(hisense_fan_raw_to_esphome_index(row->raw) == i, "fan round-trip index %u", i);
    }

    // ---- fan requests arriving as the BUILT-IN enum ----------------------------------
    // Regression for the bug found on the bench 2026-08-18: ESPHome's
    // ClimateCall::set_fan_mode(const char *) matches the built-in enum names case-insensitively
    // BEFORE the custom list, so "Auto"/"Quiet"/"Low"/"Medium"/"High" never arrive as custom
    // modes. Glue that only handled the custom path dropped five of the seven speeds silently.
    printf("[fan enum]\n");
    CHECK(esphome_fan_enum_to_index(ESPHOME_CLIMATE_FAN_AUTO) == 0, "AUTO -> index 0");
    CHECK(esphome_fan_enum_to_index(ESPHOME_CLIMATE_FAN_QUIET) == 1, "QUIET -> index 1");
    CHECK(esphome_fan_enum_to_index(ESPHOME_CLIMATE_FAN_LOW) == 2, "LOW -> index 2");
    CHECK(esphome_fan_enum_to_index(ESPHOME_CLIMATE_FAN_MIDDLE) == 3, "MIDDLE -> index 3");
    CHECK(esphome_fan_enum_to_index(ESPHOME_CLIMATE_FAN_MEDIUM) == 4, "MEDIUM -> index 4");
    CHECK(esphome_fan_enum_to_index(ESPHOME_CLIMATE_FAN_FOCUS) == 5, "FOCUS -> index 5");
    CHECK(esphome_fan_enum_to_index(ESPHOME_CLIMATE_FAN_HIGH) == 6, "HIGH -> index 6");
    CHECK(esphome_fan_enum_to_index(ESPHOME_CLIMATE_FAN_ON) == 6, "ON -> full speed");
    // The A/C's fan cannot idle, so these degrade to auto rather than inventing a speed.
    CHECK(esphome_fan_enum_to_index(ESPHOME_CLIMATE_FAN_OFF) == 0, "OFF -> auto");
    CHECK(esphome_fan_enum_to_index(ESPHOME_CLIMATE_FAN_DIFFUSE) == 0, "DIFFUSE -> auto");

    // Every built-in fan enum must reach the wire as a real speed, which is the property the
    // dropped-command bug violated: the enum path has to end in the same command byte the
    // custom-name path produces for that step.
    for (uint8_t e : {(uint8_t) ESPHOME_CLIMATE_FAN_QUIET, (uint8_t) ESPHOME_CLIMATE_FAN_LOW,
                      (uint8_t) ESPHOME_CLIMATE_FAN_MIDDLE, (uint8_t) ESPHOME_CLIMATE_FAN_MEDIUM,
                      (uint8_t) ESPHOME_CLIMATE_FAN_FOCUS, (uint8_t) ESPHOME_CLIMATE_FAN_HIGH}) {
        uint8_t idx = esphome_fan_enum_to_index(e);
        const HisenseFanRow *row = hisense_fan_row_by_speed(idx);
        CHECK(row != 0, "enum %u maps to a real ladder step", e);
        if (row == 0) continue;
        base.fan = esphome_fan_index_to_hisense(idx);
        CHECK(cmd_byte(base, 16) == (uint8_t) (row->cmd * 2 + 1),
              "enum %u -> command byte16 0x%02X", e, row->cmd * 2 + 1);
    }

    // ---- setpoint clamp is shared with the Matter path, so just pin the contract ----
    printf("[setpoint]\n");
    CHECK(matter_clamp_setpoint_c(10) == 16, "below range clamps to 16");
    CHECK(matter_clamp_setpoint_c(40) == 32, "above range clamps to 32");
    CHECK(matter_clamp_setpoint_c(22) == 22, "in range unchanged");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
