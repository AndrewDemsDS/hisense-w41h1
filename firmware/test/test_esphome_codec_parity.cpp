// Parity: the ESPHome-style codec port (firmware/esphome/components/hisense_ac/hisense_protocol.*,
// hisense_map.h) against the original driver (firmware/src/rs485-driver/). The port exists because
// ESPHome's rules and the AmebaZ2 build cannot share one file; this test is what makes a second copy
// safe. Every builder and parser runs on both sides over the same inputs: exhaustive sweeps where the
// domain is small, the full HisenseCommand cartesian product, and seeded random frames for the
// parsers. Outputs are compared byte for byte and field by field, never with memcmp over structs.
//
// A protocol change in the original fails here until it is ported. The original's static helpers
// (link heartbeat, frame reassembler) cannot be called from outside, so those two are checked
// against the captured templates and hand-built streams instead.

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "hisense_rs485.h"
}
#include "esphome_aircon_map.h"
#include "power_estimate.h"

#include "hisense_map.h"
#include "hisense_protocol.h"

namespace H = esphome::hisense_ac;

static int g_fail = 0;
static long g_checks = 0;
#define CHECK(cond, ...) \
  do { \
    g_checks++; \
    if (!(cond)) { \
      if (g_fail < 25) { \
        printf("  FAIL %s:%d ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
      } \
      g_fail++; \
    } \
  } while (0)

// Deterministic PRNG (xorshift32) so a failure reproduces.
static uint32_t g_rng = 0x1234567u;
static uint32_t rnd() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 17;
  g_rng ^= g_rng << 5;
  return g_rng;
}

static bool same_bytes(const uint8_t *a, size_t na, const uint8_t *b, size_t nb) {
  return na == nb && memcmp(a, b, na) == 0;
}

static void to_old(const H::AcCommand &n, HisenseCommand *o) {
  memset(o, 0, sizeof(*o));
  o->mode = (HisenseMode) n.mode;
  o->setpoint = n.setpoint;
  o->fahrenheit = n.fahrenheit;
  o->fan = (HisenseFanSpeed) n.fan;
  o->vswing = (HisenseSwingMode) n.vswing;
  o->hswing = (HisenseSwingMode) n.hswing;
  o->feature = (HisenseFeature) n.feature;
  o->display = (HisenseDisplay) n.display;
}

static bool same_state(const HisenseState &o, const H::AcState &n) {
  return o.valid == n.valid && o.power_on == n.power_on && (int) o.mode == (int) n.mode &&
         o.temp_unit_f == n.temp_unit_f && o.indoor_temp_c == n.indoor_temp_c && o.setpoint_c == n.setpoint_c &&
         o.fan_raw == n.fan_raw && o.vswing_on == n.vswing_on && o.turbo_on == n.turbo_on && o.eco_on == n.eco_on &&
         o.hswing_on == n.hswing_on && o.heat_relay_on == n.heat_relay_on && o.mute_on == n.mute_on &&
         o.sleep_on == n.sleep_on && o.sleep_raw == n.sleep_raw && o.purify_on == n.purify_on &&
         o.outdoor_temp_c == n.outdoor_temp_c && o.coil_temp_c == n.coil_temp_c &&
         o.compressor_freq == n.compressor_freq && o.current_raw == n.current_raw && o.voltage_raw == n.voltage_raw;
}

static bool same_features(const HisenseFeatures &o, const H::AcFeatures &n) {
  return o.valid == n.valid && o.cool_heat == n.cool_heat && o.ai == n.ai && o.infinite_fan == n.infinite_fan &&
         o.power_save == n.power_save && o.fan_mute == n.fan_mute && o.swing_dir_8 == n.swing_dir_8 &&
         o.swing_follow == n.swing_follow && o.power_display == n.power_display && o.demand_resp == n.demand_resp &&
         o.humidity == n.humidity && o.heat_8c == n.heat_8c && o.purify == n.purify && o.ext_valid == n.ext_valid &&
         o.q_display == n.q_display && o.enable_8heat == n.enable_8heat && o.trans_102_64 == n.trans_102_64 &&
         o.reply_len == n.reply_len;
}

static bool same_faults(const HisenseFaults &o, const H::AcFaults &n) {
  return o.valid == n.valid && o.any == n.any && o.raw_indoor == n.raw_indoor && o.raw_module == n.raw_module &&
         o.raw_outdoor == n.raw_outdoor && o.raw_protect == n.raw_protect && o.in_temp == n.in_temp &&
         o.in_coil_temp == n.in_coil_temp && o.in_humidity == n.in_humidity && o.water_full == n.water_full &&
         o.in_fan_motor == n.in_fan_motor && o.grille == n.grille && o.in_vzero == n.in_vzero && o.in_com == n.in_com &&
         o.in_display == n.in_display && o.in_keys == n.in_keys && o.in_wifi == n.in_wifi && o.in_ele == n.in_ele &&
         o.in_eeprom == n.in_eeprom && o.out_eeprom == n.out_eeprom && o.out_coil_temp == n.out_coil_temp &&
         o.out_gas_temp == n.out_gas_temp && o.out_temp == n.out_temp && o.over_temp == n.over_temp;
}

