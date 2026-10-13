// Gap tests for the pure mapping and decision code (hisense_map.h) and the stock module's serial
// port restart policy (w41h1_uart_policy.h).
//
// Every check here closes a hole that coverage or mutation testing found in the other host tests
// (firmware/test/coverage/run.sh and mutate.py): a line no test ran, a branch no test took, or a
// mutant every test let through. The comment on each block names the line or the mutant it is
// there for. Line numbers are those of the file when the test was written: read them as "the
// statement that says this", since they move.
//
// The checks state what the code does today. Where today's behaviour is wrong the check is a
// KNOWN_DEFECT: it asserts the right behaviour, is reported, and does not fail the suite until
// the defect is fixed, at which point it fails with a note to turn it into a plain CHECK.

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "hisense_map.h"
#include "hisense_protocol.h"
#include "w41h1_uart_policy.h"

namespace H = esphome::hisense_ac;

static int g_fail = 0;
static int g_checks = 0;
static int g_known = 0;
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
// The condition is the behaviour the code should have and does not. Counted, printed, and no
// failure while it stays false. True means the defect is gone: that fails, so the marker cannot
// outlive the bug.
#define KNOWN_DEFECT(cond, ...) \
  do { \
    g_checks++; \
    if (cond) { \
      g_fail++; \
      printf("  FAIL %s:%d known defect no longer reproduces, make this a CHECK: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } else { \
      g_known++; \
      printf("  KNOWN DEFECT %s:%d ", __FILE__, __LINE__); \
      printf(__VA_ARGS__); \
      printf("\n"); \
    } \
  } while (0)

static const H::SpecialState NO_SPECIAL{};

static H::UnitView view(bool on, H::Mode mode, int setpoint, uint8_t fan_raw) {
  H::UnitView v;
  v.power_on = on;
  v.mode_valid = true;
  v.mode = mode;
  v.setpoint_c = (int8_t) setpoint;
  v.fan_raw = fan_raw;
  return v;
}

static H::SpecialState special(bool eco, bool turbo, bool mute, uint8_t sleep) {
  H::SpecialState s;
  s.eco = eco;
  s.turbo = turbo;
  s.mute = mute;
  s.sleep = sleep;
  return s;
}

static bool same(const H::SpecialState &a, const H::SpecialState &b) {
  return a.eco == b.eco && a.turbo == b.turbo && a.mute == b.mute && a.sleep == b.sleep;
}

// ---- Mode --------------------------------------------------------------------------------------
// climate_mode_to_hisense and hisense_mode_to_climate, the values next to the valid ones. The
// climate numbers are esphome::climate's enum (mirrored at the top of hisense_map.h), the Mode
// numbers the command index of hisense_protocol.h.
static void test_mode_enums() {
  printf("-- climate mode <-> A/C mode, values at and past the edges\n");
  struct {
    uint8_t climate;
    H::Mode mode;
  } rows[] = {{6, H::MODE_AUTO}, {2, H::MODE_COOL}, {3, H::MODE_HEAT}, {4, H::MODE_FAN}, {5, H::MODE_DRY}};
  for (const auto &r : rows) {
    H::Mode m = (H::Mode) 0x77;
    CHECK(H::climate_mode_to_hisense(r.climate, &m) && m == r.mode, "climate mode %u is A/C mode %d", r.climate,
          (int) r.mode);
    CHECK(H::hisense_mode_to_climate(r.mode) == r.climate, "and back");
  }
  // OFF (0) has its own frame, HEAT_COOL (1) is not what Hisense auto does, 7 is past the enum.
  for (int bad : {0, 1, 7, 8, 255}) {
    H::Mode m = (H::Mode) 0x77;
    CHECK(!H::climate_mode_to_hisense((uint8_t) bad, &m) && m == (H::Mode) 0x77,
          "climate mode %d is refused and the output untouched", bad);
  }
  // A Mode past AUTO reads as cool (the default arm of hisense_mode_to_climate).
  for (int bad : {5, 6, 7, 255})
    CHECK(H::hisense_mode_to_climate((H::Mode) bad) == H::CLIMATE_MODE_COOL_VALUE, "A/C mode %d shows as cool", bad);
}

// running_state and climate_action. The compressor frequency decides between working and idle,
// and the boundary is 0 against 1 (mutants `comp_freq > 0` -> `>= 0`, `> 1`).
static void test_action() {
  printf("-- what is running, at the compressor boundary\n");
  for (int m = H::MODE_FAN; m <= H::MODE_AUTO; m++)
    for (int hz : {0, 1, 255})
      CHECK(H::running_state(false, (H::Mode) m, (uint8_t) hz) == 0 &&
                H::climate_action(false, (H::Mode) m, (uint8_t) hz) == H::CLIMATE_ACTION_OFF_VALUE,
            "off is off in mode %d at %d Hz", m, hz);
  for (int hz : {0, 1, 255}) {
    CHECK(H::running_state(true, H::MODE_FAN, (uint8_t) hz) == 4, "fan-only is the fan bit at %d Hz", hz);
    CHECK(H::climate_action(true, H::MODE_FAN, (uint8_t) hz) == H::CLIMATE_ACTION_FAN_VALUE, "and action fan");
  }
  CHECK(H::running_state(true, H::MODE_HEAT, 0) == 0 && H::running_state(true, H::MODE_HEAT, 1) == 1,
        "heat: 0 and 1 Hz");
  CHECK(H::running_state(true, H::MODE_COOL, 0) == 0 && H::running_state(true, H::MODE_COOL, 1) == 2,
        "cool: 0 and 1 Hz");
  CHECK(H::running_state(true, H::MODE_HEAT, 255) == 1 && H::running_state(true, H::MODE_COOL, 255) == 2, "at 255 Hz");
  CHECK(H::running_state(true, H::MODE_DRY, 1) == 2, "dry with the compressor on counts as the cool bit");
  CHECK(H::climate_action(true, H::MODE_HEAT, 0) == H::CLIMATE_ACTION_IDLE_VALUE, "heat at 0 Hz is idle");
  CHECK(H::climate_action(true, H::MODE_HEAT, 1) == H::CLIMATE_ACTION_HEATING_VALUE, "heat at 1 Hz is heating");
  CHECK(H::climate_action(true, H::MODE_COOL, 0) == H::CLIMATE_ACTION_IDLE_VALUE, "cool at 0 Hz is idle");
  CHECK(H::climate_action(true, H::MODE_COOL, 1) == H::CLIMATE_ACTION_COOLING_VALUE, "cool at 1 Hz is cooling");
  CHECK(H::climate_action(true, H::MODE_DRY, 0) == H::CLIMATE_ACTION_IDLE_VALUE, "dry at 0 Hz is idle");
  CHECK(H::climate_action(true, H::MODE_DRY, 1) == H::CLIMATE_ACTION_DRYING_VALUE, "dry at 1 Hz is drying");
  CHECK(H::climate_action(true, H::MODE_AUTO, 0) == H::CLIMATE_ACTION_IDLE_VALUE, "auto at 0 Hz is idle");
}

// ---- Swing -------------------------------------------------------------------------------------
static void test_swing() {
  printf("-- swing, every pair and the value past the enum\n");
  CHECK(H::climate_swing(false, false) == 0 && H::climate_swing(true, true) == 1, "off and both");
  CHECK(H::climate_swing(true, false) == 2 && H::climate_swing(false, true) == 3, "vertical and horizontal");
  struct {
    uint8_t swing;
    H::SwingMode v, h;
  } rows[] = {{0, H::SWING_MODE_OFF, H::SWING_MODE_OFF},   {1, H::SWING_MODE_SWING, H::SWING_MODE_SWING},
              {2, H::SWING_MODE_SWING, H::SWING_MODE_OFF}, {3, H::SWING_MODE_OFF, H::SWING_MODE_SWING},
              {4, H::SWING_MODE_OFF, H::SWING_MODE_OFF},   {255, H::SWING_MODE_OFF, H::SWING_MODE_OFF}};
  for (const auto &r : rows) {
    H::SwingMode v = H::SWING_MODE_DIRECTION, h = H::SWING_MODE_DIRECTION;
    H::climate_swing_to_hisense(r.swing, &v, &h);
    CHECK(v == r.v && h == r.h, "climate swing %u is vertical %d, horizontal %d", r.swing, (int) v, (int) h);
  }
}

// ---- Fan ladder --------------------------------------------------------------------------------
// The table itself (a mutated cell changes one step and nothing else), the lookups one raw value
// either side of every row, and the indexes either side of 0..6.
static void test_fan_ladder() {
  printf("-- fan ladder, its cells and its edges\n");
  // raw is status byte 16 = fan index * 2, cmd the fan index (FanSpeed, hisense_protocol.h: 0x02
  // quiet, 0x0A..0x12 low..high, each confirmed there). speed is the row's place on the ladder.
  struct {
    uint8_t raw, speed, percent;
    H::FanSpeed cmd;
  } want[] = {{0x02, 1, 10, H::FAN_SPEED_QUIET},    {0x0A, 2, 25, H::FAN_SPEED_LOW},
              {0x0C, 3, 42, H::FAN_SPEED_MED_LOW},  {0x0E, 4, 58, H::FAN_SPEED_MID},
              {0x10, 5, 75, H::FAN_SPEED_MED_HIGH}, {0x12, 6, 100, H::FAN_SPEED_HIGH}};
  CHECK(H::FAN_TABLE_LEN == 6 && H::FAN_INDEX_MAX == 6 && H::FAN_INDEX_AUTO == 0, "six speeds, auto is index 0");
  CHECK(H::FAN_RAW_AUTO == 0x01, "auto is status byte 0x01");
  for (size_t i = 0; i < 6; i++) {
    const H::FanRow &r = H::FAN_TABLE[i];
    CHECK(r.raw == want[i].raw && r.speed == want[i].speed && r.percent == want[i].percent && r.cmd == want[i].cmd,
          "row %zu is raw 0x%02X, speed %u, %u%%, cmd %d", i, r.raw, r.speed, r.percent, (int) r.cmd);
    CHECK(r.raw == (uint8_t) (r.cmd * 2), "row %zu: status byte is the fan index * 2", i);
    CHECK(H::fan_row_by_raw(r.raw) == &r && H::fan_row_by_speed(r.speed) == &r, "row %zu found by raw and by speed", i);
    CHECK(H::fan_raw_to_index(r.raw) == r.speed && H::fan_raw_to_cmd(r.raw) == r.cmd, "row %zu maps both ways", i);
    CHECK(H::fan_index_to_hisense(r.speed) == r.cmd, "index %u commands %d", r.speed, (int) r.cmd);
    // One either side of a row is no row (odd status bytes are command bytes, not status).
    for (int d : {-1, 1}) {
      uint8_t near = (uint8_t) (r.raw + d);
      if (near == H::FAN_RAW_AUTO)
        continue;
      CHECK(H::fan_row_by_raw(near) == nullptr && H::fan_raw_to_index(near) == 0 &&
                H::fan_raw_to_cmd(near) == H::FAN_SPEED_NOCHANGE,
            "raw 0x%02X next to row %zu is unknown", near, i);
    }
  }
  CHECK(H::fan_row_by_raw(0x01) == nullptr && H::fan_raw_to_index(0x01) == 0, "auto is not a row and reads as index 0");
  CHECK(H::fan_raw_to_cmd(0x01) == H::FAN_SPEED_AUTO, "auto syncs the shadow as auto");
  CHECK(H::fan_raw_to_cmd(0x00) == H::FAN_SPEED_NOCHANGE && H::fan_raw_to_cmd(0xFF) == H::FAN_SPEED_NOCHANGE,
        "0x00 and 0xFF leave the shadow alone");
  CHECK(H::fan_row_by_speed(0) == nullptr && H::fan_row_by_speed(7) == nullptr, "no row for speed 0 or 7");
  CHECK(H::fan_index_to_hisense(0) == H::FAN_SPEED_AUTO, "index 0 is auto");
  CHECK(H::fan_index_to_hisense(7) == H::FAN_SPEED_AUTO && H::fan_index_to_hisense(255) == H::FAN_SPEED_AUTO,
        "an index past the ladder falls back to auto");

  // shadow_fan_from_status: only the quiet step is withheld.
  CHECK(H::shadow_fan_from_status(0x02) == H::FAN_SPEED_NOCHANGE, "the quiet step never enters the shadow");
  CHECK(H::shadow_fan_from_status(0x01) == H::FAN_SPEED_AUTO && H::shadow_fan_from_status(0x0A) == H::FAN_SPEED_LOW &&
            H::shadow_fan_from_status(0x12) == H::FAN_SPEED_HIGH,
        "auto, low and high do");

  // climate_fan_to_index, every value of esphome::climate's fan enum (mirrored in hisense_map.h)
  // and the first one past it.
  const uint8_t by_enum[] = {6 /* ON */,   0 /* OFF */,    0 /* AUTO */,  2 /* LOW */,     4 /* MEDIUM */,
                             6 /* HIGH */, 3 /* MIDDLE */, 5 /* FOCUS */, 0 /* DIFFUSE */, 1 /* QUIET */};
  for (uint8_t f = 0; f < 10; f++)
    CHECK(H::climate_fan_to_index(f) == by_enum[f], "climate fan %u is ladder index %u", f, by_enum[f]);
  CHECK(H::climate_fan_to_index(10) == 0 && H::climate_fan_to_index(255) == 0, "past the enum: auto");

  const uint8_t published[] = {0, 2, 2, 3, 4, 5, 6, 7};
  for (uint8_t i = 0; i < 8; i++)
    CHECK(H::fan_published_index(i) == published[i], "index %u is published as %u", i, published[i]);
}

// ---- Setpoint ----------------------------------------------------------------------------------
// The clamp at 15/16 and 32/33 (mutants on `<`, `>` and on the returned bound), far outside, and
// the two outputs of setpoint_to_cmd in each panel unit. The range is hisense_protocol.h's
// SETPOINT_MIN_C/MAX_C (16..32 C) and SETPOINT_MIN_F/MAX_F (61..90 F), "sample-confirmed" there.
static void test_setpoint() {
  printf("-- setpoint clamp and unit, at the bounds\n");
  struct {
    int in, out;
  } clamp[] = {{INT_MIN, 16}, {-129, 16}, {-1, 16}, {0, 16},  {15, 16},  {16, 16},  {17, 17},  {24, 24},
               {31, 31},      {32, 32},   {33, 32}, {61, 32}, {127, 32}, {128, 32}, {300, 32}, {INT_MAX, 32}};
  for (const auto &c : clamp)
    CHECK(H::clamp_setpoint_c(c.in) == c.out, "clamp(%d) is %d", c.in, c.out);

  // Celsius panel: the wire byte is the clamped value, and a stale F flag is cleared.
  for (const auto &c : clamp) {
    H::AcCommand cmd;
    cmd.fahrenheit = true;
    cmd.setpoint = 77;
    CHECK(H::setpoint_to_cmd(c.in, false, &cmd) == c.out && cmd.setpoint == c.out && !cmd.fahrenheit,
          "C panel, %d wanted: %d returned and on the wire", c.in, c.out);
  }
  // Fahrenheit panel: the return value stays Celsius, the wire byte is c_to_f of it. 16 C is 61 F
  // (60.8) and 32 C is 90 F (89.6): the two ends of the F range.
  struct {
    int in, c, f;
  } fpanel[] = {{INT_MIN, 16, 61}, {15, 16, 61}, {16, 16, 61}, {17, 17, 63}, {20, 20, 68},     {24, 24, 75},
                {25, 25, 77},      {31, 31, 88}, {32, 32, 90}, {33, 32, 90}, {INT_MAX, 32, 90}};
  for (const auto &r : fpanel) {
    H::AcCommand cmd;
    CHECK(H::setpoint_to_cmd(r.in, true, &cmd) == r.c && cmd.setpoint == r.f && cmd.fahrenheit,
          "F panel, %d wanted: %d C returned, %d F on the wire (got %d)", r.in, r.c, r.f, cmd.setpoint);
    CHECK(H::setpoint_in_range(cmd.setpoint, true), "which the builder accepts");
  }

  // setpoint_request_allowed: every mode, running and not.
  for (int m = H::MODE_FAN; m <= H::MODE_AUTO; m++) {
    CHECK(H::setpoint_request_allowed(false, (H::Mode) m), "not running: mode %d takes a setpoint into the shadow", m);
    const bool takes = m == H::MODE_COOL || m == H::MODE_HEAT;
    CHECK(H::setpoint_request_allowed(true, (H::Mode) m) == takes, "running in mode %d: %s", m,
          takes ? "allowed" : "refused");
  }

  // sync_shadow_setpoint: a value the builder would refuse leaves BOTH fields of the shadow alone
  // (mutants that move the two assignments above the check).
  for (int bad : {-128, 5, 15, 33, 127}) {
    for (int unit_f = 0; unit_f < 2; unit_f++) {
      H::AcCommand cmd;
      cmd.setpoint = 21;
      cmd.fahrenheit = unit_f == 0;  // the opposite of the frame's unit
      CHECK(!H::sync_shadow_setpoint((int8_t) bad, unit_f != 0, &cmd) && cmd.setpoint == 21 &&
                cmd.fahrenheit == (unit_f == 0),
            "status setpoint %d C (%s panel) is refused, shadow untouched", bad, unit_f ? "F" : "C");
    }
  }
  for (int good : {16, 24, 32}) {
    H::AcCommand cmd;
    cmd.fahrenheit = true;
    CHECK(H::sync_shadow_setpoint((int8_t) good, false, &cmd) && cmd.setpoint == good && !cmd.fahrenheit,
          "C panel: %d C taken as it is", good);
    H::AcCommand cmdf;
    CHECK(H::sync_shadow_setpoint((int8_t) good, true, &cmdf) && cmdf.setpoint == H::c_to_f(good) && cmdf.fahrenheit,
          "F panel: %d C taken as %d F", good, cmdf.setpoint);
  }
}

// Was a DEFECT, fixed. On a Fahrenheit panel the status setpoint is a whole number of F.
// parse_status() turns it into whole Celsius, and the shadow used to be filled from that, converted
// back: 13 of the 30 values the panel can show did not come back (62 F became 63, 65 became 64),
// and the next combined frame moved the setpoint the user chose on the remote. The shadow now takes
// the frame's own byte (AcState::setpoint_raw).
static void test_fahrenheit_shadow_round_trip() {
  printf("-- an F panel's setpoint through the status frame and back into the shadow\n");
  int via_celsius = 0;
  for (int f = H::SETPOINT_MIN_F; f <= H::SETPOINT_MAX_F; f++) {
    // What parse_status() stores for status byte 19 = f on an F panel.
    H::AcState st;
    st.valid = true;
    st.temp_unit_f = true;
    st.setpoint_raw = (uint8_t) f;
    st.setpoint_c = H::f_to_c(f);
    H::AcCommand cmd;
    CHECK(H::sync_shadow_setpoint(st, &cmd) && cmd.setpoint == f && cmd.fahrenheit,
          "%d F reported is %d F in the shadow", f, cmd.setpoint);
    // The old path, still there for the Celsius side and for parity with the reference driver.
    H::AcCommand old;
    CHECK(H::sync_shadow_setpoint(st.setpoint_c, true, &old) && old.setpoint >= f - 1 && old.setpoint <= f + 1,
          "%d F through Celsius comes back within one degree (got %d)", f, old.setpoint);
    via_celsius += old.setpoint != f;
  }
  CHECK(via_celsius == 13, "through Celsius, %d of 30 values would move: that is what the raw byte avoids",
        via_celsius);
  // Out of range on an F panel: the shadow is left alone, both fields.
  for (int bad : {0, 60, 91, 127, 128, 200, 255}) {
    H::AcState st;
    st.temp_unit_f = true;
    st.setpoint_raw = (uint8_t) bad;
    st.setpoint_c = H::f_to_c(72);  // a setpoint_c that would pass must not rescue a bad raw byte
    H::AcCommand cmd;
    cmd.setpoint = 21;
    CHECK(!H::sync_shadow_setpoint(st, &cmd) && cmd.setpoint == 21 && !cmd.fahrenheit, "F panel reporting %d: refused",
          bad);
  }
  // A Celsius panel goes by setpoint_c as before, whatever setpoint_raw holds.
  for (int c = H::SETPOINT_MIN_C; c <= H::SETPOINT_MAX_C; c++) {
    H::AcState st;
    st.setpoint_c = (int8_t) c;
    st.setpoint_raw = 77;
    H::AcCommand cmd;
    cmd.fahrenheit = true;
    CHECK(H::sync_shadow_setpoint(st, &cmd) && cmd.setpoint == c && !cmd.fahrenheit, "C panel: %d C taken", c);
  }
  H::AcState cold;
  cold.setpoint_c = 15;
  cold.setpoint_raw = 15;
  H::AcCommand kept;
  CHECK(!H::sync_shadow_setpoint(cold, &kept) && kept.setpoint == 24, "C panel reporting 15: refused");
  // Every whole Celsius value does survive C -> F -> C, so the Celsius side of ESPHome is exact.
  for (int c = H::SETPOINT_MIN_C; c <= H::SETPOINT_MAX_C; c++)
    CHECK(H::f_to_c(H::c_to_f(c)) == c, "%d C survives the trip through F", c);
}

// ---- Eco / turbo --------------------------------------------------------------------------------
static void test_feature() {
  printf("-- byte 33 from status and after a send\n");
  CHECK(H::feature_from_status(false, false) == H::FEATURE_NONE, "neither");
  CHECK(H::feature_from_status(true, false) == H::FEATURE_ECO, "eco");
  CHECK(H::feature_from_status(false, true) == H::FEATURE_TURBO, "turbo");
  CHECK(H::feature_from_status(true, true) == H::FEATURE_ECO, "both reported: eco wins");
  CHECK(H::feature_after_send(H::FEATURE_ECO_OFF) == H::FEATURE_NONE, "the eco clear is one-shot");
  CHECK(H::feature_after_send(H::FEATURE_NONE) == H::FEATURE_NONE &&
            H::feature_after_send(H::FEATURE_ECO) == H::FEATURE_ECO &&
            H::feature_after_send(H::FEATURE_TURBO) == H::FEATURE_TURBO,
        "the others stay in the shadow");
}

// ---- Presets ------------------------------------------------------------------------------------
// The table cell by cell (a mutated flag changes what a preset name does on the unit), the index
// constants against the names, and the support mask one bit at a time.
static void test_presets() {
  printf("-- preset table, names and support mask\n");
  struct {
    const char *name;
    bool eco, turbo, mute;
    uint8_t sleep;
  } want[] = {{"none", false, false, false, 0},         {"eco", true, false, false, 0},
              {"quiet", false, false, true, 0},         {"turbo", false, true, false, 0},
              {"eco_quiet", true, false, true, 0},      {"sleep_general", false, false, false, 1},
              {"sleep_old", false, false, false, 2},    {"sleep_young", false, false, false, 3},
              {"sleep_kids", false, false, false, 4},   {"eco_sleep_general", true, false, false, 1},
              {"eco_sleep_old", true, false, false, 2}, {"eco_sleep_young", true, false, false, 3},
              {"eco_sleep_kids", true, false, false, 4}};
  CHECK(H::PRESET_COUNT == 13, "thirteen presets");
  for (size_t i = 0; i < 13 && i < H::PRESET_COUNT; i++) {
    const H::PresetRow &r = H::PRESETS[i];
    CHECK(std::strcmp(r.name, want[i].name) == 0 && r.eco == want[i].eco && r.turbo == want[i].turbo &&
              r.mute == want[i].mute && r.sleep == want[i].sleep,
          "row %zu is %s", i, want[i].name);
    CHECK(H::preset_index(want[i].name) == (int) i, "%s is found at %zu", want[i].name, i);
    // An exact state is detected as its own row when the unit supports everything.
    CHECK(H::preset_detect(special(want[i].eco, want[i].turbo, want[i].mute, want[i].sleep), 0x0F) == i,
          "%s is detected from its own flags", want[i].name);
  }
  CHECK(H::PRESET_NONE == 0 && H::PRESET_ECO == 1 && H::PRESET_QUIET == 2 && H::PRESET_TURBO == 3,
        "the index constants name those rows");
  CHECK(std::strcmp(H::PRESETS[H::PRESET_FIRST_SLEEP + 1].name, "sleep_general") == 0 &&
            std::strcmp(H::PRESETS[H::PRESET_FIRST_SLEEP + 4].name, "sleep_kids") == 0,
        "PRESET_FIRST_SLEEP + profile is the sleep row");
  CHECK(H::PRESET_FIRST_CUSTOM == 2, "none and eco are the ESPHome built-ins, the rest are custom names");

  CHECK(H::preset_index(nullptr) == -1 && H::preset_index("") == -1, "no name, empty name");
  CHECK(H::preset_index("Eco") == -1 && H::preset_index("eco ") == -1 && H::preset_index("ec") == -1,
        "names match whole and case-sensitive");

  // preset_available: the index bound (mutant `>=` -> `>`), then one support bit per flag.
  CHECK(!H::preset_available(H::PRESET_COUNT, 0x0F) && !H::preset_available(H::PRESET_COUNT + 1, 0x0F) &&
            !H::preset_available((size_t) -1, 0x0F),
        "no preset at or past the end of the table");
  CHECK(H::preset_available(H::PRESET_COUNT - 1, 0x0F), "the last row is a preset");
  CHECK(H::SUPPORT_ECO == 0x01 && H::SUPPORT_QUIET == 0x02 && H::SUPPORT_TURBO == 0x04 && H::SUPPORT_SLEEP == 0x08,
        "the support bits climate.py packs");
  for (size_t i = 0; i < H::PRESET_COUNT; i++) {
    const H::PresetRow &r = H::PRESETS[i];
    uint8_t needs = 0;
    if (r.eco)
      needs |= H::SUPPORT_ECO;
    if (r.mute)
      needs |= H::SUPPORT_QUIET;
    if (r.turbo)
      needs |= H::SUPPORT_TURBO;
    if (r.sleep != 0)
      needs |= H::SUPPORT_SLEEP;
    for (uint8_t support = 0; support < 16; support++)
      CHECK(H::preset_available(i, support) == ((support & needs) == needs), "%s with support 0x%X", r.name, support);
  }

  // special_from_status: status byte 17 is the profile * 2.
  for (int raw : {0, 1, 2, 3, 4, 6, 8, 9, 10, 255}) {
    H::SpecialState s = H::special_from_status(true, false, true, (uint8_t) raw);
    CHECK(s.eco && !s.turbo && s.mute && s.sleep == raw / 2, "sleep byte %d is profile %d", raw, raw / 2);
  }
  H::SpecialState t = H::special_from_status(false, true, false, 0);
  CHECK(!t.eco && t.turbo && !t.mute && t.sleep == 0, "each flag lands in its own field");

  // preset_detect with no exact row: the strongest mode that is on, turbo, eco, quiet.
  CHECK(H::preset_detect(special(true, true, false, 0), 0x0F) == H::PRESET_TURBO, "eco + turbo reads as turbo");
  CHECK(H::preset_detect(special(false, true, true, 2), 0x0F) == H::PRESET_TURBO,
        "turbo + quiet + sleep reads as turbo");
  CHECK(H::preset_detect(special(true, false, true, 3), 0x0F) == H::PRESET_ECO, "eco + quiet + sleep reads as eco");
  CHECK(H::preset_detect(special(false, false, true, 1), 0x0F) == H::PRESET_QUIET, "quiet + sleep reads as quiet");
  CHECK(H::preset_detect(special(true, false, false, 5), 0x0F) == H::PRESET_ECO,
        "eco + an unknown profile reads as eco");
  // The guess is gated by the support mask like an exact row.
  CHECK(H::preset_detect(special(true, true, false, 0), 0x0F & ~H::SUPPORT_TURBO) == H::PRESET_NONE,
        "a guessed turbo the unit does not offer reads as none");
  CHECK(H::preset_detect(special(false, false, true, 1), 0x0F & ~H::SUPPORT_QUIET) == H::PRESET_NONE,
        "a guessed quiet the unit does not offer reads as none");
  CHECK(H::preset_detect(special(false, true, false, 0), 0x0F & ~H::SUPPORT_TURBO) == H::PRESET_NONE,
        "an exact turbo the unit does not offer reads as none");
}

// special_plan over every pair of states: the plan never overruns the caller's array, follows
// the order the header documents, and (applied with special_apply) lands on the target.
static void test_special_plan() {
  printf("-- special-mode plans, every state to every preset\n");
  size_t longest = 0;
  int missed = 0;
  H::SpecialState miss_now, miss_want;
  for (int now_bits = 0; now_bits < 8; now_bits++) {
    for (uint8_t now_sleep = 0; now_sleep <= 4; now_sleep++) {
      H::SpecialState now = special(now_bits & 1, now_bits & 2, now_bits & 4, now_sleep);
      for (uint8_t target = 0; target < H::PRESET_COUNT; target++) {
        const H::PresetRow &row = H::PRESETS[target];
        H::SpecialState want = special(row.eco, row.turbo, row.mute, row.sleep);
        H::SpecialOp ops[H::PRESET_PLAN_MAX + 3];
        std::memset(ops, 0xEE, sizeof(ops));
        size_t n = H::preset_plan(now, target, ops);
        if (n > longest)
          longest = n;
        CHECK(n <= H::PRESET_PLAN_MAX, "a plan fits the caller's array (%zu ops)", n);
        CHECK(n >= H::PRESET_PLAN_MAX + 3 || (uint8_t) ops[n].kind == 0xEE, "nothing is written past the plan");
        if (same(now, want))
          CHECK(n == 0, "preset %u from its own state plans no write", target);

        H::SpecialState s = now;
        int sleep_pos = -1, feature_pos = -1;
        for (size_t k = 0; k < n && k < H::PRESET_PLAN_MAX; k++) {
          if (ops[k].kind == H::SPECIAL_OP_SLEEP && ops[k].value != 0)
            sleep_pos = (int) k;
          if (ops[k].kind == H::SPECIAL_OP_FEATURE)
            feature_pos = (int) k;
          if (ops[k].kind == H::SPECIAL_OP_SLEEP && ops[k].value == 0)
            CHECK(k == 0, "sleep off is the first write");
          H::special_apply(&s, ops[k]);
        }
        if (sleep_pos >= 0)
          CHECK(sleep_pos == (int) n - 1 && sleep_pos > feature_pos, "a sleep profile is the last write");
        if (want.sleep != 0 && feature_pos >= 0)
          CHECK(sleep_pos > feature_pos, "the profile is sent again after a byte 33 write");
        if (!same(s, want)) {
          if (missed == 0) {
            miss_now = now;
            miss_want = want;
          }
          missed++;
        }
      }
    }
  }
  // Sleep off, the two byte 33 clears, a mute write: four at most. PRESET_PLAN_MAX (5) has room.
  CHECK(longest == 4, "the longest plan is four writes (got %zu)", longest);
  // Was a DEFECT, fixed. With eco and turbo both reported (the unit shows the pair for a second or
  // two while it arbitrates), a plan to a preset without either sent only the eco clear (byte 33 =
  // 0x10). Per the Feature enum that value does not switch turbo off (0x04 does), so turbo stayed
  // on: 60 of the 2,600 plans, all from that start. The plan now sends both clears.
  CHECK(missed == 0,
        "%d plans do not reach their target, e.g. eco=%d turbo=%d mute=%d sleep=%u to eco=%d turbo=%d mute=%d "
        "sleep=%u",
        missed, miss_now.eco, miss_now.turbo, miss_now.mute, miss_now.sleep, miss_want.eco, miss_want.turbo,
        miss_want.mute, miss_want.sleep);

  H::SpecialOp ops[H::PRESET_PLAN_MAX];
  CHECK(H::preset_plan(special(true, false, true, 2), H::PRESET_COUNT, ops) == 0 &&
            H::preset_plan(special(true, false, true, 2), 255, ops) == 0,
        "a preset index past the table plans nothing");

  // Eco and turbo both on, to neither: the eco clear, then the turbo clear.
  size_t pair = H::special_plan(special(true, true, false, 0), NO_SPECIAL, ops);
  CHECK(pair == 2 && ops[0].kind == H::SPECIAL_OP_FEATURE && ops[0].value == H::FEATURE_ECO_OFF &&
            ops[1].kind == H::SPECIAL_OP_FEATURE && ops[1].value == H::FEATURE_NONE,
        "eco + turbo to none: both clears (%zu writes)", pair);
  // To one of the two it stays one write: setting either clears the other.
  pair = H::special_plan(special(true, true, false, 0), special(true, false, false, 0), ops);
  CHECK(pair == 1 && ops[0].value == H::FEATURE_ECO, "eco + turbo to eco: one write");
  pair = H::special_plan(special(true, true, false, 0), special(false, true, false, 0), ops);
  CHECK(pair == 1 && ops[0].value == H::FEATURE_TURBO, "eco + turbo to turbo: one write");

  // The single writes, value by value.
  size_t n = H::special_plan(special(true, false, false, 0), NO_SPECIAL, ops);
  CHECK(n == 1 && ops[0].kind == H::SPECIAL_OP_FEATURE && ops[0].value == H::FEATURE_ECO_OFF,
        "eco off is the eco clear");
  n = H::special_plan(special(false, true, false, 0), NO_SPECIAL, ops);
  CHECK(n == 1 && ops[0].kind == H::SPECIAL_OP_FEATURE && ops[0].value == H::FEATURE_NONE, "turbo off is neutral");
  n = H::special_plan(special(true, false, false, 0), special(false, true, false, 0), ops);
  CHECK(n == 1 && ops[0].kind == H::SPECIAL_OP_FEATURE && ops[0].value == H::FEATURE_TURBO,
        "eco to turbo is one write");
  n = H::special_plan(special(false, true, false, 0), special(true, false, false, 0), ops);
  CHECK(n == 1 && ops[0].kind == H::SPECIAL_OP_FEATURE && ops[0].value == H::FEATURE_ECO, "turbo to eco is one write");
  n = H::special_plan(NO_SPECIAL, special(false, false, true, 0), ops);
  CHECK(n == 1 && ops[0].kind == H::SPECIAL_OP_MUTE && ops[0].value == 1, "quiet on");
  n = H::special_plan(special(false, false, true, 0), NO_SPECIAL, ops);
  CHECK(n == 1 && ops[0].kind == H::SPECIAL_OP_MUTE && ops[0].value == 0, "quiet off");
  n = H::special_plan(special(false, false, false, 2), special(false, false, false, 3), ops);
  CHECK(n == 1 && ops[0].kind == H::SPECIAL_OP_SLEEP && ops[0].value == 3, "one profile to another is one write");
  n = H::special_plan(special(false, false, false, 2), special(false, false, false, 2), ops);
  CHECK(n == 0, "the same profile is no write");
  // Eco arrives while a profile is on: the profile is sent again after it.
  n = H::special_plan(special(false, false, false, 2), special(true, false, false, 2), ops);
  CHECK(n == 2 && ops[0].kind == H::SPECIAL_OP_FEATURE && ops[0].value == H::FEATURE_ECO &&
            ops[1].kind == H::SPECIAL_OP_SLEEP && ops[1].value == 2,
        "eco onto a running profile: eco, then the profile again");
  // Quiet off and eco on in one plan: the clear comes before the set.
  n = H::special_plan(special(false, false, true, 0), special(true, false, false, 0), ops);
  CHECK(n == 2 && ops[0].kind == H::SPECIAL_OP_MUTE && ops[0].value == 0 && ops[1].kind == H::SPECIAL_OP_FEATURE,
        "mute off before eco on");
  // Eco off and quiet on: byte 33 cleared before mute is set.
  n = H::special_plan(special(true, false, false, 0), special(false, false, true, 0), ops);
  CHECK(n == 2 && ops[0].kind == H::SPECIAL_OP_FEATURE && ops[0].value == H::FEATURE_ECO_OFF &&
            ops[1].kind == H::SPECIAL_OP_MUTE && ops[1].value == 1,
        "eco clear before mute on");

  // special_apply, each write and the values past the enums.
  H::SpecialState s = special(false, true, false, 0);
  H::special_apply(&s, {H::SPECIAL_OP_FEATURE, H::FEATURE_ECO});
  CHECK(s.eco && !s.turbo, "ECO sets eco and clears turbo");
  H::special_apply(&s, {H::SPECIAL_OP_FEATURE, H::FEATURE_TURBO});
  CHECK(!s.eco && s.turbo, "TURBO sets turbo and clears eco");
  s = special(true, true, true, 3);
  H::special_apply(&s, {H::SPECIAL_OP_FEATURE, H::FEATURE_ECO_OFF});
  CHECK(!s.eco && s.turbo && s.mute && s.sleep == 3, "ECO_OFF clears eco and nothing else");
  s = special(true, true, true, 3);
  H::special_apply(&s, {H::SPECIAL_OP_FEATURE, H::FEATURE_NONE});
  CHECK(s.eco && !s.turbo && s.mute && s.sleep == 3, "NONE clears turbo and nothing else");
  s = special(true, true, true, 3);
  H::special_apply(&s, {H::SPECIAL_OP_FEATURE, 9});
  CHECK(s.eco && !s.turbo, "a byte 33 value past the enum is treated as NONE");
  s = special(true, true, false, 3);
  H::special_apply(&s, {H::SPECIAL_OP_MUTE, 2});
  CHECK(s.mute && s.eco && s.turbo && s.sleep == 3, "any non-zero mute value is on");
  H::special_apply(&s, {H::SPECIAL_OP_MUTE, 0});
  CHECK(!s.mute, "0 is off");
  H::special_apply(&s, {H::SPECIAL_OP_SLEEP, 4});
  CHECK(s.sleep == 4 && s.eco && s.turbo && !s.mute, "sleep takes the profile");
  s = special(true, false, true, 2);
  H::special_apply(&s, {(H::SpecialOpKind) 7, 1});
  CHECK(same(s, special(true, false, true, 2)), "a write of no known kind changes nothing");

  // special_wanted: the projected state, then the queue in order.
  H::SpecialOp queue[] = {
      {H::SPECIAL_OP_FEATURE, H::FEATURE_TURBO}, {H::SPECIAL_OP_SLEEP, 1}, {H::SPECIAL_OP_SLEEP, 2}};
  CHECK(same(H::special_wanted(special(true, false, true, 0), queue, 0), special(true, false, true, 0)),
        "an empty queue leaves the projected state");
  CHECK(same(H::special_wanted(special(true, false, true, 0), nullptr, 0), special(true, false, true, 0)),
        "also with no queue at all");
  CHECK(same(H::special_wanted(special(true, false, true, 0), queue, 1), special(false, true, true, 0)),
        "one queued write");
  CHECK(same(H::special_wanted(special(true, false, true, 0), queue, 2), special(false, true, true, 1)), "two");
  CHECK(same(H::special_wanted(special(true, false, true, 0), queue, 3), special(false, true, true, 2)),
        "three: the later write to a field wins");
}

// forced_fan_index and fan_request_allowed.
static void test_fan_pin() {
  printf("-- the fan a special mode pins\n");
  CHECK(H::forced_fan_index(NO_SPECIAL) == -1 && H::forced_fan_index(special(true, false, false, 0)) == -1,
        "free with nothing on, and eco pins nothing");
  CHECK(H::forced_fan_index(special(false, true, false, 0)) == 6, "turbo pins high");
  CHECK(H::forced_fan_index(special(false, false, true, 0)) == 2, "quiet pins low");
  for (uint8_t p = 1; p <= 4; p++)
    CHECK(H::forced_fan_index(special(false, false, false, p)) == 2, "sleep profile %u pins low", p);
  CHECK(H::forced_fan_index(special(false, true, true, 2)) == 6, "turbo wins over quiet and sleep");
  for (int idx : {0, 1, 2, 3, 4, 5, 6, 7, 255}) {
    CHECK(H::fan_request_allowed(NO_SPECIAL, (uint8_t) idx), "free: index %d may be asked", idx);
    CHECK(H::fan_request_allowed(special(false, true, false, 0), (uint8_t) idx) == (idx == 6), "turbo: index %d", idx);
    CHECK(H::fan_request_allowed(special(false, false, true, 0), (uint8_t) idx) == (idx == 1 || idx == 2),
          "quiet: index %d", idx);
    CHECK(H::fan_request_allowed(special(false, false, false, 1), (uint8_t) idx) == (idx == 1 || idx == 2),
          "sleep: index %d", idx);
  }
}

// ---- Confirm and retry -------------------------------------------------------------------------
static void test_resend_and_reply() {
  printf("-- re-send limit and the reply verdict, at the lengths either side of each bound\n");
  CHECK(H::COMMAND_SENDS_MAX == 3, "three sends in all");
  CHECK(H::command_resend_allowed(0, true) && H::command_resend_allowed(2, true), "0 and 2 sends: again");
  CHECK(!H::command_resend_allowed(3, true) && !H::command_resend_allowed(4, true) &&
            !H::command_resend_allowed(255, true),
        "3 and more: no");
  CHECK(!H::command_resend_allowed(0, false) && !H::command_resend_allowed(2, false), "never into a silent bus");

  // A command and a reply that echoes its class (frame 13) and the byte after (frame 14), with
  // the ack value at frame 15: RE docs/10 section 4.2 step 6, as cited at CMD_REPLY_ACK_OFFSET.
  uint8_t tx[32] = {0};
  uint8_t reply[32] = {0};
  tx[13] = 0x65;  // CLASS_COMMAND
  tx[14] = 0x5A;  // arbitrary, to tell frame 14 from frame 13
  reply[13] = 0x65;
  reply[14] = 0x5A;
  reply[15] = 0x01;  // CMD_REPLY_ACK_VALUE
  CHECK(H::CMD_REPLY_ACK_OFFSET == 15 && H::CMD_REPLY_ACK_VALUE == 0x01, "ack byte: frame 15, value 1");
  CHECK(H::command_reply_verdict(tx, 32, nullptr, 32) == H::COMMAND_REPLY_NONE, "no reply buffer: none");
  CHECK(H::command_reply_verdict(tx, 32, reply, 0) == H::COMMAND_REPLY_NONE, "an empty reply: none");
  CHECK(H::command_reply_verdict(nullptr, 32, reply, 32) == H::COMMAND_REPLY_OTHER,
        "no command to compare with: other");
  // tx must reach frame 14 (15 bytes), the reply frame 15 (16 bytes).
  CHECK(H::command_reply_verdict(tx, 14, reply, 32) == H::COMMAND_REPLY_OTHER, "a 14-byte command: other");
  CHECK(H::command_reply_verdict(tx, 13, reply, 32) == H::COMMAND_REPLY_OTHER &&
            H::command_reply_verdict(tx, 0, reply, 32) == H::COMMAND_REPLY_OTHER,
        "a shorter or empty command: other");
  CHECK(H::command_reply_verdict(tx, 15, reply, 32) == H::COMMAND_REPLY_ACK, "a 15-byte command is enough");
  CHECK(H::command_reply_verdict(tx, 32, reply, 15) == H::COMMAND_REPLY_OTHER,
        "a 15-byte reply has no ack byte: other");
  CHECK(H::command_reply_verdict(tx, 32, reply, 16) == H::COMMAND_REPLY_ACK, "a 16-byte reply is enough");
  CHECK(H::command_reply_verdict(tx, 32, reply, 1) == H::COMMAND_REPLY_OTHER, "a 1-byte reply: other, not none");
  for (int ack : {0x00, 0x02, 0x81, 0xFF}) {
    reply[15] = (uint8_t) ack;
    CHECK(H::command_reply_verdict(tx, 32, reply, 32) == H::COMMAND_REPLY_NAK, "ack byte 0x%02X: nak", ack);
  }
  reply[15] = 0x01;
  reply[13] = 0x66;
  CHECK(H::command_reply_verdict(tx, 32, reply, 32) == H::COMMAND_REPLY_OTHER, "another class: other");
  reply[13] = 0x65;
  for (int echoed : {0x59, 0x5B, 0x00, 0xFF}) {
    reply[14] = (uint8_t) echoed;
    CHECK(H::command_reply_verdict(tx, 32, reply, 32) == H::COMMAND_REPLY_OTHER,
          "frame 14 is 0x%02X, not the echo: other", echoed);
  }
  // The verdict's numbers are what the hub logs.
  CHECK(H::COMMAND_REPLY_NONE == 0 && H::COMMAND_REPLY_ACK == 1 && H::COMMAND_REPLY_NAK == 2 &&
            H::COMMAND_REPLY_OTHER == 3,
        "none 0, ack 1, nak 2, other 3");
  reply[14] = 0x5A;
  // Only frames 13 and 14 are compared.
  reply[12] = 0x77;
  reply[16] = 0x77;
  CHECK(H::command_reply_verdict(tx, 32, reply, 32) == H::COMMAND_REPLY_ACK, "the bytes around them are not compared");
}

// view_diff one field at a time: each difference raises its own bit and no other, and the mask
// removes it (lines 638-639 and 646-647 were never run: swing and sleep).
static void test_view_diff() {
  printf("-- which fields differ, one at a time\n");
  const uint16_t all = 0x1FF;
  CHECK((H::CONFIRM_POWER | H::CONFIRM_MODE | H::CONFIRM_SETPOINT | H::CONFIRM_FAN | H::CONFIRM_SWING | H::CONFIRM_ECO |
         H::CONFIRM_TURBO | H::CONFIRM_MUTE | H::CONFIRM_SLEEP) == all,
        "nine fields, nine distinct bits");
  // What the structs hold before they are filled (mutants on the member initialisers).
  H::UnitView blank;
  CHECK(!blank.power_on && !blank.mode_valid && blank.mode == H::MODE_COOL && blank.setpoint_c == 0 &&
            blank.fan_raw == 0 && !blank.vswing && !blank.hswing && same(blank.special, NO_SPECIAL),
        "a new view: off, no mode, 0 C, fan byte 0, nothing special");
  CHECK(!NO_SPECIAL.eco && !NO_SPECIAL.turbo && !NO_SPECIAL.mute && NO_SPECIAL.sleep == 0,
        "a new special state: nothing on");
  H::CommandIntent none;
  CHECK(none.fields == 0 && none.known == 0 && !none.want.power_on, "a new intent asks for nothing");
  H::PendingCommand idle;
  CHECK(!idle.active && idle.before_known == 0 && idle.sent_ms == 0 && idle.resends == 0 && idle.intent.fields == 0,
        "a new pending slot is idle");
  H::PowerIntent pi;
  CHECK(!pi.pending && !pi.on, "a new power intent is not pending");
  CHECK(H::CONFIRM_WAIT == 0 && H::CONFIRM_DONE == 1 && H::CONFIRM_RESEND == 2 && H::CONFIRM_YIELD == 3 &&
            H::CONFIRM_GIVE_UP == 4,
        "the verdicts' numbers, as logged");
  CHECK(H::RESEND_COMBINED == 0 && H::RESEND_POWER_OFF == 1 && H::RESEND_POWER_ON_PAIR == 2 &&
            H::RESEND_POWER_ON_ALONE == 3,
        "the re-send kinds' numbers");
  CHECK(H::SPECIAL_OP_FEATURE == 0 && H::SPECIAL_OP_MUTE == 1 && H::SPECIAL_OP_SLEEP == 2,
        "the special-mode write kinds");

  H::UnitView a = view(true, H::MODE_COOL, 24, 0x0A);
  CHECK(H::view_diff(a, a, all) == 0, "a view does not differ from itself");
  struct {
    const char *what;
    uint16_t bit;
    void (*change)(H::UnitView *);
  } rows[] = {
      {"power", H::CONFIRM_POWER, [](H::UnitView *v) { v->power_on = false; }},
      {"mode", H::CONFIRM_MODE, [](H::UnitView *v) { v->mode = H::MODE_HEAT; }},
      {"setpoint", H::CONFIRM_SETPOINT, [](H::UnitView *v) { v->setpoint_c = 25; }},
      {"fan", H::CONFIRM_FAN, [](H::UnitView *v) { v->fan_raw = 0x0C; }},
      {"vertical swing", H::CONFIRM_SWING, [](H::UnitView *v) { v->vswing = true; }},
      {"horizontal swing", H::CONFIRM_SWING, [](H::UnitView *v) { v->hswing = true; }},
      {"eco", H::CONFIRM_ECO, [](H::UnitView *v) { v->special.eco = true; }},
      {"turbo", H::CONFIRM_TURBO, [](H::UnitView *v) { v->special.turbo = true; }},
      {"mute", H::CONFIRM_MUTE, [](H::UnitView *v) { v->special.mute = true; }},
      {"sleep", H::CONFIRM_SLEEP, [](H::UnitView *v) { v->special.sleep = 2; }},
  };
  for (const auto &r : rows) {
    H::UnitView b = a;
    r.change(&b);
    CHECK(H::view_diff(a, b, all) == r.bit && H::view_diff(b, a, all) == r.bit, "%s alone raises 0x%03X", r.what,
          r.bit);
    CHECK(H::view_diff(a, b, (uint16_t) (all & ~r.bit)) == 0, "%s masked out raises nothing", r.what);
    CHECK(H::view_diff(a, b, r.bit) == r.bit, "%s alone in the mask", r.what);
  }
  // Both swing axes at once is still the one swing bit, and swapped axes differ.
  H::UnitView b = a;
  b.vswing = b.hswing = true;
  CHECK(H::view_diff(a, b, all) == H::CONFIRM_SWING, "both axes: one bit");
  H::UnitView c = a, d = a;
  c.vswing = true;
  d.hswing = true;
  CHECK(H::view_diff(c, d, all) == H::CONFIRM_SWING, "vertical against horizontal differs");
  // A mode is compared only when both sides hold one (branch 3 of line 632).
  H::UnitView e = a;
  e.mode = H::MODE_DRY;
  e.mode_valid = false;
  CHECK(H::view_diff(a, e, all) == 0 && H::view_diff(e, a, all) == 0,
        "an unknown mode on either side is no difference");
  e.mode_valid = true;
  CHECK(H::view_diff(a, e, all) == H::CONFIRM_MODE, "known on both sides: it is");

  // unit_view_from_status, field by field.
  H::AcState st;
  st.power_on = true;
  st.mode = H::MODE_DRY;
  st.setpoint_c = 19;
  st.fan_raw = 0x10;
  st.vswing_on = true;
  st.eco_on = true;
  st.sleep_raw = 6;
  H::UnitView v = H::unit_view_from_status(st);
  CHECK(v.power_on && v.mode_valid && v.mode == H::MODE_DRY && v.setpoint_c == 19 && v.fan_raw == 0x10,
        "the plain fields");
  CHECK(v.vswing && !v.hswing && v.special.eco && !v.special.turbo && !v.special.mute && v.special.sleep == 3,
        "swing and special modes (sleep byte 6 is profile 3)");
  st = H::AcState{};
  st.hswing_on = true;
  st.turbo_on = true;
  st.mute_on = true;
  st.mode = (H::Mode) 7;
  v = H::unit_view_from_status(st);
  CHECK(!v.power_on && !v.vswing && v.hswing && !v.special.eco && v.special.turbo && v.special.mute &&
            v.special.sleep == 0,
        "and the other half");
  CHECK(!v.mode_valid, "status mode 7 is no mode");
}

// intent_from_command, intent_from_special and intent_to_command: the arms no test ran.
static void test_intents() {
  printf("-- intents: swing, the unknown kinds, and a fan with no command value\n");
  // Swing: only SWING counts as on. DIRECTION moves the louvre to a position and holds it.
  H::AcCommand c;
  c.vswing = H::SWING_MODE_SWING;
  c.hswing = H::SWING_MODE_DIRECTION;
  H::CommandIntent i = H::intent_from_command(c, H::CONFIRM_SWING);
  CHECK(i.fields == H::CONFIRM_SWING && i.want.vswing && !i.want.hswing, "vertical swing on, horizontal held");
  c.vswing = H::SWING_MODE_OFF;
  c.hswing = H::SWING_MODE_SWING;
  i = H::intent_from_command(c, H::CONFIRM_SWING);
  CHECK(!i.want.vswing && i.want.hswing, "and the other way round");
  // A value SwingMode does not have is not "swing" either.
  c.vswing = (H::SwingMode) 4;
  c.hswing = (H::SwingMode) 2;
  i = H::intent_from_command(c, H::CONFIRM_SWING);
  CHECK(!i.want.vswing && !i.want.hswing, "swing values past the enum are not checked as swinging");
  c.hswing = (H::SwingMode) 4;
  CHECK(!H::intent_from_command(c, H::CONFIRM_SWING).want.hswing, "horizontal 4 neither");
  c.vswing = H::SWING_MODE_OFF;
  c.hswing = H::SWING_MODE_SWING;
  i = H::intent_from_command(c, H::CONFIRM_SWING);
  CHECK(i.known == (H::CONFIRM_MODE | H::CONFIRM_SETPOINT | H::CONFIRM_SWING | H::CONFIRM_FAN),
        "a plain combined frame knows mode, setpoint, swing and (auto) fan, not power");
  CHECK(!i.want.power_on && i.want.fan_raw == 0x01, "auto fan is checked as status 0x01");
  // Fields the frame holds no value for are dropped from what is checked.
  i = H::intent_from_command(c, H::CONFIRM_ECO | H::CONFIRM_SLEEP | H::CONFIRM_MODE);
  CHECK(i.fields == H::CONFIRM_MODE, "eco and sleep are not a combined frame's to confirm");
  // A Celsius shadow goes into the intent as it is (the F arm is in test_esphome_confirm.cpp).
  c.setpoint = 27;
  c.fahrenheit = false;
  CHECK(H::intent_from_command(c, H::CONFIRM_SETPOINT).want.setpoint_c == 27, "C panel: setpoint as it is");
  c.setpoint = 77;  // 25 C
  c.fahrenheit = true;
  CHECK(H::intent_from_command(c, H::CONFIRM_SETPOINT).want.setpoint_c == 25, "F panel: 77 F is checked as 25 C");

  // intent_from_special: a kind past the enum asks for nothing (lines 728-729).
  i = H::intent_from_special({(H::SpecialOpKind) 3, 1});
  CHECK(i.fields == 0 && i.known == 0, "a write of no known kind asks for nothing");
  i = H::intent_from_special({H::SPECIAL_OP_FEATURE, 9});
  CHECK(i.fields == H::CONFIRM_TURBO && !i.want.special.turbo, "a byte 33 value past the enum is turbo off, as NONE");
  i = H::intent_from_special({H::SPECIAL_OP_MUTE, 0});
  CHECK(i.fields == H::CONFIRM_MUTE && i.known == H::CONFIRM_MUTE && !i.want.special.mute, "quiet off");
  i = H::intent_from_special({H::SPECIAL_OP_SLEEP, 0});
  CHECK(i.fields == H::CONFIRM_SLEEP && i.known == H::CONFIRM_SLEEP && i.want.special.sleep == 0, "sleep off");
  i = H::intent_from_special({H::SPECIAL_OP_FEATURE, H::FEATURE_ECO});
  CHECK(i.known == H::CONFIRM_ECO && !i.want.special.turbo && !i.want.special.mute, "eco on knows only eco");
  H::CommandIntent off = H::intent_power_off();
  CHECK(off.known == H::CONFIRM_POWER && off.fields == H::CONFIRM_POWER, "power off knows only power");

  // intent_to_command: the swing arm (lines 746-748) and each field on its own.
  H::CommandIntent s;
  s.fields = H::CONFIRM_SWING;
  s.want.vswing = true;
  s.want.hswing = false;
  s.want.mode = H::MODE_DRY;
  s.want.setpoint_c = 30;
  s.want.fan_raw = 0x12;
  H::AcCommand shadow;
  shadow.hswing = H::SWING_MODE_SWING;
  H::intent_to_command(s, false, &shadow);
  CHECK(shadow.vswing == H::SWING_MODE_SWING && shadow.hswing == H::SWING_MODE_OFF, "swing goes back into the shadow");
  CHECK(shadow.mode == H::MODE_COOL && shadow.setpoint == 24 && shadow.fan == H::FAN_SPEED_AUTO,
        "and nothing else does");
  s.want.vswing = false;
  s.want.hswing = true;
  H::intent_to_command(s, false, &shadow);
  CHECK(shadow.vswing == H::SWING_MODE_OFF && shadow.hswing == H::SWING_MODE_SWING, "the other axis");
  s.fields = 0;
  s.want.vswing = true;
  H::intent_to_command(s, false, &shadow);
  CHECK(shadow.vswing == H::SWING_MODE_OFF && shadow.hswing == H::SWING_MODE_SWING, "no field asked: nothing written");
  // A fan with no command value keeps the shadow's fan (branch 1 of line 743).
  s.fields = H::CONFIRM_FAN;
  s.want.fan_raw = 0x55;
  shadow.fan = H::FAN_SPEED_MID;
  H::intent_to_command(s, false, &shadow);
  CHECK(shadow.fan == H::FAN_SPEED_MID, "a status fan byte that is no speed leaves the shadow's fan");
  s.want.fan_raw = 0x01;
  H::intent_to_command(s, false, &shadow);
  CHECK(shadow.fan == H::FAN_SPEED_AUTO, "auto goes back as auto");
  // The setpoint goes back in the panel's unit.
  s.fields = H::CONFIRM_SETPOINT;
  s.want.setpoint_c = 25;
  H::intent_to_command(s, true, &shadow);
  CHECK(shadow.setpoint == 77 && shadow.fahrenheit, "25 C goes back to an F panel as 77 F");
  H::intent_to_command(s, false, &shadow);
  CHECK(shadow.setpoint == 25 && !shadow.fahrenheit, "and to a C panel as 25");
}

// intent_expected_fields, one excuse at a time (line 767 had five outcomes never taken).
static void test_expected_fields() {
  printf("-- what the unit can be expected to show, one excuse at a time\n");
  const uint16_t all = 0x1FF;
  H::CommandIntent i;
  i.fields = i.known = all;
  const uint16_t no_fan = (uint16_t) (all & ~H::CONFIRM_FAN);
  const uint16_t no_setpoint = (uint16_t) (all & ~H::CONFIRM_SETPOINT);
  const uint16_t turbo_left = (uint16_t) (all & ~(H::CONFIRM_MODE | H::CONFIRM_SETPOINT | H::CONFIRM_FAN));

  H::UnitView cool = view(true, H::MODE_COOL, 24, 0x01);
  CHECK(H::intent_expected_fields(i, cool, NO_SPECIAL) == all, "cool, nothing special: every field");
  CHECK(H::intent_expected_fields(i, view(true, H::MODE_HEAT, 24, 0x01), NO_SPECIAL) == all, "heat: every field");
  // Powered down: power alone, whatever else is true.
  H::UnitView off = view(false, H::MODE_COOL, 24, 0x01);
  CHECK(H::intent_expected_fields(i, off, NO_SPECIAL) == H::CONFIRM_POWER, "powered down: power only");
  H::CommandIntent no_power = i;
  no_power.fields = (uint16_t) (all & ~H::CONFIRM_POWER);
  CHECK(H::intent_expected_fields(no_power, off, NO_SPECIAL) == 0, "powered down and power not asked: nothing");

  // Each of the four fan pins on its own excuses the fan and only the fan.
  H::UnitView v = cool;
  v.special.mute = true;
  CHECK(H::intent_expected_fields(i, v, NO_SPECIAL) == no_fan, "quiet on now: the fan is the unit's");
  v = cool;
  v.special.sleep = 1;
  CHECK(H::intent_expected_fields(i, v, NO_SPECIAL) == no_fan, "a sleep profile on now: same");
  CHECK(H::intent_expected_fields(i, cool, special(false, false, true, 0)) == no_fan, "quiet on its way: same");
  CHECK(H::intent_expected_fields(i, cool, special(false, false, false, 4)) == no_fan, "a profile on its way: same");
  CHECK(H::intent_expected_fields(i, cool, special(true, false, false, 0)) == all, "eco pins nothing");
  v = cool;
  v.special.eco = true;
  CHECK(H::intent_expected_fields(i, v, NO_SPECIAL) == all, "now or on its way");
  // Turbo, now or on its way: mode, setpoint and fan.
  v = cool;
  v.special.turbo = true;
  CHECK(H::intent_expected_fields(i, v, NO_SPECIAL) == turbo_left, "turbo on now: mode, setpoint and fan excused");
  CHECK(H::intent_expected_fields(i, cool, special(false, true, false, 0)) == turbo_left, "turbo on its way: same");
  // A status mode that is no mode: the same three.
  v = cool;
  v.mode_valid = false;
  CHECK(H::intent_expected_fields(i, v, NO_SPECIAL) == turbo_left, "no mode in the status: same three");
  // Modes: the setpoint outside cool and heat, the fan in dry.
  CHECK(H::intent_expected_fields(i, view(true, H::MODE_AUTO, 24, 0x01), NO_SPECIAL) == no_setpoint,
        "auto: no setpoint");
  CHECK(H::intent_expected_fields(i, view(true, H::MODE_FAN, 24, 0x01), NO_SPECIAL) == no_setpoint,
        "fan-only: no setpoint");
  CHECK(
      H::intent_expected_fields(i, view(true, H::MODE_DRY, 24, 0x01), NO_SPECIAL) == (uint16_t) (no_setpoint & no_fan),
      "dry: neither setpoint nor fan");
  // Nothing is ever added to what was asked.
  H::CommandIntent only_fan;
  only_fan.fields = only_fan.known = H::CONFIRM_FAN;
  CHECK(H::intent_expected_fields(only_fan, cool, NO_SPECIAL) == H::CONFIRM_FAN, "only the fan asked: only the fan");
  CHECK(H::intent_expected_fields(only_fan, view(true, H::MODE_DRY, 24, 0x01), NO_SPECIAL) == 0, "in dry: nothing");
}

// pending_begin: what `before` keeps when a second command arrives (lines 819-824 were never
// run), and the fields that decide a later verdict.
static void test_pending() {
  printf("-- a second command while the first still waits\n");
  // First: a setpoint request, 24 -> 22, with the fan on low and no sleep profile.
  H::AcCommand c;
  c.setpoint = 22;
  H::UnitView at_first = view(true, H::MODE_COOL, 24, 0x0A);
  at_first.special.sleep = 1;
  H::PendingCommand p;
  H::pending_begin(&p, H::intent_from_command(c, H::CONFIRM_SETPOINT), at_first, 1000);
  CHECK(p.active && p.intent.fields == H::CONFIRM_SETPOINT && p.before_known == H::CONFIRM_SETPOINT &&
            p.sent_ms == 1000 && p.resends == 0,
        "the first command is waiting, with what the unit showed when it was asked");
  CHECK(p.before.setpoint_c == 24 && p.before.fan_raw == 0x0A && p.before.special.sleep == 1,
        "before: 24 C, low, profile 1");

  // Second: a swing request. The unit meanwhile reports 23 C: someone moved it.
  p.resends = 2;
  c.vswing = H::SWING_MODE_SWING;
  H::UnitView at_second = view(true, H::MODE_COOL, 23, 0x0C);
  at_second.special.sleep = 2;
  H::pending_begin(&p, H::intent_from_command(c, H::CONFIRM_SWING), at_second, 2000);
  CHECK(p.intent.fields == (H::CONFIRM_SETPOINT | H::CONFIRM_SWING), "both requests are now checked");
  CHECK(p.before_known == (H::CONFIRM_SETPOINT | H::CONFIRM_SWING), "and `before` is good for both");
  // Line 820: the setpoint's `before` is still the 24 the first request saw.
  CHECK(p.before.setpoint_c == 24, "the setpoint's before stays 24 (got %d)", p.before.setpoint_c);
  CHECK(p.before.fan_raw == 0x0C && p.before.special.sleep == 2, "fields not asked for take the newer status");
  CHECK(p.sent_ms == 2000 && p.resends == 0, "the clock and the re-send count start again");
  // What that buys: 23 is neither the 24 it was nor the 22 asked for, so the remote wins.
  uint16_t unmet = 0xFFFF;
  H::UnitView later = view(true, H::MODE_COOL, 23, 0x0C);
  later.vswing = true;
  CHECK(H::confirm_decision(p, later, NO_SPECIAL, 2000 + H::CONFIRM_SETTLE_MS, 2, &unmet) == H::CONFIRM_YIELD &&
            unmet == H::CONFIRM_SETPOINT,
        "a third setpoint value yields to whoever set it");

  // Lines 822 and 824: a fan and a sleep request keep their `before` the same way.
  H::PendingCommand q;
  H::CommandIntent fan_and_sleep;
  fan_and_sleep.fields = fan_and_sleep.known = H::CONFIRM_FAN | H::CONFIRM_SLEEP | H::CONFIRM_MODE;
  fan_and_sleep.want = view(true, H::MODE_HEAT, 24, 0x12);
  fan_and_sleep.want.special.sleep = 3;
  H::pending_begin(&q, fan_and_sleep, at_first, 1000);
  H::CommandIntent swing_only;
  swing_only.fields = swing_only.known = H::CONFIRM_SWING;
  swing_only.want.vswing = true;
  H::UnitView moved = view(true, H::MODE_DRY, 23, 0x0C);
  moved.special.sleep = 2;
  moved.mode_valid = false;
  H::pending_begin(&q, swing_only, moved, 2000);
  CHECK(q.before.fan_raw == 0x0A, "the fan's before stays low (got 0x%02X)", q.before.fan_raw);
  CHECK(q.before.special.sleep == 1, "the profile's before stays 1 (got %u)", q.before.special.sleep);
  CHECK(q.before.mode == H::MODE_COOL && q.before.mode_valid, "the mode's before stays cool, and valid");
  CHECK(q.before.setpoint_c == 23, "the setpoint was not asked for: it takes the newer status");
  // The newer command carries no value for the older fields, so only what it knows is checked.
  CHECK(q.intent.fields == H::CONFIRM_SWING && q.intent.known == H::CONFIRM_SWING,
        "fields the newer command holds no value for are dropped");

  // A field asked for again loses its `before` (the repeated mask).
  H::PendingCommand r;
  c = H::AcCommand{};
  c.setpoint = 22;
  H::pending_begin(&r, H::intent_from_command(c, H::CONFIRM_SETPOINT), view(true, H::MODE_COOL, 24, 0x01), 1000);
  c.setpoint = 20;
  H::pending_begin(&r, H::intent_from_command(c, H::CONFIRM_SETPOINT | H::CONFIRM_MODE),
                   view(true, H::MODE_COOL, 23, 0x01), 2000);
  CHECK(r.before_known == H::CONFIRM_MODE, "asked twice: the setpoint's before is no longer evidence");
  CHECK(r.intent.want.setpoint_c == 20, "and the newest value is the one checked");
  CHECK(H::confirm_decision(r, view(true, H::MODE_COOL, 23, 0x01), NO_SPECIAL, 2000 + H::CONFIRM_SETTLE_MS, 2,
                            nullptr) == H::CONFIRM_RESEND,
        "so a third value is a lost command, sent again");

  // Power: a power-on still waiting is kept by a command that does not mention power.
  H::PendingCommand w;
  c = H::AcCommand{};
  H::pending_begin(&w, H::intent_from_command(c, H::CONFIRM_POWER | H::CONFIRM_MODE),
                   view(false, H::MODE_COOL, 24, 0x01), 1000);
  CHECK(w.intent.want.power_on && (w.intent.fields & H::CONFIRM_POWER) != 0, "power-on is waiting");
  H::pending_begin(&w, H::intent_from_command(c, H::CONFIRM_SETPOINT), view(false, H::MODE_COOL, 24, 0x01), 1500);
  CHECK((w.intent.fields & H::CONFIRM_POWER) != 0 && (w.intent.known & H::CONFIRM_POWER) != 0 && w.intent.want.power_on,
        "a setpoint request keeps the power-on that is still waiting");
  CHECK(w.intent.fields == (H::CONFIRM_POWER | H::CONFIRM_MODE | H::CONFIRM_SETPOINT), "with everything else");
  // Switching off drops everything else, `before` included.
  H::pending_begin(&w, H::intent_power_off(), view(true, H::MODE_COOL, 24, 0x01), 3000);
  CHECK(
      w.active && w.intent.fields == H::CONFIRM_POWER && !w.intent.want.power_on && w.before_known == H::CONFIRM_POWER,
      "power off drops the rest");
  // Switching ON is not switching off: what was waiting is carried into the power-on command.
  H::PendingCommand on;
  c = H::AcCommand{};
  c.setpoint = 21;
  H::pending_begin(&on, H::intent_from_command(c, H::CONFIRM_SETPOINT), view(false, H::MODE_COOL, 24, 0x01), 1000);
  H::pending_begin(&on, H::intent_from_command(c, H::CONFIRM_POWER | H::CONFIRM_MODE),
                   view(false, H::MODE_COOL, 24, 0x01), 1500);
  CHECK(on.intent.fields == (H::CONFIRM_POWER | H::CONFIRM_MODE | H::CONFIRM_SETPOINT) && on.intent.want.power_on,
        "a power-on command keeps the setpoint request that was waiting");
  // A slot that is no longer waiting carries nothing over, whatever it still holds.
  H::PendingCommand stale;
  stale.active = false;
  stale.intent.fields = stale.intent.known = H::CONFIRM_FAN | H::CONFIRM_POWER;
  stale.intent.want.power_on = true;
  stale.before_known = H::CONFIRM_MODE;
  stale.before.mode = H::MODE_HEAT;
  stale.before.mode_valid = true;
  c = H::AcCommand{};
  H::pending_begin(&stale, H::intent_from_command(c, H::CONFIRM_SETPOINT), view(true, H::MODE_DRY, 24, 0x01), 5000);
  CHECK(stale.active && stale.intent.fields == H::CONFIRM_SETPOINT && stale.before_known == H::CONFIRM_SETPOINT,
        "a finished command's fields are not carried into the next one");
  CHECK(stale.before.mode == H::MODE_DRY && !stale.intent.want.power_on && (stale.intent.known & H::CONFIRM_POWER) == 0,
        "nor its `before`, nor its power request");
  // A command that asks for nothing checkable leaves nothing waiting.
  H::PendingCommand z;
  H::pending_begin(&z, H::CommandIntent{}, view(true, H::MODE_COOL, 24, 0x01), 1000);
  CHECK(!z.active && z.intent.fields == 0, "nothing asked: nothing waits");

  // pending_on_wire: only a waiting command's clock moves.
  H::pending_on_wire(&z, 9000);
  CHECK(z.sent_ms == 1000, "an idle slot keeps its time");
  H::pending_on_wire(&w, 9000);
  CHECK(w.sent_ms == 9000, "a waiting command counts from the wire");
}

// confirm_decision at each time and count boundary. CONFIRM_SETTLE_MS (3900) and
// CONFIRM_STALE_MS (20000) are the header's; the tests name them and their neighbours.
static void test_confirm_boundaries() {
  printf("-- the verdict, one millisecond and one re-send either side of each limit\n");
  CHECK(H::CONFIRM_SETTLE_MS == 3900 && H::CONFIRM_STALE_MS == 20000, "settle 3.9 s, stale 20 s");
  CHECK(H::COMMAND_RESEND_MAX == 2 && H::SPECIAL_RESEND_MAX == 1, "two re-sends, one for a special mode");
  CHECK(H::CONFIRM_YIELD_FIELDS == (H::CONFIRM_MODE | H::CONFIRM_SETPOINT), "mode and setpoint can yield");
  CHECK(H::SPECIAL_SETTLE_MS == 10000 && H::TELEMETRY_REFRESH_MS == 60000, "special-mode pacing 10 s, refresh 60 s");

  H::AcCommand c;
  c.setpoint = 22;
  const H::UnitView unmoved = view(true, H::MODE_COOL, 24, 0x01);
  const uint32_t t0 = 0xFFFFF000u;  // the clock wraps while this command waits
  H::PendingCommand p;
  H::pending_begin(&p, H::intent_from_command(c, H::CONFIRM_SETPOINT), unmoved, t0);
  uint16_t unmet = 0xFFFF;
  CHECK(H::confirm_decision(p, unmoved, NO_SPECIAL, t0, 2, &unmet) == H::CONFIRM_WAIT && unmet == 0,
        "at once: wait, and unmet is cleared");
  unmet = 0xFFFF;
  CHECK(H::confirm_decision(p, unmoved, NO_SPECIAL, t0 + 3899, 2, &unmet) == H::CONFIRM_WAIT && unmet == 0,
        "3899 ms: wait");
  CHECK(H::confirm_decision(p, unmoved, NO_SPECIAL, t0 + 3900, 2, &unmet) == H::CONFIRM_RESEND &&
            unmet == H::CONFIRM_SETPOINT,
        "3900 ms: judged, and the setpoint is unmet");
  CHECK(H::confirm_decision(p, unmoved, NO_SPECIAL, t0 + 19999, 2, nullptr) == H::CONFIRM_RESEND,
        "19999 ms: still re-sent");
  CHECK(H::confirm_decision(p, unmoved, NO_SPECIAL, t0 + 20000, 2, nullptr) == H::CONFIRM_GIVE_UP, "20000 ms: too old");
  CHECK(H::confirm_decision(p, unmoved, NO_SPECIAL, t0 + 20001, 2, nullptr) == H::CONFIRM_GIVE_UP, "and after");
  // The re-send budget.
  p.resends = 1;
  CHECK(H::confirm_decision(p, unmoved, NO_SPECIAL, t0 + 3900, 2, nullptr) == H::CONFIRM_RESEND,
        "one of two used: again");
  p.resends = 2;
  CHECK(H::confirm_decision(p, unmoved, NO_SPECIAL, t0 + 3900, 2, nullptr) == H::CONFIRM_GIVE_UP,
        "two of two used: give up");
  p.resends = 3;
  CHECK(H::confirm_decision(p, unmoved, NO_SPECIAL, t0 + 3900, 2, nullptr) == H::CONFIRM_GIVE_UP,
        "past the budget too");
  p.resends = 0;
  CHECK(H::confirm_decision(p, unmoved, NO_SPECIAL, t0 + 3900, 0, nullptr) == H::CONFIRM_GIVE_UP, "a budget of none");
  CHECK(H::confirm_decision(p, unmoved, NO_SPECIAL, t0 + 3900, 1, nullptr) == H::CONFIRM_RESEND, "a budget of one");
  // Met, yielded, and the order of the two: yield is decided before the age.
  unmet = 0xFFFF;
  CHECK(
      H::confirm_decision(p, view(true, H::MODE_COOL, 22, 0x01), NO_SPECIAL, t0 + 3900, 2, &unmet) == H::CONFIRM_DONE &&
          unmet == 0,
      "the unit shows 22: done");
  CHECK(
      H::confirm_decision(p, view(true, H::MODE_COOL, 22, 0x01), NO_SPECIAL, t0 + 25000, 2, nullptr) == H::CONFIRM_DONE,
      "done is done at any age");
  CHECK(
      H::confirm_decision(p, view(true, H::MODE_COOL, 26, 0x01), NO_SPECIAL, t0 + 3900, 2, nullptr) == H::CONFIRM_YIELD,
      "26, neither 24 nor 22: someone else moved it");
  CHECK(H::confirm_decision(p, view(true, H::MODE_COOL, 26, 0x01), NO_SPECIAL, t0 + 25000, 2, nullptr) ==
            H::CONFIRM_YIELD,
        "a yield is told from a stale command at any age");
  // Not waiting: done, with unmet cleared, whatever the status says.
  H::PendingCommand idle;
  unmet = 0xFFFF;
  CHECK(H::confirm_decision(idle, unmoved, NO_SPECIAL, 123456, 2, &unmet) == H::CONFIRM_DONE && unmet == 0,
        "nothing waiting: done");
  CHECK(H::confirm_decision(idle, unmoved, NO_SPECIAL, 123456, 2, nullptr) == H::CONFIRM_DONE, "also without unmet");
  // A fan that is neither what it was nor what was asked does not yield: the unit moves it alone.
  H::PendingCommand f;
  c = H::AcCommand{};
  c.fan = H::FAN_SPEED_HIGH;
  H::pending_begin(&f, H::intent_from_command(c, H::CONFIRM_FAN), view(true, H::MODE_COOL, 24, 0x0A), 0);
  CHECK(H::confirm_decision(f, view(true, H::MODE_COOL, 24, 0x0E), NO_SPECIAL, 3900, 2, nullptr) == H::CONFIRM_RESEND,
        "a third fan value is sent again, not yielded");

  // resend_frames, each of the four.
  H::CommandIntent on_and_mode;
  on_and_mode.fields = on_and_mode.known = H::CONFIRM_POWER | H::CONFIRM_MODE;
  on_and_mode.want.power_on = true;
  H::CommandIntent on_only;
  on_only.fields = on_only.known = H::CONFIRM_POWER;
  on_only.want.power_on = true;
  CHECK(H::resend_frames(on_and_mode, H::CONFIRM_MODE) == H::RESEND_COMBINED,
        "power met, mode not: the combined frame");
  CHECK(H::resend_frames(on_and_mode, 0) == H::RESEND_COMBINED, "nothing unmet: the combined frame");
  CHECK(H::resend_frames(on_and_mode, H::CONFIRM_POWER) == H::RESEND_POWER_ON_PAIR, "still off, mode asked: the pair");
  CHECK(H::resend_frames(on_and_mode, H::CONFIRM_POWER | H::CONFIRM_MODE) == H::RESEND_POWER_ON_PAIR,
        "both unmet: the pair");
  CHECK(H::resend_frames(on_only, H::CONFIRM_POWER) == H::RESEND_POWER_ON_ALONE,
        "still off, nothing else asked: alone");
  CHECK(H::resend_frames(H::intent_power_off(), H::CONFIRM_POWER) == H::RESEND_POWER_OFF, "still on: the off frame");
  H::CommandIntent off_and_more = H::intent_power_off();
  off_and_more.fields |= H::CONFIRM_MODE;
  CHECK(H::resend_frames(off_and_more, H::CONFIRM_POWER) == H::RESEND_POWER_OFF, "off wins over anything else asked");
}

// ---- Power estimate ----------------------------------------------------------------------------
// P[mW] = 4150 * raw^2 (the panel-meter calibration in the header), capped at 50 kW. The cap is
// first reached at raw 110: 4150 * 109^2 = 49 306 150, 4150 * 110^2 = 50 215 000.
static void test_power_estimate() {
  printf("-- power estimate, at zero, at the cap and at the top of the byte\n");
  CHECK(H::POWER_MW_PER_COUNT2 == 4150 && H::POWER_PF_PERMIL == 950 && H::POWER_MW_MAX == 50000000, "the calibration");
  CHECK(H::active_power_mw(0) == 0, "raw 0 is 0 W");
  CHECK(H::active_power_mw(1) == 4150, "raw 1 is 4.15 W");
  CHECK(H::active_power_mw(2) == 16600, "raw 2 is 16.6 W: the square, not the double");
  CHECK(H::active_power_mw(10) == 415000, "raw 10 is 415 W");
  CHECK(H::active_power_mw(29) == 3490150, "raw 29 is 3490.15 W");
  CHECK(H::active_power_mw(109) == 49306150, "raw 109 is the last value below the cap");
  CHECK(H::active_power_mw(110) == 50000000, "raw 110 is capped at 50 kW");
  CHECK(H::active_power_mw(255) == 50000000, "raw 255 too (269.9 kW uncapped)");
  for (int raw = 1; raw < 256; raw++)
    CHECK(H::active_power_mw((uint8_t) raw) >= H::active_power_mw((uint8_t) (raw - 1)) &&
              H::active_power_mw((uint8_t) raw) <= H::POWER_MW_MAX,
          "raw %d: never falls, never passes the cap", raw);

  CHECK(
      H::voltage_mv(0) == 0 && H::voltage_mv(1) == 1000 && H::voltage_mv(230) == 230000 && H::voltage_mv(255) == 255000,
      "byte 50 is whole volts");

  // I[mA] = P[mW] * 1000 / (V * 950).
  CHECK(H::active_current_ma(29, 0) == 0 && H::active_current_ma(255, 0) == 0, "voltage unknown: 0, no division");
  CHECK(H::active_current_ma(0, 230) == 0, "no power: no current");
  CHECK(H::active_current_ma(29, 230) == 15973, "3490.15 W at 230 V, pf 0.95: 15.973 A");
  CHECK(H::active_current_ma(10, 230) == 1899, "415 W at 230 V: 1.899 A");
  CHECK(H::active_current_ma(10, 1) == 436842, "at 1 V: the smallest divisor");
  CHECK(H::active_current_ma(255, 1) == 52631578, "the largest value the function returns");
  CHECK(H::active_current_ma(255, 255) == 206398, "capped power at 255 V");

  // energy_add and energy_mwh.
  uint64_t acc = 5;
  H::energy_add(&acc, 0, 1000);
  CHECK(acc == 5, "0 W adds nothing");
  H::energy_add(&acc, -1, 1000);
  CHECK(acc == 5, "negative power adds nothing (import only)");
  H::energy_add(&acc, 1, 1000);
  CHECK(acc == 1005, "1 mW for 1 s is 1000 mW ms");
  H::energy_add(&acc, 3, 0);
  CHECK(acc == 1005, "no time: nothing");
  acc = 0;
  H::energy_add(&acc, 50000000, 0xFFFFFFFFu);
  CHECK(acc == 50000000ULL * 0xFFFFFFFFULL, "50 kW for 49.7 days does not overflow");
  CHECK(H::energy_mwh(0) == 0 && H::energy_mwh(3599999) == 0 && H::energy_mwh(3600000) == 1 &&
            H::energy_mwh(7199999) == 1,
        "3 600 000 mW ms to the mWh, rounded down");
  acc = 0;
  for (int s = 0; s < 3600; s++)
    H::energy_add(&acc, 1000000, 1000);
  CHECK(H::energy_mwh(acc) == 1000000, "1 kW for an hour is 1 kWh");
}

// ---- Telemetry pacing and the outdoor placeholder ----------------------------------------------
static void test_telemetry() {
  printf("-- when a sensor value goes out\n");
  CHECK(H::telemetry_publish_due(false, 1.0f, 1.0f, false), "the first value always");
  CHECK(H::telemetry_publish_due(true, 1.0f, 1.0f, true), "a refresh always");
  CHECK(!H::telemetry_publish_due(true, 1.0f, 1.0f, false), "an unchanged value does not");
  CHECK(H::telemetry_publish_due(true, 1.0f, 1.5f, false), "a changed one does");
  CHECK(H::telemetry_publish_due(true, 1.5f, 1.0f, false), "a falling one too");
  CHECK(H::telemetry_publish_due(true, NAN, 1.0f, false), "unknown becoming known does");
  CHECK(H::telemetry_publish_due(true, 1.0f, NAN, false), "known becoming unknown does");
  CHECK(!H::telemetry_publish_due(true, NAN, NAN, false), "unknown staying unknown does not");
  CHECK(!H::telemetry_publish_due(true, 0.0f, -0.0f, false), "0 and -0 are the same reading");

  // Only the pair 0xEC, 0xEC (-20 and -20 as int8) is the placeholder: one byte either side of
  // it, on either sensor, is a reading. 0xEC is OUTDOOR_TEMP_PLACEHOLDER_RAW (one unit,
  // 2026-10-10, see the header).
  CHECK(H::OUTDOOR_TEMP_PLACEHOLDER_RAW == 0xEC, "the placeholder byte");
  for (int outdoor : {-21, -20, -19}) {
    for (int coil : {-21, -20, -19}) {
      H::OutdoorTempGate g;
      const bool placeholder = outdoor == -20 && coil == -20;
      CHECK(H::outdoor_temps_measured(&g, (int8_t) outdoor, (int8_t) coil) == !placeholder,
            "outdoor %d, coil %d at start is %s", outdoor, coil, placeholder ? "unknown" : "a reading");
      CHECK(g.seen_real == !placeholder, "and the gate remembers it");
    }
  }
  H::OutdoorTempGate g;
  g.seen_real = true;
  H::outdoor_temp_gate_reset(&g);
  CHECK(!g.seen_real, "a link drop closes the gate");
}

// ---- Serial port restart policy (w41h1_uart_policy.h) ------------------------------------------
// Mutants `/*w->was_up = true;*/` (line 31) and `if (0==1)` for `if (w->was_up)` (line 37) passed
// the existing test: every loss it stages comes at least one retry period after time 0, where the
// periodic rule fires at the same look as the "just dropped" rule. Here the loss comes 2 s in.
static void test_uart_policy() {
  printf("-- serial port restart: a link lost inside the first retry period\n");
  CHECK(W41H1_UART_RETRY_MS == 15000, "retry every 15 s");
  W41H1UartWatch w;
  CHECK(!w.started && !w.was_up && w.last_ms == 0 && w.restarts == 0, "a new watch is idle");
  CHECK(!w41h1_uart_restart_due(&w, true, false, 1000), "link up at 1 s: nothing");
  CHECK(w.started && w.was_up && w.restarts == 0, "the watch has seen the link");
  CHECK(w41h1_uart_restart_due(&w, false, false, 2000), "link lost at 2 s: restart at once");
  CHECK(w.restarts == 1 && w.last_ms == 2000 && !w.was_up, "counted, timed from the loss, and no longer up");
  CHECK(!w41h1_uart_restart_due(&w, false, false, 3000), "the next look: not again");
  CHECK(!w41h1_uart_restart_due(&w, false, false, 16999), "14.999 s after the restart: not yet");
  CHECK(w41h1_uart_restart_due(&w, false, false, 17000), "15 s after it: again");
  CHECK(w.restarts == 2 && w.last_ms == 17000, "counted, and the period restarts");

  // Never up: the first look only starts the clock, and does not count as a restart.
  W41H1UartWatch n;
  CHECK(!w41h1_uart_restart_due(&n, false, false, 500), "first look, link down: nothing");
  CHECK(n.started && n.last_ms == 500 && n.restarts == 0 && !n.was_up, "the clock runs from the first look");
  CHECK(!w41h1_uart_restart_due(&n, false, false, 15499), "one millisecond short of a period: nothing");
  CHECK(w41h1_uart_restart_due(&n, false, false, 15500), "a full period: restart");
  CHECK(n.restarts == 1, "counted once");

  // Transmitting at the look that finds the loss: the watch stays "up", so the restart is still
  // the immediate kind at the next look, however soon.
  W41H1UartWatch t;
  w41h1_uart_restart_due(&t, true, false, 1000);
  CHECK(!w41h1_uart_restart_due(&t, false, true, 2000) && t.was_up && t.restarts == 0 && t.last_ms == 0,
        "DE high: nothing changes in the watch");
  CHECK(w41h1_uart_restart_due(&t, false, false, 2001), "DE low a millisecond later: the restart that was due");
  CHECK(t.restarts == 1 && t.last_ms == 2001, "counted then");
  // A first look while transmitting does not start the clock either.
  W41H1UartWatch u;
  CHECK(!w41h1_uart_restart_due(&u, false, true, 100) && !u.started && u.last_ms == 0,
        "boot, DE high: clock not started");
}

int main() {
  printf("== ESPHome map and serial policy: coverage and mutation gaps ==\n");
  test_mode_enums();
  test_action();
  test_swing();
  test_fan_ladder();
  test_setpoint();
  test_fahrenheit_shadow_round_trip();
  test_feature();
  test_presets();
  test_special_plan();
  test_fan_pin();
  test_resend_and_reply();
  test_view_diff();
  test_intents();
  test_expected_fields();
  test_pending();
  test_confirm_boundaries();
  test_power_estimate();
  test_telemetry();
  test_uart_policy();
  printf("  %d checks, %d failed, %d known defects\n", g_checks, g_fail, g_known);
  printf(g_fail ? "== MAP GAPS FAILED ==\n" : "== MAP GAPS OK ==\n");
  return g_fail ? 1 : 0;
}
