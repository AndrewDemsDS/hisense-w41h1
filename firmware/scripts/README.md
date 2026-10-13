# firmware/scripts

The tools that build, test, lint and flash the ESPHome firmware. For each one this page says what
it does, what it needs, and, for the ones that write flash or reach a machine, what stands
between a wrong argument and damage.

A newcomer needs one of them: `dev.py`. The step-by-step use of it is in
[`docs/guide/Build-Flash-Test.md`](../../docs/guide/Build-Flash-Test.md).

## `dev.py`

```bash
python3 firmware/scripts/dev.py <command> [--board B] [--port P] [--sim-port S] [--name N] [--friendly-name T]
```

It needs only `python3`, and prints every external command before it runs it. The word `esphome`
may follow the command (`dev.py build esphome`): it dates from when there were other firmwares
and is optional.

| Command | Does | Writes or drives |
|---|---|---|
| `doctor` | checks the tools and the ESPHome version | nothing |
| `fetch` | installs the pinned ESPHome with `pipx`, after asking | the machine it runs on |
| `test` | `firmware/test/run_tests.sh`, then `esphome config` on the board file | nothing |
| `build` | `esphome compile` on the board file | the build directory |
| `erase --port P` | erases the whole flash of an ESP32 board | **the board's flash** |
| `flash --port P` | `esphome run --device P`: build, flash over USB, follow the log | **the board's flash** |
| `monitor --port P` | `esphome logs --device P` | nothing |
| `bench --port P --sim-port S` | flashes the board, then runs `virtual_ac.py` on the adapter and follows the node's log | **the board's flash** |
| `next` | prints the staged bring-up and its safety warnings | nothing |
| `walk` | guided: doctor, test, build, flash, next, asking before each | what the steps it runs write |

Options:

| Option | Meaning |
|---|---|
| `--board` | `c3` (ESP32-C3 SuperMini, the default, `w41h1-esp32c3.yaml`), `c3-plus` (SuperMini Plus, `w41h1-esp32c3-plus.yaml`), `classic` (ESP32-D0WDQ6, `w41h1.yaml`), `amebaz2-factory`, `amebaz2-sdk`, `amebaz2-native` (the stock module, by flash layout) |
| `--port` | the board's serial device, for example `/dev/ttyACM0` |
| `--sim-port` | the USB-TTL or USB-RS485 adapter that `virtual_ac.py` talks through (bench) |
| `--name`, `--friendly-name` | the node's hostname and its name in Home Assistant (default `hisense-ac`, `Air Conditioner`). Give every node after the first its own |

