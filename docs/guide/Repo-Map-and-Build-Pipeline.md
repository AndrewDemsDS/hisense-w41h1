# Repo Map and Build Pipeline

Where the code lives, how it is tested and built, and what CI checks.

← back to [Home](Home) · siblings: [Protocol Overview](Protocol-Overview) ·
[Testing and QA](Testing-and-QA) · [Build, Flash & Test](Build-Flash-Test)

---

## Repository layout

| Path | What |
|---|---|
| `firmware/esphome/` | **the firmware**: board files (`w41h1*.yaml`), shared `packages/` (with the version in `packages/version.yaml`), the `hisense_ac` and `w41h1_module` components under `components/`, upstream-shaped tests under `tests/` |
| `firmware/test/` | host tests, `virtual_ac.py`, the hardware actuation check, and `reference-driver/` |
| `firmware/test/reference-driver/` | the project's first bus driver, frozen. The component's codec is tested against it |
| `firmware/scripts/` | `dev.py` (the one entry point), `amebaz2_image.py` (stock-module image format), the lint scripts, `version-lint.py` and the compile gate CI runs |
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
`firmware/esphome/` ([Build Environment](ESP32-Build-Environment#by-hand)). There is no release
image to package: a node is built from the tree and updated over the air with `esphome run`
([ESPHome Build](ESPHome-Build#updates)).

`firmware/esphome/secrets.yaml` is gitignored. `secrets.yaml.example` holds placeholders, and CI
builds with those.

### The stock module

Three board files, one per flash layout a module can have (`w41h1-amebaz2-*.yaml`). The image must
be built for the layout the unit has, and the build fails when the image's slot addresses are not
those of the layout the file names. What the scripts add for the module:

- `dev.py build esphome --board amebaz2-<layout>` compiles it and makes sure the slot image exists.
- `matter-convert.py convert --board amebaz2-<layout>` re-signs that image for a unit that still
  runs the retired Matter firmware ([Converting a Stock Module](Converting-a-Stock-Module-to-ESPHome)).
- `matter-convert.py slots` and `matter-convert.py backup` read such a unit's slot state.

`matter-convert.py` reads its settings from `firmware/scripts/ota-release.env`
(gitignored, copy `ota-release.env.example`). `firmware/built-images/` is where their output goes,
also gitignored.

How the module boots and what its bootloader accepts:
`firmware/docs/10-stock-module-boot-and-recovery.md`.

## Version and releases

The firmware version is written once, in `firmware/esphome/packages/version.yaml`, as ESPHome's
`project:` block. `packages/node.yaml` includes that file, so every node built from the packages
carries the version and Home Assistant shows it as the device's firmware. It is semver and
started at 2.0.0. The Matter firmwares were 1.x.

A release is the git tag `v<version>` on `main`, and `CHANGELOG.md` has a section per release.
The number moves only when a release is cut. Between releases, changes are listed under
`## Unreleased`. `firmware/scripts/version-lint.py` checks the pieces against each
other (below), and `CONTRIBUTING.md` ("Releasing") has the four steps.

## Hooks

Hooks are opt-in per clone: `git config core.hooksPath firmware/.githooks`. The **pre-commit hook**
runs `firmware/test/run_tests.sh` when the host tests, `dev.py` or a component's sources are
staged, `version-lint.py` when a package, the changelog or a workflow is staged,
`cpp-lint.sh check --no-tidy` on staged C/C++ files, and `stop-slop.sh` on staged markdown
(bypass: `--no-verify`).

## Continuous integration (GitHub Actions)

`.github/workflows/` mirrors the local gate:

- **`qa.yaml`** runs on every push and pull request, on GitHub-hosted runners. A newer push to
  the same branch or pull request cancels the run in progress.
  - `host-qa`, the one required check: shellcheck on every tracked shell script, ruff,
    `cpp-lint.sh check` (clang-format, custom rules and clang-tidy on the C/C++ we own),
    `firmware/test/run_tests.sh`, the sanitizer build of the host tests, and `esphome config` on
    every board file and on the upstream-shaped component tests.
  - `lint-extra`: actionlint and zizmor on the workflows, `docs/guide/lint.py` on the guide,
    and cppcheck on the pure sources of the components. Its tools are pinned in
    `.github/ci/lint/requirements.txt`.
  - `changes` decides whether the three jobs below run: they are skipped when the push or pull
    request touches nothing under `firmware/esphome/` (markdown aside) and none of the scripts
    they call. A docs-only change is then done when `host-qa` is.
  - `version`: `version-lint.py` with the full history. The version is `MAJOR.MINOR.PATCH` and
    not below the newest release tag, `CHANGELOG.md` has a section for it or an Unreleased one,
    and every pin of the ESPHome release names the same one. In a pull request it also fails
    when a file under `firmware/esphome/components/` or `firmware/esphome/packages/` changed and
    `CHANGELOG.md` did not.
  - `esphome-amebaz2`: a real compile of all three stock-module layouts.
  - `esphome-esp32`: a real compile of every board file that includes `packages/esp32.yaml`
    (`w41h1.yaml`, `w41h1-esp32c3.yaml`, `w41h1-esp32c3-plus.yaml`) and of the hub with no entity
    platform. It fails on a compiler warning from the components and when a board image does not
    carry the version.
  - `esphome-upstream`: ESPHome's own CI gates against the component, on the pinned release.
  - `coverage`, on pushes to `main` only and never blocking: line and branch coverage of the
    pure code under the host tests, as a table in the run's summary.
- **`nightly.yaml`** runs every night, and on a pull request that changes it or what it builds:
  every fuzz harness for ten minutes with the corpus kept between nights (a crash uploads its
  reproducer), a compile of the board files `qa.yaml` does not compile (`w41h1-esp8266.yaml`,
  `w41h1-rp2040.yaml`), ESPHome's gates against their development branch (informational), and
  the `firmware/esp32-recon/` build with ESP-IDF.
- **`esphome-release.yaml`** runs when a tag `vX.Y.Z` is pushed. The tag has to equal the version
  and sit on `main`. It runs the host tests, validates and compiles every board file, and creates
  the GitHub release with that version's `CHANGELOG.md` section as its text. It attaches no
  firmware image: one built in CI holds the placeholder secrets, which are public. Started by
  hand (`gh workflow run esphome-release.yaml`) it is a dry run: every gate and compile runs and
  the summary shows the release it would have made, and nothing is tagged or published.
- **`pages.yaml`** builds and publishes the docs site when a doc changes.

No workflow lists the board files. `.github/scripts/board-groups.sh` sorts them by the package
they include (`esp32.yaml`, `amebaz2.yaml`, neither), and the workflows ask it, so a new board
file is compiled without a workflow change.

Every job runs on a GitHub-hosted runner and states its own permissions. Only the job that
creates the release can write to the repository. Dependabot proposes updates for the GitHub
Actions, the pinned Python requirements, the lint tools and the ESPHome release. The ESPHome
release is written in four more places that `version-lint.py` holds equal, so its Dependabot pull
request fails `lint-extra` until those are bumped in it. The clang-format and clang-tidy pins in
`cpp-lint.sh` are ESPHome's own and move with the ESPHome release, by hand.

## Docs site

`python3 .github/scripts/assemble_docs_site.py` assembles `_site_src` from `docs/guide/`,
`firmware/docs/` and `reverse-engineering/docs/` at build time. Nothing is duplicated in git: edit
the source docs, never a copy under `_site_src`. `python3 docs/guide/lint.py` checks the guide's
links, headings, sidebar and a leak denylist, and the `lint-extra` job runs it.

## The retired Matter builds

`main` used to carry two Matter firmwares, their SDK setup, patches, release tooling and
workflows. They were removed on 2026-10-12 and live on under the tag `matter-final`
([Matter Firmware (Retired)](Matter-Firmware-Retired)).
