# Changelog

Notable changes to this project. The format loosely follows [Keep a Changelog](https://keepachangelog.com/).
Firmware versions use the unified semver → softwareVersion-int scheme (see
[`firmware/src/version.txt`](firmware/src/version.txt) and
[`firmware/esp32-matter/CMakeLists.txt`](firmware/esp32-matter/CMakeLists.txt)).

## Unreleased

### Firmware
- ESPHome: the `bus_link` binary sensor now shows the link going down (it could only ever publish
  on), and the post-command holdoff no longer sticks on for ~24.8 days after the `millis()` sign
  flips, which skipped every readback on a unit left untouched that long.
- ESPHome: eco, quiet, turbo and the sleep profile are now `climate` presets, named exactly as the
  `hisense-unified-ac` integration names them for Matter nodes, so a climate group can sync presets
  across both firmwares. Special-mode writes (presets, switches, sleep select) share a paced queue
  that spaces them 10 s apart, and a fan change is refused while turbo, quiet or sleep owns the fan.
  New `supports_eco/quiet/turbo/sleep` options on the climate platform. The preset table, detection
  and write plan are pure functions in `esphome_aircon_map.h` with host tests. It lives under
  `firmware/src/` but changes no Matter image.
- ESPHome, **breaking**: fan modes are now `auto`, `low`, `medium_low`, `medium`, `medium_high`,
  `high`, matching `hisense-unified-ac`. `Medium-low` / `Medium-high` are renamed, and `quiet` is
  no longer a fan mode (use the `quiet` preset; the quiet step reads back as `low`). Update
  automations that set the old names.

### Fixed
- AmebaZ2 1.3.33 -> 1.3.38: PercentSetting 42 / 75 landed one fan step up (58 / 100 %). The app
  publishes FanMode by folding the six speeds into Low/Medium/High, and that readback re-commanded
  the bucket's speed along two paths: the connectedhomeip patch mapped it onto PercentSetting
  33/66/100 (now skipped for writes flagged `gW41h1AppFanModeWrite`), and it was queued to the
  app's own FanMode handler as if a client wrote it (now recorded in an own-write echo ledger,
  `matter_echo_note/consume()`, and skipped). 1.3.35-1.3.37 tried a bucket comparison instead, which
  let a stale readback undo a fan-card Medium press. ESP32 builds its FanControl in code, has neither
  path, and was not affected.

### Tooling
- C/C++ lint for every tree we own (`firmware/scripts/cpp-lint.sh check|fix`), with ESPHome's
  clang-format and clang-tidy configs and the portable rules of its `ci-custom.py`, gated in CI
  and the pre-commit hook. The shared driver, the ESP32 glue, esp32-recon and the host tests were
  reformatted and their findings cleared with no behaviour change (host objects identical), which
  is why AmebaZ2 moves to 1.3.40 and ESP32 to 1.1.16 with nothing new on the bus.

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
