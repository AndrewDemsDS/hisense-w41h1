# Repo Map and Build Pipeline

The mental model a new developer needs before touching this repo. What lives where,
why the SDK is *outside* the tree, why editing a source file can have zero build
effect, and the one command that ships a firmware.

← back to [Home](Home) · siblings: [Protocol Overview](Protocol-Overview) ·
[Testing and QA](Testing-and-QA) · [ESP32 Replacement Build](ESP32-Replacement-Build)

---

## Repository layout

The repo is **original MIT sources + patches** that overlay onto two large third-party
SDKs you fetch yourself. It does **not** contain the SDKs.

| Path | What |
|---|---|
| `firmware/src/rs485-driver/` | The A/C bus driver: `hisense_rs485.{h,cpp}`, the pure host-testable `matter_aircon_map.h`, `power_estimate.h`, and `INTEGRATION.md` (design ref + provenance). **Our code (MIT).** |
| `firmware/src/sdk-edits/` | Capture of the Matter integration: `matter_drivers.cpp` glue, the `.zap`, the `0xFFF1FC00` mfg-cluster XML, and a `README.md` documenting every in-place SDK edit. |
| `firmware/scripts/` | `ota-release.sh` (build/package/flash/OTA), `sync-files.sh`, `gen-creds.sh`, Matter helpers. |
| `firmware/flasher/` | pyusb CH341A flasher (per-sector verify + retry; use this, **not** flashrom). See [Installing the Custom Firmware](Installing-Custom-Firmware) for the two install paths (CH341A clip vs. OTA). |
| `firmware/test/` | No-hardware QA: host codec + Matter-map tests + `virtual_ac.py`. See [Testing and QA](Testing-and-QA). |
| `firmware/docs/` | Wiring plan, attestation, QA strategy, energy monitoring, and the canonical OTA/build procedure (`10-firmware-ota-procedure.md`). |
| `firmware/esp32-matter/` | The ESP32 esp-matter replacement track; see [ESP32 Replacement Build](ESP32-Replacement-Build). |
| `reverse-engineering/` | Protocol / hardware / cloud / OTA RE + `tools/` (sniffer, decoders). |
| `patches/` | Your delta to the two SDKs, `git apply`-able; base commits pinned in `versions.env`. |
| `dumps/` | ⚠️ **local-only, gitignored**: raw flash (Wi-Fi creds + device RSA key + vendor blob). Never published. |

## The SDK is outside the repo

The build runs from the **AmebaZ2 base SDK**, not the Matter component alone.
`ameba-rtos-matter` is a *component* that plugs into `ameba-rtos-z2`. Three repos live
under the SDK checkout (symlinked `./sdk`, ~15 GB, **gitignored**):

```text
sdk/                              (the SDK checkout)
├── ameba-rtos-z2/                base RTL8710C SDK; the build runs here
│   ├── component/common/application/matter/   ← ameba-rtos-matter @ release/v1.4.2
│   └── third_party/connectedhomeip → ../../connectedhomeip
└── connectedhomeip/              project-chip/connectedhomeip @ v1.4.2-branch
```

Two setup steps fetch and wire it (see `firmware/README.md`):

```bash
firmware/setup.sh    # fetch the 3 SDKs into the SDK checkout + check out the pinned commits
scripts/setup.sh     # apply patches/ + the Matter-overlay edits, copy our source in
```

The Realtek SDKs are **proprietary and not redistributable**. The repo links and pins them,
never vendors them. **The Matter integration edits live in the SDK tree**, not in this repo. When
you change the wiring you edit the SDK, then re-capture into `sdk-edits/`. Don't move
the SDK; it breaks the build.

**Upstream pins** are the single source of truth in `versions.env` (sourced by `setup.sh`),
with full provenance + licensing in `UPSTREAM.md` and `NOTICE.md`.

## The MIRROR model (why editing a file can do nothing)

