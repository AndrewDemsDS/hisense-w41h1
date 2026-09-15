# Hisense AEH-W41H1: de-cloud with custom Matter firmware

Drop the ConnectLife cloud from a Hisense **`AEH-W41H1`** A/C Wi-Fi module (Realtek **RTL8710C /
AmebaZ2**) and run the A/C from Home Assistant with no cloud. Custom **Matter** firmware on the
module bridges Matter attributes to the A/C's internal **RS-485** bus (9600 8N1). The framing and
every byte offset are hardware-confirmed against a real unit.

![The AEH-W41H1 module](images/module-external.png)

*The module: a small board in a white housing on a 4-pin cable to the indoor unit. (Photo: FCC ID
2AGCCAEH-W41H1, public record.)*

> Not affiliated with Hisense, Realtek, or the CSA. The firmware uses Matter **test** credentials,
> so it is for development and personal use: uncertified, not for sale. A bad flash can brick the
> module (recoverable from a stock dump). Do this at your own risk.

## Status

Three firmwares share one driver. In order of preference:

1. **ESPHome (recommended):** an ESP32 board and a 3.3 V RS-485 transceiver replace the dongle on
   the same 4-pin bus, and the A/C appears in Home Assistant over the ESPHome native API. No
   commissioning, no matter-server. On a live unit since 2026-08; see [ESPHome Build](ESPHome-Build).
2. **ESP32 with Matter:** the same board and wiring running esp-matter, for when a controller other
   than Home Assistant must see the unit. Runs a live unit, commissioned and updated over Matter OTA;
   see [ESP32 Replacement Build](ESP32-Replacement-Build).
3. **AmebaZ2 module:** the custom Matter `room_air_conditioner` firmware on the stock module. You
   convert it once with a CH341A clip; every update after that ships over Matter OTA; see
   [Installing the Custom Firmware](Installing-Custom-Firmware).

One script, `firmware/scripts/dev.py`, builds, flashes, tests and updates all three: see the
**[User Guide](User-Guide)**.

## How it works

```mermaid
flowchart LR
    HA[Home Assistant] <--> MS[matter-server]
    MS <-->|Matter over Wi-Fi| MOD[RTL8710C module<br/>custom AmebaZ2 firmware]
    MOD <-->|RS-485 9600 8N1| AC[A/C mainboard]
```

## Start here

| Page | What |
|---|---|
| **[User Guide](User-Guide)** | **pick a firmware and take it from clone to running node with `dev.py`** |
| [Hardware & Wiring](Hardware-and-Wiring) | the module, SoC/flash/transceiver, the A/C 4-pin port, the RS-485 bus |
| [ESPHome Build](ESPHome-Build) | the recommended firmware: an ESP32 board, native to Home Assistant |
| [ESP32 Replacement Build](ESP32-Replacement-Build) | the same ESP32 board running Matter |
| [Installing the Custom Firmware](Installing-Custom-Firmware) | the stock AmebaZ2 module, flashed with a CH341A clip |
| [Commissioning & HA Setup](Commissioning-and-HA-Setup) | commission into HA via matter-server, the cross-VLAN mDNS fix, re-interview after an OTA |
| [Everyday Control](Everyday-Control) | what entities appear, the unified climate integration, special modes, the dashboard card |
| [OTA Updates](OTA-Updates) | ship a new firmware, retry reality, version rules |
| [Recovery & Reflash](Recovery-and-Reflash) | CH341A SPI-clip recovery, the stock image, preserving commissioning |
| [FAQ & Gotchas](FAQ-Gotchas) | the load-bearing traps, in Q&A form |

## Developer guides

| Page | What |
|---|---|
| **[Build, Flash & Test](Build-Flash-Test)** | **the detail behind the User Guide: `dev.py`, one section per target, bench testing without an A/C** |
| [Repo Map & Build Pipeline](Repo-Map-and-Build-Pipeline) | where code lives, the SDK-outside-the-repo model, build/flash pipeline |
| [Protocol Overview](Protocol-Overview) | the RS-485 A/C protocol, framing, the Matter↔Hisense mapping |
| [Testing & QA](Testing-and-QA) | no-hardware host tests, the virtual A/C simulator, the HIL gate |

---

This guide is the place to start. The `firmware/docs/` and `reverse-engineering/docs/` files in
the repository hold the deep reference: design rationale, byte-level protocol, HIL notes. Each page
links to the one it draws from.
