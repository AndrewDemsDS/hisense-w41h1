// Embedded diagnostic telnet console (:2323) for the esp-matter A/C node. Lets the
// node stay on Matter while exposing the recon-style bench commands over TCP. Design
// for Matter coexistence: diag_on_status() only snapshots (no socket I/O under the
// CHIP stack lock); the `watch` stream and the TCP accept loop run in their own tasks.
#include <stdio.h>
#include <stdlib.h>
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
        " eco=%d turbo=%d mute=%d sleep=%d(0x%02x) vswing=%d hswing=%d heatrelay=%d I=%u V=%u\r\n"
        " unit=%s (#5, byte26 bit1; UNVERIFIED -- flip the remote to F and diff `raw`)\r\n"
        " link_req=0x%02x (\"77\" bits in the 0x1E reply: 0x08 reconfig / 0x20 smartcfg;\r\n"
        "   0x00 = the A/C is NOT asking to recommission -- if it stays 0 while you press the\r\n"
        "   remote sequence, the request never reaches us and the fault is upstream of Matter)\r\n",
        st->power_on, mode_name(st->mode), st->setpoint_c, st->indoor_temp_c,
        st->outdoor_temp_c, st->coil_temp_c, st->fan_raw, st->compressor_freq,
        st->eco_on, st->turbo_on, st->mute_on, st->sleep_on, st->sleep_raw,
        st->vswing_on, st->hswing_on, st->heat_relay_on,
        (unsigned)st->current_raw, (unsigned)st->voltage_raw,
        st->temp_unit_f ? "F" : "C",
        (unsigned) hisense_get_last_link_req());
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
    // #49: report BOTH rival readings of outbound envelope [7]/[8], from the SAME
    // DevType (0x0A) reply -- the frame stock samples and the one v10207 died on.
    uint8_t dhi = 0, dlo = 0, ehi = 0, elo = 0;
    bool dseen = hisense_get_link_token(&dhi, &dlo);        // inner [3]/[4] = device-type
    bool eseen = hisense_get_devtype_envelope(&ehi, &elo);  // envelope [9]/[10]

    printf("#49 outbound envelope [7]/[8] sources, from the 0x0A DevType reply:\r\n");
    printf("  device-type  inner [3]/[4] : %02x %02x  [%s]   <- USED (stock 0x9b6f2194)\r\n",
           dhi, dlo, dseen ? "learned from A/C" : "DEFAULT 01 01 — no 0x0A reply parsed");
    if (eseen)
        printf("  session tok  envel [9]/[10]: %02x %02x  [captured]      <- v10207 stamped this\r\n",
               ehi, elo);
    else
        printf("  session tok  envel [9]/[10]: --      [no 0x0A reply seen yet]\r\n");

    if (dseen && eseen) {
        if (dhi != ehi || dlo != elo)
            printf("  VERDICT: sources DIFFER on the 0x0A frame -> \"session token\" reading is\r\n"
                   "           disproven on the wire; device-type is correct. (docs/10 §4.5)\r\n");
        else
            printf("  VERDICT: sources agree on this frame -- v10207's failure is NOT explained\r\n"
                   "           by this reply; re-open the root cause. (docs/10 §4.5)\r\n");
    } else if (!dseen) {
        printf("  VERDICT: the learning path did NOT fire -- outbound [7]/[8] is riding the\r\n"
               "           01 01 default, so #49's robustness fix is still unproven here.\r\n");
    }
    return 0;
}

