// What the entities ask and show (hisense_entities.h): the climate entity's request as a plan, a
// status frame as what the climate entity, the switches and the sleep select show, and the fan
// ladder's names. The entity classes need ESPHome and cannot be built here. They translate a call
// into the plain structs these functions take and carry the result out, so every decision an
// entity makes is checked in this file.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "hisense_entities.h"
#include "hisense_map.h"

namespace H = esphome::hisense_ac;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond, ...) \
  do { \
    g_checks++; \
    if (!(cond)) { \
      g_fail++; \
      printf("  FAIL %s:%d ", __FILE__, __LINE__); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } \
  } while (0)

static const uint8_t ALL = H::SUPPORT_ECO | H::SUPPORT_QUIET | H::SUPPORT_TURBO | H::SUPPORT_SLEEP;

// A unit running in cool at 24 C, auto fan, with a status frame in.
static H::ClimateContext running() {
  H::ClimateContext c;
  c.have_status = true;
  c.power_expected = true;
  c.preset_support = ALL;
  return c;
}

static H::AcState status(bool on, H::Mode mode, int setpoint, uint8_t fan_raw) {
  H::AcState st;
  st.valid = true;
  st.power_on = on;
  st.mode = mode;
  st.setpoint_c = (int8_t) setpoint;
  st.indoor_temp_c = 23;
  st.fan_raw = fan_raw;
  return st;
}

// ---- The fan ladder's names ---------------------------------------------------------------------
static void test_fan_names() {
  printf("-- fan ladder: which steps are custom, and what the others are published as\n");
  for (uint8_t idx = 0; idx <= H::FAN_INDEX_MAX; idx++) {
    const bool custom = idx == 3 || idx == 5;
    CHECK(H::fan_index_is_custom(idx) == custom, "index %u custom=%d", idx, custom);
    if (custom) {
      CHECK(H::fan_custom_index(H::fan_custom_name(idx)) == idx, "index %u: name and back", idx);
    }
  }
  CHECK(std::strcmp(H::fan_custom_name(3), "medium_low") == 0 && std::strcmp(H::fan_custom_name(5), "medium_high") == 0,
        "the two names Home Assistant shows");
  CHECK(H::fan_custom_name(3) == H::FAN_CUSTOM_NAMES[0] && H::fan_custom_name(5) == H::FAN_CUSTOM_NAMES[1],
        "and they are the strings the entity registers (ESPHome keeps the pointer)");
  CHECK(H::fan_custom_index(nullptr) == -1 && H::fan_custom_index("high") == -1 && H::fan_custom_index("") == -1,
        "anything else is not a custom step");
  CHECK(H::fan_index_to_climate_fan(0) == H::CLIMATE_FAN_AUTO_VALUE, "auto");
  CHECK(H::fan_index_to_climate_fan(1) == H::CLIMATE_FAN_LOW_VALUE, "the quiet step shows as low");
  CHECK(H::fan_index_to_climate_fan(2) == H::CLIMATE_FAN_LOW_VALUE, "low");
  CHECK(H::fan_index_to_climate_fan(4) == H::CLIMATE_FAN_MEDIUM_VALUE, "medium");
  CHECK(H::fan_index_to_climate_fan(6) == H::CLIMATE_FAN_HIGH_VALUE, "high");
  // Every step that is not custom comes back from its enum to the same step (quiet as low).
  for (uint8_t idx = 0; idx <= H::FAN_INDEX_MAX; idx++) {
    if (!H::fan_index_is_custom(idx)) {
      CHECK(H::climate_fan_to_index(H::fan_index_to_climate_fan(idx)) == H::fan_published_index(idx),
            "index %u: enum and back", idx);
    }
  }
}

