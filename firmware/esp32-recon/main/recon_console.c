// esp32-recon: shared core state + the esp_console command set (used by both the
// UART REPL and the TCP :2323 server, since esp_console_run dispatches to the same
// registered commands).
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "esp_console.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "recon.h"

static const char *TAG = "recon";
#define NVS_NS "recon"

// ---------------------------------------------------------------------------
// Shared state (mutex-guarded; written from the bus/tap task, read by consoles)
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_mtx;
static recon_stats_t     s_stats;
static FILE             *s_watch;          // live-stream sink, or NULL
static bool              s_watch_hex;      // include raw hex in watch lines

static struct {
    bool  armed;
    int   field;
    int   expected;
    int   remaining;   // status frames left to satisfy the match
    FILE *out;
} s_verify;

// Raw-frame delta finder: latest + snapshot, per kind (0=status, 1=cmd).
#define RAW_KINDS 2
#define RAW_MAX   256
static uint8_t s_raw_last[RAW_KINDS][RAW_MAX];
static int     s_raw_last_len[RAW_KINDS];
static uint8_t s_raw_base[RAW_KINDS][RAW_MAX];
static int     s_raw_base_len[RAW_KINDS];

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

void recon_core_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    memset(&s_stats, 0, sizeof(s_stats));
    s_watch = NULL;
    s_verify.armed = false;
}

// ---------------------------------------------------------------------------
// Capture mode (NVS-persisted; switch by reboot-into-mode)
// ---------------------------------------------------------------------------
const char *recon_mode_str(recon_mode_t m) { return m == RECON_MODE_MASTER ? "master" : "tap"; }

recon_mode_t recon_mode_get(void)
{
    nvs_handle_t h;
    uint8_t v = RECON_MODE_TAP;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "mode", &v);
        nvs_close(h);
    }
    return v == RECON_MODE_MASTER ? RECON_MODE_MASTER : RECON_MODE_TAP;
}

void recon_mode_set_and_reboot(recon_mode_t m)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "mode", (uint8_t)m);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "mode -> %s; rebooting", recon_mode_str(m));
    fflush(NULL);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

// ---------------------------------------------------------------------------
// Pretty-print a decoded status
// ---------------------------------------------------------------------------
static const char *mode_name(HisenseMode m)
{
    switch (m) {
    case HISENSE_MODE_FAN:  return "fan";
    case HISENSE_MODE_HEAT: return "heat";
    case HISENSE_MODE_COOL: return "cool";
    case HISENSE_MODE_DRY:  return "dry";
    case HISENSE_MODE_AUTO: return "auto";
    default:                return "?";
    }
}

void recon_print_state(FILE *out, const HisenseState *st, bool with_hex,
                       const uint8_t *raw, int rawlen)
{
    if (!out || !st) return;
    fprintf(out,
        " power=%d mode=%s set=%dC in=%dC out=%dC coil=%dC fan=0x%02x comp=%dHz\r\n"
        " eco=%d turbo=%d mute=%d sleep=%d(raw0x%02x) vswing=%d hswing=%d heatrelay=%d"
        " I=%u V=%u\r\n",
        st->power_on, mode_name(st->mode), st->setpoint_c, st->indoor_temp_c,
        st->outdoor_temp_c, st->coil_temp_c, st->fan_raw, st->compressor_freq,
        st->eco_on, st->turbo_on, st->mute_on, st->sleep_on, st->sleep_raw,
        st->vswing_on, st->hswing_on, st->heat_relay_on,
        (unsigned)st->current_raw, (unsigned)st->voltage_raw);
    if (with_hex && raw && rawlen > 0) {
        fprintf(out, " raw[%d]:", rawlen);
        for (int i = 0; i < rawlen; i++) fprintf(out, " %02x", raw[i]);
        fprintf(out, "\r\n");
    }
}

// ---------------------------------------------------------------------------
// Watch sink + verify plumbing
// ---------------------------------------------------------------------------
void recon_watch_set(FILE *sink)
{
    LOCK(); s_watch = sink; UNLOCK();
}

// A console session is closing: drop any watch sink or armed verify that points
// at its FILE* so the bus/tap task never writes to a closed stream.
void recon_watch_clear_if(FILE *sink)
{
    LOCK();
    if (s_watch == sink) s_watch = NULL;
    if (s_verify.armed && s_verify.out == sink) s_verify.armed = false;
    UNLOCK();
}