// RE docs/11 §6 Q1: does this A/C answer the 0x66/40 ProductType poll at all? It gates the whole
// model-capability story — with no reply, stock's STRUCT_A stays at its 9-entry static baseline
// (fan/swing/eco all `supported=0`) and every runtime refinement is moot.
static int cmd_features(int, char **)
{
    HisenseFeatures ft;
    if (!hisense_get_features(&ft)) {
        printf("0x66/40 ProductType: NO reply parsed yet.\r\n"
               "  -> the A/C has not answered the ProductType poll on this boot.\r\n"
               "  -> RE docs/11 §5: STRUCT_A would stay at the 9-entry static baseline.\r\n");
        return 0;
    }
    printf("0x66/40 ProductType: REPLY PARSED — the A/C DOES answer. (docs/11 §6 Q1)\r\n");
    printf("  cool_heat=%d ai=%d infinite_fan=%d power_save/eco=%d fan_mute/quiet=%d\r\n",
           ft.cool_heat, ft.ai, ft.infinite_fan, ft.power_save, ft.fan_mute);
    printf("  swing_dir_8=%d swing_follow=%d humidity=%d power_display=%u demand_resp=%u\r\n",
           ft.swing_dir_8, ft.swing_follow, ft.humidity,
           (unsigned)ft.power_display, (unsigned)ft.demand_resp);
    // Names corrected 2026-07-16 (RE docs/10 §5a): same byte reads, right names.
    printf("  purify=%d ([0x0A]&0x08)   8heat=%d ([0x0D]&0x80, 8C frost-guard)\r\n",
           ft.purify, ft.heat_8c);
    if (ft.ext_valid) {
        printf("  q_display=%d ([0x1A]&0x40)  enable_8heat=%d ([0x1A]&0x04)  "
               "trans_102_64=%d ([0x19]&0x08 -> profile '199')\r\n",
               ft.q_display, ft.enable_8heat, ft.trans_102_64);
    } else {
        printf("  q_display/enable_8heat/trans_102_64: UNKNOWN "
               "(reply %uB, need >39B to carry bytes 38/39)\r\n", (unsigned)ft.reply_len);
    }
    printf("  raw 0x66/40 reply length = %uB\r\n", (unsigned)ft.reply_len);
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

// Bench probe for #52 (display byte unknown). Sends the CURRENT command frame with one
// payload byte replaced, so a miss is a no-op rather than a surprise mode/setpoint change.
//
// Deliberately single-shot: there is no auto-sweep. hisense_parse_status() decodes no
// display state (power_display/q_display are ProductType CAPABILITY bits, not live
// status), so the only oracle is the panel LED and every probe needs a human looking at
// the unit before the next one goes out. An unattended sweep would just log offsets.
static int cmd_tx(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: tx <offset> <value>   (both accept 0x.. or decimal)\r\n"
               "  payload offsets %u..%u; header and checksum/terminator are rejected\r\n"
               "  current suspect (#52): tx 36 0xC0  then  tx 36 0x40\r\n"
               "  watch the PANEL after each probe -- there is no status read-back\r\n",
               (unsigned) HISENSE_CMD_HEADER_LEN, (unsigned) HISENSE_CMD_CHK_OFFSET - 1);
        return 1;
    }
    long off = strtol(argv[1], NULL, 0);
    long val = strtol(argv[2], NULL, 0);
    if (val < 0 || val > 0xFF) { printf("value out of range (0..0xFF)\r\n"); return 1; }

    switch (diag_tx_override((int) off, (uint8_t) val)) {
    case -1:
        printf("rejected: offset %ld is outside the payload [%u,%u)\r\n",
               off, (unsigned) HISENSE_CMD_HEADER_LEN, (unsigned) HISENSE_CMD_CHK_OFFSET);
        return 1;
    case -2:
        printf("TX queue full -- NOT sent, retry\r\n");
        return 1;
    case -3: {
        // The offset was fine; the SHADOW is unbuildable. Almost always an out-of-range
        // setpoint resynced from the A/C, which drops every combined frame, not just this
        // probe. Print the state and the fix rather than making the next person derive it.
        int mode = -1, sp = -1, f = -1;
        diag_get_cmd_state(&mode, &sp, &f);
        printf("rejected: the command SHADOW is invalid, so the builder refused.\r\n"
               "  shadow: mode=%d setpoint=%d fahrenheit=%d (valid setpoint: %d..%d C)\r\n"
               "  the offset was fine -- EVERY combined command is being dropped, not just\r\n"
               "  this probe. Fix: write the setpoint attribute matching the CURRENT mode\r\n"
               "  (HeatSetpoint while in heat), which reloads the shadow with a legal value.\r\n",
               mode, sp, f, HISENSE_SETPOINT_MIN_C, HISENSE_SETPOINT_MAX_C);
        return 1;
    }
    default:
        break;
    }
    printf("sent: frame[%ld] = 0x%02lX (every other byte = current A/C state)\r\n"
           "  -> check the panel now; any change is attributable to this byte alone\r\n"
           "  -> to undo, drive the unit normally from HA (rebuilds the baseline frame)\r\n",
           off, (unsigned long) val);
    return 0;
}