// A random, well-formed A/C -> module frame of `len` bytes with a valid LEN byte and checksum.
static void random_frame(uint8_t *f, size_t len, uint8_t cls, uint8_t sub) {
  for (size_t i = 0; i < len; i++)
    f[i] = (uint8_t) rnd();
  f[0] = 0xF4;
  f[1] = 0xF5;
  f[2] = 0x01;
  f[3] = 0x40;
  f[4] = (uint8_t) (len - 9);
  f[13] = cls;
  f[14] = sub;
  uint32_t sum = 0;
  for (size_t i = 2; i < len - 4; i++)
    sum += f[i];
  f[len - 4] = (uint8_t) (sum >> 8);
  f[len - 3] = (uint8_t) sum;
  f[len - 2] = 0xF4;
  f[len - 1] = 0xFB;
}

static void test_constants() {
  CHECK(H::CMD_FRAME_LEN == HISENSE_CMD_FRAME_LEN && H::CMD_HEADER_LEN == HISENSE_CMD_HEADER_LEN &&
            H::CMD_CHK_OFFSET == HISENSE_CMD_CHK_OFFSET && H::CMD_END_OFFSET == HISENSE_CMD_END_OFFSET,
        "command frame geometry");
  CHECK(same_bytes(H::CMD_HEADER, H::CMD_HEADER_LEN, HISENSE_CMD_HEADER, HISENSE_CMD_HEADER_LEN), "command header");
  CHECK(same_bytes(H::STATUS_REQUEST, H::STATUS_REQUEST_LEN, HISENSE_STATUS_REQUEST, HISENSE_STATUS_REQUEST_LEN),
        "status request");
  CHECK(same_bytes(H::LINK_INIT_0A, H::LINK_INIT_0A_LEN, HISENSE_LINK_INIT_0A, HISENSE_LINK_INIT_0A_LEN), "link 0A");
  CHECK(same_bytes(H::LINK_INIT_07, H::LINK_INIT_07_LEN, HISENSE_LINK_INIT_07, HISENSE_LINK_INIT_07_LEN), "link 07");
  CHECK(same_bytes(H::LINK_HEARTBEAT, H::LINK_HEARTBEAT_LEN, HISENSE_LINK_HEARTBEAT, HISENSE_LINK_HEARTBEAT_LEN),
        "link heartbeat template");
  CHECK(H::BUS_BAUD_RATE == HISENSE_UART_BAUD, "baud");
  CHECK(H::SETPOINT_MIN_C == HISENSE_SETPOINT_MIN_C && H::SETPOINT_MAX_C == HISENSE_SETPOINT_MAX_C &&
            H::SETPOINT_MIN_F == HISENSE_SETPOINT_MIN_F && H::SETPOINT_MAX_F == HISENSE_SETPOINT_MAX_F,
        "setpoint ranges");
  CHECK(H::FAULT_BYTE_INDOOR == HISENSE_FAULT_BYTE_INDOOR && H::FAULT_BYTE_MODULE == HISENSE_FAULT_BYTE_MODULE &&
            H::FAULT_BYTE_OUTDOOR == HISENSE_FAULT_BYTE_OUTDOOR && H::FAULT_BYTE_PROTECT == HISENSE_FAULT_BYTE_PROTECT,
        "fault byte offsets");
  CHECK(H::SPECIAL_SETTLE_MS == HISENSE_SPECIAL_SETTLE_MS, "special-mode settle");
  CHECK(H::PRESET_COUNT == ESPHOME_PRESET_COUNT && H::PRESET_FIRST_CUSTOM == ESPHOME_PRESET_FIRST_CUSTOM,
        "preset table size");
  for (size_t i = 0; i < H::PRESET_COUNT; i++) {
    const EsphomePresetRow &o = k_esphome_presets[i];
    const H::PresetRow &n = H::PRESETS[i];
    CHECK(strcmp(o.name, n.name) == 0 && o.eco == n.eco && o.turbo == n.turbo && o.mute == n.mute && o.sleep == n.sleep,
          "preset row %zu", i);
  }
  CHECK(H::FAN_TABLE_LEN == HISENSE_FAN_TABLE_LEN, "fan table size");
  for (size_t i = 0; i < H::FAN_TABLE_LEN; i++) {
    CHECK(k_hisense_fan_table[i].raw == H::FAN_TABLE[i].raw && k_hisense_fan_table[i].speed == H::FAN_TABLE[i].speed &&
              k_hisense_fan_table[i].percent == H::FAN_TABLE[i].percent &&
              (int) k_hisense_fan_table[i].cmd == (int) H::FAN_TABLE[i].cmd,
          "fan row %zu", i);
  }
  // Enum values are wire arithmetic (mode * 2 + 1, fan * 2 + 1), so they must match too.
  CHECK((int) H::MODE_FAN == (int) HISENSE_MODE_FAN && (int) H::MODE_HEAT == (int) HISENSE_MODE_HEAT &&
            (int) H::MODE_COOL == (int) HISENSE_MODE_COOL && (int) H::MODE_DRY == (int) HISENSE_MODE_DRY &&
            (int) H::MODE_AUTO == (int) HISENSE_MODE_AUTO,
        "mode enum");
  CHECK((int) H::FAN_SPEED_AUTO == (int) HISENSE_FAN_AUTO && (int) H::FAN_SPEED_QUIET == (int) HISENSE_FAN_QUIET &&
            (int) H::FAN_SPEED_LOW == (int) HISENSE_FAN_LOW &&
            (int) H::FAN_SPEED_MED_LOW == (int) HISENSE_FAN_MED_LOW &&
            (int) H::FAN_SPEED_MID == (int) HISENSE_FAN_MID &&
            (int) H::FAN_SPEED_MED_HIGH == (int) HISENSE_FAN_MED_HIGH &&
            (int) H::FAN_SPEED_HIGH == (int) HISENSE_FAN_HIGH &&
            (int) H::FAN_SPEED_NOCHANGE == (int) HISENSE_FAN_NOCHANGE,
        "fan enum");
}

