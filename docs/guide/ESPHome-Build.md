# ESPHome Build

The recommended firmware: the **same ESP32 board and wiring as the Matter replacement build**, but
running ESPHome instead of a Matter stack. The A/C arrives in Home Assistant as a native `climate`
device over the ESPHome API, with no commissioning and no `matter-server`. Source and full
rationale: `firmware/esphome/README.md` and `firmware/docs/15-esphome-path.md`.

← back to [Home](Home) · siblings: [ESP32 Replacement Build](ESP32-Replacement-Build) ·
[Repo Map and Build Pipeline](Repo-Map-and-Build-Pipeline) · [Protocol Overview](Protocol-Overview)

---

## Home Assistant only: read this first

Matter is what carries this A/C to Apple Home, Google Home and Alexa. ESPHome talks to Home
Assistant and to nothing else. If any controller other than HA needs to see the unit, flash the
Matter firmware instead ([ESP32 Replacement Build](ESP32-Replacement-Build)) and stop here. That one
question is the whole decision; everything below assumes the answer is "HA only".

## Why it exists

The RS-485 protocol is the expensive part of this project, and it is finished, hardware-validated
and covered by host tests. Everything above it exists to move decoded values into Home Assistant.
On the Matter path that transport costs a 1961-line app, a debug console with its own build
flavour, a release script that archives delta-OTA bases, test credentials, a commissioning flow,
and a companion HACS integration that decodes the diagnostics bitmaps. ESPHome replaces all of it with
a YAML file and a custom component of roughly 1250 lines, and every fault bit becomes its own
diagnostic entity instead of a bitmap that two repositories must agree on.

What you give up is Matter. The full three-way comparison, with measured image sizes and toolchain
footprints, is `firmware/docs/13-path-comparison.md` in the repo.

## What you get

| Entity | Covers |
|---|---|
| `climate` | power, mode (auto/cool/heat/dry/fan_only), setpoint 16 to 32, fan (auto, low, medium_low, medium, medium_high, high), swing, current temperature, action, special-mode presets |
| `switch` | Eco, Turbo, Quiet, panel display |
| `select` | Sleep profile (Off / General / Old / Young / Kids) |

Quiet is a preset, not a fan speed: the A/C only reaches it through its mute flag, and while it is
on the fan reads `low`. The fan and preset names match what `hisense-unified-ac` gives a Matter
node, so the same unit state looks identical on either firmware.

The climate presets are `none`, `eco`, `quiet`, `turbo`, `eco_quiet`, `sleep_general`,
`sleep_old`, `sleep_young`, `sleep_kids` and `eco_sleep_*` for each profile: the same names the
`hisense-unified-ac` integration gives a Matter node, so a climate group (for example Climate
Group Helper) can sync presets across units on either firmware. A preset that needs several bus
writes spaces them 10 s apart, because the A/C ignores a special-mode command that lands too soon
after the previous one, so a combined preset such as `eco_sleep_old` takes about 10 s to settle.
While turbo, quiet or a sleep profile is active they own the fan, and a fan change is refused
rather than silently undone a second later. The switches and the select still work and share the
same pacing, so keep them for dashboards or delete them if the presets are all you use.
| `sensor` | indoor, outdoor and coil temperature, compressor Hz, power, voltage, current, bus checksum errors, uptime, Wi-Fi signal |
| `binary_sensor` | aux heat relay, bus link, aggregate fault, 18 per-bit faults, 13 capability flags |
| `text_sensor` | A/C device type (the learned link bytes), reset reason |

Plus what ESPHome gives for free: OTA, a captive-portal AP fallback, logs streamed over the API on
the deployed image, and `total_daily_energy` feeding the HA Energy dashboard.

## Hardware

Identical to the Matter ESP32 track: an ESP32 board and a **3.3 V** RS-485 transceiver on the A/C's
4-pin connector. Use the pin tables and the GPIO warnings from
[ESP32 Replacement Build](ESP32-Replacement-Build) and [Hardware & Wiring](Hardware-and-Wiring)
without change, including the ground-loop rule for bench work and the warning against a 5 V MAX485
module. The shipped defaults are the validated classic-ESP32 set (TX 19, RX 18, DE 4); an ESP32-C3
SuperMini uses 5 / 6 / 10.

There is one file per board in `firmware/esphome/`, and each holds only the node's name, the three
pins and the platform block. The entities come from a shared package, so they are the same on
every board.

