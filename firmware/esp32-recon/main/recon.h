// esp32-recon shared surface. See CMakeLists.txt / README.md for the big picture.
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "hisense_rs485.h"   // HisenseState / HisenseCommand / builders / parsers (extern "C")

#ifdef __cplusplus
extern "C" {
#endif

// ---- Capture mode (persisted in NVS; switch = reboot-into-mode) --------------
typedef enum { RECON_MODE_TAP = 0, RECON_MODE_MASTER = 1 } recon_mode_t;

recon_mode_t recon_mode_get(void);                 // NVS, default TAP (drives nothing)
void         recon_mode_set_and_reboot(recon_mode_t m);
const char  *recon_mode_str(recon_mode_t m);

// ---- Shared frame stats + latest decoded state -------------------------------
typedef struct {
    uint32_t     frames;      // valid status frames decoded (both modes)
    uint32_t     chkfail;     // frames rejected by framing/checksum (tap only)
    uint32_t     cmd_frames;  // module->A/C command frames seen (tap only)
    bool         have_state;
    HisenseState last;
} recon_stats_t;

// Called on every VALID status frame — by the driver callback (master) and by the
// tap decoder. Updates stats, runs pending auto-verify, and streams to the watcher.
void recon_on_status(const HisenseState *st);
void recon_note_chkfail(void);
void recon_note_cmd_frame(void);
void recon_get_stats(recon_stats_t *out);
void recon_core_init(void);                         // create the state mutex

// ---- Live "watch" streaming sink (a console's FILE*) -------------------------
void recon_watch_set(FILE *sink);                   // NULL = off
void recon_watch_clear_if(FILE *sink);              // drop the sink if it's this FILE*
// Stream a non-status frame (command / link / bad) to the watcher as a hexdump.
void recon_watch_note_raw(const char *label, const uint8_t *buf, int len);

// ---- Raw-frame delta finder (RE instrument for unknown fields) ---------------
// Retain the latest raw frame per kind so `snap`/`diff` can reveal which byte/bit
// changed when a feature is toggled on the remote. kind 0 = status (A/C->module),
// 1 = command (module->A/C). Populated by the tap decoder.
void recon_note_raw(int kind, const uint8_t *buf, int len);

// ---- Command auto-verify (master): confirm the next status reflects a set ----
enum { RV_TEMP = 1, RV_MODE, RV_FAN_RAW, RV_POWER, RV_ECO, RV_TURBO, RV_MUTE, RV_SLEEP };
void recon_verify_arm(FILE *out, int field, int expected);

// ---- Console -----------------------------------------------------------------
void recon_register_commands(void);                 // register all esp_console commands
void recon_print_state(FILE *out, const HisenseState *st, bool with_hex, const uint8_t *raw, int rawlen);

// ---- Net (WiFi STA + mDNS + TCP :2323 console) -------------------------------
void recon_net_start(void);                         // start wifi (if creds) + mdns + tcp server
bool recon_wifi_start_from_nvs(void);               // returns true if creds were present
esp_err_t recon_wifi_set_creds(const char *ssid, const char *pass);  // store in NVS + connect
void recon_wifi_clear(void);                         // wipe creds
void recon_wifi_status(FILE *out);

// ---- Tap (passive UART1 reader) ----------------------------------------------
void recon_tap_start(void);

// ---- Selftest (golden vectors, no hardware) ----------------------------------
int recon_selftest(FILE *out);                      // returns number of failures

#ifdef __cplusplus
}
#endif
