# Repo Map and Build Pipeline

Where the code lives, how it is tested and built, and what CI checks.

← back to [Home](Home) · siblings: [Protocol Overview](Protocol-Overview) ·
[Testing and QA](Testing-and-QA) · [Build, Flash & Test](Build-Flash-Test)

---

## Repository layout

| Path | What |
|---|---|
| `firmware/esphome/` | **the firmware**: board files (`w41h1*.yaml`), shared `packages/`, the `hisense_ac` component under `components/`, the stock-module board support, upstream-shaped tests under `tests/` |
| `firmware/test/` | host tests, `virtual_ac.py`, the hardware actuation check, and `reference-driver/` |
| `firmware/test/reference-driver/` | the project's first bus driver, frozen. The component's codec is tested against it |
| `firmware/scripts/` | `dev.py` (the one entry point), `amebaz2_image.py` (stock-module image format), the lint scripts |
| `firmware/flasher/` | CH341A clip tools for the stock module's SPI flash: dump, write, status register |
| `firmware/esp32-recon/` | a small ESP-IDF diagnostic tool: a console on the A/C bus, built on the reference driver |
| `firmware/docs/` | design notes and hardware reference, published under "Firmware" on the docs site |
| `reverse-engineering/` | the protocol and stock-firmware analysis, capture tools, hardware notes |
| `docs/guide/` | this guide |
| `docs-site/` | the docs site's landing pages and theme config |
| `dumps/` | local only and gitignored: raw flash dumps hold Wi-Fi credentials and device keys |

Nothing third-party is vendored. ESPHome and the toolchains it downloads are fetched on the
build machine (`NOTICE.md`).

## One component, three kinds of hardware

```
Home Assistant -- ESPHome native API over Wi-Fi -- MCU -- RS-485 9600 8N1 -- A/C mainboard
```

| Layer | Files | Testable |
|---|---|---|
| Codec | `components/hisense_ac/hisense_protocol.{h,cpp}` | host, compared with the reference driver |
| Mapping and decisions | `components/hisense_ac/hisense_map.h` | host, pure functions |
| Bus scheduler | `components/hisense_ac/hisense_bus.{h,cpp}` | host, against a simulated A/C |
| ESPHome glue | `hisense_ac.cpp`, `hisense_climate.cpp`, the switch and select platforms, the `*.py` schemas | not on the host: `esphome config`, a compile, and hardware |

The component uses ESPHome's `uart:` component and nothing from a chip SDK, so the same sources
build for the ESP32 boards, the stock module (LibreTiny) and the example ESP8266 and RP2040 files.
What differs per board is one YAML file: the node name, three pins and the platform block.

**Standing rule:** a glue *decision* (is this write my own echo, should the shadow sync, is this
command confirmed) belongs as a pure function in `hisense_map.h` with a host test, not in the glue.

## Build: use the script

```
python3 firmware/scripts/dev.py test  esphome                  # host tests + esphome config
python3 firmware/scripts/dev.py build esphome --board c3       # esphome compile
python3 firmware/scripts/dev.py flash esphome --board c3 --port /dev/ttyACM0
```

`dev.py` wraps `esphome` and prints each command, so everything can also be run by hand from
`firmware/esphome/` ([Build Environment](ESP32-Build-Environment#by-hand)). There is no version
file to bump and no release image to package: a node is built from the tree and updated over the
air with `esphome run` ([ESPHome Build](ESPHome-Build#updates)).

`firmware/esphome/secrets.yaml` is gitignored. `secrets.yaml.example` holds placeholders, and CI
builds with those.

### The stock module

Three board files, one per flash layout a module can have (`w41h1-amebaz2-*.yaml`). The image must
be built for the layout the unit has, and the build fails when the image's slot addresses are not
those of the layout the file names. What `dev.py` adds for the module:

- `build esphome --board amebaz2-<layout>` compiles it and makes sure the slot image exists.
- `convert esphome --board amebaz2-<layout>` re-signs that image for a unit that still runs the
  retired Matter firmware ([Converting a Stock Module](Converting-a-Stock-Module-to-ESPHome)).
- `matter-slots` and `matter-backup` read such a unit's slot state.

`convert` and the two probes read their settings from `firmware/scripts/ota-release.env`
(gitignored, copy `ota-release.env.example`). `firmware/built-images/` is where their output goes,
also gitignored.

How the module boots and what its bootloader accepts:
`firmware/docs/10-stock-module-boot-and-recovery.md`.

## Hooks

Hooks are opt-in per clone: `git config core.hooksPath firmware/.githooks`. The **pre-commit hook**
runs `firmware/test/run_tests.sh` when the host tests, `dev.py` or the component's protocol sources
are staged, `cpp-lint.sh check --no-tidy` on staged C/C++ files, and `stop-slop.sh` on staged
markdown (bypass: `--no-verify`).

## Continuous integration (GitHub Actions)

`.github/workflows/` mirrors the local gate:

- **`qa.yaml`** runs on every push and pull request, on GitHub-hosted runners:
  - `host-qa`: shellcheck, ruff, `cpp-lint.sh check` (clang-format, custom rules and clang-tidy on
    the C/C++ we own), `firmware/test/run_tests.sh`, and `esphome config` on every board file and
    on the upstream-shaped component tests.
  - `esphome-amebaz2`: a real compile of all three stock-module layouts.
  - `esphome-upstream`: ESPHome's own CI gates against the component, on the pinned release
    (required) and on their development branch (informational).
- **`pages.yaml`** builds and publishes the docs site when a doc changes.

There is no release workflow and no self-hosted runner. Dependabot proposes updates for the GitHub
Actions and the pinned Python requirements.

## Docs site

`python3 .github/scripts/assemble_docs_site.py` assembles `_site_src` from `docs/guide/`,
`firmware/docs/` and `reverse-engineering/docs/` at build time. Nothing is duplicated in git: edit
the source docs, never a copy under `_site_src`. `python3 docs/guide/lint.py` checks the guide's
links, headings, sidebar and a leak denylist.

## The retired Matter builds

`main` used to carry two Matter firmwares, their SDK setup, patches, release tooling and
workflows. They were removed on 2026-10-12 and live on under the tag `matter-final`
([Matter Firmware (Retired)](Matter-Firmware-Retired)).