// ---- The climate entity's request ---------------------------------------------------------------
static void test_plan_power() {
  printf("-- climate request: off, and nothing before the first status\n");
  H::ClimateRequest off;
  off.has_mode = true;
  off.mode = H::CLIMATE_MODE_OFF_VALUE;
  off.has_target = true;  // dropped: a combined frame does nothing on a unit that is off
  off.target_c = 28;
  off.has_preset = true;
  off.preset = H::CLIMATE_PRESET_ECO_VALUE;
  H::ClimatePlan p = H::climate_plan(off, running());
  CHECK(p.power_off && !p.send_combined && p.preset == -1 && !p.mute_on && p.refused == 0,
        "off is the power frame alone, whatever else the call carried");
  CHECK(p.show_mode && p.mode == H::CLIMATE_MODE_OFF_VALUE && !p.show_target, "and the entity shows off");

  H::ClimateContext boot;  // no status frame yet
  p = H::climate_plan(off, boot);
  CHECK(p.power_off && p.refused == 0, "off goes out before the first status: it needs no shadow");
  H::ClimateRequest cool;
  cool.has_mode = true;
  cool.mode = H::CLIMATE_MODE_COOL_VALUE;
  p = H::climate_plan(cool, boot);
  CHECK(p.refused == H::CLIMATE_REFUSED_NO_STATUS && !p.send_combined && !p.power_off && !p.show_mode,
        "a mode does not: the frame would carry the shadow's defaults");
  H::ClimateRequest fan;
  fan.has_fan = true;
  fan.fan = H::CLIMATE_FAN_HIGH_VALUE;
  p = H::climate_plan(fan, boot);
  CHECK(p.refused == H::CLIMATE_REFUSED_NO_STATUS && !p.send_combined && p.fan_index == -1, "nor does a fan speed");
  H::ClimateRequest nothing;
  p = H::climate_plan(nothing, running());
  CHECK(!p.power_off && !p.send_combined && p.preset == -1 && !p.mute_on && p.refused == 0 && p.fields == 0,
        "an empty call is an empty plan");
}

static void test_plan_mode() {
  printf("-- climate request: mode, with power on in the same frame when the unit is off\n");
  static const struct {
    uint8_t climate;
    H::Mode mode;
  } MODES[] = {{H::CLIMATE_MODE_COOL_VALUE, H::MODE_COOL},
               {H::CLIMATE_MODE_HEAT_VALUE, H::MODE_HEAT},
               {H::CLIMATE_MODE_DRY_VALUE, H::MODE_DRY},
               {H::CLIMATE_MODE_FAN_ONLY_VALUE, H::MODE_FAN},
               {H::CLIMATE_MODE_AUTO_VALUE, H::MODE_AUTO}};
  for (const auto &m : MODES) {
    for (int on = 0; on < 2; on++) {
      H::ClimateContext c = running();
      c.power_expected = on != 0;
      c.shadow.setpoint = 21;
      c.shadow.fan = H::FAN_SPEED_MID;
      H::ClimateRequest rq;
      rq.has_mode = true;
      rq.mode = m.climate;
      const H::ClimatePlan p = H::climate_plan(rq, c);
      CHECK(p.send_combined && p.cmd.mode == m.mode && p.fields == H::CONFIRM_MODE && p.refused == 0,
            "mode %u: one combined frame", m.climate);
      CHECK(p.power_on == (on == 0), "mode %u on a unit that is %s: power_on=%d", m.climate, on ? "on" : "off",
            p.power_on);
      CHECK(p.show_mode && p.mode == m.climate && !p.show_target && p.fan_index == -1 && !p.show_swing,
            "mode %u: the entity shows the mode and nothing else", m.climate);
      CHECK(p.cmd.setpoint == 21 && p.cmd.fan == H::FAN_SPEED_MID, "the rest of the shadow rides along unchanged");
    }
  }
  H::ClimateRequest heat_cool;
  heat_cool.has_mode = true;
  heat_cool.mode = H::CLIMATE_MODE_HEAT_COOL_VALUE;
  const H::ClimatePlan p = H::climate_plan(heat_cool, running());
  CHECK(p.refused == H::CLIMATE_REFUSED_MODE && !p.send_combined && !p.show_mode && !p.power_on,
        "heat_cool is not a mode of this unit: refused, nothing sent");
}

