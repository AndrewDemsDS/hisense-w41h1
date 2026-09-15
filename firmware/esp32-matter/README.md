# ESP32-Matter Hisense A/C bridge

The esp-matter firmware for an **ESP32 + RS-485 transceiver** that replaces the AEH-W41H1 module,
reusing `../src/rs485-driver/` unchanged through the ESP-IDF HAL in `components/hisense_hal/`.

This README is the developer reference for this directory. The user guide covers the rest, so it
is not repeated here:

- why this track exists, the reused files, the HAL-shim model, BOM, **wiring for both boards, the
  GPIO and ground-loop warnings**, staged bring-up and status:
  [`docs/guide/ESP32-Replacement-Build.md`](../../docs/guide/ESP32-Replacement-Build.md)
- fetch, build, erase, flash and the `virtual_ac.py` bench, with what is hardware-verified:
  [`docs/guide/Build-Flash-Test.md`](../../docs/guide/Build-Flash-Test.md)
- delta OTA and release: [`docs/guide/OTA-Updates.md`](../../docs/guide/OTA-Updates.md)
- choosing between the three firmwares: [`../docs/13-path-comparison.md`](../docs/13-path-comparison.md)

## Layout

| Path | What |
|---|---|
| `main/` | the esp-matter app: endpoints built in code, callbacks, cluster reads/writes through `matter_aircon_map.h` |
| `components/hisense_hal/` | ESP-IDF implementation of the driver's `serial_*` / `gpio_*` surface; per-target pins in `include/PinNames.h` |
| `components/hisense_rs485/` | registers `../src/rs485-driver/` as an IDF component (no copy) |
| `smoketest/` | separate IDF project: `busmon`, a live bus monitor with no Matter stack |
| `sdkconfig.defaults{,.esp32c3}` | build config; the `.esp32c3` overlay is the only target-specific file besides `PinNames.h` |

## Build

From this directory:

```
. $IDF_PATH/export.sh && . $ESP_MATTER_PATH/export.sh
idf.py set-target esp32c3 && idf.py build flash monitor   # esp32 for the classic ESP32-D0WDQ6
```

Or `firmware/scripts/dev.py build esp32 --board c3` from the repo root. Images that ship over OTA
come from `firmware/scripts/esp32-release.sh`, never from a plain `idf.py build`.

## Why a brand-new board must be erased first

The guide gives the rule; this is the diagnosis, for when you hit it anyway.

`idf.py flash` writes only bootloader / partition table / app / otadata, so it **leaves the NVS
partition untouched**, including whatever the vendor's factory test firmware left there. If that NVS
holds a Wi-Fi station config, `ESPWiFiDriver::Init()` adopts it into `mStagingNetwork` straight out
of `esp_wifi_get_config(WIFI_IF_STA)`, and `AddOrUpdateNetwork()` then rejects **any** SSID that
isn't the stale one:

```cpp
// connectedhomeip/src/platform/ESP32/NetworkCommissioningDriver.cpp
VerifyOrReturnError(mStagingNetwork.ssidLen == 0 || NetworkMatch(mStagingNetwork, ssid),
                    Status::kBoundsExceeded);
```

Every commissioning attempt then dies at `WiFiNetworkSetup` with `CHIP Error 0x000000AC: Internal
error`, the controller's generic mapping of a non-success `networkingStatus`. Two things make this
expensive to diagnose: the useful line, `Received NetworkConfig response, networkingStatus=2`
(2 = `kBoundsExceeded`), is a `ChipLogProgress` that matter-server **suppresses** at its default SDK
log level, and the rejection is instant (~150 ms, no radio activity), so it reads like an
association or credentials problem when the radio was never involved. Recover the line with:

```
# temporary, on the matter-server host; revert afterwards
command: [..., "--log-level-sdk", "progress"]
```

`kBoundsExceeded` from this driver has exactly one source (the length checks return `kOutOfRange`,
the backup failure returns `kUnknownError`), so status 2 *is* the stale-NVS diagnosis. Observed on a
new ESP32-C3 SuperMini, 2026-08-07.

## Design notes

- **`xTaskCreate` stack unit differs by platform.** The bus task's `1024` is *words* on AmebaZ2
  (4 KB) but *bytes* on ESP-IDF (1 KB), so the status callback's `printf` overflowed into a
  double-exception crash loop. Fixed via `#ifndef HISENSE_BUS_TASK_STACK` (default 1024) plus
  `-DHISENSE_BUS_TASK_STACK=4096` in `components/hisense_rs485/CMakeLists.txt`.
- **DE-release timing.** The HAL's `serial_putc()` calls `uart_wait_tx_done()` after
  `uart_write_bytes()` (which only queues to the FIFO), so the last byte leaves the wire before
  `hisense_tx_raw()` drops DE; full 160-byte replies decode with passing checksums.
- **C3 DE moved from GPIO7 to GPIO10** on 2026-08-10 after a wiring fault shorted the DE/EN line
  to 3V3. The C3 pin set avoids the strapping pins (2/8/9), USB (18/19) and UART0 (20/21).
- **Envelope `[7]/[8]` is the A/C's device-type/sub-type, not a session token** (issue I16,
  measured 2026-07-16). Read from the DevType (`0x0A`) reply's inner `[3]/[4]` and stamped on later
  frames; defaults to `01 01`. Do **not** stamp that reply's envelope `[9]/[10]`: it is `00 00` on
  the `0x0A` frame, and shipping it (v10207) made the A/C reject every frame until an OTA recovery.
  See `RE/docs/10` §4.5 and the guide's Protocol Overview.
- **Commissioning window / "77"** is reimplemented against esp-matter's
  `CommissioningWindowManager` in `main/`; the mapping is shared, the CHIP glue is new.

## Status detail

Full functional and structural parity with the AmebaZ2 build (summary and endpoint list in the
guide). Built with ESP-IDF v5.5.4 and esp-matter, commissioned into matter-server, every cluster
carrying live A/C data. Every endpoint has a UserLabel `ha_entitylabel` (via a minimal in-RAM
`DeviceInfoProvider`) so Home Assistant names the entities.

Done: **#63** (write/control direction proven, a `SystemMode=Cool` from HA powered the A/C on),
**#64** (compile-tune), **#65** (`on_recommission` 1:1 port + Wi-Fi-gated BLE), **#66**
(commissioned into HA), **#15/#17/#18** (special-mode switches + telemetry ported).

Known issue: outdoor/coil TemperatureMeasurement `MeasuredValue` reads null in matter-server though
the device writes correct values every poll (serial-verified; the identical nullable path populates
ep1 LocalTemperature, and a matter-server restart didn't clear it), a suspected esp-matter
dynamic-endpoint read/report quirk, under investigation.