static void test_build_command() {
  static const uint8_t modes[] = {0, 1, 2, 3, 4};
  static const uint8_t fans[] = {0, 1, 2, 5, 6, 7, 8, 9, 0xFF};
  static const uint8_t swings[] = {0, 1, 3};
  long built = 0;
  for (uint8_t mode : modes)
    for (int unit = 0; unit < 2; unit++)
      for (int sp = unit ? 55 : 10; sp <= (unit ? 95 : 36); sp++)
        for (uint8_t fan : fans)
          for (uint8_t vs : swings)
            for (uint8_t hs : swings)
              for (uint8_t feat = 0; feat < 4; feat++)
                for (uint8_t disp = 0; disp < 3; disp++) {
                  H::AcCommand n;
                  n.mode = (H::Mode) mode;
                  n.setpoint = (int8_t) sp;
                  n.fahrenheit = unit != 0;
                  n.fan = (H::FanSpeed) fan;
                  n.vswing = (H::SwingMode) vs;
                  n.hswing = (H::SwingMode) hs;
                  n.feature = (H::Feature) feat;
                  n.display = (H::Display) disp;
                  HisenseCommand o;
                  to_old(n, &o);
                  uint8_t a[64], b[64];
                  size_t na = hisense_build_command(&o, a, sizeof(a));
                  size_t nb = H::build_command(n, b, sizeof(b));
                  CHECK(same_bytes(a, na, b, nb), "build_command mode=%u sp=%d f=%d fan=%u vs=%u hs=%u feat=%u disp=%u",
                        mode, sp, unit, fan, vs, hs, feat, disp);
                  built++;
                }
  // Capacity: one short of the stuffed maximum must be refused by both.
  H::AcCommand n;
  HisenseCommand o;
  to_old(n, &o);
  uint8_t a[64], b[64];
  for (size_t cap = 0; cap < 56; cap++) {
    CHECK(hisense_build_command(&o, a, cap) == H::build_command(n, b, cap), "build_command cap %zu", cap);
  }
  printf("  build_command: %ld commands compared\n", built);
}

static void test_overrides() {
  H::AcCommand n;
  n.mode = H::MODE_HEAT;
  n.setpoint = 23;
  n.fan = H::FAN_SPEED_MID;
  HisenseCommand o;
  to_old(n, &o);
  static const uint8_t vals[] = {0x00, 0x01, 0x30, 0x7F, 0xF4, 0xFF};
  for (int off = -2; off < 60; off++) {
    for (uint8_t v : vals) {
      uint8_t a[64], b[64];
      size_t na = hisense_build_command_override(&o, a, sizeof(a), off, v);
      size_t nb = H::build_command_override(n, b, sizeof(b), off, v);
      CHECK(same_bytes(a, na, b, nb), "override off=%d v=0x%02X", off, v);
    }
  }
  for (int o1 = 10; o1 < 52; o1 += 3) {
    for (int o2 = 10; o2 < 52; o2++) {
      uint8_t a[64], b[64];
      size_t na = hisense_build_command_override2(&o, a, sizeof(a), o1, 0x5A, o2, 0xF4);
      size_t nb = H::build_command_override(n, b, sizeof(b), o1, 0x5A, o2, 0xF4);
      CHECK(same_bytes(a, na, b, nb), "override2 %d %d", o1, o2);
    }
  }
  // The two-byte override must not leak into a later plain command (the original uses a global).
  uint8_t a[64], b[64];
  size_t na = hisense_build_command(&o, a, sizeof(a));
  size_t nb = H::build_command(n, b, sizeof(b));
  CHECK(same_bytes(a, na, b, nb), "plain command after override2");
}

