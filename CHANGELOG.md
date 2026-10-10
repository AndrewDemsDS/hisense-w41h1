# Changelog

Notable changes to this project. The format loosely follows [Keep a Changelog](https://keepachangelog.com/).
Firmware versions use the unified semver → softwareVersion-int scheme (see
[`firmware/src/version.txt`](firmware/src/version.txt) and
[`firmware/esp32-matter/CMakeLists.txt`](firmware/esp32-matter/CMakeLists.txt)).

## Unreleased

### ESPHome
- Confirm and retry. A command frame the unit does not answer is sent again in the next bus cycle
  (three sends at most). A command the unit answers and does not apply is sent again once its
  status shows that, four seconds later (twice at most, once for eco, turbo, quiet and sleep).
  After that the node logs a warning and shows what the unit reports. A newer command replaces
  the one being checked, and a lost link drops it. The unit's own overrides (its setpoint outside
  cool and heat, turbo, the fan under quiet and sleep) are not treated as lost commands. New
  optional sensors `command_retries` and `failed_commands`; `unanswered_commands` now counts every
  unanswered send.
- A mode request to a unit that is off is one frame carrying mode and power-on (byte 18 = mode
  bits with `0x0C`), as the stock module sends it. It was a power frame followed by a mode frame,
  and losing the second left the unit running in its last mode (#168). The re-send falls back to
  the two frames if the unit is still off.
- `beeper` switch: clears the buzzer bit (frame byte 23 bit 2) on every command frame the node
  sends. The bit comes from the stock module's `t_beep` handling, found by static analysis of the
  stock image. Stored on the node, on by default.

### Tooling
- `firmware/scripts/esphome-upstream-check.sh` runs ESPHome's own CI scripts against the
  `hisense_ac` component in a checkout of `esphome/esphome`. CI requires it to pass against the
  pinned ESPHome release and also reports the result against their `dev` branch. The component
  tests moved to `firmware/esphome/tests/components/hisense_ac/`, the path they take upstream.
- ESP32 1.1.18: carries the Matter glue fixes from the feedback-loop audit (writes that follow the
  node's own command inside the status lag, out-of-range status modes, the 3-bit status mode
  field). First build of that code for the ESP32 targets.
- ESP32 1.1.17: no firmware source change. First release built with ESP-IDF v5.5.5 and esp-matter
  `release/v1.6`, and the first ESP32 release through `dev.py`. Its delta patch against 1.1.16 is
  large because the toolchain changed.
- AmebaZ2 1.3.45: no firmware change. It is the first image built, staged and flashed through
  `dev.py` after the release scripts were removed, and it carries the commit pin for
  `ameba-rtos-matter`.
- `firmware/scripts/dev.py` is now the release engine for both Matter targets. The logic of
  `ota-release.sh`, `esp32-release.sh` and `ota-guards.sh` was ported into it and the three scripts
  are removed (#143). Use `dev.py ota amebaz2 <step>` and `dev.py ota esp32 <step>`; the steps and
  flags keep their names. `dev.py` is a build-clock input for AmebaZ2, so the port moves
  `SOURCE_DATE_EPOCH` for the tree. AmebaZ2 images were byte-identical between the script and
  `dev.py` at a fixed clock.

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
- ESPHome: log levels are used consistently. `WARN` for a lost link, an unsent command, a bad
  checksum or a fault. `INFO` for recovery. `DEBUG` for status changes and commands. `VERBOSE` for
  every status frame and unanswered poll. `VERY_VERBOSE` for the raw bytes of every frame. The
  once-a-second status line is no longer logged at `DEBUG`.
- ESPHome, **breaking**: fan modes are now `auto`, `low`, `medium_low`, `medium`, `medium_high`,
  `high`, matching `hisense-unified-ac`. `Medium-low` / `Medium-high` are renamed, and `quiet` is
  no longer a fan mode (use the `quiet` preset; the quiet step reads back as `low`). Update
  automations that set the old names.
- ESPHome, **breaking**: the unit's own auto mode is now the climate mode `auto` instead of
  `heat_cool`, so Home Assistant labels it "Auto". Update automations and climate groups that set
  `heat_cool`. The mapping lives in `esphome_aircon_map.h`, which is under `firmware/src/`, so the
  AmebaZ2 version moves to 1.3.46 with no change to any Matter image.
- ESPHome: the component is self-contained and builds on ESP8266 as well as ESP32. The fallback
  transport that ran the shared driver's bus task is removed, along with the `tx_override` bench
  services. State is published on change, with sensors refreshed once a minute, instead of on
  every status frame.

### Fixed
- AmebaZ2 1.3.48, and the ESP32 build from its next release: four faults found by an audit of the
  Matter write and readback paths after #168. The decisions are pure functions in
  `matter_aircon_map.h` with host tests. None has been on hardware yet.
  - Choosing the fan's Auto preset, or the A/C reporting fan Auto, made the AmebaZ2 node command
    the fan to High and then back to Auto. The FanControl server nulls PercentSetting and
    SpeedSetting when FanMode becomes Auto, and the null byte was read as 255 %.
  - A write that followed one of the node's own commands inside the second or two the status
    takes to catch up was compared with the old status and dropped: On then Off left the unit
    running, a mode chosen on a powered-down unit lost the setpoint sent with it, and Cool to Heat
    lost the heating setpoint. On AmebaZ2 a powered-down frame read during a power-on could also
    switch the unit off again through OnOff, the same way #168 describes for SystemMode. While a
    command settles (3 s), power and mode are now taken from what was commanded, for the guards
    and for what OnOff and SystemMode report. A mode the A/C ignored therefore reads back as the
    old one after 3 s, not at once.
  - A status frame with a mode nibble outside the known ones (0 to 3, 5 and 6) was copied into the
    command shadow, where it encodes as a different mode: the next setpoint or fan change could
    have switched the unit to Fan or Heat. The shadow now keeps its last good mode.
  - The status mode was read as four bits. The stock capability table defines it as three (byte
    18, bits 4 to 6), so a frame with bit 7 set decoded to an unknown mode and was reported as
    Cool. The driver and the ESPHome codec port now both mask three bits.
- AmebaZ2 1.3.47: the node no longer commands its own stale SystemMode readback. A status frame
  parsed before the A/C applied a mode change is published over the client's value, and if the
  status had moved on by the time that readback was handled it was sent to the A/C as a new
  request: Auto could go back to Cool, or the unit could be switched off again. The node now notes
  the values it publishes and never commands them. The decision is a pure function in
  `matter_aircon_map.h` with a host test. Not yet confirmed on hardware as the cause of #168.
- ESPHome: `w41h1.yaml` now integrates energy with `method: left`. Since power is published on
  change, the default `right` method misplaced up to a minute of energy at each compressor start
  and stop. Nodes built from an older copy of the YAML need the same line.
- AmebaZ2 1.3.44: General Diagnostics NetworkInterfaces now lists every valid IPv6 address, not only
  the link-local one. The Ameba port copied address slot 0 and hard-coded the count to 1, so the
  SLAAC and DHCPv6 addresses in slots 1 and up were never reported. Tentative and duplicated slots
  are skipped, as on ESP32 (#139).
- AmebaZ2 builds: the image's build date (`SOURCE_DATE_EPOCH`) is now the author date of the newest
  commit touching the image inputs, not the HEAD commit time. An image built on a branch and
  flashed before merging now matches the tag rebuild of the merge commit byte for byte, which is
  what the release workflow promises. `ota-release.sh epoch` prints the value, `build` refuses a
  shallow clone and warns on uncommitted inputs, and the release workflow checks out full history.
- AmebaZ2 1.3.43: the node is now dual-stack (IPv4 enabled in connectedhomeip) so it answers mDNS
  on 224.0.0.251 as well as ff02::fb. The Realtek SDK ships with IPv4 off, which left mDNS
  IPv6-only. Behind UniFi Multicast Enhancement the AP delivers IPv4 mDNS to IGMP members and no
  IPv6 mDNS at all, so the node never heard a query: it still announced itself, but a restarted
  controller could not resolve it and CASE failed until the node happened to reassociate. The ESP32
  build was already dual-stack.
- AmebaZ2 1.3.42: the app resends MLD and IGMP reports after each 4-way handshake. The Realtek SDK
  raises the lwIP link before the first association and leaves it up across disconnects, so lwIP's
  own link-up re-report never runs and the AP forgets the node's group memberships after a
  reassociation.
- AmebaZ2 1.3.33 -> 1.3.38: PercentSetting 42 / 75 landed one fan step up (58 / 100 %). The app
  publishes FanMode by folding the six speeds into Low/Medium/High, and that readback re-commanded
  the bucket's speed along two paths: the connectedhomeip patch mapped it onto PercentSetting
  33/66/100 (now skipped for writes flagged `gW41h1AppFanModeWrite`), and it was queued to the
  app's own FanMode handler as if a client wrote it (now recorded in an own-write echo ledger,
  `matter_echo_note/consume()`, and skipped). 1.3.35-1.3.37 tried a bucket comparison instead, which
  let a stale readback undo a fan-card Medium press. ESP32 builds its FanControl in code, has neither
  path, and was not affected.
- `ota-release.sh lint` (the pre-commit hook) no longer refuses a tree whose version equals the
  recorded on-device version, so the tree that was just flashed can be committed without
  `--no-verify`. A version below the device still fails. `flash` now refuses an unbumped version
  itself instead of relying on lint having run first. `.released-version` is one mark for the
  repo, not per node, so `OTA_ALLOW_SAME_VERSION=1` lets `flash` roll the same version out to
  another unit (equal only, with a warning). (#136)

### Tooling
- C/C++ lint for every tree we own (`firmware/scripts/cpp-lint.sh check|fix`), with ESPHome's
  clang-format and clang-tidy configs and the portable rules of its `ci-custom.py`, gated in CI
  and the pre-commit hook. The shared driver, the ESP32 glue, esp32-recon and the host tests were
  reformatted and their findings cleared with no behaviour change (host objects identical), which
  is why AmebaZ2 moves to 1.3.40 and ESP32 to 1.1.16 with nothing new on the bus.
- `ota-release.sh tag` and `esp32-release.sh tag` now ask GitHub whether a runner with the
  `sdk-builder` label is online before printing the push command, and warn if none is. Both release
  workflows run only on that self-hosted runner, so a tag pushed while it is down sat in `queued`
  with no error (#138). The check only warns. If `gh` is missing, not logged in, or the API call
  fails, it says the check was skipped and carries on.

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
