/* ---------------------------------------------------------------------------
 * AmebaZ2 diagnostic console (:2323) -- issue #23 / docs/11 §6 Q1b.
 *
 * The ESP32 build has a `:2323` console; the AmebaZ2 had NO remote diagnostic
 * surface at all. Its only output was ChipLogProgress over UART, which needs
 * physical access to the module, so a capability question about an AmebaZ2 unit
 * could not be answered without opening the A/C. That is the gap this closes:
 * `features` reads the cached 0x66/40 ProductType reply per unit, which is the
 * measurement docs/11 §5.1's runtime-gating rule depends on.
 *
 * NOT a port of the ESP32 diag_console.cpp. That one leans on esp_console plus
 * per-task stdout redirection to the client socket, and neither exists here, so
 * this is a small line-oriented REPL that formats into a buffer and writes to
 * the socket directly. Same questions answered, much less machinery.
 *
 * DEBUG FLAVOUR ONLY (#22). This listener has no authentication and is reachable
 * from anywhere on the L2. It is a bench instrument and must never ship in a
 * release image, so everything here is inside HISENSE_DEBUG_BUILD.
 *
 * Included by matter_drivers.cpp on purpose: a new file added to SRC_CPP is
 * never compiled or linked by the ameba makefiles (a dep-tracking bug documented
 * in CLAUDE.md), so it rides an always-rebuilt TU instead. Do NOT add it to
 * SRC_CPP "to fix" the build.
 * ------------------------------------------------------------------------- */
#pragma once

// Flavour selector. `ota-release.sh build --debug` generates hisense_flavour.h into the SDK
// example dir defining HISENSE_DEBUG_BUILD; a normal build removes it, so release is the default
// and you cannot ship the console by forgetting a flag. -DHISENSE_DEBUG_BUILD also works.
#if defined(__has_include)
#  if __has_include("hisense_flavour.h")
#    include "hisense_flavour.h"
#  endif
#endif

#ifdef HISENSE_DEBUG_BUILD

#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>

#define HISENSE_DIAG_PORT      2323
#define HISENSE_DIAG_BUF       512

// Last status frame, snapshotted from the bus task. Plain struct: the bus task
// writes it and the console task reads it, and a torn read here would only
// misprint a diagnostic line, never affect control.
static HisenseState s_diag_status;
static bool         s_diag_have_status;
static uint32_t     s_diag_frames;

static void hisense_diag_on_status(const HisenseState *st)
{
    if (st == NULL || !st->valid) return;
    s_diag_status     = *st;
    s_diag_have_status = true;
    s_diag_frames++;
}

static void diag_say(int sock, const char *s)
{
    if (s != NULL) lwip_write(sock, s, strlen(s));
}

static void diag_cmd_features(int sock)
{
    char b[HISENSE_DIAG_BUF];
    HisenseFeatures ft;

    if (!hisense_get_features(&ft)) {
        diag_say(sock,
            "0x66/40 ProductType: NO reply parsed yet.\r\n"
            "  -> the A/C has not answered the ProductType poll on this boot.\r\n");
        return;
    }
    snprintf(b, sizeof(b),
        "0x66/40 ProductType: REPLY PARSED (docs/11 6 Q1b, per UNIT not per model)\r\n"
        "  cool_heat=%d ai=%d infinite_fan=%d power_save/eco=%d fan_mute/quiet=%d\r\n"
        "  swing_dir_8=%d swing_follow=%d humidity=%d power_display=%u demand_resp=%u\r\n"
        "  purify=%d ([0x0A]&0x08)   8heat=%d ([0x0D]&0x80, 8C frost-guard)\r\n",
        ft.cool_heat, ft.ai, ft.infinite_fan, ft.power_save, ft.fan_mute,
        ft.swing_dir_8, ft.swing_follow, ft.humidity,
        (unsigned) ft.power_display, (unsigned) ft.demand_resp,
        ft.purify, ft.heat_8c);
    diag_say(sock, b);

    if (ft.ext_valid) {
        snprintf(b, sizeof(b),
            "  q_display=%d ([0x1A]&0x40)  enable_8heat=%d ([0x1A]&0x04)  trans_102_64=%d ([0x19]&0x08)\r\n",
            ft.q_display, ft.enable_8heat, ft.trans_102_64);
    } else {
        snprintf(b, sizeof(b),
            "  q_display/enable_8heat/trans_102_64: UNKNOWN (reply %uB, need >39B)\r\n",
            (unsigned) ft.reply_len);
    }
    diag_say(sock, b);

    snprintf(b, sizeof(b), "  raw 0x66/40 reply length = %uB\r\n", (unsigned) ft.reply_len);
    diag_say(sock, b);
}