static void test_fixed_builders() {
  uint8_t a[64], b[64];
  for (int on = 0; on < 2; on++) {
    for (size_t cap = 0; cap < 56; cap++) {
      size_t na = hisense_build_power_frame(on != 0, a, cap);
      size_t nb = H::build_power_frame(on != 0, b, cap);
      CHECK(same_bytes(a, na, b, nb), "power on=%d cap=%zu", on, cap);
    }
    size_t na = hisense_build_mute_frame(on != 0, a, sizeof(a));
    size_t nb = H::build_mute_frame(on != 0, b, sizeof(b));
    CHECK(same_bytes(a, na, b, nb), "mute %d", on);
  }
  for (int p = 0; p < 256; p++) {
    size_t na = hisense_build_sleep_frame((uint8_t) p, a, sizeof(a));
    size_t nb = H::build_sleep_frame((uint8_t) p, b, sizeof(b));
    CHECK(same_bytes(a, na, b, nb), "sleep profile %d", p);
  }
  for (size_t cap = 0; cap < 56; cap++) {
    CHECK(hisense_build_mute_frame(true, a, cap) == H::build_mute_frame(true, b, cap), "mute cap %zu", cap);
    size_t na = hisense_build_producttype_request(a, cap);
    size_t nb = H::build_producttype_request(b, cap);
    CHECK(same_bytes(a, na, b, nb), "producttype cap %zu", cap);
  }
}

static void test_link_heartbeat() {
  // The original builder is static; its "heard" output is exactly the captured template, which
  // carries B0 at byte 16 and a valid checksum.
  uint8_t b[64];
  size_t nb = H::build_link_heartbeat(true, b, sizeof(b));
  CHECK(same_bytes(b, nb, HISENSE_LINK_HEARTBEAT, HISENSE_LINK_HEARTBEAT_LEN), "heartbeat (heard) == capture");
  nb = H::build_link_heartbeat(false, b, sizeof(b));
  CHECK(nb == HISENSE_LINK_HEARTBEAT_LEN && b[16] == 0xF0 && hisense_status_checksum_ok(b, nb),
        "heartbeat (not heard): byte16 0xF0, checksum valid");
  CHECK(H::build_link_heartbeat(true, b, HISENSE_LINK_HEARTBEAT_LEN + 1) == 0, "heartbeat refuses a short buffer");
}

static void test_stamp_link_token() {
  const uint8_t *frames[] = {HISENSE_STATUS_REQUEST, HISENSE_LINK_INIT_0A, HISENSE_LINK_INIT_07,
                             HISENSE_LINK_HEARTBEAT};
  const size_t lens[] = {HISENSE_STATUS_REQUEST_LEN, HISENSE_LINK_INIT_0A_LEN, HISENSE_LINK_INIT_07_LEN,
                         HISENSE_LINK_HEARTBEAT_LEN};
  uint8_t cmd[64];
  HisenseCommand o;
  H::AcCommand n;
  to_old(n, &o);
  size_t ncmd = hisense_build_command(&o, cmd, sizeof(cmd));
  for (int f = 0; f < 5; f++) {
    const uint8_t *in = f < 4 ? frames[f] : cmd;
    size_t len = f < 4 ? lens[f] : ncmd;
    for (int hi = 0; hi < 256; hi += 5) {
      for (int lo = 0; lo < 256; lo += 7) {
        uint8_t a[80], b[80];
        size_t na = hisense_stamp_link_token(in, len, (uint8_t) hi, (uint8_t) lo, a, sizeof(a));
        size_t nb = H::stamp_link_token(in, len, (uint8_t) hi, (uint8_t) lo, b, sizeof(b));
        CHECK(same_bytes(a, na, b, nb), "stamp frame %d hi=%d lo=%d", f, hi, lo);
      }
    }
    for (size_t l = 0; l <= len + 2; l++) {
      uint8_t a[80], b[80];
      CHECK(hisense_stamp_link_token(in, l, 1, 1, a, sizeof(a)) == H::stamp_link_token(in, l, 1, 1, b, sizeof(b)),
            "stamp frame %d len %zu", f, l);
    }
  }
  // Random garbage, including a bad start marker and oversize LEN.
  for (int i = 0; i < 20000; i++) {
    uint8_t in[80], a[80], b[80];
    for (auto &x : in)
      x = (uint8_t) rnd();
    if (i & 1) {
      in[0] = 0xF4;
      in[1] = 0xF5;
    }
    size_t len = rnd() % 80;
    size_t cap = rnd() % 80;
    size_t na = hisense_stamp_link_token(in, len, 2, 3, a, cap);
    size_t nb = H::stamp_link_token(in, len, 2, 3, b, cap);
    CHECK(same_bytes(a, na, b, nb), "stamp random %d", i);
  }
}

