# NOTICE: third-party components and licensing

What this project is licensed under, what it depends on, and what it took from earlier work.

This project (the **original code** in `firmware/esphome/`, `firmware/test/`,
`firmware/scripts/`, `firmware/flasher/`, `firmware/esp32-recon/`, the
`reverse-engineering/tools/`, and all documentation) is licensed **MIT**: see
[`LICENSE`](LICENSE).

No third-party source file or binary is vendored. The build fetches its tools on your machine.
Two things in the tree do come from elsewhere, and both are described below: three literal bus
frames and a 16-byte frame header that are the same bytes as in a community project, and lint
configuration adapted from ESPHome's. The vendor's firmware, which the reverse-engineering pages
analyse, is not in the repository in any form.

## Third-party dependencies (fetched, not redistributed)

| Component | License | How it is used |
|---|---|---|
| **ESPHome** ([esphome/esphome](https://github.com/esphome/esphome)) | MIT (Python and tooling) / GPL-3.0 (C++ runtime), © ESPHome | Builds the firmware. `hisense_ac` is an external component in this repo. Version `2026.7.4` is pinned in `.github/workflows/qa.yaml`, `.github/workflows/esphome-release.yaml`, `firmware/scripts/dev.py`, `firmware/scripts/esphome-upstream-check.sh` and `.github/ci/esphome/requirements.txt`. `firmware/scripts/version-lint.py` fails when they differ. |
| **ESP-IDF** ([espressif/esp-idf](https://github.com/espressif/esp-idf)) | Apache-2.0, © Espressif | Downloaded by ESPHome for ESP32 builds. Also the SDK of `firmware/esp32-recon/`. |
| **LibreTiny** ([libretiny-eu/libretiny](https://github.com/libretiny-eu/libretiny)) and the **Realtek AmebaZ2 SDK** it wraps | MIT (LibreTiny). The Realtek SDK is proprietary, © Realtek Semiconductor | Downloaded by ESPHome for the stock-module builds. No Realtek source or binary is in this repository. |

A firmware image you build links ESPHome's GPL-3.0 runtime. That matters if you distribute the
binary. This repository distributes source only, and its releases attach no image.

Smaller dependencies, each fetched under its own licence and none copied into the tree:

| Where | What it fetches |
|---|---|
| `firmware/flasher/requirements.txt` | pyusb, for the CH341A clip tools |
| `reverse-engineering/tools/requirements.txt` | capstone, qrcode and pyserial, for the analysis and capture scripts |
| `firmware/esp32-recon/main/idf_component.yml` | Espressif's `mdns` component, from the ESP Component Registry |
| `.github/ci/lint/requirements.txt`, `.github/ci/esphome/requirements.txt` | the lint and test tools CI installs (actionlint, zizmor, shellcheck, cppcheck, ruff, gcovr, uv, ESPHome) |
| `firmware/scripts/cpp-lint.sh` | the pinned clang-format and clang-tidy wheels |
| `docs-site/_config.yml` | the `just-the-docs` Jekyll theme, loaded as a remote theme when the docs site is built |

## Acknowledgements: earlier work on this A/C bus

Two community projects for Hisense air conditioners came before this one, and the protocol work
here started from them. Licences are as GitHub reported them on 2026-10-13.

| Project | Licence | What this project took from it |
|---|---|---|
| [pslawinski/esphome_airconintl](https://github.com/pslawinski/esphome_airconintl) | None stated. GitHub reports no licence and the repository has no licence file. | The first payload layout and value encodings of the command and status frames (`messages.h`, `device_status.h`, `aircon_climate.h`), its sample frames as the first check of the checksum and of the `0xF4` doubling, and the three literal frames listed below. |
| [deiger/AirCon](https://github.com/deiger/AirCon) | GPL-3.0 | The first tables of semantic values (mode, fan, sleep, the on/off features) in `reverse-engineering/docs/05-esp32-replacement.md`. The mode numbering is still in use. Its fan table did not match this bus and was replaced. |

Three frames in this repository are the same bytes as in `components/airconintl/messages.h` of
`esphome_airconintl`, and were taken from it:

| Here | There | What it is |
|---|---|---|
| `FRAME_ON` in `firmware/esphome/components/hisense_ac/hisense_protocol.cpp` | `on[]` | switches the unit on |
| `FRAME_OFF` in the same file | `off[]` | switches the unit off |
| `STATUS_REQUEST` in `hisense_protocol.h` | `request_status[]` | the status poll |

The frozen first driver in `firmware/test/reference-driver/` carries the same three and says so in
its comments. The 16-byte `CMD_HEADER` is also the opening of every command frame (class `0x65`) in
that file.

The bytes are the same because they are what the indoor unit acts on: it answers this poll, and
it switches on and off on these two sequences, on every unit this firmware runs on. The vendor
module's firmware explains the header and the poll field by field
(`reverse-engineering/docs/03-rs485-ac-protocol.md` and `10-stock-fw-init-and-comms.md`). Several
bytes of the off frame are not understood, so that frame is kept as published. No capture of these
three frames from this project's own bus is on record.

Worked out here, from the vendor module's firmware and on a real unit: the frame envelope and the
checksum, the status byte map and the command values (which corrected several inherited ones),
and the link bring-up and heartbeat frames, captured from the stock module's transmit line.

## Adapted lint tooling

The root `.clang-format` and `.clang-tidy`, and the custom rules in
`firmware/scripts/cpp-lint.py`, are adapted from ESPHome's `.clang-format`, `.clang-tidy` and
`script/ci-custom.py` (ESPHome's Python and tooling are MIT-licensed, © 2019 ESPHome).
Nothing from ESPHome's GPL C++ runtime is copied.

## The retired Matter firmwares

Until 2026-10 this repository carried two Matter firmwares, as original sources plus patches over
Realtek's SDKs, connectedhomeip and esp-matter. They used CSA **test** attestation credentials
(test Vendor ID `0xFFF1`), were never certified and could not be sold. They are retired (issue
#182). The tree at the tag `matter-final` has them, with the `NOTICE.md` and `UPSTREAM.md` that
describe their dependencies and pins. The script that moved a stock module off them used
connectedhomeip's `ota_image_tool.py` (Apache-2.0, © Project CHIP Authors) from a checkout of your
own. It was removed from `main` on 2026-10-27, and the last release that carries it is `v2.0.2`.

## AI assistance

Parts of this project were developed with AI assistance. See the README's *AI assistance*
section; commits carry an `Assisted-by: AI` trailer.
