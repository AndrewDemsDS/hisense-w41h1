# Hardware

What is inside the AEH-W41H1 module, how it connects to the A/C, and how its flash was read. The
pin diagrams of the two chips and of the 4-pin connector are in
[`../hardware/pinouts.md`](../hardware/pinouts.md).

## Status of this page

Current reference. The part numbers were read off the board, the flash id comes from the dump
tool, the pin functions of the SoC come from the firmware (doc 10 §2.1), and the connector's pin
order was measured on the bench. Nothing here is inferred from another model.

## Overview

The module is a sealed plastic dongle that plugs into the indoor unit through a 4-pin connector
carrying 5 V, ground and an RS-485 pair. Under an RF shield sit the Wi-Fi SoC and its flash. The
RS-485 transceiver is outside the shield, next to the cable connector.

| Part | Marking | Role |
|---|---|---|
| Wi-Fi SoC | Realtek RTL8710C (AmebaZ2), ARM Cortex-M, FreeRTOS v10.0.1 | Wi-Fi, BLE, the vendor's Matter stack and cloud client, the bus driver |
| Flash | GigaDevice GD25Q32(B), 4 MB SPI NOR, SOIC-8, JEDEC id `C8 40 16` | firmware and settings |
| Transceiver | Union Semiconductor UM3352E, SOIC-8, MAX485 compatible | RS-485 to the A/C mainboard |
| Board | `MW8415C.02`, Qingdao Hisense | carrier |
| Power | 5 V, 450 mA, from the A/C's 4-pin port | |

The firmware confirms the SoC with the strings `AmebaZIIRTL8710C`, `amebaz2` and `ARM_RTL8710C`.

The flash holds two firmware slots and the settings near the top. The layout is in
[13-stock-fw-feature-map.md](13-stock-fw-feature-map.md#persistent-storage-map).

On the transceiver, pins 6 and 7 (A and B) go to the A/C mainboard. Pin 1 (RO) carries the bytes
the A/C sends and pin 4 (DI) the bytes the module sends, both as logic-level UART, which makes
them the two points to tap. The SoC drives pin 3 (DE) from `PA_17`
([03-rs485-ac-protocol.md](03-rs485-ac-protocol.md#physical-layer)).

## Reading the flash with a CH341A

1. Set the CH341A to programmer mode. It then enumerates as USB `1a86:5512`. In TTL/UART mode it
   is `1a86:5523` and will not work.
2. Clip onto the GD25Q32. Pin 1 is the corner with the dot. A read in circuit often fails,
   because the SoC powers up through the clip and drives the SPI lines. If the read returns
   `0xFF` or no device, hold the SoC in reset, or lift or desolder the flash.
3. Run `python3 firmware/flasher/ch341dump.py dump.bin`. It checks the chip id (`c84016`), reads
   the chip twice and writes the file only when both reads agree.

Do not write with flashrom on this programmer: it leaves silent partial writes. The project's own
tools are used for reading too, so there is one path. Writing and recovery are covered in
[`docs/guide/Recovery-and-Reflash.md`](../../docs/guide/Recovery-and-Reflash.md).

The common black CH341A drives the SPI lines at about 5 V even in its "3.3 V" mode. That is a
hazard for a 3.3 V flash. Use a board modified for 3.3 V or a level adapter.

A dump holds the Wi-Fi credentials and the device's keys. It never goes into the repository
([`dumps/README.md`](../../dumps/README.md)).