static void test_plan_setpoint() {
  printf("-- climate request: setpoint, only where the unit takes one\n");
  H::ClimateRequest rq;
  rq.has_target = true;
  rq.target_c = 25.6f;
  H::ClimateContext c = running();
  H::ClimatePlan p = H::climate_plan(rq, c);
  CHECK(p.send_combined && p.fields == H::CONFIRM_SETPOINT && p.cmd.setpoint == 26 && !p.cmd.fahrenheit,
        "25.6 C in cool: rounded to 26");
  CHECK(p.show_target && p.target_c == 26 && !p.power_on && !p.show_mode, "and shown as 26");
  rq.target_c = 25.4f;
  CHECK(H::climate_plan(rq, c).cmd.setpoint == 25, "25.4 rounds down");
  rq.target_c = 40;
  p = H::climate_plan(rq, c);
  CHECK(p.cmd.setpoint == 32 && p.target_c == 32, "above the range: clamped to 32, and 32 is shown");
  rq.target_c = 3;
  p = H::climate_plan(rq, c);
  CHECK(p.cmd.setpoint == 16 && p.target_c == 16, "below: 16");

  c.panel_f = true;
  rq.target_c = 25;
  p = H::climate_plan(rq, c);
  CHECK(p.cmd.fahrenheit && p.cmd.setpoint == 77 && p.target_c == 25, "F panel: the frame carries 77 F, shown as 25 C");

  // The unit keeps its own setpoint outside cool and heat.
  for (int mode = 0; mode <= H::MODE_AUTO; mode++) {
    H::ClimateContext cm = running();
    cm.shadow.mode = (H::Mode) mode;
    rq.target_c = 20;
    p = H::climate_plan(rq, cm);
    const bool takes = mode == H::MODE_COOL || mode == H::MODE_HEAT;
    CHECK(p.send_combined == takes && p.show_target == takes &&
              ((p.refused & H::CLIMATE_REFUSED_SETPOINT) != 0) == !takes,
          "shadow mode %d: setpoint %s", mode, takes ? "sent" : "refused");
    if (!takes) {
      CHECK(p.cmd.setpoint == cm.shadow.setpoint, "a refused setpoint does not enter the shadow");
    }
  }
  // The mode of the same call decides, not the shadow's.
  H::ClimateContext dry = running();
  dry.shadow.mode = H::MODE_DRY;
  H::ClimateRequest both;
  both.has_mode = true;
  both.mode = H::CLIMATE_MODE_HEAT_VALUE;
  both.has_target = true;
  both.target_c = 22;
  p = H::climate_plan(both, dry);
  CHECK(p.send_combined && p.fields == (H::CONFIRM_MODE | H::CONFIRM_SETPOINT) && p.cmd.mode == H::MODE_HEAT &&
            p.cmd.setpoint == 22,
        "dry to heat with a setpoint: both in one frame");
  both.mode = H::CLIMATE_MODE_DRY_VALUE;
  p = H::climate_plan(both, running());
  CHECK(p.send_combined && p.fields == H::CONFIRM_MODE && (p.refused & H::CLIMATE_REFUSED_SETPOINT) != 0,
        "cool to dry with a setpoint: the mode goes, the setpoint is refused");
  // A unit that is off and stays off: the value still goes into the frame, for the mode change
  // that may follow.
  H::ClimateContext off = running();
  off.power_expected = false;
  off.shadow.mode = H::MODE_AUTO;
  rq.target_c = 27;
  p = H::climate_plan(rq, off);
  CHECK(p.send_combined && p.cmd.setpoint == 27 && !p.power_on, "off, in auto: setpoint taken into the shadow");
  both.mode = H::CLIMATE_MODE_AUTO_VALUE;
  p = H::climate_plan(both, off);
  CHECK(p.power_on && (p.refused & H::CLIMATE_REFUSED_SETPOINT) != 0 && p.fields == H::CONFIRM_MODE,
        "off to auto with a setpoint: switching on makes it a running unit, so the setpoint is refused");
}