static void diag_cmd_poll(int sock)
{
    char b[HISENSE_DIAG_BUF];

    if (!s_diag_have_status) {
        diag_say(sock, "no status decoded yet (is the A/C bus connected + powered?)\r\n");
        return;
    }
    snprintf(b, sizeof(b),
        "last of %u frames:\r\n"
        " power=%d mode=%d set=%dC in=%dC out=%dC coil=%dC fan=0x%02x comp=%dHz\r\n"
        " eco=%d turbo=%d mute=%d sleep=%d vswing=%d hswing=%d heatrelay=%d\r\n"
        " unit=%s (#5, byte26 bit1; UNVERIFIED)\r\n",
        (unsigned) s_diag_frames,
        s_diag_status.power_on, (int) s_diag_status.mode, s_diag_status.setpoint_c,
        s_diag_status.indoor_temp_c, s_diag_status.outdoor_temp_c, s_diag_status.coil_temp_c,
        s_diag_status.fan_raw, s_diag_status.compressor_freq,
        s_diag_status.eco_on, s_diag_status.turbo_on, s_diag_status.mute_on,
        s_diag_status.sleep_on, s_diag_status.vswing_on, s_diag_status.hswing_on,
        s_diag_status.heat_relay_on,
        s_diag_status.temp_unit_f ? "F" : "C");
    diag_say(sock, b);
}

static void diag_handle_line(int sock, char *line)
{
    while (*line == ' ') line++;
    char *e = line + strlen(line);
    while (e > line && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) *--e = 0;

    if (line[0] == 0)                       return;
    if (!strcmp(line, "features"))          { diag_cmd_features(sock); return; }
    if (!strcmp(line, "poll"))              { diag_cmd_poll(sock);     return; }
    if (!strcmp(line, "version")) {
        char b[128];
        snprintf(b, sizeof(b), "AmebaZ2 Hisense A/C, softwareVersion %u (DEBUG flavour)\r\n",
                 (unsigned) CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION);
        diag_say(sock, b);
        return;
    }
    if (!strcmp(line, "help")) {
        diag_say(sock,
            "commands: features | poll | version | help | quit\r\n"
            "  features  cached 0x66/40 ProductType capability flags for THIS unit\r\n"
            "  poll      last decoded A/C status frame\r\n");
        return;
    }
    diag_say(sock, "unknown command (try `help`)\r\n");
}

static void hisense_diag_console_task(void *arg)
{
    (void) arg;
    struct sockaddr_in a;
    int ls = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { ChipLogError(DeviceLayer, "diag: socket failed"); vTaskDelete(NULL); return; }

    int one = 1;
    lwip_setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&a, 0, sizeof(a));
    a.sin_family      = AF_INET;
    a.sin_port        = htons(HISENSE_DIAG_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    if (lwip_bind(ls, (struct sockaddr *) &a, sizeof(a)) < 0 || lwip_listen(ls, 1) < 0) {
        ChipLogError(DeviceLayer, "diag: bind/listen :%d failed", HISENSE_DIAG_PORT);
        lwip_close(ls); vTaskDelete(NULL); return;
    }
    ChipLogProgress(DeviceLayer, "diag console listening on :%d (DEBUG build)", HISENSE_DIAG_PORT);

    for (;;) {
        int cs = lwip_accept(ls, NULL, NULL);
        if (cs < 0) continue;

        diag_say(cs, "AmebaZ2 Hisense A/C -- diagnostic console. type 'help'.\r\n");
        char line[128];
        unsigned n = 0;
        for (;;) {
            char c;
            int r = lwip_read(cs, &c, 1);
            if (r <= 0) break;                       // client closed
            if (c == '\n' || c == '\r') {
                line[n] = 0;
                if (!strcmp(line, "quit") || !strcmp(line, "exit")) { n = 0; break; }
                diag_handle_line(cs, line);
                diag_say(cs, "> ");
                n = 0;
            } else if (n < sizeof(line) - 1) {
                line[n++] = c;
            }
        }
        lwip_close(cs);
    }
}

static void hisense_diag_console_start(void)
{
    // Own task: accept() blocks, and the Matter task must never block on I/O.
    // 1024 words (~4KB) covers the snprintf buffers, which are the only real users.
    if (xTaskCreate(hisense_diag_console_task, "hisense_diag", 1024, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ChipLogError(DeviceLayer, "diag: task create failed");
    }
}

#else  /* !HISENSE_DEBUG_BUILD -- release flavour: nothing compiled in */

static inline void hisense_diag_on_status(const HisenseState *st) { (void) st; }
static inline void hisense_diag_console_start(void) {}

#endif /* HISENSE_DEBUG_BUILD */
