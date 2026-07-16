# ESP32-Matter Hisense A/C bridge

Local, cloud-free Matter control of a Hisense A/C by putting an **ESP32 + RS-485
transceiver** on the indoor unit's Wi-Fi-module bus — replacing the (fragile,
increasingly unobtainable) AEH-W41H1 RTL8710C module while **reusing the
hardware-validated RS-485 protocol work unchanged**.

## Why this exists
The W41H1 modules die easily (ESD/handling) and are hard to source in the EU. The
*protocol* is the hard part, and it's already done and validated
(`../src/rs485-driver/`, `../../reverse-engineering/docs/03`). An ESP32 that speaks
the same bus bytes **is** a W41H1 to the mainboard — there is no Hisense auth on the
wire, just the RS-485 handshake the driver already emulates.

## What is reused UNCHANGED (do not fork)
| File | Role |
|---|---|
| `../src/rs485-driver/hisense_rs485.{h,cpp}` | codec + bus task + DE half-duplex + "77" handler — **compiles as-is** against the HAL shim below |
| `../src/rs485-driver/matter_aircon_map.h` | Matter↔Hisense mapping (host-tested) |
| `../src/rs485-driver/power_estimate.h` | power estimate (clamped) |
| `../test/virtual_ac.py`, `../test/*` | bench simulator + host tests — develop against these before touching the real bus |

The **only** platform-specific glue is `components/hisense_hal/` — an ESP-IDF
implementation of the same mbed-style `serial_*` / `gpio_*` surface the driver's
host-test stubs define (`../test/hal_stub.h`). Swap the HAL, keep the driver.

## Architecture
```
Hisense mainboard ──RS-485(A/B)── MAX3485 ──UART1── ESP32 ── Matter/Wi-Fi ── Home Assistant
                                    DE/RE ←GPIO┘        │
                          (hisense_rs485.cpp, unchanged)│
                                    matter_aircon_map.h ─┘→ esp-matter Thermostat+FanControl clusters
```
Validated on a classic **ESP32-D0WDQ6** (the board on hand). esp32c3/s3 also work.
- `hisense_hal` presents `serial_api.h`/`gpio_api.h`/`PinNames.h`/`platform_stdlib.h`
  + FreeRTOS shims so the driver's `#include`s resolve. RX is a "soft IRQ": a task
  blocks on the UART event queue and calls the driver's registered `RxIrq` handler,
  which drains via `serial_readable()`/`serial_getc()` — identical contract to AmebaZ2.
- `main/` is the esp-matter app: it registers the callbacks (`hisense_set_recommission_cb`,
  status uplink) and maps cluster attribute writes/reads through `matter_aircon_map.h`.

## Hardware (BOM ~€5)
- **ESP32-C3** dev board (Wi-Fi+BLE, spare UART, Matter-capable). 5 V on VIN.
- **3.3 V RS-485 transceiver**: MAX3485 / SP3485 / SN65HVD75. **NOT a 5 V MAX485 module**
  (its RO would push 5 V into the ESP32 RX and kill it).
- Power + bus from the A/C's module connector (**5 V confirmed**).

### Wiring (edit GPIOs in `components/hisense_hal/include/PinNames.h`)
Pins below are the **hardware-validated** set (live bus read confirmed 2026-07-12).
| A/C connector | → | ESP32 | transceiver |
|---|---|---|---|
| 5 V | → | VIN/5V | (Vcc from 3V3) |
| GND | → | GND | GND |
| RS-485 A | → | | A |
| RS-485 B | → | | B |
| | | TX `PA_14`(**GPIO19**) → | DI |
| | | RX `PA_13`(**GPIO18**) ← | RO |
| | | DE `PA_17`(**GPIO4**) → | DE+RE (tied) |

> ⚠️ **Never use GPIO16/17 for UART on a WROVER/D0WDQ6 module** — they're bonded to the
> PSRAM die and dead as I/O *even with SPIRAM disabled*. TX-on-17 was the cause of a whole
> "RX=0 but internal loopback passes" debugging session; internal loopback bypasses the pads.
> **Bench tip:** prove the pins with a bare GPIO19→GPIO18 jumper (RX must track TX) *before*
> wiring the transceiver.
>
> ⚠️ **Ground loop / brownout:** while the ESP32 is USB-powered (bring-up stage 2), connect
> **only A/B** to the A/C — routing the mains-earthed A/C GND to the laptop-earthed ESP32
> browns it out (RTCWDT resets, flash-read errors). A/C GND/5V only join at stage 3 (powered
> from the connector, no laptop).

