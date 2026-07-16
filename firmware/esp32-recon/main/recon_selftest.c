// esp32-recon selftest: run the codec golden vectors on-target. Because the ESP32
// links the SAME firmware/src/rs485-driver/hisense_rs485.cpp that the host tests
// (firmware/test/test_codec.cpp) validate, matching outputs here prove the flashed
// codec is byte-identical to the host-validated one. No hardware needed.
#include <string.h>
#include "recon.h"

// 16-byte A/C->module status header (from test_codec.cpp make_status): [4]=0x97=151
// -> total frame = 151+9 = 160 bytes.
static const uint8_t STATUS_HDR[16] = {
    0xF4,0xF5,0x01,0x40,0x97,0x01,0x00,0xFE,0x01,0x01,0x01,0x01,0x00,0x66,0x00,0x01
};

// Build a 160-byte (un-stuffed) status frame with a correct big-endian checksum
// over [2,156). mode_nibble goes into byte[18] bits4-7, run into bits2-3.
static int make_status(uint8_t *f, int mode_nibble, int run, int set_c, int indoor_c,
                       uint8_t fan_raw, int outdoor_c, uint8_t flags1, uint8_t flags2,
                       uint8_t comp, uint8_t sleep_raw, int coil_c)
{
    memset(f, 0, 160);
    memcpy(f, STATUS_HDR, 16);
    f[16] = fan_raw;
    f[17] = sleep_raw;
    f[18] = (uint8_t)((mode_nibble << 4) | (run << 2));
    f[19] = (uint8_t)set_c;
    f[20] = (uint8_t)indoor_c;
    f[35] = flags1;
    f[36] = flags2;
    f[42] = comp;
    f[44] = (uint8_t)outdoor_c;
    f[45] = (uint8_t)coil_c;
    uint32_t sum = 0;
    for (int i = 2; i < 156; i++) sum += f[i];
    f[156] = (uint8_t)(sum >> 8);
    f[157] = (uint8_t)(sum & 0xFF);
    f[158] = 0xF4;
    f[159] = 0xFB;
    return 160;
}

#define CHECK(cond, name) do { \
        if (cond) { /* ok */ } \
        else { fails++; fprintf(out, "  [FAIL] %s\r\n", name); } \
    } while (0)

