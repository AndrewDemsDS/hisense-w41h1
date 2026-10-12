---
title: Home
nav_order: 1
description: >-
  Run a Hisense air conditioner locally from Home Assistant with no ConnectLife cloud: ESPHome
  firmware on an ESP32 in the module bay, or on the stock AEH-W41H1 module itself.
---

# Hisense AEH-W41H1: de-cloud your air conditioner

Run a **Hisense air conditioner entirely locally** through Home Assistant, with no ConnectLife
account and no cloud dependency. The A/C's `AEH-W41H1` Wi-Fi module talks to the unit over an
internal RS-485 bus; this project replaces what sits on that bus with firmware you control.

There is one firmware, an ESPHome component, and three kinds of hardware it runs on:

1. **An ESP32 board** (recommended): drop an ESP32 plus an RS-485 transceiver into the module bay
   and the A/C appears natively in Home Assistant. No pairing code, no extra integration.
2. **The stock W41H1 module** itself, through ESPHome's LibreTiny platform. No added hardware, but
   no rollback, and a clip on the flash chip is the only recovery. First run in an A/C on
   2026-10-10.
3. **Other ESPHome chips** (ESP8266, RP2040): example configs that compile and have not been run.

One script, `python3 firmware/scripts/dev.py`, tests, builds and flashes.
Start with the **[User Guide](guide/User-Guide.html)**.

## Matter firmware (retired)

Until 2026-10 the project also shipped two Matter firmwares. Both are retired
([issue #182](https://github.com/AndrewDemsDS/hisense-w41h1/issues/182)) and live on under the tag
`matter-final`. What that means for a unit that runs one, and how to move it to ESPHome:
[Matter firmware (retired)](guide/Matter-Firmware-Retired.html).

Everything below is written from a working system, not a plan. The RS-485 protocol was
reverse-engineered from the stock firmware and validated against live hardware.

## Not sure if your A/C is supported?

**[Check compatibility](compatibility.html)**: which units are confirmed, and how to tell in a minute
without opening anything.

## Start here

| Page | What it covers |
|---|---|
| [User guide](guide/User-Guide.html) | pick the hardware, then `dev.py` from clone to running node |
| [Hardware and wiring](guide/Hardware-and-Wiring.html) | pinout, the 4-pin module port, RS-485 A/B |
| [Build environment](guide/ESP32-Build-Environment.html) | from a clean Linux machine to a flashed board, every step by hand |
| [ESPHome build](guide/ESPHome-Build.html) | the firmware: entities, boards, adopting it in Home Assistant, updates |
| [ESP32 replacement board](guide/ESP32-Replacement-Build.html) | parts, wiring, the pins to avoid, staged bring-up |
| [Everyday control](guide/Everyday-Control.html) | modes, fan, swing, Eco / Quiet / Turbo / Sleep |
| [Entities and diagnostics](guide/Entities-and-Diagnostics.html) | every entity, the beeper, the bus counters, confirm and retry |
| [Climate groups](guide/Climate-Groups.html) | several A/Cs as one thermostat |
| [Recovery and reflash](guide/Recovery-and-Reflash.html) | getting back from a bad flash |
| [FAQ and gotchas](guide/FAQ-Gotchas.html) | the things that actually bite |

## Choosing the hardware

Use an ESP32 board unless you have a reason to keep the original module.
[The comparison](firmware/13-path-comparison.html) sets the two side by side on cost, first flash,
recovery, toolchain and maturity, with figures measured on this project's own hardware.

Apple Home, Google Home and Alexa are reached through Home Assistant's own Matter or HomeKit
bridge.

## Reverse engineering

The protocol and firmware analysis, if you want to port this to another Hisense unit or verify the
claims:

- [RS-485 A/C protocol](internals/03-rs485-ac-protocol.html): framing, checksum, every byte offset
- [Stock firmware init and comms](internals/10-stock-fw-init-and-comms.html): disassembly of the stock dongle
- [Device-type to capability map](internals/11-model-capability-map.html): how the A/C advertises its own features
- [Hardware](internals/01-hardware.html) · [Cloud and firewall](internals/04-cloud-and-firewall.html) · [ESP32 replacement](internals/05-esp32-replacement.html)

## Firmware notes

- [The ESPHome firmware: design and the stock module](firmware/15-esphome-path.html)
- [The stock module: how it boots, and recovery](firmware/10-stock-module-boot-and-recovery.html)
- [ESP32 board notes](firmware/16-esp32-board-notes.html) · [QA strategy](firmware/04-qa-strategy.html) · [Energy monitoring](firmware/09-energy-monitoring.html)
- [Stock parity gaps](firmware/07-stock-parity-gaps.html): what the stock firmware does that this does not, yet

## Source

Code, issues and releases: [github.com/AndrewDemsDS/hisense-w41h1](https://github.com/AndrewDemsDS/hisense-w41h1)

Not affiliated with Hisense, Realtek or ESPHome. Reverse engineering of hardware you own, for
interoperability.
