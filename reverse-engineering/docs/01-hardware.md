# Hardware

## Overview

The AEH-W41H1 is a sealed plastic dongle that plugs into the A/C indoor unit via a
**4-pin connector** carrying **5 V, GND, and an RS-485 A/B pair**.

Inside, under an RF shield can, is the Wi-Fi SoC + its external flash. The RS-485
transceiver sits outside the shield next to the cable connector.

## Components

### Wi-Fi SoC — Realtek RTL8710C (AmebaZ2)
- ARM Cortex-M, FreeRTOS v10.0.1.
- Runs the Wi-Fi stack, BLE (used for Matter commissioning), the Matter
  (`connectedhomeip`) stack, the ConnectLife cloud client, and the RS-485 driver to the A/C.
- Firmware confirms strings: `AmebaZIIRTL8710C`, `amebaz2`, `ARM_RTL8710C`.

### Flash — GigaDevice GD25Q32(B)
- 4 MB (32 Mbit) SPI NOR, SOIC-8, JEDEC ID `C8 40 16`.
- Standard 25-series pinout: `1=CS 2=DO 3=WP 4=GND 5=DI 6=CLK 7=HOLD 8=VCC`.
- Fully dumpable with a CH341A + SOIC-8 clip; see [`tools/dump_flash.sh`](../tools/dump_flash.sh).
- Two firmware images (OTA1 + OTA2) + a KV/FTL config partition near the top of flash.

### RS-485 transceiver — Union Semiconductor UM3352E
- MAX485-compatible, 8-pin SOIC. This is the A/C communication path.
- Pinout: `1=RO 2=/RE 3=DE 4=DI 5=GND 6=A 7=B 8=VCC`.
- Bus A/B (pins 6/7) go to the A/C mainboard. RO (pin 1) = incoming bytes as
  logic-level UART; DI (pin 4) = outgoing bytes. Handy sniff points.

## Dumping the flash (CH341A)

1. Set the CH341A to **programmer mode** (it enumerates as USB `1a86:5512`; the
   TTL/UART mode is `1a86:5523` and won't work with flashrom).
2. Clip the SOIC-8 onto the GD25Q32 (pin 1 = dot corner). In-circuit reads often fail
   because the SoC back-powers/contends the bus — desolder or lift the flash, or hold the
   SoC in reset, if you get `0xFF`/no-device.
3. `flashrom -p ch341a_spi` should identify `GD25Q32(B)`. Then read twice and compare.

> ⚠️ The common black CH341A drives the SPI lines at ~5 V even in "3.3 V" mode — a real
> hazard for 3.3 V flash. Use a 3.3 V-modded board or a level adapter if you can.