static void test_plan_fan() {
  printf("-- climate request: fan, by either route, and what a special mode holds\n");
  static const struct {
    uint8_t fan;
    H::FanSpeed speed;
    int index;
  } ENUMS[] = {
      {H::CLIMATE_FAN_AUTO_VALUE, H::FAN_SPEED_AUTO, 0},      {H::CLIMATE_FAN_LOW_VALUE, H::FAN_SPEED_LOW, 2},
      {H::CLIMATE_FAN_MIDDLE_VALUE, H::FAN_SPEED_MED_LOW, 3}, {H::CLIMATE_FAN_MEDIUM_VALUE, H::FAN_SPEED_MID, 4},
      {H::CLIMATE_FAN_FOCUS_VALUE, H::FAN_SPEED_MED_HIGH, 5}, {H::CLIMATE_FAN_HIGH_VALUE, H::FAN_SPEED_HIGH, 6},
      {H::CLIMATE_FAN_ON_VALUE, H::FAN_SPEED_HIGH, 6},        {H::CLIMATE_FAN_OFF_VALUE, H::FAN_SPEED_AUTO, 0}};
  for (const auto &e : ENUMS) {
    H::ClimateRequest rq;
    rq.has_fan = true;
    rq.fan = e.fan;
    const H::ClimatePlan p = H::climate_plan(rq, running());
    CHECK(p.send_combined && p.fields == H::CONFIRM_FAN && p.cmd.fan == e.speed && p.fan_index == e.index &&
              !p.mute_on && p.refused == 0,
          "fan enum %u: speed 0x%02X, shown as index %d", e.fan, (unsigned) e.speed, e.index);
  }
  H::ClimateRequest custom;
  custom.custom_fan = "medium_low";
  custom.has_fan = true;  // ESPHome never sets both; the custom name wins if it did
  custom.fan = H::CLIMATE_FAN_HIGH_VALUE;
  H::ClimatePlan p = H::climate_plan(custom, running());
  CHECK(p.send_combined && p.cmd.fan == H::FAN_SPEED_MED_LOW && p.fan_index == 3, "custom medium_low");
  custom.custom_fan = "medium_high";
  p = H::climate_plan(custom, running());
  CHECK(p.send_combined && p.cmd.fan == H::FAN_SPEED_MED_HIGH && p.fan_index == 5, "custom medium_high");
  custom.custom_fan = "gale";
  p = H::climate_plan(custom, running());
  CHECK(!p.send_combined && p.fan_index == -1 && p.refused == 0, "a custom name that is no step: nothing");

  // Quiet as a fan speed is the mute flag, never the fan byte (its command value puts the unit on high).
  H::ClimateRequest quiet;
  quiet.has_fan = true;
  quiet.fan = H::CLIMATE_FAN_QUIET_VALUE;
  H::ClimateContext c = running();
  c.shadow.fan = H::FAN_SPEED_MID;
  p = H::climate_plan(quiet, c);
  CHECK(p.mute_on && !p.send_combined && p.fields == 0 && p.cmd.fan == H::FAN_SPEED_MID, "quiet: mute on, no fan byte");
  CHECK(p.fan_index == H::FAN_INDEX_LOW, "and the fan shows as low");

  // A special mode that holds the fan: another speed is refused, the held one is accepted.
  H::ClimateRequest high;
  high.has_fan = true;
  high.fan = H::CLIMATE_FAN_HIGH_VALUE;
  H::ClimateRequest low;
  low.has_fan = true;
  low.fan = H::CLIMATE_FAN_LOW_VALUE;
  H::ClimateContext turbo = running();
  turbo.projected.turbo = true;
  p = H::climate_plan(low, turbo);
  CHECK(p.refused == H::CLIMATE_REFUSED_FAN && !p.send_combined && p.fan_index == -1, "turbo: low is refused");
  CHECK(H::climate_plan(high, turbo).send_combined, "turbo: high is what it holds");
  H::ClimateContext muted = running();
  muted.projected.mute = true;
  CHECK(H::climate_plan(high, muted).refused == H::CLIMATE_REFUSED_FAN, "quiet: high is refused");
  CHECK(H::climate_plan(low, muted).send_combined, "quiet: low is accepted");
  H::ClimateContext asleep = running();
  asleep.projected.sleep = 3;
  CHECK(H::climate_plan(high, asleep).refused == H::CLIMATE_REFUSED_FAN, "sleep: high is refused");
  // A refused fan does not take the rest of the call with it.
  H::ClimateRequest fan_and_swing = high;
  fan_and_swing.has_swing = true;
  fan_and_swing.swing = H::CLIMATE_SWING_VERTICAL_VALUE;
  p = H::climate_plan(fan_and_swing, muted);
  CHECK(p.refused == H::CLIMATE_REFUSED_FAN && p.send_combined && p.fields == H::CONFIRM_SWING &&
            p.cmd.vswing == H::SWING_MODE_SWING && p.cmd.fan == muted.shadow.fan,
        "fan refused, swing in the same call still sent");
}

