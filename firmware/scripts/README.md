# firmware/scripts

Build, test and flash the ESPHome firmware. If you are new here, you almost never call anything
but the one entry point.

## Start here: `dev.py`

`python3 firmware/scripts/dev.py <cmd> esphome` is the single front door. It prints each command
before it runs it, and guides you through the safe order.

```
dev.py doctor  esphome                 # check the tools and the ESPHome version (read-only)
dev.py test    esphome                 # host QA + esphome config
dev.py build   esphome --board c3      # compile
dev.py flash   esphome --board c3 --port /dev/ttyACM0
dev.py walk    esphome                 # guided doctor -> test -> build -> flash, asks at each step
```

Run `dev.py --help` for every command. It needs only `python3`.

Three commands exist for a stock module that still runs the retired Matter firmware (issue #182):
`convert esphome --board amebaz2-<layout>` wraps an ESPHome image so that firmware installs it, and
`matter-slots <ip>` and `matter-backup <ip>` read the unit's slot state. They are described in
`docs/guide/Converting-a-Stock-Module-to-ESPHome.md`.

## The other files

| File | What it does |
|---|---|
| `amebaz2_image.py` | The stock module's image format: sub-image chain walk, verify, re-sign (#75). Used by `dev.py convert`, tested by `../test/test_image_chain.py`. |
| `cpp-lint.sh`, `cpp-lint.py` | C/C++ lint for every tree we own: `check` (clang-format, custom rules, clang-tidy: what CI runs) and `fix`. See `docs/guide/Testing-and-QA.md`. |
| `esphome-upstream-check.sh` | Runs ESPHome's own CI scripts against the `hisense_ac` component, in a checkout of `esphome/esphome`. |
| `version-lint.py` | The firmware version (`../esphome/packages/version.yaml`), the changelog and the release tag, checked against each other: `--tag vX.Y.Z` before a release, `--base <ref>` for the changelog rule of a pull request, `--notes` to print a version's changelog section. Runs in `run_tests.sh`, the pre-commit hook and CI. |
| `esphome-compile-check.sh` | Compiles the ESPHome configurations it is given and fails on a compiler warning from this repo's components or an image without the version. CI's compile gate for the ESP32 boards and for a release. |
| `stop-slop.sh` | Pre-commit gate that blocks the clearest AI-writing tells in newly added prose. |
| `ota-release.env.example` | Template for `ota-release.env` (gitignored): the settings of `convert` and the `matter-*` commands. Nothing else reads it. Never commit your filled-in copy. |

## Where the rest lives

Host tests are with the tests: `firmware/test/run_tests.sh` runs every layer. The clip tools for
the stock module's flash are in `firmware/flasher/`. The Matter release steps this directory used
to hold (`dev.py ota ...`, the matter-server helpers) are in the tree at the tag `matter-final`.