void recon_watch_note_raw(const char *label, const uint8_t *buf, int len)
{
    LOCK();
    if (s_watch) {
        fprintf(s_watch, "[%s] dir=0x%02x class=0x%02x len=%d:",
                label, len > 2 ? buf[2] : 0, len > 13 ? buf[13] : 0, len);
        if (s_watch_hex) for (int i = 0; i < len; i++) fprintf(s_watch, " %02x", buf[i]);
        fprintf(s_watch, "\r\n");
        fflush(s_watch);
    }
    UNLOCK();
}

void recon_verify_arm(FILE *out, int field, int expected)
{
    LOCK();
    s_verify.armed = true;
    s_verify.field = field;
    s_verify.expected = expected;
    s_verify.remaining = 6;   // give the ~1Hz poll + settle a few frames to reflect
    s_verify.out = out;
    UNLOCK();
}

static bool verify_eval(const HisenseState *st, int field, int expected, int *got)
{
    int g;
    switch (field) {
    case RV_TEMP:    g = st->setpoint_c;         break;
    case RV_MODE:    g = (int)st->mode;          break;
    case RV_FAN_RAW: g = st->fan_raw;            break;
    case RV_POWER:   g = st->power_on ? 1 : 0;   break;
    case RV_ECO:     g = st->eco_on ? 1 : 0;     break;
    case RV_TURBO:   g = st->turbo_on ? 1 : 0;   break;
    case RV_MUTE:    g = st->mute_on ? 1 : 0;    break;
    case RV_SLEEP:   g = st->sleep_on ? 1 : 0;   break;
    default:         g = -1;                     break;
    }
    if (got) *got = g;
    return g == expected;
}

static const char *field_name(int f)
{
    switch (f) {
    case RV_TEMP: return "setpoint"; case RV_MODE: return "mode";
    case RV_FAN_RAW: return "fan_raw"; case RV_POWER: return "power";
    case RV_ECO: return "eco"; case RV_TURBO: return "turbo";
    case RV_MUTE: return "mute"; case RV_SLEEP: return "sleep";
    default: return "?";
    }
}

// ---------------------------------------------------------------------------
// Called on every valid status frame (bus task in master, tap task in tap).
// ---------------------------------------------------------------------------
void recon_on_status(const HisenseState *st)
{
    if (!st || !st->valid) return;
    LOCK();
    s_stats.frames++;
    s_stats.have_state = true;
    s_stats.last = *st;

    if (s_watch) {
        fprintf(s_watch, "[frame %u]\r\n", (unsigned)s_stats.frames);
        recon_print_state(s_watch, st, s_watch_hex, NULL, 0);
        fflush(s_watch);
    }

    if (s_verify.armed) {
        int got = -1;
        if (verify_eval(st, s_verify.field, s_verify.expected, &got)) {
            fprintf(s_verify.out, "[verify] %s == %d  PASS\r\n",
                    field_name(s_verify.field), s_verify.expected);
            s_verify.armed = false;
            fflush(s_verify.out);
        } else if (--s_verify.remaining <= 0) {
            fprintf(s_verify.out, "[verify] %s: got %d, want %d  FAIL\r\n",
                    field_name(s_verify.field), got, s_verify.expected);
            s_verify.armed = false;
            fflush(s_verify.out);
        }
    }
    UNLOCK();
}

void recon_note_chkfail(void)    { LOCK(); s_stats.chkfail++;    UNLOCK(); }
void recon_note_cmd_frame(void)  { LOCK(); s_stats.cmd_frames++; UNLOCK(); }

void recon_note_raw(int kind, const uint8_t *buf, int len)
{
    if (kind < 0 || kind >= RAW_KINDS || len <= 0) return;
    if (len > RAW_MAX) len = RAW_MAX;
    LOCK();
    memcpy(s_raw_last[kind], buf, len);
    s_raw_last_len[kind] = len;
    UNLOCK();
}