// #38: decoded f_e_* fault bits. Mapping derived from the stock firmware capability table
// plus the extractor at 0x9b6f8ac8 (RE docs/10 §7), NOT yet seen against a unit actually
// reporting a fault, so the raw bytes are printed alongside the decode. A healthy unit
// reads all-zero, which on its own confirms nothing.
static int cmd_faults(int, char **)
{
    HisenseFaults f;
    if (!hisense_get_faults(&f)) {
        printf("no fault data yet (no status frame long enough parsed this boot)\r\n");
        return 0;
    }
    printf("raw: [%d]=0x%02x [%d]=0x%02x [%d]=0x%02x [%d]=0x%02x  ->  %s\r\n",
           HISENSE_FAULT_BYTE_INDOOR,  f.raw_indoor,
           HISENSE_FAULT_BYTE_MODULE,  f.raw_module,
           HISENSE_FAULT_BYTE_OUTDOOR, f.raw_outdoor,
           HISENSE_FAULT_BYTE_PROTECT, f.raw_protect,
           f.any ? "FAULT(S) PRESENT (map is PROVISIONAL -- cross-check `raw`)" : "all clear");
    if (!f.any) {
        printf("  (all-zero is expected on a healthy unit and does NOT validate the map)\r\n");
        return 0;
    }
    struct { bool v; const char *n; } named[] = {
        { f.in_temp,"indoor temp sensor" },      { f.in_coil_temp,"indoor coil sensor" },
        { f.in_humidity,"indoor humidity" },     { f.water_full,"condensate tray full" },
        { f.in_fan_motor,"indoor fan motor / up-machine" },
        { f.grille,"grille / dw-machine" },      { f.in_vzero,"zero-cross detect" },
        { f.in_com,"indoor<->outdoor comms" },   { f.in_display,"display board" },
        { f.in_keys,"keypad" },                  { f.in_wifi,"wifi module" },
        { f.in_ele,"indoor electrical" },        { f.in_eeprom,"indoor eeprom" },
        { f.out_eeprom,"outdoor eeprom" },       { f.out_coil_temp,"outdoor coil sensor" },
        { f.out_gas_temp,"outdoor gas sensor" }, { f.out_temp,"outdoor temp sensor" },
        { f.over_temp,"over-temperature protection (hot or cold)" },
    };
    for (auto &x : named) if (x.v) printf("  FAULT: %s\r\n", x.n);
    // A set bit with no name still matters: warn rather than imply we understood the byte.
    // Byte 66 bit 7 is a mode flag, not a fault, so it must not be reported as an unnamed
    // fault bit either (see HISENSE_FAULT_NONFAULT_PROTECT).
    uint8_t known[4] = { 0xFF, 0xF8, 0x78,
                         (uint8_t)(0x10 | HISENSE_FAULT_NONFAULT_PROTECT) };
    uint8_t rawb[4]  = { f.raw_indoor, f.raw_module, f.raw_outdoor, f.raw_protect };
    int off[4] = { HISENSE_FAULT_BYTE_INDOOR, HISENSE_FAULT_BYTE_MODULE,
                   HISENSE_FAULT_BYTE_OUTDOOR, HISENSE_FAULT_BYTE_PROTECT };
    for (int i = 0; i < 4; i++) {
        uint8_t unknown = (uint8_t)(rawb[i] & ~known[i]);
        if (unknown) printf("  UNNAMED fault bits 0x%02x in byte %d\r\n", unknown, off[i]);
    }
    return 0;
}

