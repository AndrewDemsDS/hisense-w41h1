# Changelog

Notable changes to this project. The format loosely follows [Keep a Changelog](https://keepachangelog.com/).
Firmware versions use the unified semver → softwareVersion-int scheme (see
[`firmware/src/version.txt`](firmware/src/version.txt) and
[`firmware/esp32-matter/CMakeLists.txt`](firmware/esp32-matter/CMakeLists.txt)).

## Diagnostics exposed to Home Assistant - 2026-07-22

### Firmware
- ESP32 (node 35) 1.1.3 -> 1.1.6: decode CompressorHz, Features1, and Faults1 from the ep1
  manufacturer cluster (#82); extend the Wi-Fi TX-power throttle from the HTTP break-glass path to
  the Matter (BDX) OTA path via a `HisenseOTARequestorDriver` (#84); add an RX checksum helper with
  an observe-only mismatch counter, a heap-watermark task, and bus-task-scoped WDT panic (#87).
- AmebaZ2 (node 14) 1.3.20 -> 1.3.22: adopt the shared checksum observe-only change (1.3.21) and
  surface the mismatch counter on the `:2323` console (1.3.22, #88).

### Home Assistant
- The `hisense-unified-ac` integration renders four diagnostic entities per node (compressor
  frequency, capabilities, faults, bus link) on nodes 14, 35, and 62. Closes #38 and #39.

### Fixed
- #83: the ESP32 module that would not join Wi-Fi was hitting a brownout crash loop at `phy_init`
  from an RF-cal current spike on a marginal 5 V rail. CHIP already auto-reconnects; the fix is a
  470-1000 uF bulk cap, not an app-level reconnect handler.

## Initial public release - 2026-07-16

First public cut. Everything below already ran privately; this release drops the personal details.

### Firmware
- AmebaZ2 (RTL8710C) Matter `room_air_conditioner` firmware with the reverse-engineered Hisense
  RS-485 driver: HVAC mode/setpoint/fan/swing, Eco/Quiet/Turbo/Sleep, and derived power/voltage.
- ESP32 replacement firmware (esp-matter) that mirrors the same control surface for dead modules.
- OTA over Wi-Fi after the initial CH341 flash. `ota-release.sh` sets the AmebaZ2 boot-slot serial
  so an update sticks instead of rolling back.

### Tooling & docs
- `ota-release.sh` runs the build/package/stage/flash pipeline. Host-only QA (`firmware/test/`)
  covers codec, Matter↔A/C mapping, and virtual-A/C round-trip checks.
- Reverse-engineering docs: RS-485 protocol, cloud/firewall, stock-FW init, hardware.
- CI: a host lint gate on every push/PR, plus tagged-release builds that attach `.bin`/`.ota` to
  GitHub Releases (both the AmebaZ2 and ESP32 builds run on a self-hosted SDK runner).
