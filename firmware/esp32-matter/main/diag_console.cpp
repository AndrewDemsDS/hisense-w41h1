// Embedded diagnostic telnet console (:2323) for the esp-matter A/C node. Lets the
// node stay on Matter while exposing the recon-style bench commands over TCP. Design
// for Matter coexistence: diag_on_status() only snapshots (no socket I/O under the
// CHIP stack lock); the `watch` stream and the TCP accept loop run in their own tasks.
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_console.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "diag_console.h"

static const char *TAG = "diag";
#define DIAG_TCP_PORT 2323

static SemaphoreHandle_t s_mtx;
static HisenseState      s_snap;
static bool              s_have;
static uint32_t          s_frames;
static FILE             *s_watch;      // session sink for `watch`, or NULL
static bool              s_watch_on;

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

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

static void print_state(FILE *out, const HisenseState *st)
{
    fprintf(out,
        " power=%d mode=%s set=%dC in=%dC out=%dC coil=%dC fan=0x%02x comp=%dHz\r\n"
        " eco=%d turbo=%d mute=%d sleep=%d(0x%02x) vswing=%d hswing=%d heatrelay=%d I=%u V=%u\r\n",
        st->power_on, mode_name(st->mode), st->setpoint_c, st->indoor_temp_c,
        st->outdoor_temp_c, st->coil_temp_c, st->fan_raw, st->compressor_freq,
        st->eco_on, st->turbo_on, st->mute_on, st->sleep_on, st->sleep_raw,
        st->vswing_on, st->hswing_on, st->heat_relay_on,
        (unsigned)st->current_raw, (unsigned)st->voltage_raw);
}

extern "C" void diag_on_status(const HisenseState *st)
{
    if (!st || !st->valid || !s_mtx) return;
    LOCK();
    s_snap = *st; s_have = true; s_frames++;
    UNLOCK();
}

// ---- commands (output via per-task stdout = the client socket) --------------
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
        if (hi < 0) hi = v; else { if (n >= cap) return -1; out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    return hi < 0 ? n : -1;
}

static int cmd_token(int, char **)
{
    uint8_t hi = 0, lo = 0;
    bool seen = hisense_get_link_token(&hi, &lo);
    printf("link session token (reply [9]/[10]): %02x %02x  [%s]\r\n",
           hi, lo, seen ? "captured" : "seed — no reply yet");
    if (seen && (hi != 0x01 || lo != 0x01))
        printf("  != hardcoded 01 01 -> #49 capture+echo REQUIRED for this unit\r\n");
    else if (seen)
        printf("  == hardcoded 01 01 -> this unit is safe; #49 is a robustness fix\r\n");
    return 0;
}

static int cmd_poll(int, char **)
{
    LOCK(); HisenseState s = s_snap; bool h = s_have; uint32_t f = s_frames; UNLOCK();
    if (!h) { printf("no status decoded yet (is the A/C bus connected + powered?)\r\n"); return 0; }
    printf("last of %u frames:\r\n", (unsigned)f);
    print_state(stdout, &s);
    return 0;
}

static int cmd_watch(int argc, char **argv)
{
    bool on = !(argc >= 2 && !strcmp(argv[1], "off"));
    LOCK(); s_watch = on ? stdout : NULL; s_watch_on = on; UNLOCK();
    printf("watch %s\r\n", on ? "on (streaming ~1Hz; `watch off` to stop)" : "off");
    return 0;
}

static int cmd_decode(int argc, char **argv)
{
    if (argc < 2) { printf("usage: decode <hexbytes>\r\n"); return 1; }
    uint8_t f[256]; char joined[600]; joined[0] = 0;
    for (int i = 1; i < argc; i++) strncat(joined, argv[i], sizeof(joined) - strlen(joined) - 1);
    int n = parse_hex(joined, f, sizeof(f));
    if (n <= 0) { printf("bad hex\r\n"); return 1; }
    HisenseState st; HisenseFeatures ft;
    if (hisense_parse_status(f, (size_t)n, &st) && st.valid) { printf("status (%dB):\r\n", n); print_state(stdout, &st); }
    else if (hisense_parse_features(f, (size_t)n, &ft) && ft.valid)
        printf("producttype (%dB): cool_heat=%d ai=%d power_save=%d fan_mute=%d swing8=%d purify=%d\r\n",
               n, ft.cool_heat, ft.ai, ft.power_save, ft.fan_mute, ft.swing_dir_8, ft.purify);
    else printf("not a valid status/producttype frame (class byte[13]=0x%02x)\r\n", n > 13 ? f[13] : 0);
    return 0;
}

