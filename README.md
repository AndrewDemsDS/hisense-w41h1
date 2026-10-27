# Hisense AEH-W41H1: local A/C control with ESPHome

ESPHome firmware that takes the place of the ConnectLife cloud on a Hisense A/C with an
`AEH-W41H1` Wi-Fi module. It speaks the A/C's internal RS-485 bus and presents the unit to Home
Assistant as a native climate device, with no cloud.

> Not affiliated with, endorsed by, or supported by Hisense, Realtek or ESPHome. See
> [`NOTICE.md`](NOTICE.md). Reverse engineering of hardware you own, for interoperability. Do this
> at your own risk: a bad flash can brick the stock module (recoverable with a dump you took
> first).

**Documentation: [andrewdemsds.github.io/hisense-w41h1](https://andrewdemsds.github.io/hisense-w41h1/).**
The site is generated from [`docs/guide/`](docs/guide/), [`firmware/docs/`](firmware/docs/) and
[`reverse-engineering/docs/`](reverse-engineering/docs/). This README only points into it.

```
Home Assistant ── ESPHome native API over Wi-Fi ──► ESP32 board or stock module
                                                    (hisense_ac ESPHome component)
                                                           │ RS-485 (9600 8N1)
                                                           ▼
                                                    A/C mainboard
```

## What you get

- A climate entity in Home Assistant over the ESPHome API: mode (including auto), setpoint
  (16 to 32 °C), six fan settings, vertical swing, and Eco, Quiet, Turbo and Sleep as presets.
- Switches for the special modes, the panel display and the command beeper.
- Commands that are checked: the node compares each one with the unit's status and sends it
  again when the unit missed it.
- Temperatures, compressor frequency, a power and energy estimate, and bus diagnostics.
- Updates over Wi-Fi with ESPHome's own OTA.

What each entity is and its limits: [Everyday Control](docs/guide/Everyday-Control.md).

## Which hardware

One firmware, three kinds of hardware.

| Hardware | State |
|---|---|
| An ESP32 board (ESP32-C3 SuperMini or classic ESP32) with a 3.3 V RS-485 transceiver, in place of the module | recommended. On live units since 2026-08 |
| The stock module itself (Realtek RTL8710C), through ESPHome's LibreTiny platform | one module in an A/C since 2026-10-10. No rollback, and a SOIC-8 clip is the only recovery |
| Other ESPHome chips (ESP8266, RP2040) | example configs that compile. Never run |

The [User Guide](docs/guide/User-Guide.md) compares them and says which page to read next.

## The quickest path to a working unit

With an ESP32 board wired as in
[ESP32 Replacement Board](docs/guide/ESP32-Replacement-Build.md), and ESPHome 2026.9.1 installed
(`pipx install esphome==2026.9.1`):

```
git clone https://github.com/AndrewDemsDS/hisense-w41h1.git
cd hisense-w41h1/firmware/esphome
cp secrets.yaml.example secrets.yaml      # then fill in Wi-Fi, a hotspot password and an API key
esphome run w41h1-esp32c3.yaml            # ESP32-C3 SuperMini. Classic ESP32: w41h1.yaml
```

Home Assistant then discovers the node, and you adopt it with the API key. Do not wire a freshly
flashed board straight into the A/C: the guide brings it up in three stages. For local builds and
the bench there is a wrapper script, `python3 firmware/scripts/dev.py`
([Build, Flash and Test](docs/guide/Build-Flash-Test.md)).

> **Stock module: dump it first.** The only complete way back to what a module had on it is a
> whole-chip write, with a CH341A and a clip, of a dump taken before the first flash.

## Where everything is

| I want to | Read |
|---|---|
| choose the hardware and follow the route | [User Guide](docs/guide/User-Guide.md) |
| know the module and the 4-pin port | [Hardware and Wiring](docs/guide/Hardware-and-Wiring.md) |
| install the tools and fill in the secrets | [Build Environment](docs/guide/ESP32-Build-Environment.md) |
| add the node to Home Assistant, update it, pin a release | [ESPHome Firmware](docs/guide/ESPHome-Build.md) |
| run ESPHome on the stock module | [The Stock Module](docs/guide/Stock-Module-Build.md) |
| see every entity and read the diagnostics | [Entities and Diagnostics](docs/guide/Entities-and-Diagnostics.md) |
| fix something | [Troubleshooting and FAQ](docs/guide/FAQ-Gotchas.md), [Recovery and Reflash](docs/guide/Recovery-and-Reflash.md) |
| find my way around the code and the tests | [Repo Map and Build Pipeline](docs/guide/Repo-Map-and-Build-Pipeline.md), [Testing and QA](docs/guide/Testing-and-QA.md) |
| read the protocol | [`reverse-engineering/docs/03-rs485-ac-protocol.md`](reverse-engineering/docs/03-rs485-ac-protocol.md) |
| contribute, or report a security problem | [`CONTRIBUTING.md`](CONTRIBUTING.md), [`SECURITY.md`](SECURITY.md) |
| see what changed | [`CHANGELOG.md`](CHANGELOG.md) |

Once local control works, you can block the vendor module's way out to the internet:
[`reverse-engineering/docs/04-cloud-and-firewall.md`](reverse-engineering/docs/04-cloud-and-firewall.md).

## Matter firmware (retired)

Until 2026-10-12 this repository also shipped two Matter firmwares. Both are retired. The last
tree that has them is the signed tag
[`matter-final`](https://github.com/AndrewDemsDS/hisense-w41h1/tree/matter-final). A unit that
runs one keeps working and can be moved to ESPHome over the air:
[Matter Firmware (Retired)](docs/guide/Matter-Firmware-Retired.md).

## AI assistance

I built this with AI assistance across the code, reverse-engineering, and docs. Commits carry an
`Assisted-by: AI` trailer.

## License

Original code and docs: **MIT** ([`LICENSE`](LICENSE)). Third-party tools the build fetches and
what was adapted from them: [`NOTICE.md`](NOTICE.md).

Credits: the protocol work started from
[`pslawinski/esphome_airconintl`](https://github.com/pslawinski/esphome_airconintl) and
[`deiger/AirCon`](https://github.com/deiger/AirCon). What came from each is in
[`NOTICE.md`](NOTICE.md#acknowledgements-earlier-work-on-this-ac-bus).
