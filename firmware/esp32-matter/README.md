# ESP32-Matter Hisense A/C bridge

Local, cloud-free Matter control of a Hisense A/C by putting an **ESP32 + RS-485
transceiver** on the indoor unit's Wi-Fi-module bus, replacing the (fragile,
increasingly unobtainable) AEH-W41H1 RTL8710C module while **reusing the
hardware-validated RS-485 protocol work unchanged**.

> **Deciding between the two paths?** See
> [`../docs/13-path-comparison.md`](../docs/13-path-comparison.md) for a measured comparison of
> this path against the AmebaZ2 stock-module path (cost, toolchain, OTA, diagnostics, flash budget).

## Why this exists
The W41H1 modules die easily (ESD/handling) and are hard to source in the EU. The
*protocol* is the hard part, and it's already done and validated
(`../src/rs485-driver/`, `../../reverse-engineering/docs/03`). An ESP32 that speaks
the same bus bytes **is** a W41H1 to the mainboard, there is no Hisense auth on the
wire, just the RS-485 handshake the driver already emulates.

## What is reused UNCHANGED (do not fork)
| File | Role |
|---|---|
| `../src/rs485-driver/hisense_rs485.{h,cpp}` | codec + bus task + DE half-duplex + "77" handler, **compiles as-is** against the HAL shim below |
| `../src/rs485-driver/matter_aircon_map.h` | Matter↔Hisense mapping (host-tested) |
| `../src/rs485-driver/power_estimate.h` | power estimate (clamped) |
| `../test/virtual_ac.py`, `../test/*` | bench simulator + host tests, develop against these before touching the real bus |

The **only** platform-specific glue is `components/hisense_hal/`: an ESP-IDF
implementation of the same mbed-style `serial_*` / `gpio_*` surface the driver's
host-test stubs define (`../test/hal_stub.h`). Swap the HAL, keep the driver.

## Architecture
```
Hisense mainboard ──RS-485(A/B)── MAX3485 ──UART1── ESP32 ── Matter/Wi-Fi ── Home Assistant
                                    DE/RE ←GPIO┘        │
                          (hisense_rs485.cpp, unchanged)│
                                    matter_aircon_map.h ─┘→ esp-matter Thermostat+FanControl clusters
```
Bus-validated against a real A/C on a classic **ESP32-D0WDQ6**. The **ESP32-C3 SuperMini** is the
smaller successor board and the preferred target going forward: same firmware, a fraction of the
footprint. It builds, commissions onto the fabric, and drives the RS-485 loop correctly against
`../test/virtual_ac.py` (handshake, 1 Hz polls, state landing in the Matter clusters), but it has
**not yet met a real mainboard**. Everything target-specific is confined to
`sdkconfig.defaults.esp32c3` and one `#if` in `PinNames.h`, so `idf.py set-target esp32c3` is the
whole switch.
- `hisense_hal` presents `serial_api.h`/`gpio_api.h`/`PinNames.h`/`platform_stdlib.h`
  + FreeRTOS shims so the driver's `#include`s resolve. RX is a "soft IRQ": a task
  blocks on the UART event queue and calls the driver's registered `RxIrq` handler,
  which drains via `serial_readable()`/`serial_getc()`: identical contract to AmebaZ2.
- `main/` is the esp-matter app: it registers the callbacks (`hisense_set_recommission_cb`,
  status uplink) and maps cluster attribute writes/reads through `matter_aircon_map.h`.

## Hardware (BOM ~€5)
- **ESP32-C3 SuperMini** (Wi-Fi+BLE, spare UART, Matter-capable, 4 MB flash, ~22×18 mm, small
  enough to sit where the W41H1 did). 5 V on the `5V` pin. A classic ESP32 dev board also works;
  see the wiring table for its pin column.
- **3.3 V RS-485 transceiver**: MAX3485 / SP3485 / SN65HVD75. **NOT a 5 V MAX485 module**
  (its RO would push 5 V into the ESP32 RX and kill it).
- Power + bus from the A/C's module connector (**5 V confirmed**).

### Wiring (GPIOs live in `components/hisense_hal/include/PinNames.h`)
The pin map is **per-target** and selected automatically by `idf.py set-target`, so there is
nothing to edit by hand when you swap boards. Pick your board's column.

