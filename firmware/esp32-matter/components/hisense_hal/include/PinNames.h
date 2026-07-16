#pragma once
// hisense_rs485.{h,cpp} reference the AmebaZ2 pin names PA_14 (UART TX), PA_13
// (UART RX) and PA_17 (RS-485 DE). On ESP32 we remap those symbols to the GPIO
// numbers you actually wire to -- so the driver stays UNCHANGED.
//
// Target board here: classic ESP32-D0WDQ6 (idf.py set-target esp32). These are
// UART1-safe output GPIOs (avoid 6-11 = SPI flash, 34-39 = input-only).
// >>> EDIT THESE THREE to match your wiring <<<
typedef int PinName;
// NOTE: GPIO16 & GPIO17 are WROVER PSRAM pins -- physically bonded to the PSRAM
// die on WROVER modules and unusable as I/O even with SPIRAM disabled. TX was on
// GPIO17 and the original RX on GPIO16; that (not the transceiver) is why external
// RX stayed 0 while internal loopback passed. Both UART pins now avoid 16/17.
#define PA_14  19   // UART1 TX  -> transceiver DI  (moved off GPIO17: WROVER PSRAM pin)
#define PA_13  18   // UART1 RX  <- transceiver RO  (moved off GPIO16: WROVER PSRAM pin)
#define PA_17   4   // RS-485 DE -> transceiver DE+RE (tied)
