// Diagnostic telnet console (:2323) embedded in the esp-matter A/C firmware, so the
// node stays a Matter device AND exposes recon-style diagnostics over the network:
//   nc <node-ip> 2323   ->  token | poll | watch | decode | selftest | tx | faults | raw | help
#pragma once
#include "hisense_rs485.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the console (esp_console + TCP :2323 server + a watch task). Call once,
// after hisense_init(); binds on INADDR_ANY so it's ready when Wi-Fi comes up.
void diag_console_start(void);

// Feed each valid status frame. Snapshots ONLY (no I/O) — safe to call from the
// bus callback while the CHIP stack lock is held.
void diag_on_status(const HisenseState *st);

// Implemented in app_main.cpp. Why the last boot happened (#12). Captured once at startup.
// The point is diagnosing an unresponsive module without a serial cable: a BROWNOUT here
// would confirm the A/C 5 V rail sagging, which is currently only a hypothesis.
void diag_get_boot_reason(int *code, const char **text);

// Implemented in app_main.cpp. Sends the current command frame with ONE payload byte
// overridden (see hisense_build_command_override). Backs the console's `tx`.
// Returns 0 sent, -1 offset rejected, -2 TX queue full, -3 shadow invalid (builder
// refused). -1 and -3 are deliberately distinct: conflating them made `tx` blame the
// offset for what was really a poisoned shadow setpoint.
int diag_tx_override(int off, uint8_t val);

// Current command shadow, so the console can explain a -3 instead of guessing.
void diag_get_cmd_state(int *mode, int *setpoint, int *fahrenheit);

#ifdef __cplusplus
}
#endif
