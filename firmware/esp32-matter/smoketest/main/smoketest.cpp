// On-target smoke test: runs the Hisense codec golden vectors on the ESP32, through
// the REUSED driver (../../src/rs485-driver/hisense_rs485.cpp) linked against the ESP32
// HAL (hisense_hal). Proves the port compiles + the codec runs correctly on silicon.
// Does NOT call hisense_init() -> no UART/bus task, so no transceiver needed.
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "hisense_rs485.h"
#include "matter_aircon_map.h"
#include "power_estimate.h"

static const char *TAG = "smoketest";
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } \
    else { g_fail++; ESP_LOGE(TAG, "FAIL: " __VA_ARGS__); } } while (0)

static uint16_t besum(const uint8_t *f, int end)
{ uint32_t s = 0; for (int i = 2; i < end; i++) s += f[i]; return (uint16_t)(s & 0xFFFF); }

// Build a 160-byte STATUS frame the way the real A/C does (mirrors test_codec.cpp).
static void make_status(uint8_t *out, bool power, HisenseMode mode, int8_t setp,
                        int8_t indoor, uint8_t fan_raw, uint8_t f1, uint8_t f2,
                        uint8_t sleep_raw, uint8_t comp, int8_t outd)
{
    static const uint8_t hdr[16] = {0xF4,0xF5,0x01,0x40,0x97,0x01,0x00,0xFE,
                                    0x01,0x01,0x01,0x01,0x00,0x66,0x00,0x01};
    memset(out, 0, 160); memcpy(out, hdr, 16);
    uint8_t sm = (mode == HISENSE_MODE_AUTO) ? 5 : (uint8_t)mode;
    out[16] = fan_raw; out[17] = sleep_raw;
    out[18] = (uint8_t)((sm << 4) | ((power ? 2 : 0) << 2));
    out[19] = (uint8_t)setp; out[20] = (uint8_t)indoor;
    out[35] = f1; out[36] = f2; out[42] = comp; out[44] = (uint8_t)outd;
    uint16_t ck = besum(out, 156); out[156] = ck >> 8; out[157] = ck & 0xFF;
    out[158] = HISENSE_ETX1; out[159] = HISENSE_ETX2;   // F4 FB end tag (was missing)
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Hisense codec on-target smoke test (ESP32-D0WDQ6) ===");
    uint8_t s[160]; HisenseState st;

    // --- status parse round-trip ---
    make_status(s, true, HISENSE_MODE_COOL, 22, 21, 0x01, 0, 0, 0, 0, 32);
    CHECK(hisense_parse_status(s, 160, &st), "parse 160B COOL frame");
    CHECK(st.power_on, "power_on");
    CHECK(st.mode == HISENSE_MODE_COOL, "mode COOL got %d", st.mode);
    CHECK(st.setpoint_c == 22, "setpoint %d exp 22", st.setpoint_c);
    CHECK(st.indoor_temp_c == 21, "indoor %d exp 21", st.indoor_temp_c);
    CHECK(st.outdoor_temp_c == 32, "outdoor %d exp 32", st.outdoor_temp_c);

    // AUTO remap (status nibble 5 -> enum AUTO)
    make_status(s, true, HISENSE_MODE_AUTO, 24, 23, 0x01, 0, 0, 0, 0, 30);
    hisense_parse_status(s, 160, &st);
    CHECK(st.mode == HISENSE_MODE_AUTO, "AUTO remap got %d", st.mode);

    // reject a corrupted frame
    make_status(s, true, HISENSE_MODE_COOL, 22, 21, 0x01, 0, 0, 0, 0, 32); s[100] ^= 0xFF;
    CHECK(!hisense_parse_status(s, 160, &st), "reject bad checksum");

    // --- command builder produces a valid framed output ---
    HisenseCommand cmd = {};
    cmd.mode = HISENSE_MODE_COOL; cmd.setpoint = 22; cmd.fan = HISENSE_FAN_LOW;
    cmd.vswing = HISENSE_SWING_OFF; cmd.hswing = HISENSE_SWING_OFF; cmd.feature = HISENSE_FEATURE_NONE;
    uint8_t f[HISENSE_CMD_FRAME_LEN + 2];
    size_t n = hisense_build_command(&cmd, f, sizeof(f));
    CHECK(n > 0, "build_command returns a frame (n=%u)", (unsigned)n);
    CHECK(f[0] == 0xF4 && f[1] == 0xF5, "cmd frame header F4 F5");
    // out-of-range setpoint must be rejected (returns 0)
    HisenseCommand bad = cmd; bad.setpoint = 99;
    CHECK(hisense_build_command(&bad, f, sizeof(f)) == 0, "reject setpoint 99");

    // --- "77" debounce: fire once after HOLD, glitch never fires ---
    uint8_t streak = 0; bool latched = false;
    CHECK(!hisense_recommission_debounce(HISENSE_LINK_REQ_RECONFIG, &streak, &latched, 3), "no fire f1");
    CHECK(!hisense_recommission_debounce(HISENSE_LINK_REQ_RECONFIG, &streak, &latched, 3), "no fire f2");
    CHECK( hisense_recommission_debounce(HISENSE_LINK_REQ_RECONFIG, &streak, &latched, 3), "fire on f3");
    CHECK(!hisense_recommission_debounce(HISENSE_LINK_REQ_RECONFIG, &streak, &latched, 3), "no re-fire while held");

    // --- power estimate clamp (fix #8) ---
    CHECK(hisense_active_power_mw(0) == 0, "power @0 == 0");
    CHECK(hisense_active_power_mw(255) == HISENSE_POWER_MW_MAX, "power @255 clamped");

    ESP_LOGI(TAG, "==================================================");
    if (g_fail == 0)
        ESP_LOGI(TAG, "== ALL %d CODEC TESTS PASSED ON ESP32 ==", g_pass);
    else
        ESP_LOGE(TAG, "== %d PASSED, %d FAILED ==", g_pass, g_fail);
    ESP_LOGI(TAG, "==================================================");
}
