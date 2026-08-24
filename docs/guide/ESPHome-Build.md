# ESPHome Build

The third firmware track: the **same ESP32 board and wiring as the Matter replacement build**, but
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
| `climate` | power, mode (auto/cool/heat/dry/fan_only), setpoint 16 to 32, 7-step fan, swing, current temperature, action |
| `switch` | Eco, Turbo, Quiet, panel display |
| `select` | Sleep profile (Off / General / Old / Young / Kids) |
| `sensor` | indoor, outdoor and coil temperature, compressor Hz, power, voltage, current, bus checksum errors |
| `binary_sensor` | aux heat relay, bus link, aggregate fault, 18 per-bit faults, 13 capability flags |
| `text_sensor` | A/C device type (the learned link bytes) |

Plus what ESPHome gives for free: OTA, a captive-portal AP fallback, logs streamed over the API on
the deployed image, and `total_daily_energy` feeding the HA Energy dashboard.

## Hardware

Identical to the Matter ESP32 track: an ESP32 board and a **3.3 V** RS-485 transceiver on the A/C's
4-pin connector. Use the pin tables and the GPIO warnings from
[ESP32 Replacement Build](ESP32-Replacement-Build) and [Hardware & Wiring](Hardware-and-Wiring)
without change, including the ground-loop rule for bench work and the warning against a 5 V MAX485
module. The shipped defaults are the validated classic-ESP32 set (TX 19, RX 18, DE 4); an ESP32-C3
SuperMini uses 5 / 6 / 10.

There is deliberately **no `uart:` block** in the YAML. The driver's own HAL opens the port so the
DE timing that took a multi-day debug to find stays exactly as validated. Pins are set on the
`hisense_ac:` component instead.

## Flash it

```bash
pip install esphome                       # tested against 2026.7.4
cd firmware/esphome
cp secrets.yaml.example secrets.yaml      # Wi-Fi credentials + an API encryption key
esphome run w41h1.yaml                    # build, flash, then follow the logs
esphome logs w41h1.yaml                   # logs only, later
```

Home Assistant discovers the node over mDNS and adopts it with the API key from `secrets.yaml`.

On a factory-fresh board, erase first (`esptool.py erase_flash`, then `esphome run w41h1.yaml
--device <port>`). A stale vendor Wi-Fi config left in NVS is a documented time sink on these
boards.

## Bring it up in stages

Same three stages as the Matter track, for the same reason: never leave the A/C in an unknown
state.

1. **Bench, no A/C.** Run the host tests (`firmware/test/run_tests.sh`, which includes the ESPHome
   mapping test), then drive the firmware against `firmware/test/virtual_ac.py` over a USB-TTL
   adapter. Watch the `AC bus link` sensor go on and the climate entity populate.
2. **Real bus, USB-powered.** Tap **A and B only**, mind the ground-loop warning, and confirm the
   decoded status: indoor temperature, mode, compressor Hz. This proves the read direction.
3. **Full integration.** Power from the connector's 5 V rail and close the unit up.

`firmware/test/hil_esphome_actuation.py` drives a real node over the API and checks two things per
control: that the command lands, and that nothing else moved. It snapshots and restores the unit's
state, so it leaves a live A/C as it found it. See [Testing & QA](Testing-and-QA).

## Capability gating is a YAML decision

The Matter builds hide eco, quiet and display at runtime when the A/C reports it lacks them,
because a commissioned Matter node's endpoint list is fixed. Here you delete the entities your unit
does not have. Flash with everything declared, read the `capability_*` binary sensors your A/C
answers with, then trim the YAML to match.

## Shared code, one copy

Nothing under `firmware/esphome/` reimplements the protocol. The component registers
`firmware/src/rs485-driver/` and the ESP-IDF HAL from the Matter build as **local ESP-IDF
components**, so both compile in place, unmodified, with no sync step and no second copy. A
protocol fix lands once and reaches all three firmwares, which is how the two 2026-08 bus fixes
(the panel-display collateral and the missing single-field frame marker) reached the Matter builds
from ESPHome bring-up. The only ESPHome-specific file is an enum-mapping header covered by a host
test.

Symlinks and copied headers both fail here: on the ESP-IDF framework ESPHome forwards only `-D` and
`-W` compiler flags, so no `-I` can reach the driver's angle-bracket HAL includes. Registering real
IDF components is what makes them resolve.

## Status

Newest of the three tracks. Phases 1 to 3 (hub and climate, the full control surface, telemetry and
diagnostics) are done and the firmware has driven a live A/C from Home Assistant since 2026-08,
including all four sleep profiles. Stage 3 of the bring-up (running from the connector's 5 V rail,
closed up) and a sniffer pass on the wire are still open, so treat this track as the one with the
least field time. Progress is tracked in `firmware/docs/15-esphome-path.md` and issue #13.
