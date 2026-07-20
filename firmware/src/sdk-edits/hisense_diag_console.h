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
// #: counts downlink hand-off attempts (see diag_cmd_sys). Incremented by hisense_on_status in
// matter_drivers.cpp right where it posts, so a stalled consumer is distinguishable from a
// producer that never runs.
static uint32_t     s_diag_downlink_posts;
static uint32_t     s_diag_downlink_runs;   // handler ENTRIES (consumer side)
static bool         s_diag_init_completed;  // driver init task reached its end
// Last init milestone reached, so a stalled init says WHERE it stopped, not just that it did.
// Bumped in order through matter_driver_room_aircon_init (keep the stage list in diag_cmd_sys
// in sync). Stage N means N completed and N+1 did not.
static uint8_t      s_diag_init_stage;

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

/* #38: decoded f_e_* fault bits. The payload base is PROVISIONAL (15, chosen because it
 * is the only nearby base that reads all-zero on a healthy unit; the disassembly-derived
 * 13 was falsified on hardware), so the raw bytes are always printed alongside. All-zero
 * on a healthy unit validates nothing. */
static void diag_cmd_faults(int sock)
{
    char b[HISENSE_DIAG_BUF];
    HisenseFaults f;

    if (!hisense_get_faults(&f)) {
        diag_say(sock, "no fault data yet (no status frame long enough parsed this boot)\r\n");
        return;
    }
    snprintf(b, sizeof(b),
        "raw: [%d]=0x%02x [%d]=0x%02x [%d]=0x%02x [%d]=0x%02x  ->  %s\r\n",
        HISENSE_FAULT_BYTE_INDOOR,  f.raw_indoor,
        HISENSE_FAULT_BYTE_MODULE,  f.raw_module,
        HISENSE_FAULT_BYTE_OUTDOOR, f.raw_outdoor,
        HISENSE_FAULT_BYTE_PROTECT, f.raw_protect,
        f.any ? "FAULT(S) PRESENT (map PROVISIONAL -- cross-check `raw`)" : "all clear");
    diag_say(sock, b);
    if (!f.any) {
        diag_say(sock, "  (all-zero is expected on a healthy unit and does NOT validate the map)\r\n");
        return;
    }
    snprintf(b, sizeof(b),
        "  indoor : temp=%d coil=%d humid=%d water=%d fan/up=%d grille/dw=%d vzero=%d com=%d\r\n"
        "  module : display=%d keys=%d wifi=%d ele=%d eeprom=%d\r\n"
        "  outdoor: eeprom=%d coil=%d gas=%d temp=%d   protect: overtemp=%d\r\n",
        f.in_temp, f.in_coil_temp, f.in_humidity, f.water_full,
        f.in_fan_motor, f.grille, f.in_vzero, f.in_com,
        f.in_display, f.in_keys, f.in_wifi, f.in_ele, f.in_eeprom,
        f.out_eeprom, f.out_coil_temp, f.out_gas_temp, f.out_temp, f.over_temp);
    diag_say(sock, b);
}

/* Why the Matter attributes can be stale while THIS console shows live data.
 *
 * Symptom that motivated it: the bus decoded thousands of frames and `poll` showed correct
 * values, while every status-derived Matter attribute sat at its .zap default (LocalTemperature
 * 0, outdoor/coil 0) so HA rendered the sensors wrong/unavailable. Crucially none of them were
 * NULL, which rules out the link-lost path (that calls SetNull) and points at the downlink event
 * never reaching the handler at all.
 *
 * hisense_on_status() runs in the BUS task and hands off via PostDownlinkEvent() ->
 * DownlinkEventQueue -> DownlinkTask -> matter_driver_downlink_update_handler(). That hand-off
 * fails SILENTLY in two ways, neither of which surfaces anywhere: the queue is NULL (posted
 * before matter_interaction_start_downlink() ran), or the queue fills because DownlinkTask was
 * never created (its xTaskCreate can fail on a heap-starved device, and this firmware adds
 * several tasks of its own).
 *
 * s_diag_downlink_posts counts hand-off attempts from our side. Compare it with the frame count
 * from `poll` and with free heap:
 *   posts climbing + attributes stale  -> the consumer side is the problem (task/queue)
 *   posts NOT climbing                 -> hisense_on_status is not reaching the post
 *   free heap very low                 -> task creation plausibly failed at boot
 */