static void test_parsers() {
  // Random well-formed status frames, lengths around the real 160, plus deliberately broken ones.
  for (int i = 0; i < 60000; i++) {
    uint8_t f[256];
    size_t len = 30 + rnd() % 180;
    random_frame(f, len, 0x66, (uint8_t) (i % 3 == 0 ? 0x40 : 0x00));
    int breakage = rnd() % 8;
    if (breakage == 1)
      f[rnd() % len] ^= (uint8_t) (1u << (rnd() % 8));
    if (breakage == 2)
      f[4]++;
    if (breakage == 3)
      f[len - 1] = 0x00;
    if (breakage == 4)
      f[1] = 0x00;
    size_t plen = (breakage == 5) ? len - 1 : len;

    HisenseState os;
    H::AcState ns;
    memset(&os, 0xA5, sizeof(os));
    bool ro = hisense_parse_status(f, plen, &os);
    bool rn = H::parse_status(f, plen, &ns);
    CHECK(ro == rn, "parse_status result, frame %d", i);
    if (ro && rn) {
      CHECK(same_state(os, ns), "parse_status fields, frame %d", i);
    }

    HisenseFeatures ofe;
    H::AcFeatures nfe;
    memset(&ofe, 0, sizeof(ofe));
    bool fo = hisense_parse_features(f, plen, &ofe);
    bool fn = H::parse_features(f, plen, &nfe);
    CHECK(fo == fn, "parse_features result, frame %d", i);
    if (fo && fn) {
      CHECK(same_features(ofe, nfe), "parse_features fields, frame %d", i);
    }

    HisenseFaults ofa;
    H::AcFaults nfa;
    bool ao = hisense_parse_faults(f, plen, &ofa);
    bool an = H::parse_faults(f, plen, &nfa);
    CHECK(ao == an && same_faults(ofa, nfa), "parse_faults, frame %d", i);

    CHECK(hisense_status_checksum_ok(f, plen) == H::status_checksum_ok(f, plen), "checksum_ok, frame %d", i);

    uint8_t oh = 0, ol = 0, nh = 0, nl = 0;
    f[13] = (i % 2) ? 0x0A : f[13];
    bool dvo = hisense_devtype_from_reply(f, plen, &oh, &ol);
    bool dvn = H::devtype_from_reply(f, plen, &nh, &nl);
    CHECK(dvo == dvn && oh == nh && ol == nl, "devtype, frame %d", i);
  }
  // Fault parsing gates each group on length separately, so walk every length.
  uint8_t f[80];
  for (auto &x : f)
    x = (uint8_t) rnd();
  for (size_t len = 0; len < sizeof(f); len++) {
    HisenseFaults o;
    H::AcFaults n;
    bool ro = hisense_parse_faults(f, len, &o);
    bool rn = H::parse_faults(f, len, &n);
    CHECK(ro == rn && same_faults(o, n), "parse_faults len %zu", len);
  }
}

static void test_bitmaps() {
  for (int i = 0; i < 20000; i++) {
    uint32_t w = rnd();
    HisenseFeatures o;
    memset(&o, 0, sizeof(o));
    hisense_features_from_bitmap32(w, &o);
    H::AcFeatures n = H::features_from_bitmap32(w);
    CHECK(same_features(o, n), "features_from_bitmap32 0x%08X", (unsigned) w);
    CHECK(hisense_features_to_bitmap32(&o) == H::features_to_bitmap32(n), "features_to_bitmap32 0x%08X", (unsigned) w);

    HisenseFaults fo;
    memset(&fo, 0, sizeof(fo));
    H::AcFaults fn;
    bool *ob[] = {&fo.in_temp,      &fo.in_coil_temp, &fo.in_humidity, &fo.water_full, &fo.in_fan_motor,
                  &fo.grille,       &fo.in_vzero,     &fo.in_com,      &fo.in_display, &fo.in_keys,
                  &fo.in_wifi,      &fo.in_ele,       &fo.in_eeprom,   &fo.out_eeprom, &fo.out_coil_temp,
                  &fo.out_gas_temp, &fo.out_temp,     &fo.over_temp,   &fo.any,        &fo.valid};
    bool *nb[] = {&fn.in_temp,      &fn.in_coil_temp, &fn.in_humidity, &fn.water_full, &fn.in_fan_motor,
                  &fn.grille,       &fn.in_vzero,     &fn.in_com,      &fn.in_display, &fn.in_keys,
                  &fn.in_wifi,      &fn.in_ele,       &fn.in_eeprom,   &fn.out_eeprom, &fn.out_coil_temp,
                  &fn.out_gas_temp, &fn.out_temp,     &fn.over_temp,   &fn.any,        &fn.valid};
    uint32_t bits = rnd();
    for (size_t k = 0; k < sizeof(ob) / sizeof(ob[0]); k++)
      *ob[k] = *nb[k] = ((bits >> k) & 1u) != 0;
    CHECK(hisense_faults_to_bitmap32(&fo) == H::faults_to_bitmap32(fn), "faults_to_bitmap32 bits 0x%08X",
          (unsigned) bits);
  }
}