void recon_get_stats(recon_stats_t *out)
{
    LOCK(); *out = s_stats; UNLOCK();
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
// Parse a hex string ("F4F5.." or "f4 f5 ..") into bytes. Returns count or -1.
static int parse_hex(const char *s, uint8_t *out, int cap)
{
    int n = 0, hi = -1;
    for (; *s; s++) {
        char c = *s;
        if (isspace((unsigned char)c) || c == ':' || c == ',') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return -1;
        if (hi < 0) hi = v;
        else { if (n >= cap) return -1; out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    return hi < 0 ? n : -1;   // must be an even number of nibbles
}

static bool is_master(FILE *out)
{
    if (recon_mode_get() == RECON_MODE_MASTER) return true;
    fprintf(out, "err: command needs master mode (run `mode master`, then reboot)\r\n");
    return false;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
static int cmd_mode(int argc, char **argv)
{
    if (argc < 2) { printf("mode = %s\r\n", recon_mode_str(recon_mode_get())); return 0; }
    if (!strcmp(argv[1], "tap"))         recon_mode_set_and_reboot(RECON_MODE_TAP);
    else if (!strcmp(argv[1], "master")) recon_mode_set_and_reboot(RECON_MODE_MASTER);
    else { printf("usage: mode [tap|master]\r\n"); return 1; }
    return 0;   // (reboots before returning)
}

static int cmd_stats(int argc, char **argv)
{
    (void)argc; (void)argv;
    extern volatile uint32_t g_hal_tx_bytes, g_hal_rx_bytes, g_hal_evt_total, g_hal_evt_data;
    extern volatile uint8_t  g_hal_rx_task_up;
    recon_stats_t s; recon_get_stats(&s);
    printf("mode=%s frames=%u chkfail=%u cmd_frames=%u | HAL tx=%u rx=%u "
           "rx_task_up=%u evts=%u data=%u\r\n",
           recon_mode_str(recon_mode_get()), (unsigned)s.frames, (unsigned)s.chkfail,
           (unsigned)s.cmd_frames, (unsigned)g_hal_tx_bytes, (unsigned)g_hal_rx_bytes,
           (unsigned)g_hal_rx_task_up, (unsigned)g_hal_evt_total, (unsigned)g_hal_evt_data);
    return 0;
}

static int cmd_poll(int argc, char **argv)
{
    (void)argc; (void)argv;
    recon_stats_t s; recon_get_stats(&s);
    if (!s.have_state) { printf("no status decoded yet\r\n"); return 0; }
    printf("last of %u frames:\r\n", (unsigned)s.frames);
    recon_print_state(stdout, &s.last, false, NULL, 0);
    return 0;
}

static int cmd_watch(int argc, char **argv)
{
    bool on = true, hex = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "off")) on = false;
        else if (!strcmp(argv[i], "on")) on = true;
        else if (!strcmp(argv[i], "hex")) hex = true;
    }
    if (on) { s_watch_hex = hex; recon_watch_set(stdout);
              printf("watching frames (hex=%d). run `watch off` to stop.\r\n", hex); }
    else    { recon_watch_set(NULL); printf("watch off\r\n"); }
    return 0;
}

// --- set / power (master) ---
// Persistent command shadow for `set` (so single-field sets combine sanely).
static HisenseCommand s_cmd = { HISENSE_MODE_COOL, 24, false, HISENSE_FAN_AUTO,
                                HISENSE_SWING_OFF, HISENSE_SWING_OFF,
                                HISENSE_FEATURE_NONE, false };

static int fan_expected_raw(HisenseFanSpeed f)
{
    switch (f) {                       // status raw (even) vs command (odd); see header
    case HISENSE_FAN_AUTO:     return 0x01;
    case HISENSE_FAN_QUIET:    return 0x02;
    case HISENSE_FAN_LOW:      return 0x0A;
    case HISENSE_FAN_MED_LOW:  return 0x0C;
    case HISENSE_FAN_MID:      return 0x0E;
    case HISENSE_FAN_MED_HIGH: return 0x10;
    case HISENSE_FAN_HIGH:     return 0x12;
    default:                   return -1;
    }
}

static int cmd_set(int argc, char **argv)
{
    if (!is_master(stdout)) return 1;
    if (argc < 3) {
        printf("usage: set <mode|temp|fan|swing|eco|turbo|mute|sleep> <value>\r\n"
               "  mode cool|heat|dry|fan|auto   temp <16..32>\r\n"
               "  fan auto|quiet|low|medlow|mid|medhigh|high   swing on|off\r\n"
               "  eco on|off   turbo on|off   mute on|off   sleep 0..4\r\n");
        return 1;
    }
    const char *k = argv[1], *v = argv[2];
    uint8_t f[HISENSE_CMD_FRAME_LEN + 2];
    size_t n = 0;
    int vfield = 0, vexp = 0;

    if (!strcmp(k, "temp")) {
        int t = atoi(v);
        s_cmd.setpoint = (int8_t)t;
        n = hisense_build_command(&s_cmd, f, sizeof(f));
        vfield = RV_TEMP; vexp = t;
    } else if (!strcmp(k, "mode")) {
        HisenseMode m;
        if      (!strcmp(v, "cool")) m = HISENSE_MODE_COOL;
        else if (!strcmp(v, "heat")) m = HISENSE_MODE_HEAT;
        else if (!strcmp(v, "dry"))  m = HISENSE_MODE_DRY;
        else if (!strcmp(v, "fan"))  m = HISENSE_MODE_FAN;
        else if (!strcmp(v, "auto")) m = HISENSE_MODE_AUTO;
        else { printf("bad mode\r\n"); return 1; }
        s_cmd.mode = m;
        n = hisense_build_command(&s_cmd, f, sizeof(f));
        vfield = RV_MODE; vexp = (int)m;
    } else if (!strcmp(k, "fan")) {
        HisenseFanSpeed fs;
        if      (!strcmp(v, "auto"))    fs = HISENSE_FAN_AUTO;
        else if (!strcmp(v, "quiet"))   fs = HISENSE_FAN_QUIET;
        else if (!strcmp(v, "low"))     fs = HISENSE_FAN_LOW;
        else if (!strcmp(v, "medlow"))  fs = HISENSE_FAN_MED_LOW;
        else if (!strcmp(v, "mid"))     fs = HISENSE_FAN_MID;
        else if (!strcmp(v, "medhigh")) fs = HISENSE_FAN_MED_HIGH;
        else if (!strcmp(v, "high"))    fs = HISENSE_FAN_HIGH;
        else { printf("bad fan\r\n"); return 1; }
        s_cmd.fan = fs;
        n = hisense_build_command(&s_cmd, f, sizeof(f));
        vfield = RV_FAN_RAW; vexp = fan_expected_raw(fs);
    } else if (!strcmp(k, "swing")) {
        s_cmd.vswing = strcmp(v, "on") ? HISENSE_SWING_OFF : HISENSE_SWING_SWING;
        n = hisense_build_command(&s_cmd, f, sizeof(f));
    } else if (!strcmp(k, "eco")) {
        bool on = !strcmp(v, "on");
        s_cmd.feature = on ? HISENSE_FEATURE_ECO : HISENSE_FEATURE_ECO_OFF;
        n = hisense_build_command(&s_cmd, f, sizeof(f));
        vfield = RV_ECO; vexp = on ? 1 : 0;
    } else if (!strcmp(k, "turbo")) {
        bool on = !strcmp(v, "on");
        s_cmd.feature = on ? HISENSE_FEATURE_TURBO : HISENSE_FEATURE_NONE;
        n = hisense_build_command(&s_cmd, f, sizeof(f));
        vfield = RV_TURBO; vexp = on ? 1 : 0;
    } else if (!strcmp(k, "mute")) {
        bool on = !strcmp(v, "on");
        n = hisense_build_mute_frame(on, f, sizeof(f));
        vfield = RV_MUTE; vexp = on ? 1 : 0;
    } else if (!strcmp(k, "sleep")) {
        int p = atoi(v);
        n = hisense_build_sleep_frame((uint8_t)p, f, sizeof(f));
        vfield = RV_SLEEP; vexp = p > 0 ? 1 : 0;
    } else {
        printf("unknown key '%s'\r\n", k);
        return 1;
    }

    if (!n) { printf("build failed (bad value / buffer)\r\n"); return 1; }
    if (!hisense_send_frame(f, n)) { printf("send queue full\r\n"); return 1; }
    printf("[tx] %s %s -> %uB queued\r\n", k, v, (unsigned)n);
    if (vfield) recon_verify_arm(stdout, vfield, vexp);
    return 0;
}

static int cmd_power(int argc, char **argv)
{
    if (!is_master(stdout)) return 1;
    if (argc < 2) { printf("usage: power on|off\r\n"); return 1; }
    bool on = !strcmp(argv[1], "on");
    uint8_t f[HISENSE_CMD_FRAME_LEN + 2];
    size_t n = hisense_build_power_frame(on, f, sizeof(f));
    if (!n || !hisense_send_frame(f, n)) { printf("send failed\r\n"); return 1; }
    printf("[tx] power %s -> %uB queued\r\n", on ? "on" : "off", (unsigned)n);
    recon_verify_arm(stdout, RV_POWER, on ? 1 : 0);
    return 0;
}

static int cmd_producttype(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!is_master(stdout)) return 1;
    uint8_t f[64];
    size_t n = hisense_build_producttype_request(f, sizeof(f));
    if (!n || !hisense_send_frame(f, n)) { printf("send failed\r\n"); return 1; }
    HisenseFeatures ft;
    if (hisense_get_features(&ft) && ft.valid) {
        printf("features(last): cool_heat=%d ai=%d inf_fan=%d power_save=%d fan_mute=%d "
               "swing8=%d purify=%d humidity=%d disp=%u dr=%u\r\n",
               ft.cool_heat, ft.ai, ft.infinite_fan, ft.power_save, ft.fan_mute,
               ft.swing_dir_8, ft.purify, ft.humidity, ft.power_display, ft.demand_resp);
    } else {
        printf("producttype requested; no features parsed yet — retry in ~1s\r\n");
    }
    return 0;
}