static void test_plan_swing_and_preset() {
  printf("-- climate request: swing, presets by either route\n");
  static const struct {
    uint8_t swing;
    bool v, h;
  } SWINGS[] = {{H::CLIMATE_SWING_OFF_VALUE, false, false},
                {H::CLIMATE_SWING_VERTICAL_VALUE, true, false},
                {H::CLIMATE_SWING_HORIZONTAL_VALUE, false, true},
                {H::CLIMATE_SWING_BOTH_VALUE, true, true}};
  for (const auto &s : SWINGS) {
    H::ClimateRequest rq;
    rq.has_swing = true;
    rq.swing = s.swing;
    H::ClimateContext c = running();
    c.shadow.vswing = s.v ? H::SWING_MODE_OFF : H::SWING_MODE_SWING;  // the opposite of what is asked
    c.shadow.hswing = s.h ? H::SWING_MODE_OFF : H::SWING_MODE_SWING;
    const H::ClimatePlan p = H::climate_plan(rq, c);
    CHECK(p.send_combined && p.fields == H::CONFIRM_SWING && p.show_swing && p.swing == s.swing, "swing %u sent",
          s.swing);
    CHECK((p.cmd.vswing == H::SWING_MODE_SWING) == s.v && (p.cmd.hswing == H::SWING_MODE_SWING) == s.h,
          "swing %u: vertical=%d horizontal=%d", s.swing, s.v, s.h);
  }

  H::ClimateRequest rq;
  rq.has_preset = true;
  rq.preset = H::CLIMATE_PRESET_NONE_VALUE;
  H::ClimatePlan p = H::climate_plan(rq, running());
  CHECK(p.preset == H::PRESET_NONE && !p.send_combined && p.refused == 0, "none arrives as the enum");
  rq.preset = H::CLIMATE_PRESET_ECO_VALUE;
  CHECK(H::climate_plan(rq, running()).preset == H::PRESET_ECO, "eco arrives as the enum");
  rq.preset = 3;  // CLIMATE_PRESET_BOOST: never advertised
  p = H::climate_plan(rq, running());
  CHECK(p.preset == -1 && p.refused == H::CLIMATE_REFUSED_PRESET, "another built-in preset is refused");
  for (size_t i = H::PRESET_FIRST_CUSTOM; i < H::PRESET_COUNT; i++) {
    H::ClimateRequest named;
    named.custom_preset = H::PRESETS[i].name;
    CHECK(H::climate_plan(named, running()).preset == (int) i, "custom preset %s", H::PRESETS[i].name);
  }
  H::ClimateRequest named;
  named.custom_preset = "eco";  // ESPHome turns this name into the enum, so as a custom name it is nothing
  p = H::climate_plan(named, running());
  CHECK(p.preset == -1 && p.refused == H::CLIMATE_REFUSED_PRESET, "a built-in name as a custom preset is refused");
  named.custom_preset = "party";
  CHECK(H::climate_plan(named, running()).refused == H::CLIMATE_REFUSED_PRESET, "an unknown name is refused");

  // A preset this unit was configured without.
  H::ClimateContext no_turbo = running();
  no_turbo.preset_support = ALL & ~H::SUPPORT_TURBO;
  named.custom_preset = "turbo";
  p = H::climate_plan(named, no_turbo);
  CHECK(p.preset == -1 && p.refused == H::CLIMATE_REFUSED_PRESET, "turbo on a unit without it: refused");
  H::ClimateContext no_eco = running();
  no_eco.preset_support = H::SUPPORT_QUIET;
  rq.preset = H::CLIMATE_PRESET_ECO_VALUE;
  CHECK(H::climate_plan(rq, no_eco).refused == H::CLIMATE_REFUSED_PRESET, "eco on a unit without it: refused");
  // No special mode at all: the entity has no preset control, and nothing is logged for a stray one.
  H::ClimateContext none = running();
  none.preset_support = 0;
  p = H::climate_plan(rq, none);
  CHECK(p.preset == -1 && p.refused == 0, "no preset support: ignored without a refusal");

  // A preset and a mode in one call: both are planned.
  H::ClimateRequest both;
  both.has_mode = true;
  both.mode = H::CLIMATE_MODE_HEAT_VALUE;
  both.custom_preset = "quiet";
  p = H::climate_plan(both, running());
  CHECK(p.send_combined && p.preset == H::PRESET_QUIET && p.fields == H::CONFIRM_MODE, "mode and preset together");
}

