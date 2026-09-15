# Hisense AEH-W41H1 de-cloud with custom Matter firmware

Replace the ConnectLife cloud on a Hisense **`AEH-W41H1`** A/C Wi-Fi module (Realtek
**RTL8710C / AmebaZ2**) with custom **Matter** firmware, for local control from Home
Assistant, **zero cloud**.

> Not affiliated with, endorsed by, or supported by Hisense, Realtek, or the CSA. Uses Matter
> **test** credentials → development/personal use only, **not a certified Matter product**, **not
> for sale**. See [`NOTICE.md`](NOTICE.md). Reverse-engineering of hardware you own, for
> interoperability. Do this at your own risk, a bad flash can brick the module (recoverable with
> the stock dump).

**📖 Documentation: [andrewdemsds.github.io/hisense-w41h1](https://andrewdemsds.github.io/hisense-w41h1/)**,
wiring, flashing, commissioning, everyday control, OTA, recovery, building from source, and the
reverse-engineering write-ups, all searchable in one place. The site is generated from
[`docs/guide/`](docs/guide/), [`firmware/docs/`](firmware/docs/) and
[`reverse-engineering/docs/`](reverse-engineering/docs/); this README only points into it.

## What you get

- **Local Matter control**: the A/C commissions into `python-matter-server` / Home Assistant; no
  ConnectLife, no `hijuconn` cloud.
- **Full control surface**: HVAC mode (incl. Auto), setpoint (16–32 °C), fan (6 speeds), vertical
  swing, and Eco / Quiet / Turbo / Sleep special modes.
- **Energy monitoring**: live power (W) + voltage, derived from the bus current proxy.
- **OTA updates over Wi-Fi**: after the first CH341 flash, everything else is wireless.
- **Three firmwares, one driver**, in order of preference: an ESP32 board running ESPHome (Home
  Assistant only), the same ESP32 running Matter, or the stock AmebaZ2 module running Matter.
- **One script for all of it**: `python3 firmware/scripts/dev.py` checks tools, fetches SDKs,
  tests, builds, flashes and ships OTA updates for every target.

## How it works

```
Home Assistant ─┬─ python-matter-server ── Matter/Wi-Fi ──► RTL8710C module
                                                             (custom AmebaZ2 Matter firmware)
                                                                    │ RS-485 (9600 8N1)
                                                                    ▼
                                                             A/C mainboard
```

The module runs the Realtek AmebaZ2 Matter `room_air_conditioner` example with **our RS-485
driver** bridging Matter attributes ↔ the A/C's internal RS-485 bus (protocol
reverse-engineered + sniff-validated, see [`reverse-engineering/docs/03`](reverse-engineering/docs/03-rs485-ac-protocol.md)).

## Get started

```
python3 firmware/scripts/dev.py walk esphome   # or esp32, or amebaz2; asks before every step
```

| I want to | Read |
|---|---|
| pick a firmware and go from clone to running node | [User Guide](docs/guide/User-Guide.md) |
| know the hardware and the 4-pin port | [Hardware & Wiring](docs/guide/Hardware-and-Wiring.md) |
| 1. run ESPHome on an ESP32 (recommended, Home Assistant only) | [ESPHome Build](docs/guide/ESPHome-Build.md) |
| 2. run Matter on an ESP32 | [ESP32 Replacement Build](docs/guide/ESP32-Replacement-Build.md) |
| 3. flash a stock AmebaZ2 module (CH341A clip, once) | [Installing the Firmware](docs/guide/Installing-Custom-Firmware.md), prebuilt images on [Releases](https://github.com/AndrewDemsDS/hisense-w41h1/releases) |
| commission a Matter build into Home Assistant | [Commissioning & HA Setup](docs/guide/Commissioning-and-HA-Setup.md) |
| update over the air | [OTA Updates](docs/guide/OTA-Updates.md) (`dev.py ota <target> release`) |
| un-brick or go back to stock | [Recovery & Reflash](docs/guide/Recovery-and-Reflash.md) |
| the detail behind each `dev.py` step | [Build, Flash & Test](docs/guide/Build-Flash-Test.md) |
| find my way around the repo and the SDK overlay model | [Repo Map & Build Pipeline](docs/guide/Repo-Map-and-Build-Pipeline.md) |

> **⚠️ No remote way back to stock (yet).** Once a module runs this firmware, the only supported
> return to stock ConnectLife firmware is a whole-chip CH341A write, which needs a dump of the stock
> flash taken **before** you first flashed. Take that dump. Remote revert is tracked in the
> [issues](https://github.com/AndrewDemsDS/hisense-w41h1/issues).

## Releases & CI

A host-only QA gate runs on every push and PR. Pushing a signed `amebaz2-vX.Y.Z` or `esp32-vX.Y.Z`
tag builds and publishes a GitHub Release with the firmware and `SHA256SUMS` attached. Details:
[Repo Map & Build Pipeline](docs/guide/Repo-Map-and-Build-Pipeline.md#continuous-integration--releases-github-actions).

## Attestation & credentials

CSA **test** creds (VID `0xFFF1`/PID `0x8001`), dev-only, uncertified. Details: [`NOTICE.md`](NOTICE.md#matter-credentials).

## De-clouding your whole home

Once local control works, block the module's WAN egress (deny `*.hijuconn.com` + the OTA host), see
[`reverse-engineering/docs/04`](reverse-engineering/docs/04-cloud-and-firewall.md). Research on
flashing the **other** units over the air (via the stock firmware's dormant Matter stack, no CH341) is
tracked in the [issues](https://github.com/AndrewDemsDS/hisense-w41h1/issues) (Fleet-OTA).

## AI assistance

I built this with AI assistance across the code, reverse-engineering, and docs. Commits carry an
`Assisted-by: AI` trailer.

## License

Original code and docs: **MIT** ([`LICENSE`](LICENSE)). Third-party components (Realtek SDKs,
proprietary, not vendored; connectedhomeip, Apache-2.0) and the credential caveat: [`NOTICE.md`](NOTICE.md).
