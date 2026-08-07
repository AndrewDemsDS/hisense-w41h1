#pragma once
// hisense_rs485.{h,cpp} reference the AmebaZ2 pin names PA_14 (UART TX), PA_13
// (UART RX) and PA_17 (RS-485 DE). On ESP32 we remap those symbols to the GPIO
// numbers you actually wire to -- so the driver stays UNCHANGED.
//
// The map is per-target because the two bench boards have incompatible constraints (the
// classic ESP32's dead PSRAM pins vs the C3's USB pins). CONFIG_IDF_TARGET_* comes from
// sdkconfig.h, which IDF force-includes into every component, so the right block is chosen
// by `idf.py set-target` with nothing to edit by hand.
// >>> EDIT THE THREE DEFINES IN YOUR BOARD'S BLOCK to match your wiring <<<
typedef int PinName;

#if defined(CONFIG_IDF_TARGET_ESP32C3)
// ---- ESP32-C3 SuperMini (idf.py set-target esp32c3) ----
// Chosen as three CONSECUTIVE pins on one header row, so the transceiver is a single 3-wire
// run. Every pin here is a plain GPIO: no strapping pin (2 / 8 / 9), no USB (18 / 19, which
// carry the only link to the board and are not broken out anyway), no UART0 (20 / 21).
//
// NOTE: 5/6/7 double as the JTAG pins MTDI/MTCK/MTDO. That is harmless here -- debugging goes
// through the C3's BUILT-IN USB-Serial/JTAG, so external JTAG is never muxed in, and the pins
// boot as ordinary GPIOs.
#define PA_14   5   // UART1 TX  -> transceiver DI
#define PA_13   6   // UART1 RX  <- transceiver RO
#define PA_17   7   // RS-485 DE -> transceiver DE+RE (tied)
// ⚠️ DE floats until gpio_init() runs. Fit a ~10k pulldown from DE to GND so the transceiver
// stays in receive through reset and the download-mode window: a DE that drifts high parks a
// second driver on the A/C's bus and corrupts traffic between the mainboard and everything
// else on it. Costs one resistor; the failure looks like a flaky bus, not like a wiring bug.

#else
// ---- classic ESP32-D0WDQ6 (idf.py set-target esp32) ----
// UART1-safe output GPIOs (avoid 6-11 = SPI flash, 34-39 = input-only).
// NOTE: GPIO16 & GPIO17 are WROVER PSRAM pins -- physically bonded to the PSRAM
// die on WROVER modules and unusable as I/O even with SPIRAM disabled. TX was on
// GPIO17 and the original RX on GPIO16; that (not the transceiver) is why external
// RX stayed 0 while internal loopback passed. Both UART pins now avoid 16/17.
#define PA_14  19   // UART1 TX  -> transceiver DI  (moved off GPIO17: WROVER PSRAM pin)
#define PA_13  18   // UART1 RX  <- transceiver RO  (moved off GPIO16: WROVER PSRAM pin)
#define PA_17   4   // RS-485 DE -> transceiver DE+RE (tied)
#endif