static int cmd_raw(int argc, char **argv)
{
    if (argc < 2) { printf("usage: raw <hexbytes>   (master; sends a literal frame)\r\n"); return 1; }
    if (!is_master(stdout)) return 1;
    uint8_t f[128];
    // join remaining args so "raw f4 f5 .." works
    char joined[300]; joined[0] = 0;
    for (int i = 1; i < argc; i++) { strncat(joined, argv[i], sizeof(joined) - strlen(joined) - 1); }
    int n = parse_hex(joined, f, sizeof(f));
    if (n <= 0) { printf("bad hex\r\n"); return 1; }
    if (!hisense_send_frame(f, (size_t)n)) { printf("send queue full\r\n"); return 1; }
    printf("[tx] raw %dB queued. NOTE: the A/C's reply isn't surfaced here — run a\r\n"
           "     second unit in `mode tap` to capture it, or use `decode <hex>`.\r\n", n);
    return 0;
}

static int cmd_decode(int argc, char **argv)
{
    if (argc < 2) { printf("usage: decode <hexbytes>   (offline parse; any mode)\r\n"); return 1; }
    uint8_t f[256];
    char joined[600]; joined[0] = 0;
    for (int i = 1; i < argc; i++) { strncat(joined, argv[i], sizeof(joined) - strlen(joined) - 1); }
    int n = parse_hex(joined, f, sizeof(f));
    if (n <= 0) { printf("bad hex\r\n"); return 1; }
    HisenseState st;
    HisenseFeatures ft;
    if (hisense_parse_status(f, (size_t)n, &st) && st.valid) {
        printf("status frame (%dB):\r\n", n);
        recon_print_state(stdout, &st, false, NULL, 0);
    } else if (hisense_parse_features(f, (size_t)n, &ft) && ft.valid) {
        printf("producttype frame (%dB): cool_heat=%d ai=%d power_save=%d fan_mute=%d "
               "swing8=%d purify=%d\r\n", n, ft.cool_heat, ft.ai, ft.power_save,
               ft.fan_mute, ft.swing_dir_8, ft.purify);
    } else {
        printf("not a valid status/producttype frame (framing/checksum/len). "
               "class byte[13]=0x%02x\r\n", n > 13 ? f[13] : 0);
    }
    return 0;
}

