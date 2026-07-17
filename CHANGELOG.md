# Changelog

Notable changes to this project. The format loosely follows [Keep a Changelog](https://keepachangelog.com/).
Firmware versions use the unified semver → softwareVersion-int scheme (see
[`firmware/src/version.txt`](firmware/src/version.txt) and
[`firmware/esp32-matter/CMakeLists.txt`](firmware/esp32-matter/CMakeLists.txt)).

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