// Hexdump the last status frame. The point of this is falsifiability: the fault map is
// derived from firmware, so being able to read the actual bytes is what lets someone
// prove it wrong. Also the general tool for mapping any still-unknown status field.
/* Hexdump the last 0x1E LINK reply. This is the frame that is SUPPOSED to carry the "77"
 * recommission request in payload[4] (byte 17). On this unit seven remote presses never moved
 * that byte, so dump the whole frame, press the sequence, dump again, and diff: whichever byte
 * actually changes is the real request. Beats trusting an RE bit map that has already been
 * wrong elsewhere in this protocol. */
static int cmd_link(int, char **)
{
    uint8_t f[40];
    uint8_t n = hisense_get_last_link_frame(f, sizeof(f));
    if (!n) { printf("no 0x1E LINK reply captured yet\r\n"); return 0; }
    printf("last 0x1E LINK reply, %u bytes:\r\n", (unsigned) n);
    for (uint8_t i = 0; i < n; i += 16) {
        printf("  %3u:", (unsigned) i);
        for (uint8_t k = i; k < i + 16 && k < n; k++) printf(" %02x", f[k]);
        printf("\r\n");
    }
    printf("  payload[4] = byte[17] = 0x%02x   (documented \"77\" bits: 0x08 reconfig / 0x20 smartcfg)\r\n",
           n > 17 ? f[17] : 0);
    printf("  masked link_req = 0x%02x\r\n", (unsigned) hisense_get_last_link_req());
    return 0;
}

static int cmd_raw(int, char **)
{
    uint8_t f[HISENSE_RAW_SNAPSHOT_LEN];
    size_t n = hisense_get_last_status_frame(f, sizeof(f));
    if (!n) { printf("no status frame captured yet\r\n"); return 0; }
    printf("last status frame, %u bytes:\r\n", (unsigned) n);
    for (size_t i = 0; i < n; i += 16) {
        printf("  %3u:", (unsigned) i);
        for (size_t k = i; k < i + 16 && k < n; k++) printf(" %02x", f[k]);
        printf("\r\n");
    }
    printf("  fault bytes: [%d]=0x%02x [%d]=0x%02x [%d]=0x%02x [%d]=0x%02x\r\n",
           HISENSE_FAULT_BYTE_INDOOR,  n > HISENSE_FAULT_BYTE_INDOOR  ? f[HISENSE_FAULT_BYTE_INDOOR]  : 0,
           HISENSE_FAULT_BYTE_MODULE,  n > HISENSE_FAULT_BYTE_MODULE  ? f[HISENSE_FAULT_BYTE_MODULE]  : 0,
           HISENSE_FAULT_BYTE_OUTDOOR, n > HISENSE_FAULT_BYTE_OUTDOOR ? f[HISENSE_FAULT_BYTE_OUTDOOR] : 0,
           HISENSE_FAULT_BYTE_PROTECT, n > HISENSE_FAULT_BYTE_PROTECT ? f[HISENSE_FAULT_BYTE_PROTECT] : 0);
    printf("  (base 15 is PROVISIONAL -- confirm by diffing this dump against a real fault)\r\n");
    return 0;
}