Environment: `ESPHOME` names the esphome command (it may carry arguments, as in `uvx esphome`).
`PLATFORMIO_BUILD_CACHE_DIR` moves the build cache (default
`~/.cache/w41h1-dev/pio-build-cache`), and an empty value switches it off. The cache and its
caveats are in [`firmware/esphome/README.md`](../esphome/README.md#build-cache).

What protects against a wrong target:

- **`erase`** is refused for the stock module's boards. It warns that it is for a brand-new
  board, asks you to type `ERASE` with the chip and the port in the prompt, and passes `--chip`
  to esptool, so a port that leads to another kind of ESP is not erased. It cannot tell a new
  board from a node in service: erasing a working node throws away its saved Wi-Fi settings.
- **`flash`** and **`bench`** need `--port` and are refused for the stock module's boards
  (`--board amebaz2-*` has `doctor`, `fetch`, `test` and `build` only). `bench` asks whether the
  bench wiring is in place before it flashes. Nothing in `dev.py` checks that the board on the
  port is the one `--board` names, and it does not ask before flashing. The image holds your
  `secrets.yaml`.
- **`--board amebaz2`** without a layout is refused: an image for the wrong layout bricks the
  unit at its first update.
- A board file without `secrets.yaml` beside it is refused with the command that creates it.

Not proven: the guide's verification table records `dev.py flash` and `dev.py bench` as not yet
run against hardware through this script.

## `matter-convert.py`

> **For units on the retired Matter firmware only.** This script moves a stock module off this
> project's Matter firmware (not on `main` any more, last tree: tag `matter-final`). Nothing in
> it is needed to build, flash or update the ESPHome firmware. The procedure is in
> [`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md).

| Command | Does | Writes or drives |
|---|---|---|
| `convert --board amebaz2-factory\|amebaz2-sdk [--name N] [--software-version V] [--serial S] [--stage]` | re-signs the ESPHome image that `dev.py build` left behind with an FWHS serial in the Matter numbering and wraps it as a Matter `.ota` with its provider manifest, in `firmware/built-images/` | files. With `--stage`: **the OTA provider host** |
| `slots <unit-ip>` | reads both firmware slots' serials from a unit, over the Matter firmware's break-glass listener | nothing (read-only) |
| `backup <unit-ip> [out.bin]` | saves the unit's inactive slot and prints its address, which tells the flash layout | a local file (read-only on the unit) |

Settings come from `firmware/scripts/ota-release.env` (copy `ota-release.env.example`, the file
is gitignored), or from the file `ENVF` names. `convert` needs connectedhomeip's
`ota_image_tool.py` (`OTA_TOOL`), which is not part of this repository.

What protects against a wrong image or target:

- `convert` verifies the built image before it touches it and again after re-signing (the
  manifest signature, every sub-image trailer and the byte-sum, through `amebaz2_image.py`),
  and refuses an image that does not fit the slot of the layout named. It refuses
  `amebaz2-native`, which has no Matter firmware to install the image.
- It warns, and goes on, when the softwareVersion is not above the last Matter release (the unit
  is never offered the update) or the serial is not above that release's serial (the unit
  installs the image and boots the old one again). It warns for `amebaz2-sdk`: only the factory
  layout has been converted on hardware.
- **Nothing checks that the unit has the layout you named.** That is yours to establish first,
  with `backup` or the unit's history
  ([`firmware/docs/15-esphome-path.md`](../docs/15-esphome-path.md#which-flash-layout-a-unit-has)).
  The `.ota` is addressed by vendor and product id, so every unit of this product on the
  provider is offered it.
- `--stage` copies the `.ota` and its manifest to the provider directory on `PI_HOST` through a
  root container, renames this product's other manifests to `*.json.archived` (reversible,
  `OTA_KEEP_MANIFESTS=1` skips that), and **restarts matter-server**, which takes every Matter
  device of that server offline for about 100 s. It stops before installing anything when a step
  fails. The update itself is still started by hand, per unit, from Home Assistant.
- `slots` and `backup` send a token (`BREAKGLASS_TOKEN`) to the unit and only read. `backup`
  verifies what it received and saves nothing when the check fails.

Tested on the host by `firmware/test/test_matter_convert.py`. The conversion ran on hardware on
2026-10-10, with the factory layout.

## The libraries and lints

| File | What it does |
|---|---|
| `amebaz2_image.py` | the stock module's image format: walk the sub-image chain, verify, re-sign (#75). A module with no command line. Used by `matter-convert.py` and the clip writers in `firmware/flasher/`, tested by `firmware/test/test_image_chain.py`. The recipe is in [`firmware/docs/10-stock-module-boot-and-recovery.md`](../docs/10-stock-module-boot-and-recovery.md) |
| `version-lint.py` | holds the firmware version (`firmware/esphome/packages/version.yaml`), the changelog, the release tag and the ESPHome pins together. Options below |
| `cpp-lint.sh`, `cpp-lint.py` | C and C++ lint for every tree this repository owns. Options below |
| `esphome-compile-check.sh <config.yaml>...` | compiles the configurations it is given (paths relative to `firmware/esphome/`) and fails when one does not build, when one of this repository's components warns, when a board file's image does not carry the version, or when a stock-module build leaves no slot image of its layout. CI's compile gate. It copies the placeholder secrets when there is no `secrets.yaml`, so **never flash an image it built** |
| `esphome-upstream-check.sh` | runs ESPHome's own CI scripts against `hisense_ac` in a checkout of `esphome/esphome` that it clones under `~/.cache/hisense-w41h1/esphome-upstream` (`ci-custom.py`, `build_codeowners.py`, ruff, pylint, `test_build_components.py`). The default ref is the pinned release. `ESPHOME_UPSTREAM_REF=dev` checks against their development branch. It sends nothing upstream |
| `stop-slop.sh` | the pre-commit gate on newly added prose: it blocks an em dash in the added lines of staged Markdown |
| `ota-release.env.example` | template for the settings of `matter-convert.py`. Never commit a filled-in copy |

### `version-lint.py`

| Form | Checks |
|---|---|
| no arguments | the version is `MAJOR.MINOR.PATCH`, `CHANGELOG.md` has a section for it or an `## Unreleased` one, the version is not below the newest `v*` tag (when the clone has tags), and every place that pins the ESPHome release names the same one |
| `--tag vX.Y.Z` | also: the tag is `v` plus the version, and the changelog has that version's own section |
| `--base <ref>` | also: if anything under `firmware/esphome/components/` or `packages/` differs from the merge base with that ref, the changelog has to differ too |
| `--print` | prints the version and nothing else |
| `--notes [VERSION]` | prints that version's changelog section, for the release text |
| `--repo <dir>` | checks another tree (how its own test feeds it bad input) |

It runs in `run_tests.sh`, the pre-commit hook, the QA workflow and the release workflow.

### `cpp-lint.sh`

| Form | Does |
|---|---|
| `check [--no-tidy] [PATH...]` | `clang-format --dry-run -Werror`, the custom rules, clang-tidy. What CI runs |
| `fix [--no-tidy] [PATH...]` | applies the whitespace rules, the clang-tidy fixes that cannot change behaviour, then `clang-format -i` |
| `files [PATH...]` | lists the files in scope |
| `compdb [OUT_DIR]` | writes `compile_commands.json` for the host-compilable set |

It pins clang-format 13.0.1 and clang-tidy 22.1.8, the versions ESPHome pins. Lookup order:
`$CLANG_FORMAT` and `$CLANG_TIDY`, a binary on the path that reports the pinned version, then a
private environment under `~/.cache/hisense-w41h1/cpp-lint` that it creates on first use
(`CPP_LINT_NO_INSTALL=1` forbids that). Exit code 3 means a pinned tool is missing. The rules and
the scope are described in [`docs/guide/Testing-and-QA.md`](../../docs/guide/Testing-and-QA.md).

## Releasing

The version lives in `firmware/esphome/packages/version.yaml`, and a release is the tag
`vX.Y.Z` on `main`. How one is cut is in
[`CONTRIBUTING.md`](../../CONTRIBUTING.md#releasing). The release workflow
(`.github/workflows/esphome-release.yaml`) checks the tag against the version and the changelog,
runs the host tests, compiles every board file and publishes a GitHub release with the changelog
section as its text and no firmware images. As of 2026-10-13 it has run once, as a dry run from a
branch, and no release has been tagged.

## Where the rest lives

Host tests: [`firmware/docs/04-qa-strategy.md`](../docs/04-qa-strategy.md). The clip tools for
the stock module's flash: [`firmware/flasher/README.md`](../flasher/README.md). The Matter
release steps this directory used to hold (`dev.py ota ...`, the matter-server helpers) are in
the tree at the tag `matter-final`.