| File | Board | Status |
|---|---|---|
| `w41h1.yaml` | classic ESP32, TX 19 / RX 18 / DE 4 | runs on a live A/C |
| `w41h1-esp32c3.yaml` | ESP32-C3 SuperMini, 5 / 6 / 10 | runs on a live A/C |
| `w41h1-amebaz2-factory.yaml`, `-sdk.yaml`, `-native.yaml` | the stock W41H1 module itself, one file per flash layout | factory layout runs on one module in an A/C (2026-10-10); sdk and native compile only (see below) |
| `w41h1-esp8266.yaml`, `w41h1-rp2040.yaml` | ESP8266 D1 mini, Raspberry Pi Pico W | examples, compile only, never run |

TX and RX go to the `uart:` block and DE to the `hisense_ac:` component, all three through the
`tx_pin`, `rx_pin` and `de_pin` substitutions. The component drives DE itself with the
hardware-validated timing (5 ms settle, 25 ms drain).

## Flash it

The guided way, from the repo root (details in [Build, Flash & Test](Build-Flash-Test#esphome)):

```bash
python3 firmware/scripts/dev.py fetch esphome                            # esphome==2026.7.4 + secrets.yaml
python3 firmware/scripts/dev.py erase esphome --port /dev/ttyACM0        # factory-fresh board only
python3 firmware/scripts/dev.py flash esphome --board c3 --port /dev/ttyACM0
```

By hand:

```bash
pipx install esphome==2026.7.4            # the pinned version; plain pip is refused on recent distro Pythons
cd firmware/esphome
cp secrets.yaml.example secrets.yaml      # Wi-Fi credentials + an API encryption key
esphome run w41h1.yaml                    # classic ESP32: build, flash, follow the logs
esphome run w41h1-esp32c3.yaml            # C3 SuperMini
esphome logs w41h1.yaml                   # logs only, later
```

The older form, `esphome -s board esp32-c3-devkitm-1 -s tx_pin 5 -s rx_pin 6 -s de_pin 10 run
w41h1.yaml`, still works and builds the same node as `w41h1-esp32c3.yaml`. It is what `dev.py
--board c3` runs.

Home Assistant discovers the node over mDNS and adopts it with the API key from `secrets.yaml`.

On a factory-fresh board, erase first: `dev.py erase esphome --port <port>`, then flash with
`dev.py flash esphome --board <c3|classic> --port <port>`, so the board overrides are applied.
By hand, that is `esptool.py erase_flash` followed by the matching `esphome run` line above with
`--device <port>`. A stale vendor Wi-Fi config left in NVS is a documented time sink on these
boards. Never erase a board that is already running: that throws away its Wi-Fi settings.

## Bring it up in stages

Use the same three stages as the Matter track
([ESP32 Replacement Build](ESP32-Replacement-Build#staged-bring-up-never-leave-the-ac-in-an-unknown-state)),
with `dev.py test esphome` and `dev.py bench esphome` for stage 1. What passing looks like on this
firmware: in stage 1 the `AC bus link` sensor turns on and the climate entity populates; in stage 2
the decoded indoor temperature, mode and compressor Hz match the unit.

`firmware/test/hil_esphome_actuation.py` drives a real node over the API and checks two things per
control: that the command lands, and that nothing else moved. It snapshots and restores the unit's
state, so it leaves a live A/C as it found it. See [Testing & QA](Testing-and-QA).

## Capability gating is a YAML decision

The Matter builds hide eco, quiet and display at runtime when the A/C reports it lacks them,
because a commissioned Matter node's endpoint list is fixed. Here you delete the entities your unit
does not have. Flash with everything declared, read the `capability_*` binary sensors your A/C
answers with, then trim the YAML to match. For the presets, set `supports_eco`, `supports_quiet`,
`supports_turbo` or `supports_sleep` to `false` on the climate platform; every preset that needs
the missing mode disappears.

## The stock module, without a replacement board

The `w41h1-amebaz2-*.yaml` files build this same firmware for the chip inside the AEH-W41H1
itself, a Realtek RTL8710C, through ESPHome's LibreTiny platform. The module that came with the
A/C runs ESPHome and nothing is added to the unit. One module with the factory layout has run it
in an A/C since 2026-10-10. A unit that already runs this project's Matter firmware is converted
over the air: [Converting a Stock Module from Matter to ESPHome](Converting-a-Stock-Module-to-ESPHome).

There are three files because a module has one of three flash layouts, and the image must be
built for the one it has. An image built for another layout writes its first update to the wrong
place and then invalidates the running image, which leaves a unit only the clip recovers.

| File | For a module that |
|---|---|
| `w41h1-amebaz2-factory.yaml` | went from the vendor firmware to this project's Matter firmware over the air |
| `w41h1-amebaz2-sdk.yaml` | was first written with this project's clip image |
| `w41h1-amebaz2-native.yaml` | was written from scratch with LibreTiny's own partition table and bootloader |

If you do not know the unit's history, the Matter firmware (1.3.9 or later) can tell you:
`dev.py ota amebaz2 revert --backup <unit-ip>` prints the address of the inactive slot. `0x10000`
or `0x190000` means factory, `0xC000` or `0x1B8000` means sdk. Once a unit runs ESPHome, its
`Flash layout` diagnostic sensor reports whether the image matches the module, and an image that
does not match refuses updates.

**What was measured on that module (2026-10-10):** it boots under the factory bootloader and joins
Wi-Fi, the layout check reports a match, the A/C answers status polls and commands, and two
updates with `esphome upload` went through, each back within about 10 seconds. One mains power
cycle came back clean.

**One fault was found and fixed.** Twice the receive side of the bus port stopped and stayed
stopped until a power cycle, while commands still reached the unit. The cause is in LibreTiny's
receive buffer, which loses count of a byte when the receive interrupt lands inside a read. The
component now reads with interrupts masked on this platform. Reopening the port also clears it,
so the board package still restarts the port when the bus link drops and every 15 seconds while
it stays down, as a fallback. Each restart logs a warning and counts in the `Serial port
restarts` diagnostic sensor. On a unit that answers, that sensor stays at 0. If yours climbs
while the unit answers, it is recovering from something that is not understood yet. It also
climbs on a unit that does not answer at all, which a restart cannot cure: one of the two
converted modules ended that way, for a reason outside the chip that is still open.

**Not measured:** the sdk and native layouts (they compile only), any run longer than an
afternoon, an interrupted update, and the return to the Matter firmware. It is the newest and
least proven combination here. Before you put it into an A/C you depend on, the known risks:

- LibreTiny rates this chip family 2 out of 5 for stability.
- A restart or an update can leave the chip dark until power has been off for about 30 seconds
  (LibreTiny issue 396). The file carries the workaround reported there. With it, no hang was
  seen on the tested module.
- Nothing rolls a bad update back, unlike the ESP32. An image that does not boot needs the clip.
- A module that does not run this project's Matter firmware needs the SOIC-8 clip or UART download
  mode for the first flash ([Installing Custom Firmware](Installing-Custom-Firmware)). That route
  has not been used for this build.
- Flash `0x1000..0x3FFF` holds the module's own calibration data and must not be overwritten. The
  full-flash image the build produces is blank there, so it cannot be written as it is with the
  clip. The over-the-air conversion does not touch that region.
- Dump the whole chip first if you can. The dump is the only way back.

The checks done so far and the full list are in `firmware/docs/15-esphome-path.md`.

## Wi-Fi power saving and the temperature placeholder

**The radio stays on.** The node config turns Wi-Fi power saving off (`power_save_mode: none`).
ESPHome's default on an ESP32 lets the radio sleep between beacons, and on a board with a weak
signal that lost most pings and dropped commands
([the measurement](Migrating-ESP32-Matter-to-ESPHome#afterwards)). A board on a supply too weak
for an always-on radio can go back with `-s wifi_power_save light`.

**Outdoor and coil temperature read "unknown" after a power cut.** For a while after mains power
returns, the unit reports both as exactly -20 C, a placeholder it sends until it has real
readings. The firmware publishes that pair as unknown and starts publishing once real values
arrive. A real -20 C is still shown once the unit has reported anything else since the node
started. This rests on one observation on one unit (2026-10-10).

## One component, its own codec

The component under `firmware/esphome/components/hisense_ac/` carries its own port of the protocol
code and talks to the bus through ESPHome's `uart:` component. It uses nothing from ESP-IDF, which
is why it builds for every board in the table above. The port cannot drift from the shared driver
the Matter builds use: a host test runs every frame builder and parser of both over the same
inputs and compares them byte for byte, and it runs in CI. A protocol fix lands in the shared
driver first and is then ported until that test passes again.

## Status

Newest of the three tracks. Phases 1 to 3 (hub and climate, the full control surface, telemetry and
diagnostics) are done and the firmware has driven a live A/C from Home Assistant since 2026-08,
including all four sleep profiles. Units run it from the connector's 5 V rail, closed up (stage 3
of the bring-up). A sniffer pass on the wire is still open, and this is the track with the least
field time. Progress is tracked in `firmware/docs/15-esphome-path.md` and issue #13.