// ---- A status frame as the climate entity shows it ----------------------------------------------
static void test_view() {
  printf("-- status frame: what the climate entity shows, in and out of the hold-off\n");
  H::AcState st = status(true, H::MODE_HEAT, 22, 0x0E);
  st.compressor_freq = 40;
  st.vswing_on = true;
  st.eco_on = true;
  H::ClimateView v = H::climate_view_from_status(st, false, false, ALL);
  CHECK(v.current_c == 23 && v.action == H::CLIMATE_ACTION_HEATING_VALUE, "telemetry: 23 C, heating");
  CHECK(v.controls && v.mode_known && v.mode == H::CLIMATE_MODE_HEAT_VALUE && v.target_c == 22, "heat at 22");
  CHECK(v.swing == H::CLIMATE_SWING_VERTICAL_VALUE && v.fan_index == 4, "vertical swing, medium fan");
  CHECK(v.preset_known && v.preset == H::PRESET_ECO, "preset eco");

  // During the hold-off the frame may predate the user's command: telemetry only.
  v = H::climate_view_from_status(st, true, false, ALL);
  CHECK(v.current_c == 23 && v.action == H::CLIMATE_ACTION_HEATING_VALUE, "hold-off: telemetry still taken");
  CHECK(!v.controls && !v.mode_known && !v.preset_known, "hold-off: nothing the user can move");
  // While a special-mode write is on its way the preset shown is the one asked for.
  v = H::climate_view_from_status(st, false, true, ALL);
  CHECK(v.controls && !v.preset_known, "special busy: controls follow, the preset does not");
  v = H::climate_view_from_status(st, false, false, 0);
  CHECK(v.controls && !v.preset_known, "no preset support: no preset");
  H::ClimateView limited = H::climate_view_from_status(st, false, false, H::SUPPORT_QUIET);
  CHECK(limited.preset_known && limited.preset == H::PRESET_NONE, "eco on a unit configured without it shows none");

  st.power_on = false;
  v = H::climate_view_from_status(st, false, false, ALL);
  CHECK(v.mode_known && v.mode == H::CLIMATE_MODE_OFF_VALUE && v.action == H::CLIMATE_ACTION_OFF_VALUE,
        "powered down: off, whatever the mode byte says");
  CHECK(v.target_c == 22, "the setpoint is still shown");

  // A mode value with no meaning keeps the mode shown, where it used to read as cool.
  st.power_on = true;
  st.mode = (H::Mode) 7;
  v = H::climate_view_from_status(st, false, false, ALL);
  CHECK(v.controls && !v.mode_known, "mode 7: the entity keeps its mode");
  st.power_on = false;
  v = H::climate_view_from_status(st, false, false, ALL);
  CHECK(v.mode_known && v.mode == H::CLIMATE_MODE_OFF_VALUE, "mode 7 on a unit that is off: off");

  // The quiet step is the mute flag showing through and is published as low.
  st = status(true, H::MODE_COOL, 24, 0x02);
  st.mute_on = true;
  v = H::climate_view_from_status(st, false, false, ALL);
  CHECK(v.fan_index == H::FAN_INDEX_LOW && v.preset == H::PRESET_QUIET, "fan_raw 0x02: low, preset quiet");
  st.fan_raw = 0x55;  // no ladder step
  CHECK(H::climate_view_from_status(st, false, false, ALL).fan_index == H::FAN_INDEX_AUTO, "an unknown fan byte: auto");
  st.fan_raw = H::FAN_RAW_AUTO;
  st.hswing_on = true;
  st.vswing_on = true;
  v = H::climate_view_from_status(st, false, false, ALL);
  CHECK(v.fan_index == H::FAN_INDEX_AUTO && v.swing == H::CLIMATE_SWING_BOTH_VALUE, "auto fan, both swings");
}

