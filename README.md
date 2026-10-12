# Hisense AEH-W41H1: local A/C control with ESPHome

Replace the ConnectLife cloud on a Hisense **`AEH-W41H1`** A/C Wi-Fi module with an ESPHome
firmware that speaks the A/C's internal RS-485 bus, for local control from Home Assistant with
**zero cloud**.

> Not affiliated with, endorsed by, or supported by Hisense, Realtek or ESPHome. See
> [`NOTICE.md`](NOTICE.md). Reverse-engineering of hardware you own, for interoperability. Do this
> at your own risk: a bad flash can brick the stock module (recoverable with a dump you took
> first).

**📖 Documentation: [andrewdemsds.github.io/hisense-w41h1](https://andrewdemsds.github.io/hisense-w41h1/)**:
wiring, flashing, everyday control, recovery, building from source, and the reverse-engineering
write-ups, all searchable in one place. The site is generated from
[`docs/guide/`](docs/guide/), [`firmware/docs/`](firmware/docs/) and
[`reverse-engineering/docs/`](reverse-engineering/docs/); this README only points into it.

## What you get

- **A native Home Assistant climate device** over the ESPHome API: no ConnectLife, no pairing
  code, no extra integration.
- **Full control surface**: HVAC mode (incl. Auto), setpoint (16–32 °C), fan (6 speeds), vertical
  swing, Eco / Quiet / Turbo / Sleep special modes as presets and switches, the panel display and
  the command beeper.
- **Commands that are confirmed**: the node checks each command against the unit's status and
  sends it again when the unit missed it.
- **Bus diagnostics**: link state, error and timeout counters, per-fault and capability flags.
- **Energy monitoring**: live power, voltage and current, and energy per day.
- **Updates over Wi-Fi** with ESPHome's own OTA.
- **One script**: `python3 firmware/scripts/dev.py` checks the tools, tests, builds and flashes.

## One firmware, three kinds of hardware

| Hardware | Status |
|---|---|
| **An ESP32 board** (ESP32-C3 SuperMini or classic ESP32) with a 3.3 V RS-485 transceiver, in place of the module | recommended. On live units since 2026-08 |
| **The stock module itself** (Realtek RTL8710C) through ESPHome's LibreTiny platform | first run in an A/C on 2026-10-10. No rollback, and a SOIC-8 clip is the only recovery |
| **Other ESPHome chips** (ESP8266, RP2040) | example configs that compile. Never run |

```
Home Assistant ── ESPHome native API over Wi-Fi ──► ESP32 board or stock module
                                                    (hisense_ac ESPHome component)
                                                           │ RS-485 (9600 8N1)
                                                           ▼
                                                    A/C mainboard
```

The protocol is reverse-engineered and sniff-validated on a real unit, see
[`reverse-engineering/docs/03`](reverse-engineering/docs/03-rs485-ac-protocol.md).

## Get started

```
python3 firmware/scripts/dev.py walk esphome   # asks before every step
```

| I want to | Read |
|---|---|
| go from clone to running node | [User Guide](docs/guide/User-Guide.md) |
| know the hardware and the 4-pin port | [Hardware & Wiring](docs/guide/Hardware-and-Wiring.md) |
| set up the build tools on a clean machine | [Build Environment](docs/guide/ESP32-Build-Environment.md) |
| know what the firmware gives me, adopt it and update it | [ESPHome Build](docs/guide/ESPHome-Build.md) |
| wire an ESP32 board | [ESP32 Replacement Board](docs/guide/ESP32-Replacement-Build.md) |
| see every entity and read the bus counters | [Entities and Diagnostics](docs/guide/Entities-and-Diagnostics.md) |
| un-brick a board or a module | [Recovery & Reflash](docs/guide/Recovery-and-Reflash.md) |
| the detail behind each `dev.py` step | [Build, Flash & Test](docs/guide/Build-Flash-Test.md) |
| find my way around the repo | [Repo Map & Build Pipeline](docs/guide/Repo-Map-and-Build-Pipeline.md) |

> **⚠️ Stock module: dump it first.** The only complete way back to what a module had on it is a
> whole-chip CH341A write of a dump taken **before** the first flash.

## Matter firmware (retired)

Until 2026-10 this repository also shipped two Matter firmwares: one for the stock module on
Realtek's AmebaZ2 SDK and one for the ESP32 board on esp-matter. Both are retired
([issue #182](https://github.com/AndrewDemsDS/hisense-w41h1/issues/182)). The last tree that has
them is the signed tag [`matter-final`](https://github.com/AndrewDemsDS/hisense-w41h1/tree/matter-final).
A unit that runs one keeps working, and can be moved to ESPHome over the air:
[Matter Firmware (Retired)](docs/guide/Matter-Firmware-Retired.md).

Apple Home, Google Home and Alexa are reached through Home Assistant's own Matter or HomeKit
bridge.

## CI

A hardware-free gate runs on every push and pull request: lints, the host test suite,
`esphome config` on every board file, a real compile for the stock module, and ESPHome's own CI
checks against the component. Details:
[Repo Map & Build Pipeline](docs/guide/Repo-Map-and-Build-Pipeline.md#continuous-integration-github-actions).

## De-clouding your whole home

Once local control works, block the module's WAN egress (deny `*.hijuconn.com` and the OTA host),
see [`reverse-engineering/docs/04`](reverse-engineering/docs/04-cloud-and-firewall.md).

## AI assistance

I built this with AI assistance across the code, reverse-engineering, and docs. Commits carry an
`Assisted-by: AI` trailer.

## License

Original code and docs: **MIT** ([`LICENSE`](LICENSE)). Third-party tools the build fetches and
what was adapted from them: [`NOTICE.md`](NOTICE.md).