// #12: why did this module last reboot? No serial cable required.
//
// Added after a module went unresponsive following an OTA and had to be recovered by
// powering it from USB. The leading hypothesis is the A/C 5 V rail sagging under the
// current draw of flash writes plus Wi-Fi TX, but there was NO evidence either way.
// ESP_RST_BROWNOUT here would confirm it; ESP_RST_PANIC would point somewhere else
// entirely. Either beats guessing.
static int cmd_bootreason(int, char **)
{
    int code = -1;
    const char *text = "unavailable";
    diag_get_boot_reason(&code, &text);
    printf("last boot: %s (code %d)\r\n", text, code);
    printf("  ESP_RST_SW after our own OTA is expected and healthy.\r\n"
           "  ESP_RST_BROWNOUT means the supply sagged: that is the A/C 5 V rail\r\n"
           "  hypothesis confirmed, and an argument for raising the brownout threshold\r\n"
           "  (currently the LOWEST, so a sag can hang the chip instead of resetting it).\r\n");
    return 0;
}

// Compact codec self-check (subset of the host golden vectors).
static int cmd_selftest(int, char **)
{
    int fails = 0;
    uint8_t f[64];
    HisenseCommand c = { HISENSE_MODE_COOL, 22, false, HISENSE_FAN_LOW,
                         HISENSE_SWING_OFF, HISENSE_SWING_OFF, HISENSE_FEATURE_ECO, HISENSE_DISPLAY_NOCHANGE };
    size_t n = hisense_build_command(&c, f, sizeof(f));
    if (!(n && f[16] == 0x0B && f[18] == 0x50 && f[19] == 0x2D && f[33] == 0x30)) fails++;
    uint8_t hi = 0, lo = 0;
    // #49: outbound [7]/[8] comes from the 0x0A reply's INNER [3]/[4] (= frame [16]/[17]),
    // not the envelope [9]/[10] (0x02/0x03 here) -- stamping the latter killed the link.
    uint8_t reply[20] = {0xF4,0xF5,0x01,0x40,0x0B,0x01,0x00,0xFE,0x01,0x02,
                         0x03,0x04,0x00,0x0A,0x00,0x00,0xAB,0xCD,0xF4,0xFB};
    if (!(hisense_devtype_from_reply(reply, 20, &hi, &lo) && hi == 0xAB && lo == 0xCD)) fails++;
    reply[13] = 0x66;   // a status reply must NOT supply a device type
    if (hisense_devtype_from_reply(reply, 20, &hi, &lo)) fails++;
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
            { "features", "#49/docs11: 0x66/40 ProductType reply — did the A/C answer?", NULL, cmd_features, NULL, NULL },
            { "watch",    "stream decoded frames ~1Hz: watch [on|off]", NULL, cmd_watch, NULL, NULL },
            { "decode",   "offline-decode a pasted hex frame", NULL, cmd_decode, NULL, NULL },
            { "selftest", "compact on-target codec self-check", NULL, cmd_selftest, NULL, NULL },
            { "faults",   "#38: decoded f_e_* fault bits + raw fault bytes", NULL, cmd_faults, NULL, NULL },
            { "bootreason", "#12: why the module last rebooted (brownout / panic / OTA)",
                          NULL, cmd_bootreason, NULL, NULL },
            { "raw",      "hexdump the last status frame (falsifies the fault map)", NULL, cmd_raw, NULL, NULL },
            { "link",     "hexdump the last 0x1E LINK reply (find the real \"77\" bit)", NULL, cmd_link, NULL, NULL },
            { "tx",       "#52 bench probe: tx <offset> <value> — current frame, one byte overridden",
                          NULL, cmd_tx, NULL, NULL },
        };
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) esp_console_cmd_register(&cmds[i]);
    }

    xTaskCreate(watch_task, "diag_watch", 4096, NULL, 3, NULL);
    xTaskCreate(tcp_task,   "diag_tcp",   6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "diag console on :%d — reach it at the node's Matter IPv6 (nc <ipv6> %d)",
             DIAG_TCP_PORT, DIAG_TCP_PORT);
}