`firmware/src/rs485-driver/` and `firmware/src/sdk-edits/` are **mirrors**. The real build
consumes copies inside the SDK example dir. At build time, `ota-release.sh`
`sync_mirror` copies the mirror files into
`ameba-rtos-z2/.../examples/room_air_conditioner/`. **Editing a mirror alone has no build
effect until you sync it.**

The exact file set is defined **once**, in `sync-files.sh`,
and shared by both `ota-release.sh sync_mirror` (build time) and `scripts/setup.sh` (the
initial copy loop) so the two can never drift. (It exists because `matter_aircon_map.h`
once went missing from `ota-release`, and mapping edits never reached a rebuild.)
`REQUIRED` files hard-fail the build if absent; `OPTIONAL` are copied only if present.

## Build + ship: use the script

Canonical entry point: `firmware/scripts/ota-release.sh`. Its env comes from
`ota-release.env` (gitignored; copy `.env.example`), which keeps real hostnames and paths
out of git. The README's `./build-rac.sh` is stale; trust the script.

```
ota-release.sh lint                        # host tests + .zap contiguity + version check (the git hook)
ota-release.sh build [--bump] [--debug]    # sync mirror→SDK, full-clean, build, verify serial+endpoints
ota-release.sh package                     # pad clip image + create .ota + manifest
ota-release.sh stage                       # scp to the matter-server host + restart matter-server
ota-release.sh flash                       # update_node (retries) + verify device booted new version
ota-release.sh release [--bump] [--flash]  # build + package + stage (+ flash)
```

The one command, day to day:

```
firmware/scripts/ota-release.sh release --bump --flash
```

## Build flavours: release and debug

Every tagged release publishes **two** images, and they differ only in diagnostics:

| flavour | contains | use it for |
|---|---|---|
| **release** (default) | no diagnostic console, no bring-up logging | anything you deploy |
| **debug** (`build --debug`) | the `:2323` console (`features`, `poll`, `version`) plus verbose logging | bench work, and answering what your own A/C reports |

The flavour lives in the **filename**, never in the version, because the version int stays unified
across both:

```
flash_rac-integrated-v10213.bin        rac-v10213.ota          <- release
flash_rac-integrated-v10213-debug.bin  rac-v10213-debug.ota    <- debug
hisense_ac_matter.bin                  hisense_ac_matter-debug.bin
```

**Why release is the default.** The `:2323` console has **no authentication**, is reachable by
anyone on the same L2, and can read state, decode frames and drive the A/C bus. That is fine on a
bench and wrong on a deployed appliance controller, so you have to ask for it explicitly.

Three things to keep straight:

- A debug and a release image at the **same version are different binaries**. Never use one as the
  other's delta-OTA base or recovery image.
- `package` takes the flavour from the environment but the content from whatever `build/` holds, so
  it **verifies the binary against the claimed flavour** and hard-fails on a mismatch. If you see
  that error, you packaged after the wrong build.
- The Matter OTA provider serves **one image per softwareVersion**, so publishing both does not mean
  both can be staged at once. Pick one to serve.

## Hooks

A git **pre-commit hook** (repo `core.hooksPath = firmware/.githooks`) runs `lint` when
`firmware/src`, `firmware/test`, or the `.zap` is staged (bypass: `--no-verify`).

## Continuous integration & releases (GitHub Actions)

`.github/workflows/` mirrors the local gate and automates release builds:

- **`qa.yaml`** runs `ota-release.sh lint` (host codec/map tests, `.zap` contiguity, version
  sanity) on every push/PR, plus a PR-only check that `firmware/src/version.txt` strictly
  increases. It is hardware-free, so it runs on a GitHub-hosted runner: the same gate as the
  pre-commit hook, so CI and local never drift.