## Bring-up, staged (never leave the A/C in an unknown state)
Work in three stages so the unit is never left in an unknown state:
1. **Bench, no A/C** — run the host codec tests (`../test/run_tests.sh`) and the on-target
   `smoketest/`; develop against `../test/virtual_ac.py` before touching the real bus.
2. **Real bus, USB-powered** — remove the module, tap **A/B only** (see the ground-loop
   warning), and poll with the `busmon` app (~1 Hz). The mainboard replies with valid,
   checksum-passing status frames that `hisense_rs485.cpp` + `matter_aircon_map.h` decode
   unchanged. This proves the **read** direction; exercise **write/control** (send a power-on,
   watch `power` flip) next.
3. **Full integration** — power from the connector's 5 V, close it up.

The read direction is hardware-proven: the mainboard accepts a non-Hisense module and the
hardcoded session token holds on this unit (robustness caveat in issue I16). Current status and
the remaining stages are tracked in the GitHub issues (`esp32-path` label) — see **Status** below.

## Gotchas & design notes
- **`xTaskCreate` stack unit differs by platform.** The bus task's `1024` is *words* on
  AmebaZ2 (4 KB) but *bytes* on ESP-IDF (1 KB) → the status callback's `printf` overflowed →
  double-exception crash loop. Fixed via `#ifndef HISENSE_BUS_TASK_STACK` default 1024 +
  `-DHISENSE_BUS_TASK_STACK=4096` in `components/hisense_rs485/CMakeLists.txt` (driver
  unchanged otherwise).
- **WROVER PSRAM pins 16/17** — dead as I/O even with SPIRAM disabled; see the wiring-table
  warning above.
- **DE-release timing.** The HAL's `serial_putc()` calls `uart_wait_tx_done()` after
  `uart_write_bytes()` (which only queues to the FIFO), so the last byte leaves the wire before
  `hisense_tx_raw()` drops DE — full 160 B replies decode with passing checksums, no truncation.
- **Session token `01 01`** is hardcoded, never echoed from the A/C (issue I16). It holds on
  this unit, but the robustness fix (capture + echo the real token) is open — don't rewrite the
  framing blind.
- **Commissioning window / "77"** recovery is AmebaZ2-CHIP specific — reimplement against
  **esp-matter's** `CommissioningWindowManager` in `main/` (the *mapping* stays; the CHIP glue
  is new).

## Build (once esp-matter is set up)
```
. $IDF_PATH/export.sh && . $ESP_MATTER_PATH/export.sh
idf.py set-target esp32 && idf.py build flash monitor   # classic ESP32-D0WDQ6 (the board on hand); esp32c3/s3 also work
```

## Status
**Full functional + structural parity with the AmebaZ2 build, commissioned and verified in HA.**
Built (ESP-IDF v5.5.4 + esp-matter), flashed, and commissioned into the Pi matter-server; every
cluster carries live A/C data and control is proven end-to-end. The RS-485 driver is the same
`../src/rs485-driver` reused unchanged (host golden tests + HAL protocol-neutrality validated).

Endpoints mirror the AmebaZ2 `.zap` 1:1 (all built as esp-matter code, not ZAP):
- **ep1** Room A/C — OnOff + Thermostat (mode/setpoint/local-temp/**running-state**) + FanControl
  (mode/percent) + **ElectricalPowerMeasurement** (watts/volts/amps via a reused CHIP delegate) +
  Hisense mfg cluster `0xFFF1FC00`
- **ep2/ep8** outdoor + coil TemperatureMeasurement
- **ep3/ep4/ep5** Eco / Quiet / Turbo On-Off switches
- **ep6** Sleep-profile ModeSelect (Off/General/Old/Young/Kids)
- **ep7** aux/PTC heat-relay Contact Sensor
- every endpoint has a UserLabel `ha_entitylabel` (via a minimal in-RAM `DeviceInfoProvider`) so HA
  names the entities (Climate/Outdoor/Eco/Quiet/Turbo/Sleep/Aux Heat/Coil)

Done: **#63** (write/control direction proven — a `SystemMode=Cool` from HA powered the A/C on),
**#64** (compile-tune), **#65** (`on_recommission` 1:1 port + Wi-Fi-gated BLE), **#66** (commissioned
into HA), **#15/#17/#18** (special-mode switches + telemetry ported).

Known issue: outdoor/coil TemperatureMeasurement `MeasuredValue` reads null in matter-server though
the device writes correct values every poll (serial-verified; the identical nullable path populates
ep1 LocalTemperature, and a matter-server restart didn't clear it) — a suspected esp-matter
dynamic-endpoint read/report quirk, under investigation.