static void test_temperatures() {
  for (int t = -400; t <= 400; t++) {
    CHECK(hisense_f_to_c(t) == H::f_to_c(t), "f_to_c(%d)", t);
    CHECK(hisense_c_to_f(t) == H::c_to_f(t), "c_to_f(%d)", t);
  }
  for (int sp = -128; sp < 128; sp++) {
    for (int f = 0; f < 2; f++) {
      CHECK(hisense_setpoint_in_range((int8_t) sp, f != 0) == H::setpoint_in_range((int8_t) sp, f != 0),
            "setpoint_in_range %d %d", sp, f);
      int8_t a = 99, b = 99;
      bool ra = hisense_shadow_setpoint_from_status((int8_t) sp, f != 0, &a);
      bool rb = H::shadow_setpoint_from_status((int8_t) sp, f != 0, &b);
      CHECK(ra == rb && a == b, "shadow_setpoint_from_status %d %d", sp, f);

      HisenseCommand oc;
      memset(&oc, 0, sizeof(oc));
      H::AcCommand nc;
      nc.setpoint = 0;
      bool sa = esphome_sync_shadow_setpoint((int8_t) sp, f != 0, &oc);
      bool sb = H::sync_shadow_setpoint((int8_t) sp, f != 0, &nc);
      CHECK(sa == sb && (!sa || (oc.setpoint == nc.setpoint && oc.fahrenheit == nc.fahrenheit)),
            "sync_shadow_setpoint %d %d", sp, f);
    }
  }
  for (int c = -60; c <= 120; c++) {
    for (int f = 0; f < 2; f++) {
      HisenseCommand oc;
      memset(&oc, 0, sizeof(oc));
      H::AcCommand nc;
      int ra = esphome_setpoint_to_cmd(c, f != 0, &oc);
      int rb = H::setpoint_to_cmd(c, f != 0, &nc);
      CHECK(ra == rb && oc.setpoint == nc.setpoint && oc.fahrenheit == nc.fahrenheit, "setpoint_to_cmd %d %d", c, f);
    }
  }
}

static void test_map() {
  for (int m = 0; m < 16; m++) {
    HisenseMode om = (HisenseMode) 99;
    H::Mode nm = (H::Mode) 99;
    bool ro = esphome_mode_to_hisense((uint8_t) m, &om);
    bool rn = H::climate_mode_to_hisense((uint8_t) m, &nm);
    CHECK(ro == rn && (!ro || (int) om == (int) nm), "climate mode %d", m);
    CHECK(hisense_mode_to_esphome((HisenseMode) m) == H::hisense_mode_to_climate((H::Mode) m), "hisense mode %d", m);
  }
  for (int p = 0; p < 2; p++)
    for (int m = 0; m < 8; m++)
      for (int hz = 0; hz < 4; hz++) {
        CHECK(hisense_to_running_state(p != 0, (HisenseMode) m, (uint8_t) hz) ==
                  H::running_state(p != 0, (H::Mode) m, (uint8_t) hz),
              "running_state %d %d %d", p, m, hz);
        CHECK(hisense_to_climate_action(p != 0, (HisenseMode) m, (uint8_t) hz) ==
                  H::climate_action(p != 0, (H::Mode) m, (uint8_t) hz),
              "climate_action %d %d %d", p, m, hz);
      }
  for (int v = 0; v < 2; v++)
    for (int h = 0; h < 2; h++)
      CHECK(hisense_to_climate_swing(v != 0, h != 0) == H::climate_swing(v != 0, h != 0), "swing %d %d", v, h);
  for (int s = 0; s < 8; s++) {
    HisenseSwingMode ov, oh;
    H::SwingMode nv, nh;
    climate_swing_to_hisense((uint8_t) s, &ov, &oh);
    H::climate_swing_to_hisense((uint8_t) s, &nv, &nh);
    CHECK((int) ov == (int) nv && (int) oh == (int) nh, "swing to hisense %d", s);
  }
  for (int raw = 0; raw < 256; raw++) {
    CHECK(hisense_fan_raw_to_esphome_index((uint8_t) raw) == H::fan_raw_to_index((uint8_t) raw), "fan raw %d", raw);
    CHECK((int) hisense_fan_raw_to_cmd((uint8_t) raw) == (int) H::fan_raw_to_cmd((uint8_t) raw), "fan cmd %d", raw);
    CHECK((int) esphome_fan_index_to_hisense((uint8_t) raw) == (int) H::fan_index_to_hisense((uint8_t) raw),
          "fan index %d", raw);
    CHECK(esphome_fan_enum_to_index((uint8_t) raw) == H::climate_fan_to_index((uint8_t) raw), "fan enum %d", raw);
    CHECK(esphome_fan_published_index((uint8_t) raw) == H::fan_published_index((uint8_t) raw), "fan pub %d", raw);
  }
  for (int e = 0; e < 2; e++)
    for (int t = 0; t < 2; t++)
      CHECK((int) hisense_feature_from_status(e != 0, t != 0) == (int) H::feature_from_status(e != 0, t != 0),
            "feature_from_status %d %d", e, t);
  for (int f = 0; f < 6; f++)
    CHECK((int) esphome_feature_after_send((HisenseFeature) f) == (int) H::feature_after_send((H::Feature) f),
          "feature_after_send %d", f);
}

