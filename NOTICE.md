# NOTICE: third-party components & licensing

This project (the **original code** in `firmware/esphome/`, `firmware/test/`,
`firmware/scripts/`, `firmware/flasher/`, `firmware/esp32-recon/`, the
`reverse-engineering/tools/`, and all documentation) is licensed **MIT**: see
[`LICENSE`](LICENSE).

Nothing third-party is vendored. The build fetches its tools on your machine.

## Third-party dependencies (fetched, not redistributed)

| Component | License | How it is used |
|---|---|---|
| **ESPHome** ([esphome/esphome](https://github.com/esphome/esphome)) | MIT (Python and tooling) / GPL-3.0 (C++ runtime), © ESPHome | Builds the firmware. `hisense_ac` is an external component in this repo. Version `2026.7.4` is pinned in `.github/workflows/qa.yaml` and `firmware/scripts/dev.py`. |
| **ESP-IDF** ([espressif/esp-idf](https://github.com/espressif/esp-idf)) | Apache-2.0, © Espressif | Downloaded by ESPHome for ESP32 builds. Also the SDK of `firmware/esp32-recon/`. |
| **LibreTiny** ([libretiny-eu/libretiny](https://github.com/libretiny-eu/libretiny)) and the **Realtek AmebaZ2 SDK** it wraps | MIT (LibreTiny). The Realtek SDK is proprietary, © Realtek Semiconductor | Downloaded by ESPHome for the stock-module builds. No Realtek source or binary is in this repository. |
| **connectedhomeip** ([project-chip/connectedhomeip](https://github.com/project-chip/connectedhomeip)) | Apache-2.0, © Project CHIP Authors | Only its `ota_image_tool.py`, and only for `dev.py convert`, which wraps an image for a unit that still runs the retired Matter firmware. You point `OTA_TOOL` at your own checkout. |

A firmware image you build links ESPHome's GPL-3.0 runtime. That matters if you distribute the
binary. This repository distributes source only.

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
describe their dependencies and pins.

## AI assistance

Parts of this project were developed with AI assistance. See the README's *AI assistance*
section; commits carry an `Assisted-by: AI` trailer.