int recon_selftest(FILE *out)
{
    int fails = 0;
    uint8_t f[64];
    size_t n;

    // ---- command builder: mode byte[18] ----
    HisenseCommand c = { HISENSE_MODE_COOL, 24, false, HISENSE_FAN_AUTO,
                         HISENSE_SWING_OFF, HISENSE_SWING_OFF, HISENSE_FEATURE_NONE, false };
    struct { HisenseMode m; uint8_t exp; } modes[] = {
        { HISENSE_MODE_AUTO, 0x90 }, { HISENSE_MODE_COOL, 0x50 },
        { HISENSE_MODE_HEAT, 0x30 }, { HISENSE_MODE_DRY, 0x70 }, { HISENSE_MODE_FAN, 0x10 },
    };
    for (unsigned i = 0; i < sizeof(modes)/sizeof(modes[0]); i++) {
        c.mode = modes[i].m;
        n = hisense_build_command(&c, f, sizeof(f));
        CHECK(n && f[18] == modes[i].exp, "cmd mode byte[18]");
    }
    c.mode = HISENSE_MODE_COOL;

    // ---- command builder: fan byte[16] ----
    struct { HisenseFanSpeed s; uint8_t exp; } fans[] = {
        { HISENSE_FAN_AUTO, 0x01 }, { HISENSE_FAN_QUIET, 0x03 }, { HISENSE_FAN_LOW, 0x0B },
        { HISENSE_FAN_MED_LOW, 0x0D }, { HISENSE_FAN_MID, 0x0F },
        { HISENSE_FAN_MED_HIGH, 0x11 }, { HISENSE_FAN_HIGH, 0x13 },
    };
    for (unsigned i = 0; i < sizeof(fans)/sizeof(fans[0]); i++) {
        c.fan = fans[i].s;
        n = hisense_build_command(&c, f, sizeof(f));
        CHECK(n && f[16] == fans[i].exp, "cmd fan byte[16]");
    }
    c.fan = HISENSE_FAN_AUTO;

    // ---- command builder: setpoint byte[19] = temp*2+1 ----
    c.setpoint = 22; n = hisense_build_command(&c, f, sizeof(f));
    CHECK(n && f[19] == 0x2D, "cmd temp 22 -> byte[19]=0x2D");
    c.setpoint = 24; n = hisense_build_command(&c, f, sizeof(f));
    CHECK(n && f[19] == 0x31, "cmd temp 24 -> byte[19]=0x31");

    // ---- command builder: feature byte[33] + vswing byte[32] ----
    c.feature = HISENSE_FEATURE_ECO;   n = hisense_build_command(&c, f, sizeof(f));
    CHECK(n && f[33] == 0x30, "cmd eco -> byte[33]=0x30");
    c.feature = HISENSE_FEATURE_TURBO; n = hisense_build_command(&c, f, sizeof(f));
    CHECK(n && f[33] == 0x0C, "cmd turbo -> byte[33]=0x0C");
    c.feature = HISENSE_FEATURE_NONE;
    c.vswing = HISENSE_SWING_SWING;    n = hisense_build_command(&c, f, sizeof(f));
    CHECK(n && f[32] == 0xC0, "cmd vswing -> byte[32]=0xC0");
    c.vswing = HISENSE_SWING_OFF;

    // ---- power frames (literal templates) ----
    n = hisense_build_power_frame(true, f, sizeof(f));
    CHECK(n >= 50 && f[46] == 0x01 && f[47] == 0xDF && f[48] == 0xF4 && f[49] == 0xFB,
          "power ON tail 01 DF F4 FB");
    n = hisense_build_power_frame(false, f, sizeof(f));
    CHECK(n >= 50 && f[46] == 0x02 && f[47] == 0x31 && f[48] == 0xF4 && f[49] == 0xFB,
          "power OFF tail 02 31 F4 FB");

    // ---- mute / sleep single-field frames ----
    n = hisense_build_mute_frame(true, f, sizeof(f));  CHECK(n && f[35] == 0x30, "mute on byte[35]=0x30");
    n = hisense_build_mute_frame(false, f, sizeof(f)); CHECK(n && f[35] == 0x10, "mute off byte[35]=0x10");
    for (int p = 0; p <= 4; p++) {
        n = hisense_build_sleep_frame((uint8_t)p, f, sizeof(f));
        CHECK(n && f[17] == (uint8_t)(p * 2 + 1), "sleep profile byte[17]=p*2+1");
    }

    // ---- producttype request ----
    n = hisense_build_producttype_request(f, sizeof(f));
    CHECK(n == 21 && f[13] == 0x66 && f[14] == 0x40 && f[17] == 0x01 && f[18] == 0xF3
          && f[19] == 0xF4 && f[20] == 0xFB, "producttype request bytes");

    // ---- status parse: field decode ----
    uint8_t s[160]; HisenseState st;
    make_status(s, HISENSE_MODE_COOL, 1, 22, 21, 0x01, 32, 0, 0, 0, 0, 0);
    CHECK(hisense_parse_status(s, 160, &st) && st.valid && st.power_on
          && st.mode == HISENSE_MODE_COOL && st.setpoint_c == 22 && st.indoor_temp_c == 21
          && st.outdoor_temp_c == 32 && st.fan_raw == 0x01, "status decode A (cool/22/21/32)");

    make_status(s, HISENSE_MODE_COOL, 1, 22, 21, 0x12, 32, 0x86, 0x04, 55, 0x04, 27);
    CHECK(hisense_parse_status(s, 160, &st) && st.valid && st.vswing_on && st.eco_on
          && st.turbo_on && st.mute_on && st.fan_raw == 0x12 && st.compressor_freq == 55
          && st.sleep_on && st.sleep_raw == 0x04 && st.coil_temp_c == 27,
          "status decode B (flags/comp/sleep/coil)");

    // AUTO remap: status mode nibble 5 -> HISENSE_MODE_AUTO
    make_status(s, 5, 1, 24, 24, 0x01, 30, 0, 0, 0, 0, 0);
    CHECK(hisense_parse_status(s, 160, &st) && st.valid && st.mode == HISENSE_MODE_AUTO,
          "status mode nibble 5 -> AUTO");

    // ---- status parse: rejects ----
    make_status(s, HISENSE_MODE_COOL, 1, 22, 21, 0x01, 32, 0, 0, 0, 0, 0);
    CHECK(!hisense_parse_status(s, 82, &st), "reject short (len 82)");
    s[20] ^= 0xFF;   // corrupt a body byte without re-checksum
    CHECK(!hisense_parse_status(s, 160, &st), "reject bad checksum");

    return fails;
}
