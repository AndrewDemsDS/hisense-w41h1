# firmware/

The ESPHome firmware for the bus of the Hisense **AEH-W41H1** A/C module, its tests and its
tools. This page is the map of the directory.

| Path | What | Start at |
|---|---|---|
| [`esphome/`](esphome/) | the firmware: the components `hisense_ac`, `w41h1_module` and `wifi_setup_mode`, the board files and the packages | [`esphome/README.md`](esphome/README.md) |
| [`test/`](test/) | host tests, the virtual A/C, the host end-to-end test, the hardware actuation check, and the frozen reference driver | [`docs/04-qa-strategy.md`](docs/04-qa-strategy.md) |
| [`scripts/`](scripts/) | `dev.py`, the one entry point, the lints and `matter-convert.py` | [`scripts/README.md`](scripts/README.md) |
| [`flasher/`](flasher/) | CH341A clip tools for the stock module's SPI flash | [`flasher/README.md`](flasher/README.md) |
| [`esp32-recon/`](esp32-recon/) | a console on the A/C bus for diagnostics and reverse engineering (ESP-IDF, no Home Assistant). Its network console has no authentication | [`esp32-recon/README.md`](esp32-recon/README.md) |
| [`docs/`](docs/) | architecture, test strategy, hardware reference | the table below |

## Quick start

From the repository root:

```
python3 firmware/scripts/dev.py walk
```

It checks the tools, runs the host tests, builds and flashes, asking before each step. The guide
starts at [`docs/guide/User-Guide.md`](../docs/guide/User-Guide.md).

## Reference docs here

| Doc | Read it for |
|---|---|
| [`docs/01-component-architecture.md`](docs/01-component-architecture.md) | how the components are built: the layers, the rule for decisions, the command path, setup mode's design |
| [`docs/04-qa-strategy.md`](docs/04-qa-strategy.md) | every test layer, what it proves, known failures, how CI runs them |
| [`docs/15-esphome-path.md`](docs/15-esphome-path.md) | the stock module through LibreTiny: measurements, flash layouts, open risks. Then the history of the design |
| [`docs/10-stock-module-boot-and-recovery.md`](docs/10-stock-module-boot-and-recovery.md) | slot selection by FWHS serial, image signing, what a rejected image looks like, the recovery rules |
| [`docs/13-path-comparison.md`](docs/13-path-comparison.md) | an ESP32 board or the stock module |
| [`docs/16-esp32-board-notes.md`](docs/16-esp32-board-notes.md) | the reasons behind the ESP32 wiring rules, Wi-Fi power saving, the device type bytes |
| [`docs/07-stock-parity-gaps.md`](docs/07-stock-parity-gaps.md) | what the vendor firmware does that this one does not |
| [`docs/09-energy-monitoring.md`](docs/09-energy-monitoring.md) | the power estimate and its calibration |
| [`docs/03-ac-uart-pins.md`](docs/03-ac-uart-pins.md) | the stock module's bus UART pins, as found in the vendor firmware |

Releases: the version is in `esphome/packages/version.yaml`, and how a release is cut is in
[`CONTRIBUTING.md`](../CONTRIBUTING.md#releasing). No release has been tagged yet.

## Matter firmware (retired)

> **Retired: the two Matter firmwares.** This directory used to hold a Matter firmware for the
> stock module (`src/sdk-edits/`, built on Realtek's SDK) and one for the ESP32
> (`esp32-matter/`). They are not in the tree any more. The last tree with them is the tag
> `matter-final`. Current: the ESPHome firmware under `esphome/`. See
> [`docs/guide/Matter-Firmware-Retired.md`](../docs/guide/Matter-Firmware-Retired.md).