// --- raw-frame delta finder: snap a baseline, toggle a feature, diff ---
static int raw_kind_arg(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "cmd")) return 1;
    return 0;   // default: status (A/C->module)
}

static int cmd_snap(int argc, char **argv)
{
    int k = raw_kind_arg(argc, argv);
    LOCK();
    int n = s_raw_last_len[k];
    if (n > 0) { memcpy(s_raw_base[k], s_raw_last[k], n); s_raw_base_len[k] = n; }
    UNLOCK();
    if (n > 0) printf("snapped %s baseline (%d bytes). toggle a feature, then `diff %s`.\r\n",
                      k ? "cmd" : "status", n, k ? "cmd" : "status");
    else printf("no %s frame captured yet (tap mode captures raw frames)\r\n", k ? "cmd" : "status");
    return 0;
}

static int cmd_diff(int argc, char **argv)
{
    int k = raw_kind_arg(argc, argv);
    uint8_t base[RAW_MAX], cur[RAW_MAX];
    int bl, cl;
    LOCK();
    bl = s_raw_base_len[k]; cl = s_raw_last_len[k];
    memcpy(base, s_raw_base[k], bl > 0 ? bl : 0);
    memcpy(cur,  s_raw_last[k], cl > 0 ? cl : 0);
    UNLOCK();
    if (bl <= 0) { printf("no baseline — run `snap %s` first\r\n", k ? "cmd" : "status"); return 1; }
    if (cl <= 0) { printf("no current %s frame captured\r\n", k ? "cmd" : "status"); return 1; }
    if (bl != cl) printf("note: length changed %d -> %d\r\n", bl, cl);
    int m = bl < cl ? bl : cl, changed = 0;
    for (int i = 0; i < m; i++) {
        if (base[i] != cur[i]) {
            printf("  off %3d (0x%02x): %02x -> %02x   bits ^%02x\r\n",
                   i, i, base[i], cur[i], (uint8_t)(base[i] ^ cur[i]));
            changed++;
        }
    }
    printf("diff %s: %d byte(s) changed\r\n", k ? "cmd" : "status", changed);
    return 0;
}