static void test_shown_changed() {
  printf("-- has anything the climate entity shows moved\n");
  H::ClimateShown a;
  a.current_c = 23;
  a.target_c = 24;
  a.action = 2;
  a.mode = 2;
  a.fan_index = 4;
  a.preset_index = 1;
  H::ClimateShown b = a;
  CHECK(!H::climate_shown_changed(a, b), "identical: no publish");
  b.current_c = 24;
  CHECK(H::climate_shown_changed(a, b), "current temperature");
  b = a;
  b.target_c = 25;
  CHECK(H::climate_shown_changed(a, b), "target");
  b = a;
  b.action = 4;
  CHECK(H::climate_shown_changed(a, b), "action");
  b = a;
  b.mode = 3;
  CHECK(H::climate_shown_changed(a, b), "mode");
  b = a;
  b.swing = 1;
  CHECK(H::climate_shown_changed(a, b), "swing");
  b = a;
  b.fan_index = 5;
  CHECK(H::climate_shown_changed(a, b), "fan");
  b = a;
  b.preset_index = 2;
  CHECK(H::climate_shown_changed(a, b), "preset");
  // Before the first frame both temperatures are NaN, and a number arriving is a change.
  H::ClimateShown boot = a;
  boot.current_c = NAN;
  CHECK(H::climate_shown_changed(boot, a) && H::climate_shown_changed(a, boot), "NaN to a number, either way");
  boot = a;
  boot.target_c = NAN;
  CHECK(H::climate_shown_changed(boot, a), "the same for the target");
}

// ---- Switches and the sleep select --------------------------------------------------------------
static void test_switches() {
  printf("-- switches: the write behind each, and what a status frame shows\n");
  H::SpecialOp op{};
  CHECK(
      H::switch_special_op(H::SWITCH_ECO, true, &op) && op.kind == H::SPECIAL_OP_FEATURE && op.value == H::FEATURE_ECO,
      "eco on");
  CHECK(H::switch_special_op(H::SWITCH_ECO, false, &op) && op.kind == H::SPECIAL_OP_FEATURE &&
            op.value == H::FEATURE_ECO_OFF,
        "eco off is the explicit clear, not the neutral value (that one clears turbo)");
  CHECK(H::switch_special_op(H::SWITCH_TURBO, true, &op) && op.kind == H::SPECIAL_OP_FEATURE &&
            op.value == H::FEATURE_TURBO,
        "turbo on");
  CHECK(H::switch_special_op(H::SWITCH_TURBO, false, &op) && op.kind == H::SPECIAL_OP_FEATURE &&
            op.value == H::FEATURE_NONE,
        "turbo off is the neutral value");
  CHECK(H::switch_special_op(H::SWITCH_QUIET, true, &op) && op.kind == H::SPECIAL_OP_MUTE && op.value == 1, "quiet on");
  CHECK(H::switch_special_op(H::SWITCH_QUIET, false, &op) && op.kind == H::SPECIAL_OP_MUTE && op.value == 0,
        "quiet off");
  op = {H::SPECIAL_OP_SLEEP, 9};
  CHECK(!H::switch_special_op(H::SWITCH_DISPLAY, true, &op) && !H::switch_special_op(H::SWITCH_BEEPER, true, &op) &&
            op.kind == H::SPECIAL_OP_SLEEP && op.value == 9,
        "the display and the beeper are not special modes");
  // Each switch write, applied, moves exactly its own flag.
  for (int kind = H::SWITCH_ECO; kind <= H::SWITCH_QUIET; kind++) {
    for (int on = 0; on < 2; on++) {
      H::SpecialState s;
      s.eco = s.turbo = s.mute = on == 0;
      H::switch_special_op((H::SwitchKind) kind, on != 0, &op);
      H::special_apply(&s, op);
      const bool got = kind == H::SWITCH_ECO ? s.eco : kind == H::SWITCH_TURBO ? s.turbo : s.mute;
      CHECK(got == (on != 0), "switch %d %s lands", kind, on ? "on" : "off");
    }
  }

  CHECK(H::special_entity_follows_status(false, false), "idle: the entities follow the unit");
  CHECK(!H::special_entity_follows_status(true, false) && !H::special_entity_follows_status(false, true) &&
            !H::special_entity_follows_status(true, true),
        "not in the hold-off, and not while a special-mode write is on its way");

  H::AcState st;
  bool on = false;
  st.eco_on = true;
  CHECK(H::switch_state_from_status(H::SWITCH_ECO, st, false, &on) && on, "eco follows byte 35 bit 2");
  CHECK(H::switch_state_from_status(H::SWITCH_TURBO, st, true, &on) && !on, "turbo is off");
  st.turbo_on = true;
  CHECK(H::switch_state_from_status(H::SWITCH_TURBO, st, false, &on) && on, "turbo follows bit 1");
  CHECK(H::switch_state_from_status(H::SWITCH_QUIET, st, true, &on) && !on, "quiet is off");
  st.mute_on = true;
  CHECK(H::switch_state_from_status(H::SWITCH_QUIET, st, false, &on) && on, "quiet follows the mute flag");
  CHECK(H::switch_state_from_status(H::SWITCH_DISPLAY, st, false, &on) && !on, "display: the standing preference, off");
  CHECK(H::switch_state_from_status(H::SWITCH_DISPLAY, st, true, &on) && on, "display: the standing preference, on");
  on = true;
  CHECK(!H::switch_state_from_status(H::SWITCH_BEEPER, st, false, &on) && on,
        "the beeper is no business of a status frame");
}