// Compact codec self-check (subset of the host golden vectors).
static int cmd_selftest(int, char **)
{
    int fails = 0;
    uint8_t f[64];
    HisenseCommand c = { HISENSE_MODE_COOL, 22, false, HISENSE_FAN_LOW,
                         HISENSE_SWING_OFF, HISENSE_SWING_OFF, HISENSE_FEATURE_ECO, false };
    size_t n = hisense_build_command(&c, f, sizeof(f));
    if (!(n && f[16] == 0x0B && f[18] == 0x50 && f[19] == 0x2D && f[33] == 0x30)) fails++;
    uint8_t hi = 0, lo = 0;
    // token = reply envelope [9]/[10]; put the probe bytes there (index 9 and 10).
    uint8_t reply[16] = {0xF4,0xF5,0x01,0x40,0x97,0x01,0x00,0xFE,0x01,0xAB,0xCD,0x00,0x00,0x66,0x00,0x01};
    if (!(hisense_link_token_from_reply(reply, 16, &hi, &lo) && hi == 0xAB && lo == 0xCD)) fails++;
    printf("selftest: %s (%d failure%s)\r\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

// ---- watch streamer (own task; no CHIP lock) --------------------------------
static void watch_task(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        LOCK();
        FILE *w = s_watch_on ? s_watch : NULL;
        HisenseState s = s_snap; bool h = s_have; uint32_t f = s_frames;
        UNLOCK();
        if (w && h) { fprintf(w, "[frame %u]\r\n", (unsigned)f); print_state(w, &s); fflush(w); }
    }
}

// ---- TCP telnet console -----------------------------------------------------
static int read_line(int fd, char *line, int cap)
{
    int n = 0;
    for (;;) {
        uint8_t b; int r = recv(fd, &b, 1, 0);
        if (r <= 0) return -1;
        if (b == 0xFF) { uint8_t sk[2]; recv(fd, sk, 2, 0); continue; }   // Telnet IAC
        if (b == '\r') continue;
        if (b == '\n') { line[n] = 0; return n; }
        if (b == 0x08 || b == 0x7f) { if (n > 0) n--; continue; }
        if (n < cap - 1) line[n++] = (char)b;
    }
}

static void handle_client(int fd)
{
    FILE *sf = fdopen(fd, "w");
    if (!sf) { close(fd); return; }
    setvbuf(sf, NULL, _IONBF, 0);
    FILE *so = stdout, *se = stderr;
    stdout = sf; stderr = sf;
    fprintf(sf, "esp-matter A/C node — diagnostic console. type 'help'.\r\nesp32-recon> ");
    char line[256];
    for (;;) {
        int len = read_line(fd, line, sizeof(line));
        if (len < 0) break;
        if (len == 0) { fprintf(sf, "esp32-recon> "); continue; }
        if (!strcmp(line, "exit") || !strcmp(line, "quit")) break;
        int ret = 0;
        esp_err_t e = esp_console_run(line, &ret);
        if (e == ESP_ERR_NOT_FOUND) fprintf(sf, "unknown command (try 'help')\r\n");
        fprintf(sf, "esp32-recon> ");
    }
    LOCK(); if (s_watch == sf) { s_watch = NULL; s_watch_on = false; } UNLOCK();
    stdout = so; stderr = se;
    fclose(sf);
}

static void tcp_task(void *)
{
    // IPv6 dual-stack: Matter A/C nodes are often on an IPv6-only IoT VLAN (fd00::/..),
    // so bind AF_INET6 + in6addr_any with IPV6_V6ONLY off -> reachable over IPv6 AND
    // (where present) IPv4-mapped. Connect with: nc <node-ipv6> 2323
    int ls = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) { ESP_LOGE(TAG, "socket() failed"); vTaskDelete(NULL); return; }
    int yes = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int off = 0; setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));  // best-effort dual-stack
    struct sockaddr_in6 a = {}; a.sin6_family = AF_INET6; a.sin6_port = htons(DIAG_TCP_PORT);
    a.sin6_addr = in6addr_any;
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 1) < 0) {
        ESP_LOGE(TAG, "bind/listen :%d failed", DIAG_TCP_PORT); close(ls); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "diagnostic console listening on :%d (IPv6 dual-stack)", DIAG_TCP_PORT);
    for (;;) {
        struct sockaddr_in6 cli; socklen_t cl = sizeof(cli);
        int fd = accept(ls, (struct sockaddr *)&cli, &cl);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        ESP_LOGI(TAG, "diag client connected");
        handle_client(fd);
        ESP_LOGI(TAG, "diag client disconnected");
    }
}

extern "C" void diag_console_start(void)
{
    s_mtx = xSemaphoreCreateMutex();

    esp_console_config_t cc = {};
    cc.max_cmdline_length = 256;
    cc.max_cmdline_args   = 12;
    if (esp_console_init(&cc) == ESP_OK) {
        esp_console_register_help_command();
        const esp_console_cmd_t cmds[] = {
            { "token",    "#49: link session token captured from the A/C reply", NULL, cmd_token, NULL, NULL },
            { "poll",     "show the latest decoded A/C status", NULL, cmd_poll, NULL, NULL },
            { "watch",    "stream decoded frames ~1Hz: watch [on|off]", NULL, cmd_watch, NULL, NULL },
            { "decode",   "offline-decode a pasted hex frame", NULL, cmd_decode, NULL, NULL },
            { "selftest", "compact on-target codec self-check", NULL, cmd_selftest, NULL, NULL },
        };
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) esp_console_cmd_register(&cmds[i]);
    }

    xTaskCreate(watch_task, "diag_watch", 4096, NULL, 3, NULL);
    xTaskCreate(tcp_task,   "diag_tcp",   6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "diag console on :%d — reach it at the node's Matter IPv6 (nc <ipv6> %d)",
             DIAG_TCP_PORT, DIAG_TCP_PORT);
}
