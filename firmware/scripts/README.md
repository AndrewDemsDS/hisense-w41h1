# firmware/scripts

The tools that build, test, lint and flash the ESPHome firmware. For each one this page says what
it does, what it needs, and, for the ones that write flash or reach a machine, what stands
between a wrong argument and damage.

A newcomer needs one of them: `dev.py`, for local builds and the bench. Units in service are
normally built and flashed from the ESPHome Device Builder. The step-by-step use of `dev.py` is in
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
| `test` | `firmware/test/run_tests.sh`, then `esphome config` on the board file | nothing |
| `build` | `esphome compile` on the board file | the build directory |
| `flash --port P` | `esphome run --device P`: build, flash over USB, follow the log | **the board's flash** |
| `monitor --port P` | `esphome logs --device P` | nothing |
| `bench --port P --sim-port S` | flashes the board, then runs `virtual_ac.py` on the adapter and follows the node's log | **the board's flash** |

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

- **`flash`** and **`bench`** need `--port` and are refused for the stock module's boards
  (`--board amebaz2-*` has `doctor`, `test` and `build` only). `bench` asks whether the
  bench wiring is in place before it flashes. Nothing in `dev.py` checks that the board on the
  port is the one `--board` names, and it does not ask before flashing. The image holds your
  `secrets.yaml`.
- **`--board amebaz2`** without a layout is refused: an image for the wrong layout bricks the
  unit at its first update.
- A board file without `secrets.yaml` beside it is refused with the command that creates it.

> **`dev.py flash` and `dev.py bench` have not run on hardware.** The guide's record of what has
> been run ([`docs/guide/Build-Flash-Test.md`](../../docs/guide/Build-Flash-Test.md#what-has-been-run))
> lists both as untested through this script.

The guided `walk`, `next` (the bring-up text), `fetch` (a `pipx install` of the pinned ESPHome)
and `erase` (a typed-confirmation `esptool erase_flash`) were removed on 2026-10-27. Install
ESPHome with `pipx install esphome==<pin>` (`doctor` names the pin), and erase a brand-new ESP32
board with `esptool.py --chip <chip> --port <port> erase_flash`.

## The Matter conversion (retired)

> **Retired on 2026-10-27.** `matter-convert.py`, the tool that moved a stock module off this
> project's retired Matter firmware, and its settings template `ota-release.env.example` are no
> longer on `main`. The last release that carries them is `v2.0.2`, and the tag `matter-final`
> has them too. Nobody needs them unless a module still runs the old Matter firmware: see
> [`docs/guide/Converting-a-Stock-Module-to-ESPHome.md`](../../docs/guide/Converting-a-Stock-Module-to-ESPHome.md).

## The libraries and lints

| File | What it does |
|---|---|
| `amebaz2_image.py` | the stock module's image format: walk the sub-image chain, verify, re-sign (#75). Its one command, `amebaz2_image.py verify <dump-or-image>`, prints every image in the file with its chain and whether each signature verifies, exits 0 or 1 and writes nothing. Used by the clip writers in `firmware/flasher/`, tested by `firmware/test/test_image_chain.py`. The recipe is in [`firmware/docs/10-stock-module-boot-and-recovery.md`](../docs/10-stock-module-boot-and-recovery.md) |
| `version-lint.py` | holds the firmware version (`firmware/esphome/packages/version.yaml`), the changelog, the release tag and the ESPHome pins together. Options below |
| `cpp-lint.sh`, `cpp-lint.py` | C and C++ lint for every tree this repository owns. Options below |
| `esphome-compile-check.sh <config.yaml>...` | compiles the configurations it is given (paths relative to `firmware/esphome/`) and fails when one does not build, when one of this repository's components warns, when a board file's image does not carry the version, or when a stock-module build leaves no slot image of its layout. CI's compile gate. It copies the placeholder secrets when there is no `secrets.yaml`, so **never flash an image it built** |
| `esphome-components.sh [regex]` | prints the names of this repository's ESPHome components, read from the directories under `firmware/esphome/components/` (one per line, or as one alternation for `grep -E`). The warning gates, the cppcheck step and the lint scope ask it, so none of them holds a list. Fails when it finds none |
| `esphome-glue-tidy.py <compile_commands.json>` | clang-tidy over the components as a board build compiles them, for the files a host build cannot reach (`wifi_setup_mode.cpp`, `w41h1_module.cpp`, code under a platform `#ifdef`). It reports and always exits 0: it is not a gate |
| `esphome-upstream-check.sh` | runs ESPHome's own CI scripts against `hisense_ac` in a checkout of `esphome/esphome` that it clones under `~/.cache/hisense-w41h1/esphome-upstream` (`ci-custom.py`, `build_codeowners.py`, ruff, pylint, `test_build_components.py`). The default ref is the pinned release. `ESPHOME_UPSTREAM_REF=dev` checks against their development branch. It sends nothing upstream |
| `pin-bump.py esphome\|idf --check\|--set V` | finds a newer ESPHome release (PyPI, stable only) or ESP-IDF image for the nightly `esp32-recon` job (Docker Hub, same `vMAJOR.MINOR` line) and, with `--set`, moves the pin: for ESPHome every place `version-lint.py` checks (it imports that table), the install lines of the docs and a `CHANGELOG.md` line, for ESP-IDF the tag and linux/amd64 digest of the image and the lines that name it. `.github/workflows/pin-bumps.yaml` runs it daily and opens the pull request. Tested by `firmware/test/test_pin_bump.py` |
| `stop-slop.sh` | the pre-commit gate on newly added prose: it blocks an em dash in the added lines of staged Markdown |

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
section as its text and no firmware images. The first release it made is 2.0.0
(2026-10-13).

## Where the rest lives

Host tests: [`firmware/docs/04-qa-strategy.md`](../docs/04-qa-strategy.md). The clip tools for
the stock module's flash: [`firmware/flasher/README.md`](../flasher/README.md). The Matter
release steps this directory used to hold (`dev.py ota ...`, the matter-server helpers) are in
the tree at the tag `matter-final`.
