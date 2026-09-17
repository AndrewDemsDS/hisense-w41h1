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
    // quiet is a preset, not a fan mode: its readback publishes as low, like the Matter wrapper
    CHECK(esphome_fan_published_index(hisense_fan_raw_to_esphome_index(0x02)) == 2,
          "raw 0x02 (quiet step) publishes as low");
    for (uint8_t i = 0; i <= ESPHOME_FAN_INDEX_MAX; i++)
        CHECK(esphome_fan_published_index(i) != 1, "index %u never publishes as quiet", i);
    CHECK(esphome_fan_published_index(3) == 3 && esphome_fan_published_index(5) == 5 &&
              esphome_fan_published_index(0) == 0, "other steps publish unchanged");

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

    // ---- #117: the setpoint travels in the PANEL's unit ----------------------------------
    // ESPHome is always Celsius, but the A/C reads byte 19 in its display unit. With the panel
    // in F, the glue sent Celsius 24 verbatim and the A/C targeted 24 F, the same failure the
    // Matter builds hit on hardware 2026-07-19 (fixed there in 5164c71, never ported here).
    printf("[setpoint unit, #117]\n");
    {
        HisenseCommand c = base;
        CHECK(esphome_setpoint_to_cmd(24, false, &c) == 24 && c.setpoint == 24 && !c.fahrenheit,
              "C panel: 24 C -> wire 24, unit C");
        c = base;
        // Home Assistant in F sends 75 F as 23.9 C, which the glue rounds to 24.
        CHECK(esphome_setpoint_to_cmd(24, true, &c) == 24 && c.setpoint == 75 && c.fahrenheit,
              "F panel: 24 C -> wire 75 F, unit F");
        CHECK(cmd_byte(c, 19) == 75 * 2 + 1, "F panel: byte19 carries 75 F");
        c = base;
        CHECK(esphome_setpoint_to_cmd(40, true, &c) == 32 && c.setpoint == 90,
              "F panel: clamp in C first, then convert (40 C -> 32 C -> 90 F)");
    }

    // ---- #117: shadow sync must validate in the wire unit --------------------------------
    // The hub copied state.setpoint_c straight into the shadow. Once the A/C held 24 F (from
    // the bug above) it reported -4 C, and every later Cool/Heat/Auto frame failed the builder's
    // range check and was dropped. Fan-only strips the setpoint so it still built: that is the
    // reported "stuck on Fan only", and the dropped fan change reverting to Auto.
    printf("[shadow sync, #117]\n");
    {
        uint8_t buf[64];
        HisenseCommand poisoned = base;
        poisoned.setpoint = hisense_f_to_c(24);   // what the old sync copied in
        CHECK(poisoned.setpoint == -4, "24 F panel reports -4 C");
        CHECK(hisense_build_command(&poisoned, buf, sizeof(buf)) == 0,
              "repro: poisoned shadow drops the COOL frame");
        poisoned.mode = HISENSE_MODE_FAN;
        CHECK(hisense_build_command(&poisoned, buf, sizeof(buf)) != 0,
              "repro: FAN_ONLY still builds, so only fan-only is reachable");

        HisenseCommand s = base;
        s.setpoint = 26;
        CHECK(!esphome_sync_shadow_setpoint(hisense_f_to_c(24), true, &s) && s.setpoint == 26 &&
                  !s.fahrenheit,
              "out-of-range report leaves the last good shadow alone");
        CHECK(hisense_build_command(&s, buf, sizeof(buf)) != 0, "COOL still builds after it");

        s = base;
        CHECK(esphome_sync_shadow_setpoint(24, true, &s) && s.setpoint == 75 && s.fahrenheit,
              "F panel 24 C report -> shadow 75 F");
        CHECK(hisense_build_command(&s, buf, sizeof(buf)) != 0, "F shadow builds");
        CHECK(esphome_sync_shadow_setpoint(22, false, &s) && s.setpoint == 22 && !s.fahrenheit,
              "panel back to C -> shadow 22 C, unit C");
    }

    // ---- eco/turbo shadow sync (port of the esp32 Matter sync, app_main.cpp) --------------
    // The hub never synced cmd.feature from status, so a Turbo set from HA kept re-asserting
    // on every later mode/setpoint/fan frame even after the remote turned it off, and a cleared
    // Eco left the one-shot ECO_OFF byte (0x10) riding every frame.
    printf("[feature sync]\n");
    CHECK(hisense_feature_from_status(false, false) == HISENSE_FEATURE_NONE, "neither -> NONE");
    CHECK(hisense_feature_from_status(true, false) == HISENSE_FEATURE_ECO, "eco -> ECO");
    CHECK(hisense_feature_from_status(false, true) == HISENSE_FEATURE_TURBO, "turbo -> TURBO");
    CHECK(hisense_feature_from_status(true, true) == HISENSE_FEATURE_ECO,
          "both reported -> ECO (same precedence as the Matter sync)");
    {
        HisenseCommand c = base;
        c.feature = HISENSE_FEATURE_TURBO;   // set from HA earlier
        c.feature = hisense_feature_from_status(false, false);   // remote turned it off
        CHECK(cmd_byte(c, 33) == 0x04, "synced shadow no longer re-asserts turbo (byte33 0x04)");
        CHECK(esphome_feature_after_send(HISENSE_FEATURE_ECO_OFF) == HISENSE_FEATURE_NONE,
              "ECO_OFF is one-shot: shadow returns to NONE after the send");
        CHECK(esphome_feature_after_send(HISENSE_FEATURE_ECO) == HISENSE_FEATURE_ECO, "ECO kept");
        CHECK(esphome_feature_after_send(HISENSE_FEATURE_TURBO) == HISENSE_FEATURE_TURBO,
              "TURBO kept");
    }

    // ---- special modes as climate presets ----------------------------------------------------
    // Names must match hisense-unified-ac verbatim so a climate group syncs presets across the
    // ESPHome and Matter paths; the ordering rules are the wrapper's measured interlocks.
    printf("[presets]\n");
    {
        static const char *const wrapper_names[] = {
            "none", "eco", "quiet", "turbo", "eco_quiet",
            "sleep_general", "sleep_old", "sleep_young", "sleep_kids",
            "eco_sleep_general", "eco_sleep_old", "eco_sleep_young", "eco_sleep_kids"};
        CHECK(ESPHOME_PRESET_COUNT == 13, "13 presets");
        for (unsigned i = 0; i < 13; i++)
            CHECK(esphome_preset_index(wrapper_names[i]) == (int) i, "name %s", wrapper_names[i]);
        CHECK(esphome_preset_index("sleep") == -1 && esphome_preset_index("Eco") == -1,
              "no built-in-colliding or case-folded names");
        // Custom rows must not collide with ESPHome's built-in preset names, or set_preset()
        // converts them to the enum and the custom list never sees them.
        static const char *const builtins[] = {"none", "home", "away", "boost", "comfort", "eco",
                                               "sleep", "activity"};
        for (unsigned i = ESPHOME_PRESET_FIRST_CUSTOM; i < ESPHOME_PRESET_COUNT; i++)
            for (unsigned b = 0; b < 8; b++)
                CHECK(strcasecmp(k_esphome_presets[i].name, builtins[b]) != 0,
                      "custom %s is not built-in %s", k_esphome_presets[i].name, builtins[b]);
        CHECK(strcmp(k_esphome_presets[ESPHOME_PRESET_NONE].name, "none") == 0 &&
                  strcmp(k_esphome_presets[ESPHOME_PRESET_ECO].name, "eco") == 0,
              "the two built-in rows come first");

        // no row pairs quiet with sleep, or turbo with anything (measured interlocks)
        for (unsigned i = 0; i < ESPHOME_PRESET_COUNT; i++) {
            const EsphomePresetRow &r = k_esphome_presets[i];
            CHECK(!(r.mute && r.sleep), "%s: quiet never with sleep", r.name);
            CHECK(!(r.turbo && (r.eco || r.mute || r.sleep)), "%s: turbo alone", r.name);
        }

        const uint8_t all = ESPHOME_SUPPORT_ECO | ESPHOME_SUPPORT_QUIET | ESPHOME_SUPPORT_TURBO |
                            ESPHOME_SUPPORT_SLEEP;
        CHECK(esphome_preset_available(0, 0), "none always available");
        CHECK(!esphome_preset_available(9, ESPHOME_SUPPORT_SLEEP), "eco_sleep needs eco");
        CHECK(!esphome_preset_available(4, ESPHOME_SUPPORT_ECO), "eco_quiet needs quiet");
        CHECK(esphome_preset_available(4, ESPHOME_SUPPORT_ECO | ESPHOME_SUPPORT_QUIET), "eco_quiet");
        CHECK(!esphome_preset_available(13, all), "out of range");

        // detection
        HisenseSpecialState s = hisense_special_from_status(false, false, false, 0);
        CHECK(esphome_preset_detect(&s, all) == 0, "nothing on -> none");
        s = hisense_special_from_status(true, false, false, 0x04);   // eco + Old (byte17 = 2*2)
        CHECK(s.sleep == 2 && esphome_preset_detect(&s, all) == 10, "eco + sleep_raw 4 -> eco_sleep_old");
        s = hisense_special_from_status(false, false, true, 0x02);   // illegal pair mid-arbitration
        CHECK(esphome_preset_detect(&s, all) == 2, "quiet + sleep in flight -> quiet");
        s = hisense_special_from_status(true, true, false, 0);
        CHECK(esphome_preset_detect(&s, all) == 3, "eco + turbo in flight -> turbo");
        s = hisense_special_from_status(false, false, false, 0x08);
        CHECK(esphome_preset_detect(&s, all) == 8, "sleep_raw 8 -> sleep_kids");
        CHECK(esphome_preset_detect(&s, ESPHOME_SUPPORT_ECO) == 0, "unoffered result -> none");

        // plans
        HisenseSpecialOp ops[ESPHOME_PRESET_PLAN_MAX];
        HisenseSpecialState none = hisense_special_from_status(false, false, false, 0);
        uint8_t n = esphome_preset_plan(&none, 0, ops);
        CHECK(n == 0, "none -> none sends nothing");

        n = esphome_preset_plan(&none, 1, ops);
        CHECK(n == 1 && ops[0].kind == HISENSE_SPECIAL_OP_FEATURE &&
                  ops[0].value == HISENSE_FEATURE_ECO, "none -> eco: one ECO write");

        HisenseSpecialState eco = hisense_special_from_status(true, false, false, 0);
        n = esphome_preset_plan(&eco, 0, ops);
        CHECK(n == 1 && ops[0].value == HISENSE_FEATURE_ECO_OFF,
              "eco -> none clears with ECO_OFF (NONE only clears turbo)");
        n = esphome_preset_plan(&eco, 3, ops);
        CHECK(n == 1 && ops[0].value == HISENSE_FEATURE_TURBO, "eco -> turbo is one byte33 write");

        HisenseSpecialState turbo = hisense_special_from_status(false, true, false, 0);
        n = esphome_preset_plan(&turbo, 0, ops);
        CHECK(n == 1 && ops[0].value == HISENSE_FEATURE_NONE, "turbo -> none clears with NONE");

        n = esphome_preset_plan(&none, 4, ops);   // eco_quiet
        CHECK(n == 2 && ops[0].kind == HISENSE_SPECIAL_OP_FEATURE &&
                  ops[1].kind == HISENSE_SPECIAL_OP_MUTE && ops[1].value == 1,
              "none -> eco_quiet: eco then mute");

        n = esphome_preset_plan(&none, 10, ops);  // eco_sleep_old
        CHECK(n == 2 && ops[0].kind == HISENSE_SPECIAL_OP_FEATURE &&
                  ops[1].kind == HISENSE_SPECIAL_OP_SLEEP && ops[1].value == 2,
              "eco before sleep (sleep then eco loses the profile)");

        HisenseSpecialState sleep_old = hisense_special_from_status(false, false, false, 0x04);
        n = esphome_preset_plan(&sleep_old, 10, ops);
        CHECK(n == 2 && ops[0].value == HISENSE_FEATURE_ECO &&
                  ops[1].kind == HISENSE_SPECIAL_OP_SLEEP && ops[1].value == 2,
              "sleep_old -> eco_sleep_old re-sends the unchanged profile after eco");

        n = esphome_preset_plan(&sleep_old, 2, ops);   // quiet
        CHECK(n == 2 && ops[0].kind == HISENSE_SPECIAL_OP_SLEEP && ops[0].value == 0 &&
                  ops[1].kind == HISENSE_SPECIAL_OP_MUTE && ops[1].value == 1,
              "sleep -> quiet clears sleep FIRST, then mute");

        HisenseSpecialState quiet = hisense_special_from_status(false, false, true, 0);
        n = esphome_preset_plan(&quiet, 7, ops);   // sleep_young
        CHECK(n == 2 && ops[0].kind == HISENSE_SPECIAL_OP_MUTE && ops[0].value == 0 &&
                  ops[1].kind == HISENSE_SPECIAL_OP_SLEEP && ops[1].value == 3,
              "quiet -> sleep_young: mute off, then sleep last");

        HisenseSpecialState eco_sleep = hisense_special_from_status(true, false, false, 0x02);
        n = esphome_preset_plan(&eco_sleep, 5, ops);   // sleep_general
        CHECK(n == 2 && ops[0].value == HISENSE_FEATURE_ECO_OFF &&
                  ops[1].kind == HISENSE_SPECIAL_OP_SLEEP && ops[1].value == 1,
              "eco_sleep_general -> sleep_general: eco off, profile re-sent");

        HisenseSpecialState eq = hisense_special_from_status(true, false, true, 0);
        n = esphome_preset_plan(&eq, 3, ops);   // turbo
        CHECK(n == 2 && ops[0].kind == HISENSE_SPECIAL_OP_MUTE && ops[0].value == 0 &&
                  ops[1].value == HISENSE_FEATURE_TURBO, "eco_quiet -> turbo: mute off, then turbo");

        CHECK(esphome_preset_plan(&none, 13, ops) == 0, "bad target sends nothing");

        // every plan, applied, lands on its target row
        HisenseSpecialState starts[] = {none, eco, turbo, sleep_old, quiet, eco_sleep, eq};
        for (unsigned a = 0; a < sizeof(starts) / sizeof(starts[0]); a++) {
            for (uint8_t t = 0; t < ESPHOME_PRESET_COUNT; t++) {
                HisenseSpecialState cur = starts[a];
                n = esphome_preset_plan(&cur, t, ops);
                CHECK(n <= ESPHOME_PRESET_PLAN_MAX, "plan fits");
                for (uint8_t i = 0; i < n; i++) hisense_special_apply(&cur, &ops[i]);
                CHECK(esphome_preset_detect(&cur, all) == t, "start %u -> %s lands", a,
                      k_esphome_presets[t].name);
            }
        }

        // fan pinning
        CHECK(esphome_fan_request_allowed(&none, 4), "free fan: any speed");
        CHECK(!esphome_fan_request_allowed(&turbo, 4) && esphome_fan_request_allowed(&turbo, 6),
              "turbo pins high");
        CHECK(esphome_fan_request_allowed(&quiet, 1) && esphome_fan_request_allowed(&quiet, 2) &&
                  !esphome_fan_request_allowed(&quiet, 0),
              "quiet pins the low profile");
        CHECK(!esphome_fan_request_allowed(&sleep_old, 6), "sleep pins low");
        CHECK(esphome_fan_request_allowed(&eco, 5), "eco does not pin the fan");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