// #49: show the link session token the driver captured from the A/C reply
// envelope [9]/[10] (master mode), vs the 01 01 the driver currently hardcodes.
static int cmd_token(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint8_t hi = 0, lo = 0;
    bool seen = hisense_get_link_token(&hi, &lo);
    printf("link session token (reply [9]/[10]): %02x %02x  [%s]\r\n",
           hi, lo, seen ? "captured" : "seed — no reply captured");
    if (!seen) {
        printf("  run in `mode master` against the A/C to capture it\r\n");
    } else if (hi == 0x01 && lo == 0x01) {
        printf("  == the hardcoded 01 01: this unit is safe; #49 is the robustness fix\r\n");
    } else {
        printf("  != hardcoded 01 01: #49 capture+echo is REQUIRED for this unit\r\n");
    }
    return 0;
}

static int cmd_selftest(int argc, char **argv)
{
    (void)argc; (void)argv;
    int fails = recon_selftest(stdout);
    printf("selftest: %s (%d failure%s)\r\n", fails == 0 ? "PASS" : "FAIL",
           fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "clear")) { recon_wifi_clear(); printf("wifi creds cleared\r\n"); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "status")) { recon_wifi_status(stdout); return 0; }
    if (argc < 3) { printf("usage: wifi <ssid> <pass> | wifi clear | wifi status\r\n"); return 1; }
    esp_err_t e = recon_wifi_set_creds(argv[1], argv[2]);
    printf("wifi creds saved (%s); connecting...\r\n", e == ESP_OK ? "ok" : "err");
    return 0;
}

// ---------------------------------------------------------------------------
void recon_register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        { "mode",  "show/set capture mode: mode [tap|master] (reboots)", NULL, cmd_mode, NULL },
        { "stats", "frame + HAL byte counters, link health", NULL, cmd_stats, NULL },
        { "poll",  "show the latest decoded A/C status", NULL, cmd_poll, NULL },
        { "watch", "stream decoded frames: watch [on|off] [hex]", NULL, cmd_watch, NULL },
        { "set",   "master: set mode|temp|fan|swing|eco|turbo|mute|sleep (auto-verifies)", NULL, cmd_set, NULL },
        { "power", "master: power on|off", NULL, cmd_power, NULL },
        { "producttype", "master: poll + show A/C feature flags", NULL, cmd_producttype, NULL },
        { "token", "#49: show link session token captured from the A/C reply", NULL, cmd_token, NULL },
        { "raw",   "master: send a literal hex frame (protocol probing)", NULL, cmd_raw, NULL },
        { "decode","offline-decode a pasted hex frame (any mode)", NULL, cmd_decode, NULL },
        { "snap",  "tap: snapshot the latest raw frame: snap [status|cmd]", NULL, cmd_snap, NULL },
        { "diff",  "tap: show which bytes/bits changed vs snap: diff [status|cmd]", NULL, cmd_diff, NULL },
        { "selftest","run codec golden vectors on-target (no hardware)", NULL, cmd_selftest, NULL },
        { "wifi",  "wifi <ssid> <pass> | wifi clear | wifi status", NULL, cmd_wifi, NULL },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}
