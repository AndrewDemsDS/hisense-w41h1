# firmware/scripts

Build, release and operate the three firmware targets (AmebaZ2, ESP32/esp-matter, ESPHome). If you
are new here, you almost never call these directly: start at the one entry point.

## Start here: `dev.py` (the all-in-one)

`python3 firmware/scripts/dev.py <cmd> <target>` is the single front door. It wraps everything else,
prints each command before it runs it, and guides you through the safe order. `dev.sh` is a thin
shim that calls `dev.py`, so the name in older docs still works.

```
dev.py doctor  esp32                 # check tools + SDK pins (read-only)
dev.py test    esp32                 # host QA + the target's lint
dev.py build   esp32 --board c3      # build from source
dev.py flash   esp32 --port /dev/ttyACM0
dev.py ota     esp32 verify          # read the live on-device version + link
dev.py walk    esp32                 # guided doctor -> test -> build -> flash, asks at each step
```

Targets: `amebaz2`, `esp32`, `esphome`. Run `dev.py --help` for every command. It needs only
`python3`; the heavy work is delegated to the scripts below.

## The engines (called by `dev.py ota ...`; run directly only if you know why)

These carry the hard-won OTA traps in their comments (FWHS serial, delta base, flavour, brownout).
Read `firmware/docs/10-firmware-ota-procedure.md` before driving them by hand.

| Script | What it does |
|---|---|
| `ota-release.sh` | AmebaZ2 build + Matter-OTA: sync mirror, full clean, sign, package, stage, flash, revert. |
| `esp32-release.sh` | ESP32 build + delta Matter-OTA: archive the delta base (#82), package, stage, flash. |
| `ota-guards.sh` | Pre-flight + staging guards shared by both release scripts (flavour, link, freshness, Pi staging, HTTP mirror). Sourced, not run. |
| `ota_guards.py` | The pure guard decisions behind `ota-guards.sh`, host-tested by `../test/test_ota_guards.py`. |
| `sync-files.sh` | Single source of truth for which repo files are copied into the SDK example dir. Sourced by the release script and `scripts/setup.sh`. |
| `esp32-lint.sh` | Host-only gate: `PROJECT_VER` vs `sdkconfig` consistency. |

## Config

| File | What it is |
|---|---|
| `ota-release.env.example` | Template for `ota-release.env` (gitignored): SDK paths, device IDs, the Pi host/dirs, the OTA venv. Copy and fill in. |
| `ota-release.env` | Your filled-in copy. Never commit it. |
| `requirements.txt` | Python deps for the operator helpers below. |

## Matter operator helpers (day-to-day, over matter-server)

Small scripts that talk to the local `python-matter-server` (or BLE) through the shared
`ms_ws.py` WebSocket helper. Handy for triage; not part of a release.

| Script | What it does |
|---|---|
| `ms_ws.py` | Shared matter-server WebSocket helper (imported by the others). |
| `mnodes.py` | List commissioned nodes and their state. |
| `drive_ac.py` | Drive an A/C (power/mode/setpoint) over Matter. |
| `matter_scan.py` | BLE scan for commissionable Matter devices. |
| `mcli.py` | Commissioning CLI (Wi-Fi creds via `MATTER_SSID`/env). |
| `gen-creds.sh` | Generate a unique commissioning discriminator + passcode. |
| `amebaz2_image.py` | AmebaZ2 image format tool: walk/verify/re-sign the sub-image chain (#75). |

## Hooks

| Script | What it is |
|---|---|
| `stop-slop.sh` | Pre-commit gate that blocks the clearest AI-writing tells in newly added prose. |

## Where the rest lives (and why it is not here)

Host tests are with the tests, not here: `firmware/test/run_tests.sh` runs every layer (codec
goldens, Matter/ESPHome maps, virtual-AC round-trip, diag contract, image-signing chain, OTA
guards). SDK fetch/setup is at `firmware/setup.sh` + `scripts/setup.sh`. CI, the pre-commit hook,
and the docs reference those paths, so they stay put; `dev.py` calls them where they are.