/* The SDK's downlink plumbing, read directly. These are non-static globals in
 * core/matter_interaction.cpp, so we can observe them rather than infer them. Three rounds of
 * inference (heap exhaustion, task-create failure, stack overflow) were all refuted by
 * measurement; this reports the queue itself.
 *   queue NULL              -> matter_interaction_start_downlink() never created it
 *   queue non-NULL, depth 0 -> posts are not landing (wrong context / send failing)
 *   depth pinned at 10      -> queue full, consumer dead
 */
extern QueueHandle_t DownlinkEventQueue;
extern TaskHandle_t  DownlinkTaskHandle;
extern uint8_t       downlink_init;

static void diag_cmd_sys(int sock)
{
    char b[HISENSE_DIAG_BUF];
    snprintf(b, sizeof(b),
        "downlink posts: %u   handler runs: %u   (posts climbing + runs flat => dead consumer)\r\n"
        "free heap: %u bytes   min-ever free: %u bytes\r\n"
        "downlink queue: %s  depth: %u  task: %s  init_flag: %u\r\n"
        "driver init completed: %s   last init stage: %u\r\n"
        "  stages: 1 hisense_init  2 queues+cbs  3 console+breakglass  4 UserLabels\r\n"
        "          5 ep9 Display seed  6 ModeSelect  7 end of init  8 EPM returned\r\n"
        "  stage N means N completed and N+1 did not -- that is where init stopped\r\n",
        (unsigned) s_diag_downlink_posts, (unsigned) s_diag_downlink_runs,
        (unsigned) xPortGetFreeHeapSize(),
        (unsigned) xPortGetMinimumEverFreeHeapSize(),
        DownlinkEventQueue ? "OK" : "NULL",
        DownlinkEventQueue ? (unsigned) uxQueueMessagesWaiting(DownlinkEventQueue) : 0u,
        DownlinkTaskHandle ? "OK" : "NULL",
        (unsigned) downlink_init,
        s_diag_init_completed ? "yes" : "NO",
        (unsigned) s_diag_init_stage);
    diag_say(sock, b);
}

/* Hexdump the last status frame. This is what falsified the fault map's base on the esp32
 * side within a minute of flashing, so it matters more than the decode does. */
/* Hexdump the last 0x1E LINK reply -- the frame that carries the "77" recommission request in
 * payload[4] (byte 17). Ported from the esp32 console because this is what actually solved "77"
 * there: the request is a ONE-FRAME pulse, so a polled snapshot cannot catch it and "the A/C
 * never asks" is indistinguishable from "we looked at the wrong moment". The living-room unit
 * asserts 0x20 (smartcfg); this unit is UNCONFIRMED and may differ. */
static void diag_cmd_link(int sock)
{
    uint8_t f[40];
    char    b[HISENSE_DIAG_BUF];
    uint8_t n = hisense_get_last_link_frame(f, (uint8_t) sizeof(f));
    int     o = 0;
    if (!n) { diag_say(sock, "no 0x1E LINK reply captured yet\r\n"); return; }
    o += snprintf(b + o, sizeof(b) - o, "last 0x1E LINK reply, %u bytes:\r\n", (unsigned) n);
    /* int counters on purpose: `k < i + 16` promotes the RHS to int, so a uint8_t k could not
     * reach it once i+16 passed 255 and the loop would never terminate (CodeQL, PR #68). */
    for (int i = 0; i < (int) n && o < (int) sizeof(b) - 64; i += 16) {
        o += snprintf(b + o, sizeof(b) - o, "  %3d:", i);
        for (int k = i; k < i + 16 && k < (int) n; k++)
            o += snprintf(b + o, sizeof(b) - o, " %02x", f[k]);
        o += snprintf(b + o, sizeof(b) - o, "\r\n");
    }
    snprintf(b + o, sizeof(b) - o,
             "  payload[4]=byte[17]=0x%02x  masked link_req=0x%02x\r\n"
             "  (\"77\" bits: 0x08 reconfig / 0x20 smartcfg. 0x00 while pressing the remote\r\n"
             "   sequence => the request never reaches us; the fault is upstream of Matter.)\r\n",
             n > 17 ? f[17] : 0, (unsigned) hisense_get_last_link_req());
    diag_say(sock, b);
}

