# firmware/

The ESPHome firmware for the Hisense **AEH-W41H1** A/C module's bus, its tests and its tools.

| Path | What |
|---|---|
| [`esphome/`](esphome/) | the firmware: the `hisense_ac` component, board files and shared packages. Start at its README |
| [`test/`](test/) | host tests (`run_tests.sh`), the virtual A/C, the hardware actuation check, and the frozen [`reference-driver/`](test/reference-driver/) the component's codec is tested against |
| [`scripts/`](scripts/) | `dev.py`, the one entry point, and the lint scripts |
| [`flasher/`](flasher/) | CH341A clip tools for the stock module's SPI flash |
| [`esp32-recon/`](esp32-recon/) | a console on the A/C bus for diagnostics and reverse engineering (ESP-IDF, no Home Assistant) |
| [`docs/`](docs/) | design notes and hardware reference |

## Quick start

From the repository root:

```
python3 firmware/scripts/dev.py walk esphome
```

The guide starts at [`docs/guide/User-Guide.md`](../docs/guide/User-Guide.md).

## Reference docs here

| Doc | What |
|---|---|
| [`docs/15-esphome-path.md`](docs/15-esphome-path.md) | design and phase log of the ESPHome firmware, the stock module through LibreTiny, flash layouts |
| [`docs/13-path-comparison.md`](docs/13-path-comparison.md) | ESP32 board or stock module |
| [`docs/10-stock-module-boot-and-recovery.md`](docs/10-stock-module-boot-and-recovery.md) | slot selection by FWHS serial, image signing, what a rejected image looks like, clip recovery |
| [`docs/16-esp32-board-notes.md`](docs/16-esp32-board-notes.md) | the reasons behind the ESP32 wiring rules |
| [`docs/04-qa-strategy.md`](docs/04-qa-strategy.md) | test strategy |
| [`docs/07-stock-parity-gaps.md`](docs/07-stock-parity-gaps.md) | what the vendor firmware does that this one does not |
| [`docs/09-energy-monitoring.md`](docs/09-energy-monitoring.md) | the power estimate and its calibration |
| [`docs/03-ac-uart-pins.md`](docs/03-ac-uart-pins.md) | the stock module's bus UART pins, as found in the vendor firmware |

## Matter firmware (retired)

This directory used to hold a Matter firmware for the stock module (`src/sdk-edits/`, built on
Realtek's SDK) and one for the ESP32 (`esp32-matter/`). Both are retired (issue #182) and live on
under the tag `matter-final`. See
[`docs/guide/Matter-Firmware-Retired.md`](../docs/guide/Matter-Firmware-Retired.md).
