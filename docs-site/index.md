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

Two hardware paths are documented and both are running on real units:

- **AmebaZ2** — reflash the stock W41H1 module in place. No added hardware.
- **ESP32** — drop an ESP32 plus an RS-485 transceiver into the module bay. Useful when the W41H1
  is dead or unobtainable, which is common.

Everything below is written from a working system, not a plan. The RS-485 protocol was
reverse-engineered from the stock firmware and validated against live hardware.

## Not sure if your A/C is supported?

**[Check compatibility](compatibility.html)** — which units are confirmed, and how to tell in a minute
without opening anything.

## Start here

| | |
|---|---|
| [Hardware and wiring](guide/Hardware-and-Wiring.html) | pinout, the 4-pin module port, RS-485 A/B |
| [Installing the custom firmware](guide/Installing-Custom-Firmware.html) | CH341A clip, or convert a stock unit over the air |
| [Commissioning and Home Assistant](guide/Commissioning-and-HA-Setup.html) | pairing into python-matter-server and HA |
| [Everyday control](guide/Everyday-Control.html) | modes, fan, swing, Eco / Quiet / Turbo / Sleep |
| [OTA updates](guide/OTA-Updates.html) | Matter OTA, the break-glass HTTP path, and the serial trap |
| [Recovery and reflash](guide/Recovery-and-Reflash.html) | getting back from a bad flash |
| [FAQ and gotchas](guide/FAQ-Gotchas.html) | the things that actually bite |

## Choosing a path

[ESP32 vs AmebaZ2](firmware/13-path-comparison.html) compares the two on cost, toolchain,
reproducibility, OTA mechanics, flash headroom and diagnostics, with figures measured on this
project's own hardware rather than taken from datasheets.

## Reverse engineering

The protocol and firmware analysis, if you want to port this to another Hisense unit or verify the
claims:

- [RS-485 A/C protocol](internals/03-rs485-ac-protocol.html) — framing, checksum, every byte offset
- [Stock firmware init and comms](internals/10-stock-fw-init-and-comms.html) — disassembly of the stock dongle
- [Device-type to capability map](internals/11-model-capability-map.html) — how the A/C advertises its own features
- [Hardware](internals/01-hardware.html) · [Cloud and firewall](internals/04-cloud-and-firewall.html) · [ESP32 replacement](internals/05-esp32-replacement.html)

## Firmware and build

- [Firmware build and OTA procedure](firmware/10-firmware-ota-procedure.html) — the canonical reference
- [Matter clusters exposed](firmware/01-expose-all-clusters.html) · [Attestation](firmware/02-fix-attestation.html)
- [QA strategy](firmware/04-qa-strategy.html) · [Energy monitoring](firmware/09-energy-monitoring.html)
- [Stock parity gaps](firmware/07-stock-parity-gaps.html) — what the stock firmware does that this does not, yet

## Source

Code, issues and releases: [github.com/AndrewDemsDS/hisense-w41h1](https://github.com/AndrewDemsDS/hisense-w41h1)

Uses Matter **test** credentials, so this is for development and personal use. It is not a
certified Matter product and is not affiliated with Hisense, Realtek or the CSA. Reverse
engineering of hardware you own, for interoperability.