static void diag_cmd_raw(int sock)
{
    uint8_t f[HISENSE_RAW_SNAPSHOT_LEN];
    char b[HISENSE_DIAG_BUF];
    size_t n = hisense_get_last_status_frame(f, sizeof(f));
    size_t i, k, off;

    if (n == 0) { diag_say(sock, "no status frame captured yet\r\n"); return; }
    snprintf(b, sizeof(b), "last status frame, %u bytes:\r\n", (unsigned) n);
    diag_say(sock, b);
    for (i = 0; i < n; i += 16) {
        off = (size_t) snprintf(b, sizeof(b), "  %3u:", (unsigned) i);
        for (k = i; k < i + 16 && k < n; k++) {
            off += (size_t) snprintf(b + off, sizeof(b) - off, " %02x", f[k]);
        }
        snprintf(b + off, sizeof(b) - off, "\r\n");
        diag_say(sock, b);
    }
}

static void diag_handle_line(int sock, char *line)
{
    while (*line == ' ') line++;
    char *e = line + strlen(line);
    while (e > line && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ')) *--e = 0;

    if (line[0] == 0)                       return;
    if (!strcmp(line, "features"))          { diag_cmd_features(sock); return; }
    if (!strcmp(line, "poll"))              { diag_cmd_poll(sock);     return; }
    if (!strcmp(line, "faults"))            { diag_cmd_faults(sock);   return; }
    if (!strcmp(line, "raw"))               { diag_cmd_raw(sock);      return; }
    if (!strcmp(line, "link"))              { diag_cmd_link(sock);     return; }
    if (!strcmp(line, "sys"))               { diag_cmd_sys(sock);      return; }
    if (!strcmp(line, "version")) {
        char b[128];
        snprintf(b, sizeof(b), "AmebaZ2 Hisense A/C, softwareVersion %u (DEBUG flavour)\r\n",
                 (unsigned) CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION);
        diag_say(sock, b);
        return;
    }
    if (!strcmp(line, "help")) {
        diag_say(sock,
            "commands: features | poll | faults | raw | link | sys | version | help | quit\r\n"
            "  features  cached 0x66/40 ProductType capability flags for THIS unit\r\n"
            "  poll      last decoded A/C status frame\r\n"
            "  faults    decoded f_e_* fault bits (#38; base PROVISIONAL)\r\n"
            "  raw       hexdump the last status frame (what falsifies the map)\r\n"
            "  sys       downlink hand-off counter + free heap (why Matter attrs go stale)\r\n");
        return;
    }
    diag_say(sock, "unknown command (try `help`)\r\n");
}

static void hisense_diag_console_task(void *arg)
{
    (void) arg;
    /* IPv6 dual-stack (#42). Matter runs over IPv6, so the node's IPv6 address is the one
     * we always know: matter-server addresses it that way and it appears in the Pi's
     * neighbour table. Its IPv4 address is DHCP, never ARP'd by anything we control, and
     * absent from every neighbour table, so an AF_INET-only console is effectively
     * unfindable. Locating this console once cost a port scan of two /24 subnets.
     *
     * Bind AF_INET6 with IPV6_V6ONLY off so it answers on BOTH, exactly as the esp32
     * console does. Setting V6ONLY is best-effort: if the option is unsupported the bind
     * still succeeds and we simply lose the v4-mapped half. */
    struct sockaddr_in6 a;
    int ls = lwip_socket(AF_INET6, SOCK_STREAM, 0);
    if (ls < 0) { ChipLogError(DeviceLayer, "diag: socket failed"); vTaskDelete(NULL); return; }

    int one = 1;
    lwip_setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int v6only = 0;
    lwip_setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_port   = htons(HISENSE_DIAG_PORT);
    a.sin6_addr   = in6addr_any;

    if (lwip_bind(ls, (struct sockaddr *) &a, sizeof(a)) < 0 || lwip_listen(ls, 1) < 0) {
        ChipLogError(DeviceLayer, "diag: bind/listen :%d failed", HISENSE_DIAG_PORT);
        lwip_close(ls); vTaskDelete(NULL); return;
    }
    ChipLogProgress(DeviceLayer, "diag console listening on :%d (DEBUG build, IPv6 dual-stack)",
                    HISENSE_DIAG_PORT);

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