- **`esp32-release.yaml`** builds the ESP32 firmware on an `esp32-vX.Y.Z` tag and attaches the
  images to a GitHub Release. It runs on the same **self-hosted** `sdk-builder` runner and reuses
  that host's installed ESP-IDF + esp-matter (via `IDF_EXPORT` / `ESP_MATTER_EXPORT` in the runner
  env), which is faster and more reliable than a cold esp-matter bootstrap in a hosted container.
- **`amebaz2-release.yaml`** builds on the **self-hosted** `sdk-builder` runner (it holds the
  proprietary Realtek SDK) on an `amebaz2-vX.Y.Z` tag, and attaches
  `flash_rac-integrated-v<int>.bin` + `rac-v<int>.ota` + the manifest to the Release.

Both release builds are tag-push only, so fork pull requests never reach the runner, and neither
hardcodes the repo owner (a fork's own tagged builds run on the fork's own runner). Release binaries
are built with `-ffile-prefix-map` so they carry no builder home path. Cut a release by pushing a tag
whose version matches the tree: `amebaz2-v$(cat firmware/src/version.txt)` or `esp32-v<PROJECT_VER>`.
`stage`/`flash` stay manual.

## The three build traps (summary: docs/10 is canonical)

Three failure modes each **ship a broken or rolling-back image with no error**. Full analysis,
addresses, and the guards are in the canonical procedure,
**`firmware/docs/10-firmware-ota-procedure.md`**.
Do not restate it; the summary:

1. **Stale build cache.** The SDK reuses a stale core (`libCHIP.a`) + main lib and links an
   inconsistent image. A mandatory FULL clean precedes every build (the plain `clean_matter*`
   targets leave the copied bsp libs + gn out dir; remove those too). Judge a build by
   **activity, not wall-clock**: a genuine full build shows ninja compiling the core (hundreds
   of `[N/353] c++ …` lines) and rebuilds `libCHIP.a` fresh. The ameba make now runs
   `-j$(nproc)`, so a genuine full build is ~110 s; the old "under 2 min = fake" rule is retired
   (it false-flags good parallel builds). `ota-release.sh build` cleans correctly.
2. **OTA serial (cost a whole session).** AmebaZ2's bootloader A/B-selects the boot slot by
   the image's `FWHS.header.serial`, **not** the Matter `softwareVersion`. `build` sets
   `serial = SERIAL_BASE + softwareVersion` and log-verifies it. Forget it → the OTA
   transfers, applies, "finishes", and the device stays on the old version (looks like a
   rollback).
3. **Non-contiguous endpoints.** Endpoints should stay `{0,1,2,…}` with no gaps (treated as
   a zero-cost precaution; whether the gap is a proven crash cause is *unconfirmed*, it was
   confounded with the serial bug). To remove an endpoint, **renumber** to close the hole.
   `lint` blocks a non-contiguous `.zap`.

### Versioning (get it wrong → the provider won't serve)

Matter OTA is keyed on `softwareVersion`; the built version must be **strictly greater**
than what's running (convention: running + 1). Bump **both**
`CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION` (int) and `…_STRING` in
`connectedhomeip/src/include/platform/CHIPDeviceConfig.h`; `--bump` does it. Lint compares
against `built-images/.released-version` (last version *confirmed booted*). Don't reuse a
rolled-back number. Details in docs/10 §1 + §9.

## Editing the data model

The `.zap` is the **only** data-model source. `endpoint_config.h`, `.matter`, and
everything under `build/chip/codegen/` are **generated outputs**; never hand-edit them. Edit
via the ZAP GUI, then re-capture the `.zap` into `sdk-edits/`. A **new manufacturer cluster**
additionally needs minimal `zzz_generated` edits (`ClusterId.h` + callback decls/defs). Full
recipe + the ZAP GUI invocation: `firmware/src/sdk-edits/README.md`.
**Never** run `scripts/tools/zap_regen_all.py` for routine changes. It whole-tree-regenerates
and clobbers the hand-made HisenseAircon edits.

Current status and open work live in the project's issue tracker.