| A/C connector | → | ESP32-C3 SuperMini | classic ESP32-D0WDQ6 | transceiver |
|---|---|---|---|---|
| 5 V | → | 5V pin | VIN/5V | (Vcc from 3V3) |
| GND | → | GND | GND | GND |
| RS-485 A | → | | | A |
| RS-485 B | → | | | B |
| | | TX `PA_14` = **GPIO5** → | **GPIO19** → | DI |
| | | RX `PA_13` = **GPIO6** ← | **GPIO18** ← | RO |
| | | DE `PA_17` = **GPIO7** → | **GPIO4** → | DE+RE (tied) |

The ESP32 column is the **hardware-validated** set (live bus read confirmed 2026-07-12). The C3
column is **simulator-validated** (`virtual_ac.py` over a USB-TTL adapter, 2026-08-07: handshake,
sustained 1 Hz polling, state reaching the Matter clusters) and chosen to be safe (see below), but
nothing has spoken to a real mainboard on those pins yet.

> ⚠️ **C3: never put the UART on GPIO18/19.** On the classic ESP32 those are the validated UART
> pins; on the C3 they are the USB D-/D+ lines feeding the built-in USB-Serial/JTAG. On the
> SuperMini that USB port is the *only* way in (there is no bridge chip), so muxing them away
> costs you flashing and console at once. They aren't even on the header. The C3 set (5/6/7) is
> three consecutive pins on one row, avoiding the strapping pins (2/8/9), USB (18/19) and
> UART0 (20/21).
>
> ⚠️ **C3: fit a ~10k pulldown on DE.** DE floats from power-on until `gpio_init()` runs, and a DE
> that drifts high parks a second driver on the A/C's RS-485 bus. It presents as intermittent bus
> corruption rather than as a wiring fault, so it is worth the one resistor.
>
> ⚠️ **Never use GPIO16/17 for UART on a WROVER/D0WDQ6 module**: they're bonded to the
> PSRAM die and dead as I/O *even with SPIRAM disabled*. TX was on GPIO17 and RX on GPIO16;
> that (not the transceiver) is why external RX stayed 0 while internal loopback passed:
> internal loopback bypasses the pads.
> **Bench tip (either board):** prove the pins with a bare TX→RX jumper (RX must track TX)
> *before* wiring the transceiver.
>
> ⚠️ **Ground loop / brownout:** while the ESP32 is USB-powered (bring-up stage 2), connect
> **only A/B** to the A/C, routing the mains-earthed A/C GND to the laptop-earthed ESP32
> browns it out (RTCWDT resets, flash-read errors). A/C GND/5V only join at stage 3 (powered
> from the connector, no laptop).

### ⚠️ Erase flash on a BRAND-NEW board before the first commission
Flash a factory-fresh board with **`idf.py -p <port> erase-flash`** first, then build/flash as
normal. `idf.py flash` writes only bootloader / partition table / app / otadata, so it **leaves
the NVS partition untouched** -- including whatever the vendor's factory test firmware left there.

If that NVS holds a Wi-Fi station config, `ESPWiFiDriver::Init()` adopts it into `mStagingNetwork`
straight out of `esp_wifi_get_config(WIFI_IF_STA)`, and `AddOrUpdateNetwork()` then rejects **any**
SSID that isn't the stale one:

```cpp
// connectedhomeip/src/platform/ESP32/NetworkCommissioningDriver.cpp
VerifyOrReturnError(mStagingNetwork.ssidLen == 0 || NetworkMatch(mStagingNetwork, ssid),
                    Status::kBoundsExceeded);
```

Every commissioning attempt then dies at `WiFiNetworkSetup` with `CHIP Error 0x000000AC:
Internal error` -- which is just the controller's generic mapping of a non-success
`networkingStatus` and says nothing about the real cause. Two things make this expensive to
diagnose: the useful line, `Received NetworkConfig response, networkingStatus=2`
(2 = `kBoundsExceeded`), is a `ChipLogProgress` that matter-server **suppresses** at its default
SDK log level, and the rejection is instant (~150 ms, no radio activity), so it reads like an
association/credentials problem when the radio was never involved. Recover the line with:

```
# temporary, on the matter-server host; revert afterwards
command: [..., "--log-level-sdk", "progress"]
```

`kBoundsExceeded` from this driver has exactly one source (the length checks return
`kOutOfRange`, the backup failure returns `kUnknownError`), so status 2 *is* the stale-NVS
diagnosis. Observed on a new ESP32-C3 SuperMini, 2026-08-07.