static void test_presets() {
  for (int idx = 0; idx < 20; idx++)
    for (int sup = 0; sup < 16; sup++)
      CHECK(esphome_preset_available((uint8_t) idx, (uint8_t) sup) == H::preset_available(idx, (uint8_t) sup),
            "preset_available %d %d", idx, sup);
  for (size_t i = 0; i < H::PRESET_COUNT; i++)
    CHECK(esphome_preset_index(H::PRESETS[i].name) == H::preset_index(H::PRESETS[i].name), "preset_index %zu", i);
  CHECK(esphome_preset_index("sleep") == H::preset_index("sleep"), "preset_index built-in name");
  CHECK(esphome_preset_index(nullptr) == H::preset_index(nullptr), "preset_index null");

  for (int bits = 0; bits < 8; bits++) {
    for (int raw = 0; raw < 256; raw++) {
      bool e = bits & 1, t = bits & 2, m = bits & 4;
      HisenseSpecialState os = hisense_special_from_status(e, t, m, (uint8_t) raw);
      H::SpecialState ns = H::special_from_status(e, t, m, (uint8_t) raw);
      CHECK(os.eco == ns.eco && os.turbo == ns.turbo && os.mute == ns.mute && os.sleep == ns.sleep,
            "special_from_status %d %d", bits, raw);
    }
    for (int sleep = 0; sleep < 7; sleep++) {
      HisenseSpecialState os = {(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (uint8_t) sleep};
      H::SpecialState ns;
      ns.eco = os.eco;
      ns.turbo = os.turbo;
      ns.mute = os.mute;
      ns.sleep = os.sleep;
      for (int sup = 0; sup < 16; sup++)
        CHECK(esphome_preset_detect(&os, (uint8_t) sup) == H::preset_detect(ns, (uint8_t) sup),
              "preset_detect bits=%d sleep=%d sup=%d", bits, sleep, sup);
      for (int w = 0; w < 10; w++)
        CHECK(esphome_fan_request_allowed(&os, (uint8_t) w) == H::fan_request_allowed(ns, (uint8_t) w),
              "fan_request_allowed bits=%d sleep=%d w=%d", bits, sleep, w);
      for (int target = 0; target < 16; target++) {
        HisenseSpecialOp oo[ESPHOME_PRESET_PLAN_MAX];
        H::SpecialOp no[H::PRESET_PLAN_MAX];
        uint8_t co = esphome_preset_plan(&os, (uint8_t) target, oo);
        size_t cn = H::preset_plan(ns, (uint8_t) target, no);
        bool same = co == cn;
        for (size_t k = 0; same && k < cn; k++)
          same = oo[k].kind == no[k].kind && oo[k].value == no[k].value;
        CHECK(same, "preset_plan bits=%d sleep=%d target=%d", bits, sleep, target);
      }
      for (int kind = 0; kind < 4; kind++) {
        for (int value = 0; value < 6; value++) {
          HisenseSpecialState oa = os;
          H::SpecialState na = ns;
          HisenseSpecialOp op = {(uint8_t) kind, (uint8_t) value};
          hisense_special_apply(&oa, &op);
          H::special_apply(&na, H::SpecialOp{(H::SpecialOpKind) kind, (uint8_t) value});
          CHECK(oa.eco == na.eco && oa.turbo == na.turbo && oa.mute == na.mute && oa.sleep == na.sleep,
                "special_apply kind=%d value=%d", kind, value);
        }
      }
    }
  }
}

static void test_power() {
  for (int i = 0; i < 256; i++) {
    CHECK(hisense_active_power_mw((uint8_t) i) == H::active_power_mw((uint8_t) i), "power %d", i);
    CHECK(hisense_voltage_mv((uint8_t) i) == H::voltage_mv((uint8_t) i), "voltage %d", i);
    for (int v = 0; v < 256; v += 3)
      CHECK(hisense_active_current_ma((uint8_t) i, (uint8_t) v) == H::active_current_ma((uint8_t) i, (uint8_t) v),
            "current %d %d", i, v);
  }
  uint64_t oa = 0, na = 0;
  for (int i = 0; i < 5000; i++) {
    int64_t p = (int64_t) (rnd() % 3000000) - 100000;
    uint32_t dt = rnd() % 5000;
    hisense_energy_add(&oa, p, dt);
    H::energy_add(&na, p, dt);
  }
  CHECK(oa == na && hisense_energy_mwh(oa) == H::energy_mwh(na), "energy integrator");
}

// The original reassembler is static, so the port is checked against hand-built streams.
static void test_frame_assembler() {
  H::FrameAssembler fa;
  uint8_t f[160];
  random_frame(f, sizeof(f), 0x66, 0x00);
  // On the real bus only a checksum byte can be 0xF4, so clear the body of it, then tune one body
  // byte until the checksum's low byte is 0xF4 and has to be stuffed on the wire.
  for (size_t i = 2; i < 156; i++)
    if (f[i] == 0xF4)
      f[i] = 0x00;
  for (int v = 0; v < 256; v++) {
    f[100] = (uint8_t) v;
    uint32_t sum = 0;
    for (size_t i = 2; i < 156; i++)
      sum += f[i];
    if ((uint8_t) sum == 0xF4 && f[100] != 0xF4) {
      f[156] = (uint8_t) (sum >> 8);
      f[157] = (uint8_t) sum;
      break;
    }
  }
  CHECK(f[157] == 0xF4 && f[156] != 0xF4, "test setup: checksum low byte is 0xF4");

  uint8_t wire[200];
  size_t w = 0;
  const uint8_t junk[] = {0x00, 0xFB, 0x12};
  for (uint8_t j : junk)
    wire[w++] = j;
  for (size_t i = 0; i < sizeof(f); i++) {
    wire[w++] = f[i];
    if (i >= 156 && i <= 157 && f[i] == 0xF4)
      wire[w++] = 0xF4;
  }
  size_t got = 0;
  for (size_t i = 0; i < w; i++) {
    size_t n = fa.feed(wire[i]);
    if (n)
      got = n;
  }
  CHECK(got == sizeof(f) && memcmp(fa.data(), f, sizeof(f)) == 0, "assembler: stuffed status frame recovered");
  H::AcState st;
  CHECK(H::parse_status(fa.data(), got, &st), "assembler output parses");

  // Our own frames (direction byte 0x00) are echoes and are dropped.
  got = 0;
  for (size_t i = 0; i < HISENSE_STATUS_REQUEST_LEN; i++)
    got |= fa.feed(HISENSE_STATUS_REQUEST[i]);
  CHECK(got == 0, "assembler: echo of our own poll dropped");

  // A missing end tag resyncs instead of returning a frame.
  uint8_t g[28];
  random_frame(g, sizeof(g), 0x1E, 0x00);
  for (size_t i = 2; i < 26; i++)
    if (g[i] == 0xF4)
      g[i] = 0x00;
  g[27] = 0x00;
  got = 0;
  for (uint8_t b : g)
    got |= fa.feed(b);
  CHECK(got == 0, "assembler: missing F4 FB rejected");

  // A LEN that claims more than the buffer holds is dropped at byte 4.
  const uint8_t big[] = {0xF4, 0xF5, 0x01, 0x40, 0xFF};
  got = 0;
  for (uint8_t b : big)
    got |= fa.feed(b);
  CHECK(got == 0, "assembler: oversize LEN dropped");
}

int main() {
  printf("== ESPHome codec parity (port vs original driver) ==\n");
  test_constants();
  test_build_command();
  test_overrides();
  test_fixed_builders();
  test_link_heartbeat();
  test_stamp_link_token();
  test_parsers();
  test_bitmaps();
  test_temperatures();
  test_map();
  test_presets();
  test_power();
  test_frame_assembler();
  printf("  %ld checks, %d failed\n", g_checks, g_fail);
  printf(g_fail ? "== CODEC PARITY FAILED ==\n" : "== CODEC PARITY OK ==\n");
  return g_fail ? 1 : 0;
}
