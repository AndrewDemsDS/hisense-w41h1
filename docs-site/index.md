---
title: Home
nav_order: 1
description: >-
  Run a Hisense air conditioner locally from Home Assistant with no ConnectLife cloud: ESPHome
  firmware on an ESP32 in the module bay, or on the stock AEH-W41H1 module itself.
---

# Hisense AEH-W41H1: local A/C control with ESPHome

Run a Hisense air conditioner from Home Assistant with no ConnectLife account and no cloud. The
A/C's `AEH-W41H1` Wi-Fi module talks to the unit over an internal RS-485 bus. This project puts
firmware you control on that bus: an ESPHome component, `hisense_ac`, that presents the unit as a
native climate device.

It runs on three kinds of hardware:

| Hardware | State |
|---|---|
| An ESP32 board with an RS-485 transceiver, in place of the module (recommended) | on live units since 2026-08 |
| The stock module itself, through ESPHome's LibreTiny platform | one module in an A/C since 2026-10-10. No rollback, and a clip on the flash chip is the only recovery |
| Other ESPHome chips (ESP8266, RP2040) | example configs that compile. Never run |

Start with the **[User Guide](guide/User-Guide.html)**: it compares the three and names the page
to read next. Not sure your unit is the right kind?
**[Check compatibility](compatibility.html).**

## The guide

| Page | What it covers |
|---|---|
| [User Guide](guide/User-Guide.html) | choose the hardware, then the route to a working unit |
| [Hardware and Wiring](guide/Hardware-and-Wiring.html) | the module, the 4-pin port, the RS-485 bus |
| [ESP32 Replacement Board](guide/ESP32-Replacement-Build.html) | parts, wiring, the pins to avoid, staged bring-up |
| [The Stock Module](guide/Stock-Module-Build.html) | ESPHome on the original module: state, risks, flash layouts |
| [Build Environment](guide/ESP32-Build-Environment.html) | the tools, the secrets file, the first flash by hand |
| [Build, Flash and Test](guide/Build-Flash-Test.html) | the `dev.py` wrapper and the bench test without an A/C |
| [ESPHome Firmware](guide/ESPHome-Build.html) | board files, adding the node to Home Assistant, updates, releases |
| [Everyday Control](guide/Everyday-Control.html) | modes, fan, swing, Eco, Quiet, Turbo and Sleep, and their limits |
| [Entities and Diagnostics](guide/Entities-and-Diagnostics.html) | every entity, the bus counters, the log lines |
| [Climate Groups](guide/Climate-Groups.html) | several A/Cs as one thermostat |
| [Troubleshooting and FAQ](guide/FAQ-Gotchas.html) | by symptom |
| [Wi-Fi Setup Mode and the Status LED](guide/Wifi-Setup-Mode.html) | moving a node to another network, the "77" gesture, safe mode |
| [Recovery and Reflash](guide/Recovery-and-Reflash.html) | getting back from a bad flash |

For contributors: [Repo Map and Build Pipeline](guide/Repo-Map-and-Build-Pipeline.html),
[Testing and QA](guide/Testing-and-QA.html), [Protocol Overview](guide/Protocol-Overview.html).

## Firmware notes

Design notes and hardware reference behind the guide.

- [Choosing the hardware](firmware/13-path-comparison.html): the ESP32 board and the stock module
  side by side, with measured figures
- [The ESPHome firmware: design, and the stock module](firmware/15-esphome-path.html)
- [The stock module: how it boots, and recovery](firmware/10-stock-module-boot-and-recovery.html)
- [ESP32 board notes](firmware/16-esp32-board-notes.html) · [QA strategy](firmware/04-qa-strategy.html) · [Energy monitoring](firmware/09-energy-monitoring.html)
- [Stock parity gaps](firmware/07-stock-parity-gaps.html): what the vendor firmware does that this one does not

## Reverse engineering

The protocol and firmware analysis, for porting this to another Hisense unit or checking the
claims:

- [RS-485 A/C protocol](internals/03-rs485-ac-protocol.html): framing, checksum, every byte offset
- [Stock firmware init and comms](internals/10-stock-fw-init-and-comms.html): disassembly of the vendor module
- [Device-type to capability map](internals/11-model-capability-map.html): how the A/C advertises its own features
- [Hardware](internals/01-hardware.html) · [Cloud and firewall](internals/04-cloud-and-firewall.html) · [ESP32 replacement](internals/05-esp32-replacement.html)

## Retired: the Matter firmware

Until 2026-10-12 the project also shipped two Matter firmwares. Both are retired and live on
under the tag `matter-final`. What that means for a unit that runs one, and how to move it to
ESPHome: [Matter Firmware (Retired)](guide/Matter-Firmware-Retired.html).

## Source

Code, issues and releases: [github.com/AndrewDemsDS/hisense-w41h1](https://github.com/AndrewDemsDS/hisense-w41h1).
A release carries no firmware image. An image holds its builder's Wi-Fi credentials and keys, so
you build your own.

Not affiliated with Hisense, Realtek or ESPHome. Reverse engineering of hardware you own, for
interoperability.
