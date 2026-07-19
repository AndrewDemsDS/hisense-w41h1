# Hisense AEH-W41H1: Reverse Engineering & Local Control

Reverse-engineering notes and tooling for the **Hisense `AEH-W41H1`** air-conditioner
Wi-Fi module (ConnectLife), with the goal of **fully local control**: no ConnectLife
cloud, no Azure.

The W41H1 is a newer ConnectLife-generation dongle. The older `AEH-W4A1` has ready-made
local integrations; the W41H1 does not, which is what motivated this work.

## TL;DR: two independent local-control paths

1. **Matter (preferred, no hardware).** The module's firmware is a Matter thermostat
   (Realtek RTL8710C running `connectedhomeip`). It can be commissioned into a local
   Matter controller (e.g. Home Assistant) and then firewalled off the internet. See
   [`docs/02-matter-local-control.md`](docs/02-matter-local-control.md).
2. **ESP32 replacement (fallback).** The module talks to the A/C mainboard over **RS-485**
   (`F4 F5 … F4 FB` frames, 9600 baud). An ESP32 + RS-485 transceiver running ESPHome can
   replace the dongle entirely. Frame format:
   [`docs/03-rs485-ac-protocol.md`](docs/03-rs485-ac-protocol.md); full command↔function
   map + value tables: [`docs/05-esp32-replacement.md`](docs/05-esp32-replacement.md);
   ready-to-flash config: [`esphome/w41h1-esp32.yaml`](esphome/w41h1-esp32.yaml).

## What's in the box (hardware)

| Part | Marking | Role |
|------|---------|------|
| Wi-Fi SoC | Realtek **RTL8710C** (AmebaZ2, FreeRTOS v10.0.1) | main MCU / Wi-Fi / BLE / Matter |
| Flash | GigaDevice **GD25Q32(B)** (4 MB SPI NOR) | external firmware storage |
| Transceiver | Union Semi **UM3352E** (MAX485-compatible) | RS-485 to the A/C mainboard |
| Board | `MW8415C.02`, Qingdao Hisense | carrier PCB |
| Power | 5 V / 450 mA via the A/C's 4-pin port | – |

Full detail: [`docs/01-hardware.md`](docs/01-hardware.md).

## Repo layout

```
docs/     protocol + hardware + Matter + cloud/firewall + ESP32 command map
tools/    sniff.py, flash dump + Matter-code extractors, protocol disassembler, QR gen
esphome/  ready-to-flash ESP32 replacement config
hardware/ pinouts, the Matter QR image
```

## Status

- [x] Identify SoC / flash / transceiver
- [x] Dump the SPI flash (verified, 2× identical reads)
- [x] Confirm Matter thermostat firmware + extract the setup code
- [x] Decode the RS-485 frame format from firmware (`F4 F5 … F4 FB`, len, checksum)
- [x] Map commands → A/C functions (mode/temp/fan/swing/features) + value tables
- [x] Draft ESPHome ESP32-replacement config
- [x] Verify the command bytes on the unit with a live capture (`tools/sniff.py`)
- [x] Document the OTA mechanism + custom-firmware feasibility (`docs/07`)

## Note on the firmware image

The raw flash dump is **intentionally not committed**: it contains the copyrighted
vendor firmware and the provisioned Wi-Fi credentials. It's kept locally and gitignored.
All analysis here is derived from it. Reverse engineering is for **interoperability with
hardware you own**.

## AI assistance

Parts of this reverse-engineering and documentation were produced with AI assistance.
