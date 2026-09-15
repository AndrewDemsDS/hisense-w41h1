---
title: Hisense AEH-W41H1 de-cloud with custom Matter firmware
nav_order: 1
description: >-
  Replace the ConnectLife cloud on a Hisense AEH-W41H1 (Realtek RTL8710C / AmebaZ2) air-conditioner
  Wi-Fi module with custom Matter firmware for local Home Assistant control. Zero cloud.
---

# Hisense AEH-W41H1: de-cloud your air conditioner

Run a **Hisense air conditioner entirely locally** through Home Assistant, with no ConnectLife
account and no cloud dependency, by replacing the firmware on its `AEH-W41H1` Wi-Fi module
(Realtek **RTL8710C / AmebaZ2**) with a custom **Matter** build.

Three firmwares are documented, all running on real units. In order of preference:

1. **ESPHome** (recommended): drop an ESP32 plus an RS-485 transceiver into the module bay and the
   A/C appears natively in Home Assistant. No commissioning, no matter-server.
2. **ESP32 with Matter**: the same board running Matter, for controllers beyond Home Assistant.
3. **AmebaZ2**: reflash the stock W41H1 module in place with Matter. No added hardware, but a
   one-time clip write.

One script, `python3 firmware/scripts/dev.py`, builds, flashes, tests and updates all three.
Start with the **[User Guide](guide/User-Guide.html)**.

Everything below is written from a working system, not a plan. The RS-485 protocol was
reverse-engineered from the stock firmware and validated against live hardware.

## Not sure if your A/C is supported?

**[Check compatibility](compatibility.html)**: which units are confirmed, and how to tell in a minute
without opening anything.

## Start here

| | |
|---|---|
| [User guide](guide/User-Guide.html) | pick a firmware, then `dev.py` from clone to running node |
| [Hardware and wiring](guide/Hardware-and-Wiring.html) | pinout, the 4-pin module port, RS-485 A/B |
| [ESPHome build](guide/ESPHome-Build.html) | recommended: ESP32, native to Home Assistant |
| [ESP32 Matter build](guide/ESP32-Replacement-Build.html) | the same ESP32 board running Matter |
| [Installing on the AmebaZ2 module](guide/Installing-Custom-Firmware.html) | flashing the stock module over a CH341A SPI clip |
| [Commissioning and Home Assistant](guide/Commissioning-and-HA-Setup.html) | pairing a Matter build into python-matter-server and HA |
| [Everyday control](guide/Everyday-Control.html) | modes, fan, swing, Eco / Quiet / Turbo / Sleep |
| [OTA updates](guide/OTA-Updates.html) | Matter OTA, the break-glass HTTP path, and the serial trap |
| [Recovery and reflash](guide/Recovery-and-Reflash.html) | getting back from a bad flash |
| [FAQ and gotchas](guide/FAQ-Gotchas.html) | the things that actually bite |

## Choosing a path

If Home Assistant is your only controller, use the [ESPHome build](guide/ESPHome-Build.html). If
anything else (Apple Home, Google Home, Alexa) must see the A/C, use Matter on the ESP32, or on the
stock AmebaZ2 module if you want to keep the original hardware.
[The path comparison](firmware/13-path-comparison.html) sets the three tracks side by side on cost,
toolchain, reproducibility, OTA mechanics, flash headroom and diagnostics, with figures measured on
this project's own hardware rather than taken from datasheets.

## Reverse engineering

The protocol and firmware analysis, if you want to port this to another Hisense unit or verify the
claims:

- [RS-485 A/C protocol](internals/03-rs485-ac-protocol.html): framing, checksum, every byte offset
- [Stock firmware init and comms](internals/10-stock-fw-init-and-comms.html): disassembly of the stock dongle
- [Device-type to capability map](internals/11-model-capability-map.html): how the A/C advertises its own features
- [Hardware](internals/01-hardware.html) · [Cloud and firewall](internals/04-cloud-and-firewall.html) · [ESP32 replacement](internals/05-esp32-replacement.html)

## Firmware and build

- [Firmware build and OTA procedure](firmware/10-firmware-ota-procedure.html): the canonical reference
- [Matter clusters exposed](firmware/01-expose-all-clusters.html) · [Attestation](firmware/02-fix-attestation.html)
- [QA strategy](firmware/04-qa-strategy.html) · [Energy monitoring](firmware/09-energy-monitoring.html)
- [Stock parity gaps](firmware/07-stock-parity-gaps.html): what the stock firmware does that this does not, yet

## Source

Code, issues and releases: [github.com/AndrewDemsDS/hisense-w41h1](https://github.com/AndrewDemsDS/hisense-w41h1)

Uses Matter **test** credentials, so this is for development and personal use. It is not a
certified Matter product and is not affiliated with Hisense, Realtek or the CSA. Reverse
engineering of hardware you own, for interoperability.