## Bring-up, staged (never leave the A/C in an unknown state)
Work in three stages so the unit is never left in an unknown state:
1. **Bench, no A/C**: run the host codec tests (`../test/run_tests.sh`) and the on-target
   `smoketest/`; develop against `../test/virtual_ac.py` before touching the real bus.
2. **Real bus, USB-powered**: remove the module, tap **A/B only** (see the ground-loop
   warning), and poll with the `busmon` app (~1 Hz). The mainboard replies with valid,
   checksum-passing status frames that `hisense_rs485.cpp` + `matter_aircon_map.h` decode
   unchanged. This proves the **read** direction; exercise **write/control** (send a power-on,
   watch `power` flip) next.
3. **Full integration**: power from the connector's 5 V, close it up.

The read direction is hardware-proven: the mainboard accepts a non-Hisense module and the link
comes up. Envelope `[7]/[8]` is now learned from the A/C's DevType reply rather than hardcoded
(issue I16). Current status and the remaining stages are tracked in the GitHub issues
(`esp32-path` label), see **Status** below.

## Gotchas & design notes
- **`xTaskCreate` stack unit differs by platform.** The bus task's `1024` is *words* on
  AmebaZ2 (4 KB) but *bytes* on ESP-IDF (1 KB) → the status callback's `printf` overflowed →
  double-exception crash loop. Fixed via `#ifndef HISENSE_BUS_TASK_STACK` default 1024 +
  `-DHISENSE_BUS_TASK_STACK=4096` in `components/hisense_rs485/CMakeLists.txt` (driver
  unchanged otherwise).
- **WROVER PSRAM pins 16/17**: dead as I/O even with SPIRAM disabled; see the wiring-table
  warning above.
- **DE-release timing.** The HAL's `serial_putc()` calls `uart_wait_tx_done()` after
  `uart_write_bytes()` (which only queues to the FIFO), so the last byte leaves the wire before
  `hisense_tx_raw()` drops DE, full 160 B replies decode with passing checksums, no truncation.
- **Envelope `[7]/[8]` is the A/C's device-type/sub-type, not a session token** (issue I16,
  measured 2026-07-16, `token` on the diag console). Read from the DevType (`0x0A`) reply's
  inner `[3]/[4]` (=`01 01` here, confirmed *learned* from the A/C) and stamped on later frames;
  defaults to `01 01` so the wire is unchanged. ⚠️ Do **not** stamp that reply's envelope
  `[9]/[10]`: it is **`00 00`** on the `0x0A` frame, and shipping it (v10207) made the A/C
  reject every frame until an OTA recovery. See `RE/docs/10` §4.5.
- **Commissioning window / "77"** recovery is AmebaZ2-CHIP specific, reimplement against
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
- **ep1** Room A/C: OnOff + Thermostat (mode/setpoint/local-temp/**running-state**) + FanControl
  (mode/percent) + **ElectricalPowerMeasurement** (watts/volts/amps via a reused CHIP delegate) +
  Hisense mfg cluster `0xFFF1FC00`
- **ep2/ep8** outdoor + coil TemperatureMeasurement
- **ep3/ep4/ep5** Eco / Quiet / Turbo On-Off switches
- **ep6** Sleep-profile ModeSelect (Off/General/Old/Young/Kids)
- **ep7** aux/PTC heat-relay Contact Sensor
- every endpoint has a UserLabel `ha_entitylabel` (via a minimal in-RAM `DeviceInfoProvider`) so HA
  names the entities (Climate/Outdoor/Eco/Quiet/Turbo/Sleep/Aux Heat/Coil)

Done: **#63** (write/control direction proven, a `SystemMode=Cool` from HA powered the A/C on),
**#64** (compile-tune), **#65** (`on_recommission` 1:1 port + Wi-Fi-gated BLE), **#66** (commissioned
into HA), **#15/#17/#18** (special-mode switches + telemetry ported).

Known issue: outdoor/coil TemperatureMeasurement `MeasuredValue` reads null in matter-server though
the device writes correct values every poll (serial-verified; the identical nullable path populates
ep1 LocalTemperature, and a matter-server restart didn't clear it), a suspected esp-matter
dynamic-endpoint read/report quirk, under investigation.