static void test_sleep_select() {
  printf("-- sleep select: status byte 17 as a profile\n");
  for (uint8_t profile = 0; profile <= H::SLEEP_PROFILE_MAX; profile++) {
    uint8_t got = 0xFF;
    CHECK(H::sleep_profile_from_status((uint8_t) (profile * 2), &got) && got == profile, "raw %u is profile %u",
          profile * 2, profile);
  }
  uint8_t got = 0x77;
  CHECK(!H::sleep_profile_from_status(10, &got) && !H::sleep_profile_from_status(0xFF, &got) && got == 0x77,
        "a value past Kids is no profile: the select keeps what it shows");
  CHECK(H::sleep_profile_from_status(9, &got) && got == 4, "an odd value reads as the profile below it");
}

// ---- Telemetry refresh --------------------------------------------------------------------------
static void test_refresh() {
  printf("-- telemetry: the periodic refresh\n");
  for (uint32_t start : {0u, 1000u, 0xFFFFFFFFu - 30000u, 0x7FFFFFFFu - 100u}) {
    H::TelemetryRefresh r;
    CHECK(H::telemetry_refresh_due(&r, start), "start %u: due on the first frame", (unsigned) start);
    CHECK(!H::telemetry_refresh_due(&r, start), "not twice in one millisecond");
    CHECK(!H::telemetry_refresh_due(&r, start + H::TELEMETRY_REFRESH_MS - 1), "not at 59.999 s");
    CHECK(H::telemetry_refresh_due(&r, start + H::TELEMETRY_REFRESH_MS), "due at 60 s, across the clock wrap too");
    CHECK(!H::telemetry_refresh_due(&r, start + H::TELEMETRY_REFRESH_MS + 1000), "and the period starts again");
    CHECK(H::telemetry_refresh_due(&r, start + 2 * H::TELEMETRY_REFRESH_MS + 5000), "a late frame is still due");
    CHECK(!H::telemetry_refresh_due(&r, start + 3 * H::TELEMETRY_REFRESH_MS + 4999),
          "60 s from that one, not from the grid");
  }
}

int main() {
  printf("== ESPHome entities: requests, views, switches, the sleep select ==\n");
  test_fan_names();
  test_plan_power();
  test_plan_mode();
  test_plan_setpoint();
  test_plan_fan();
  test_plan_swing_and_preset();
  test_view();
  test_shown_changed();
  test_switches();
  test_sleep_select();
  test_refresh();
  printf("  %d checks, %d failed\n", g_checks, g_fail);
  printf(g_fail ? "== ENTITIES FAILED ==\n" : "== ENTITIES OK ==\n");
  return g_fail ? 1 : 0;
}
