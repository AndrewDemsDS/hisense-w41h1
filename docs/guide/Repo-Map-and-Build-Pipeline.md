# Repo Map and Build Pipeline

For a contributor: where the code lives, how the firmware is put together, how a version becomes
a release, and what CI checks. The tests themselves are on [Testing and QA](Testing-and-QA).

## Repository layout

| Path | What |
|---|---|
| `firmware/esphome/` | the firmware: board files (`w41h1*.yaml`), shared `packages/`, the components under `components/`, upstream-shaped tests under `tests/` |
| `firmware/esphome/components/hisense_ac/` | the A/C component: codec, bus scheduler, hub and entities |
| `firmware/esphome/components/w41h1_module/` | what the stock module needs on top: its flash layouts, the layout check, the serial port recovery |
| `firmware/esphome/components/wifi_setup_mode/` | the on-demand Wi-Fi setup hotspot |
| `firmware/test/` | host tests, `virtual_ac.py`, the fuzz harnesses, coverage and mutation scripts, the host end-to-end test, the hardware actuation check |
| `firmware/test/reference-driver/` | the project's first bus driver, frozen. The component's codec is tested against it |
| `firmware/scripts/` | `dev.py` (test, build, flash), `matter-convert.py` (for a module on the retired Matter firmware), `amebaz2_image.py` (the stock module's image format), the lint scripts and `version-lint.py` |
| `firmware/flasher/` | CH341A clip tools for the stock module's SPI flash: dump, write, status register |
| `firmware/esp32-recon/` | a small ESP-IDF diagnostic tool: a console on the A/C bus, built on the reference driver |
| `firmware/docs/` | design notes and hardware reference, published under "Firmware" on the docs site |
| `reverse-engineering/` | the protocol and vendor-firmware analysis, capture tools, hardware notes |
| `docs/guide/` | this guide |
| `docs-site/` | the docs site's landing pages and theme configuration |
| `dumps/` | local only and gitignored: raw flash dumps hold Wi-Fi credentials and device keys |

Nothing third-party is vendored. ESPHome and the toolchains it downloads are fetched on the build
machine (`NOTICE.md`).

## One component, three kinds of hardware

```
Home Assistant -- ESPHome native API over Wi-Fi -- MCU -- RS-485 9600 8N1 -- A/C mainboard
```

The `hisense_ac` component is split by what can be tested where:

| Layer | Files in `components/hisense_ac/` | Tested |
|---|---|---|
| Codec | `hisense_protocol.{h,cpp}` | on the host, compared with the reference driver |
| Bus scheduler | `hisense_bus.{h,cpp}` | on the host, against a simulated A/C |
| Mapping and decisions | `hisense_mapping.h`, `hisense_special.h`, `hisense_confirm.h`, `hisense_telemetry.h`, `hisense_commands.h`, `hisense_entities.h` (`hisense_map.h` only includes the first four) | on the host, as pure functions |
| ESPHome glue | `hisense_ac.cpp`, `hisense_climate.cpp`, `hisense_switch.cpp`, `hisense_select.cpp`, the `*.py` schemas | in the host end-to-end test, by `esphome config`, a compile, and on hardware |

The component uses ESPHome's `uart:` component and nothing from a chip SDK, so the same sources
build for the ESP32 boards, the stock module (LibreTiny) and the example ESP8266 and RP2040
files. What differs per board is one YAML file: three pins and the platform block. The codec is a
port of the reference driver, and a protocol change that the reference also covers is made in
both, so the comparison between them keeps its meaning.

A decision in the glue (is this write my own echo, is this command confirmed) belongs in one of
the pure headers with a host test ([why](Testing-and-QA#the-rule-for-glue-code)).

## Building

`firmware/scripts/dev.py` wraps `esphome` and prints each command, so every step can also be run
by hand from `firmware/esphome/`: [Build, Flash and Test](Build-Flash-Test) and
[Build Environment](ESP32-Build-Environment). There is no release image to package. A node is
built from the tree and updated over the air.

`firmware/esphome/secrets.yaml` is gitignored. `secrets.yaml.example` holds placeholders, and CI
builds with those.

The stock module has three board files, one per flash layout, and its build fails when the
image's slot addresses are not those of the layout the file names
([The Stock Module](Stock-Module-Build)). `matter-convert.py` and its settings file
(`firmware/scripts/ota-release.env`, gitignored) exist only for a module that still runs the
retired Matter firmware ([Matter Firmware (Retired)](Matter-Firmware-Retired#a-stock-module)).
Its output goes to `firmware/built-images/`, also gitignored.

## Version and releases

The firmware version is written once, in `firmware/esphome/packages/version.yaml`, as ESPHome's
`project:` block. `packages/node.yaml` includes that file, so every node built from the packages
carries the version. It is semver and starts at 2.0.0.

A release is the git tag `v<version>` on `main`, and `CHANGELOG.md` has a section per release.
The number moves only when a release is cut. Between releases, changes are listed under
`## Unreleased`. `firmware/scripts/version-lint.py` checks the pieces against each other, and
`CONTRIBUTING.md` ("Releasing") has the steps. What a release means for someone who builds a node:
[ESPHome Firmware](ESPHome-Build#version-and-releases).

## Hooks

Hooks are opt-in per clone: `git config core.hooksPath firmware/.githooks`. The pre-commit hook
runs:

- `firmware/test/run_tests.sh` when the host tests, the scripts, the flasher or a component's
  sources are staged,
- `version-lint.py` when a package, the changelog or a workflow is staged,
- `docs/guide/lint.py` when a guide page is staged,
- `cpp-lint.sh check --no-tidy` on staged C/C++ files,
- `stop-slop.sh` on staged Markdown.

## Continuous integration (GitHub Actions)

Every job runs on a GitHub-hosted runner and states its own permissions. Only the job that creates
a release can write to the repository.

**`qa.yaml`** runs on every push and pull request. A newer push to the same branch cancels the run
in progress.

| Job | Runs | Does |
|---|---|---|
| `host-qa` | always. The one required check | shellcheck on every tracked shell script, ruff, `cpp-lint.sh check`, `firmware/test/run_tests.sh`, `firmware/test/sanitize.sh`, and `esphome config` on every board file and on the component tests |
| `version` | always | `version-lint.py` with the full history: the version is not below the newest release tag, and in a pull request a change under `firmware/esphome/components/` or `packages/` comes with a `CHANGELOG.md` entry |
| `lint-extra` | always | actionlint and zizmor on the workflows, `docs/guide/lint.py` on the guide, cppcheck on the pure sources. Its tools are pinned in `.github/ci/lint/requirements.txt` |
| `changes` | always | decides whether the jobs below run. They are skipped when nothing they build from changed, so a docs-only change is done when `host-qa` is |
| `esphome-esp32` | when the firmware changed | a real compile of every board file that includes `packages/esp32.yaml`, and of the hub with no entity platform. Fails on a compiler warning from the components |
| `esphome-amebaz2` | when the firmware changed | a real compile of all three stock-module layouts |
| `esphome-host-e2e` | when the firmware or the test changed | the host end-to-end scenarios, then clang-tidy on the hub and the entities (reported, not blocking) |
| `esphome-upstream` | when the firmware changed | ESPHome's own CI gates against the component, on the pinned release |
| `coverage` | pushes to `main`, never blocking | line and branch coverage of the pure code, as a table in the run's summary |

**`nightly.yaml`** runs every night, and on a pull request that changes it or what it builds.

| Job | Does |
|---|---|
| `fuzz` | every fuzz harness for ten minutes, with the corpus kept between nights. A crash uploads its reproducer |
| `boards` | compiles the board files no pull request compiles (`w41h1-esp8266.yaml`, `w41h1-rp2040.yaml`) |
| `host-e2e-full` | every host end-to-end scenario, the long ones included |
| `esphome-upstream-dev` | ESPHome's gates against their development branch. Informational |
| `esp32-recon` | builds `firmware/esp32-recon/` with ESP-IDF |

**`esphome-release.yaml`** runs when a tag `vX.Y.Z` is pushed. The tag has to equal the version
and sit on `main`. It runs the host tests, validates and compiles every board file, and creates
the GitHub release with that version's `CHANGELOG.md` section as its text. It attaches no
firmware image: one built in CI holds the placeholder secrets, which are public. Started by hand
it is a dry run: every gate and compile runs, the summary shows the release it would have made,
and nothing is tagged or published.

**`pages.yaml`** builds and publishes the docs site when a doc changes.

No workflow lists the board files. `.github/scripts/board-groups.sh` sorts them by the package
they include, so a new board file is compiled without a workflow change.

Dependabot proposes updates for the GitHub Actions, the pinned Python requirements, the lint
tools and the ESPHome release. The ESPHome release is written in four more places that
`version-lint.py` holds equal, so its Dependabot pull request fails until those are bumped in it.
The clang-format and clang-tidy pins in `cpp-lint.sh` are ESPHome's own and move with the ESPHome
release, by hand.

## Docs site

`python3 .github/scripts/assemble_docs_site.py` assembles `_site_src` from `docs/guide/`,
`firmware/docs/` and `reverse-engineering/docs/` at build time. Nothing is duplicated in git:
edit the source docs, never a copy under `_site_src`. The order of the guide's pages on the site
is the `GUIDE_ORDER` list in that script, and `docs/guide/_Sidebar.md` lists the same pages for
readers on GitHub. `python3 docs/guide/lint.py` checks the guide's links, headings, sidebar and a
leak denylist.

## History

`main` used to carry two Matter firmwares with their SDK setup, patches, release tooling and
workflows. They were removed on 2026-10-12: [Matter Firmware (Retired)](Matter-Firmware-Retired).
