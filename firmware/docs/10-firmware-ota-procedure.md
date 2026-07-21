# 10: Firmware build + Matter OTA procedure (canonical)

The repeatable, mistake-proof procedure for building the room_air_conditioner firmware and
shipping it to the live device over Matter OTA. Encodes the official rules **plus** the traps we
hit the hard way (unbumped versions, dep-tracking rebuilds, an endpoint gap that boot-crashed a
whole release, the OTA provider-discovery race, A/B rollback).

**Do not run these steps by hand.** They are automated in
[`firmware/scripts/ota-release.sh`](../scripts/ota-release.sh); a git pre-commit hook
(`firmware/.githooks/`) runs the fast checks on every firmware change. This doc is the *why*.

---

## 1. Versioning: unified semver → monotonic int (issue #77)

- Source of truth: **`firmware/src/version.txt`** (git-tracked) now holds a **semver**
  `MAJOR.MINOR.PATCH` (e.g. `1.2.0`), so CI can see + gate it without the SDK. The Matter
  **softwareVersion int is DERIVED**: `MAJOR*10000 + MINOR*100 + PATCH` (so `1.2.0 → 10200`), a
  readable, strictly-monotonic `uint32`. This keeps the human semver in the string + git tags
  while the int keeps climbing. `build` force-syncs the SDK header from it, both
  `CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION` (int) **and** `…_STRING` (the semver) in
  `connectedhomeip/src/include/platform/CHIPDeviceConfig.h`. Never hand-edit the header or the int,
  edit the semver in `version.txt` (or `ota-release.sh build --bump[-minor|-major]`) and commit it.
- `ota-release.sh verint <semver>` prints the derived int (CI + tooling use this; no SDK needed).
  It also accepts a legacy raw int, so a branch still on the old integer `version.txt` compares
  cleanly against a semver head. **Minor/patch must be `< 100`** (the `*10000+*100` mapping).
- The int **must be strictly greater** than the version currently running, or the provider declines
  to serve (official CSA rule). The fleet is at Ameba **sw34**; `1.x.x → ≥10000 > 34` clears it, so
  the semver can start clean while the int still increases. Tag convention: **`amebaz2-vX.Y.Z`** and
  **`esp32-vX.Y.Z`** (path-prefixed; the bare `v1.0.0`/`v1.1.0` tags are retired). `ota-release.sh tag`
  (or `release --tag`) creates the AmebaZ2 tag locally.
- **ESP32 path** mirrors this: `firmware/esp32-matter/CMakeLists.txt` derives the int from
  `PROJECT_VER` with the same formula and injects it as a compile definition that wins over
  `sdkconfig`'s `CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER` (the `#ifndef` guard in `CHIPDeviceConfig.h`),
  so the two can't drift. Edit `PROJECT_VER`, not the sdkconfig number.
- The `.ota` header carries `minApplicableSoftwareVersion` / `maxApplicableSoftwareVersion`. We
  set `min=1`, `max=(newint − 1)` so the image applies to any older device.
- The version is **compiled in**: bumping it forces a rebuild (a core header, so a wide one).
- Don't reuse a number for different bytes: if a build rolls back, the *next* attempt must be a
  **new** semver (higher int), not the same one, matter-server and the device cache by int.
  `ota-release.sh` refuses to build an int ≤ the device's current one.

## 2. What you edit vs. what is generated (never hand-edit outputs)

| Edit (source) | Generated OUTPUT, never hand-edit |
|---|---|
| `firmware/src/rs485-driver/*` (driver, mirrored to SDK) | `build/chip/codegen/zap-generated/endpoint_config.h` |
| `firmware/src/sdk-edits/*` (glue + `.zap` capture) | everything under `build/chip/codegen/` |
| `room-air-conditioner-app.zap` (the **only** data-model source) | `*.matter` |
| `CHIPDeviceConfig.h` version | |

- The `.zap` is the data-model source; `endpoint_config.h` etc. are regenerated from it each
  build (GENERATE_ZAP). **Never** run `scripts/tools/zap_regen_all.py` for routine changes, it
  whole-tree-regenerates and clobbers the hand-made `zzz_generated` HisenseAircon
  `ClusterId.h`/callback edits (see `sdk-edits/README.md`).
- A **new manufacturer cluster** needs GUI-authored `.zap` + minimal `zzz_generated` edits
  (ClusterId.h + callback decls). Standard clusters/attributes/endpoints: edit the `.zap`.

## 3. Endpoint rules (keep endpoints contiguous: precaution, not a proven crash cause)

- **Keep endpoints contiguous**: `{0,1,2,3,…}` with no gaps. Removing endpoint 2 while keeping
  3–7 produced `FIXED_ENDPOINT_ARRAY {0,1,3,4,5,6,7}` in the failed "v14" (2026-07-07), which
  A/B-rolled-back on boot. **The gap being the cause is UNCONFIRMED**, though: that same build
  also carried the **serial/boot-slot bug** (§4, the FWHS serial, not the version, picks the
  slot; it caused the *repeated* rollbacks that session) **and** a hand-edited FanControl
  `FeatureMap` (a co-suspect). So "a gap boot-crashes the device" was never isolated, it's one
  of three confounded factors, and the two others are independently known to roll a build back.
- **Evidence the current layout is fine:** the shipping **v23** endpoint set (post-I2 renumber)
  boots and runs on **two units** (nodes 11 + 14). A contiguous array is clearly *sufficient*; a
  gap has simply never been tested in isolation.
- **Guidance:** treat contiguity as a **zero-cost precaution**: when removing an endpoint,
  **renumber** to close the hole rather than risk it. `ota-release.sh lint` still blocks a
  non-contiguous `.zap`. If you ever need to *disprove* the gap theory, build a gap-only image
  (correct serial, untouched FeatureMap) and see if it boots.
- Adding endpoints (ep4–7 switches, the Electrical Sensor) works via hand-JSON in the `.zap`;
  codegen picks them up. Removing/reordering is the fragile direction.
- A GUI-authored `.zap` (`run_zaptool.sh`) is the safe way to add/remove endpoints & clusters,
  it keeps feature/attribute consistency (a hand-edited FanControl `FeatureMap` was a secondary
  suspect in the same boot crash).

## 4. Build: **full clean BEFORE every build** (non-negotiable)

The SDK's build cache reuses a **stale core (`libCHIP.a`) + main lib** and produces a
**"fake" build** that links an *inconsistent* image, it flashed + OTA'd fine but **failed to boot
and A/B-rolled-back on-device three times** (2026-07-08) before we traced it here.

**Tell a genuine build from a fake one by ACTIVITY, not wall-clock.** A genuine build shows ninja
compiling the core, **`[N/353] c++ …`, all 353 targets** (hundreds of lines), and rebuilds
`libCHIP.a` fresh (check its mtime is post-clean). A fake/stale build runs only ~900 ninja/ar lines
(the example + archiving), reuses the old `libCHIP.a`, and touches no core `.cpp.o`.

**Timing note (2026-07-14):** the ameba `make` steps now run `-j$(nproc)` (16 cores here), so a
genuine full build is **~110 s**: down from ~20–30 min when the core compiled serially. The old
"**< 2 min = fake, stop**" heuristic is **RETIRED**: it now false-flags legitimate fast builds
(v34 built in 111 s, compiled all 353 targets, booted clean, link healthy). Use the activity check
above instead. (ccache is still bypassed by the ninja core, 0 cacheable, so it isn't the speedup;
`-j` is. Fixing ccache / a tiered `--fast` build remain open speedups but are low-priority now.)

So from `…/realtek_amebaz2_v0_example/GCC-RELEASE`, always:
```
source connectedhomeip/scripts/activate.sh      # NOT piped — puts zap-cli/gn/ninja/gcc on PATH
# --- mandatory clean: clean_matter_libs + clean_matter clean the OBJECTS, but they LEAVE the
#     copied bsp libs + the gn out dir, which is the cache that wins. Remove those too: ---
make clean_matter_libs
make clean_matter
rm -f ../../../component/soc/realtek/8710c/misc/bsp/lib/common/GCC/{libCHIP.a,lib_main.a}
rm -rf ../../../component/common/application/matter/examples/room_air_conditioner/build/chip
# --- then build (serial; ninja parallelizes the core internally): ---
make room_air_conditioner_port && make is_matter
```
`ota-release.sh build` does exactly this clean-then-build automatically. **Do not** use `-j` on the
top-level make (races) and **do not** rely on incremental builds for anything you'll flash, the
cache is not trustworthy here.
Known SDK dep-tracking bugs (all handled by the script):
- **(a) `.zap`/attribute changes don't propagate**: a broken `.d` path means a regenerated
  `endpoint_config.h` does **not** rebuild `attribute-storage_lib_main.oo`, so a stale default
  ships. Always `touch attribute-storage.cpp` after a data-model change (or the nuclear
  `make clean_matter`).
- **(b) a newly-added `SRC_CPP` file is never compiled**: adding a new source to the main.mk
  does not get it built/linked (undefined refs). Workaround used for the EPM delegate: it is
  `#include`-d into `matter_drivers.cpp` (an always-rebuilt TU) and **kept out of** SRC_CPP. Don't
  "fix" that by adding it back to SRC_CPP.
- **(c) example-select / stale-ChipTest link errors**: delete all example `*_lib_main.oo` (both
  the source dir and `lib_main/Debug/obj`) + `lib_main.a`, then rebuild (`CLAUDE.md`).

## 5. OTA image + manifest

```
python3 ota_image_tool.py create -v 0xFFF1 -p 0x8001 -vn <N> -vs "<N>.0" \
    -da sha256 -mi 1 -ma <N-1> <…>/firmware_is.bin  rac-v<N>.ota
```
Plus a sidecar manifest `rac-v<N>.json` matter-server reads (VID/PID/version/`otaFileSize`/
`otaChecksum` = base64 SHA-256 of the `.ota`/`otaUrl`/min/max). `ota-release.sh` computes size +
checksum so they can't drift.

Also pad `flash_is.bin` → 4 MB `flash_rac-integrated-v<N>.bin` for the **clip** path (CH341A),
which needs no infra and is the recovery route if OTA is unavailable.

The matching **stock** recovery image is `built-images/flash_rac-stock-v1.bin` (4 MB = the stock
`room_air_conditioner` `flash_is.bin` + 0xFF pad; built-in test DAC/PAI/CD VID `0xFFF1`, pairing
code `34970112332`). Its `0x0` system-data block is byte-identical to the stock dump, so a
whole-chip CH341A write (`ch341flash-full.py`) is safe, keep it as the fallback recovery image
alongside `dumps/w41h1_dump1.bin`. Verify a fresh flash by commissioning into stock HA Matter (an
"uncertified device" warning is expected with the test certs; see `docs/02`).

## 6. Deliver via matter-server (on the Pi) + its caching

- matter-server runs on the **Pi** (`your-ha-host.local`), **not** localhost. Point your scripts
  at the Pi's `MS_WS` and your device's Matter `NODE_ID` (from `ota-release.env`); matter-server
  serves from `--ota-provider-dir /data/ota` (host `…/matter-server/ota`).
- Trigger: `check_node_update` → `update_node(node, software_version=N)`.
- Caching (manifest load-once, node-attribute cache, HA entity cache): see §9.

## 7. OTA is flaky by design: retry; and it can roll back

- `update_node` frequently returns **error 11 "Target node did not process the update file"** on
  the first attempt(s): the provider is re-commissioned ephemerally each attempt, and the target
  loses the discovery/session race with the brand-new provider (`kQuerying → kIdle`). It succeeds
  on retry once the provider is discoverable. **Always retry** (3–5×, ~15 s apart), the script
  does. Widespread python-matter-server issue, not our image.
- **A/B rollback:** if the new image fails to boot, AmebaZ2 reverts to the previous image and the
  device reports the **old** version after "OTA finished successfully". So **verify the reported
  softwareVersion actually changed**, do not trust matter-server's "finished". The script polls
  until it sees the new version (or reports a rollback).

## 8. The one command

```
firmware/scripts/ota-release.sh release --bump           # build + package + stage (no flash)
firmware/scripts/ota-release.sh release --bump --flash    # + OTA it and verify the boot
firmware/scripts/ota-release.sh lint                      # fast checks only (run by the git hook)
```
Environment-specific values (SDK path, Pi host, OTA dir, node id, VID/PID) live in
`firmware/scripts/ota-release.env` (gitignored; copy from `.env.example`) so no real hostnames or
paths are committed.

The **git pre-commit hook** (`firmware/.githooks/pre-commit`, wired via repo `core.hooksPath`)
runs `lint` whenever `firmware/` files are staged (host tests + `.zap` contiguity/version) so a
boot-crashing config or an unbumped version can't be committed. It chains the global
`prepare-commit-msg` so the `Assisted-by: AI` trailer still applies.

## 9. Caching (verified 2026-07-08): five layers, each can silently break an update

1. **Version dedup, THE one that bites.** Matter OTA is keyed on `softwareVersion`. If you
   rebuild new bytes under a version the device already runs, the device thinks it's up-to-date
   and **won't accept the image** (no error, it just never updates). *Always bump.* The lint
   compares against `built-images/.released-version` (the version last **confirmed booted**,
   written by `flash`), not a filename, so it can't be fooled by our informal `rac-vN` labels.
2. **matter-server manifest cache.** `load_local_updates()` runs **once at init**
   (`device_controller.py:186`), so a freshly-staged `.ota`/`.json` is invisible until the
   container restarts. `stage` restarts it. Symptom if skipped: `check_node_update` shows the old
   version.
3. **Ephemeral-provider junk.** Each attempt spawns + commissions a fresh provider, leaving
   `chip_kvs_ota_provider_*` + `ota_provider_*.log` in the OTA dir. Harmless but accumulates;
   `stage` prunes it. Old `.ota`/`.json` manifests also pile up, keep them (rollback images) but
   ensure **no two manifests share a `softwareVersion`** (collision → provider may serve the wrong
   bytes).
4. **matter-server node-attribute cache.** `get_node` returns cached attributes; after the reboot
   the `SoftwareVersion` refreshes via re-subscription. Don't trust the cached read, `flash`
   polls until the device *reports* the new version (this is also the A/B-rollback guard, §7).
5. **HA Matter-integration entity cache.** HA builds entities from the node structure at setup and
   caches it; after a structure-changing OTA (new/removed endpoints or clusters) the new entities
   don't appear until a **node re-interview** (or reloading the Matter integration). `flash`
   auto-calls `interview_node` on success; if entities still lag, reload the Matter integration in
   HA (Settings → Devices & Services → Matter → ⋮ → Reload).

Build-side "caching" (stale `attribute-storage`, uncompiled new source) is the dep-tracking class
in §4, handled by the `touch` + inline-include, not by these OTA-layer steps.

## 10. Build speed: ccache + parallelism (wired into `build`, no SDK edits)

A version bump recompiles most of the CHIP core (~10–15 min cold). Two multipliers, both applied
by `ota-release.sh build`:

- **Parallel:** 16 cores. The **GN core** build runs `ninja :ameba`, which already uses all cores
  by default (no change). The **make** main-lib/app build gets `-j$(nproc)` (its `%.oo` rule uses
  an *order-only* prereq, so parallel is race-safe). Override with `BUILD_JOBS=` in the env.
- **ccache: wired the OFFICIAL way (2026-07-08).** The earlier PATH-masquerade was wrong: GN bakes
  the **absolute** compiler path into `build.ninja` at gen-time, so a PATH shim never intercepts the
  ninja build (`ccache -s` stayed flat). The official mechanism (pigweed) is the GN arg
  **`pw_command_launcher = "ccache"`** → pigweed `generate_toolchain` sets GN's native
  `command_launcher` → ninja prefixes every compile with ccache. Confirmed the path: connectedhomeip's
  `build/toolchain/gcc_toolchain.gni` forwards to `$dir_pw_toolchain/generate_toolchain.gni`, which
  honours `pw_command_launcher` (`toolchain_args.gni` documents ccache as the example).
  - **GN core:** `build` injects `pw_command_launcher = "ccache"` into the args.gn generation in
    `…/amebaz2plus/make/chip_core_rules.mk` (after `ameba_cpu`), idempotently self-healing on a fresh
    SDK.
  - **make main-lib/app** (not GN): `build` passes `CC='ccache $(CROSS_COMPILE)gcc'` / `CXX=…` on the
    make line (overrides the mk's `CC`, leaves `AR/AS/LD` alone).
  - Cache at `~/.ccache` (25 G, `CCACHE_MAXSIZE`), tuned with `CCACHE_BASEDIR=$HOME` +
    `compiler_check=content` + `sloppiness=time_macros,…` (2026-07-14).
  - **REALITY CHECK (measured 2026-07-14): ccache is NOT what makes the build fast, it's bypassed
    by the ninja core.** `ccache -s` shows **0 cacheable calls** across a full build (the
    `pw_command_launcher = "ccache"` prefix isn't actually wrapping the ninja `c++` compiles), and
    the tuning above didn't change that. The real speedup is **`-j$(nproc)`** (§ above), a genuine
    full build is **~110 s** on 16 cores regardless of ccache. Getting ccache to actually wrap the
    ninja compiles (or a tiered `--fast` build that skips the unchanged core) is an OPEN optimization,
    now low-priority. Leave the ccache wiring in place (harmless); just don't expect it to help yet.

Install if missing: `sudo pacman -S ccache`. Correctness is unaffected either way.

## 11. ⚠️⚠️ THE mistake that cost a session: the AmebaZ2 OTA **serial** (2026-07-08)

**Symptom:** every OTA after v12 "transferred + applied + finished successfully" (full
`kDownloading → kApplying → kIdle` in matter-server) but the device stayed on the old version.
It looked like a boot crash / "OTAs not accepted" / A-B rollback. It was **none of those**.

**Root cause:** AmebaZ2's bootloader selects the boot slot by the firmware image's **`serial`**
(`amebaz2_firmware_is.json` → `FWHS.header.serial`), **NOT** the Matter `softwareVersion`. That
serial was **hardcoded 1100** for every build. So the new slot was never "newer" than the running
one → the bootloader kept the old slot after applying. Bumping the serial (1100 → 1114) made the
next OTA stick on the first try.

**The rule (now automated in `build`):** `FWHS.serial = SERIAL_BASE + softwareVersion`: always
strictly increasing with the version. `build` sets it before `is_matter` and **verifies**
`header-serial N` appears in the assembly log, or it refuses. Two versions of the fix are wrong:
- bumping only the Matter `softwareVersion` (what we did for days), irrelevant to the bootloader;
- a "full clean rebuild" (an earlier mis-fix aimed at a stale-cache theory), a clean build is
  consistent, but it was **not** the cause; the incremental image was fine, the *serial* was stale.
  With the serial handled, the full clean is optional belt-and-suspenders, not the fix.

**Two secondary mistakes, also now guarded:**
- **Flash false-positive:** the verify used `get_node` (matter-server's *cached* attributes), which
  returned a stale `14` right after a container restart → "success" with the device still on 12.
  Fixed: `flash` uses `read_attribute` (fresh) and requires the new version **sustained across 3
  consecutive reads**.
- **Forgetting the version bump** and **leaving an endpoint gap**: both already blocked by the
  pre-commit `lint` (version > `.released-version`; contiguous `.zap` endpoints).

### Build flavours (#22 / #23)

Every tagged release publishes **two** images. They differ only in diagnostics.

| flavour | build | contains |
|---|---|---|
| release (default) | `ota-release.sh build` | no console, no bring-up logging |
| debug | `ota-release.sh build --debug` | `:2323` console (`features`, `poll`, `version`) + verbose logging |

`--debug` generates `hisense_flavour.h` into the SDK example dir; a plain build removes it, so
release is what you get unless you ask, and the unauthenticated console cannot ship by forgetting a
flag. Set `HISENSE_FLAVOUR=debug` in `ota-release.env` to make debug your local default.

The version int is identical for both (versioning stays unified); the flavour lives in the
**filename**: `flash_rac-integrated-v<N>-debug.bin`, `rac-v<N>-debug.ota`.

Three traps specific to flavours:

1. **`package` does not know what you built.** It takes the flavour from the environment and the
   content from `build/`, so it verifies the binary against the claimed flavour (by looking for the
   console's log string) and hard-fails on a mismatch. If that fires, you packaged after the wrong
   build.
2. **Same version, different binaries.** A debug and a release image at one version are not
   interchangeable: never use one as the other's delta base or recovery image.
3. **One served image per version.** The Matter OTA provider keys on `softwareVersion`, so both
   flavours existing does not mean both can be staged at once.

### Guard summary (what the hook + build now enforce)
| Mistake | Guard | Where |
|---|---|---|
| OTA serial not bumped | `serial = SERIAL_BASE + version`, set + log-verified | `build` |
| softwareVersion not bumped | `config version > .released-version` | `lint` (pre-commit hook) |
| endpoint gap (non-contiguous) | `.zap` contiguity check + build-output check | `lint` + `build` |
| flash false-positive (stale cache) | fresh `read_attribute`, sustained ×3 | `flash` |
| manifest/version cache | restart matter-server; unique versions | `stage` (§9) |
| ESP32 built on the wrong IDF | live `idf.py --version` vs `dependencies.lock` | `esp32-release.sh build` |

**The IDF-mismatch guard (ESP32).** `dependencies.lock` records the IDF that produced the last
committed build. Sourcing a different `export.sh` (easy to do: `~/esp/esp-idf` is **v5.3.1** while
`~/esp/esp-idf-v5.5.4` is the locked one) silently builds against another toolchain. The image still
boots and passes every functional check, so nothing catches it at runtime, but:

- the whole binary shifts, so the **delta-OTA patch balloons** (measured: a 45 KB patch became
  854 KB, which matters precisely on the lossy links where OTA already fails), and
- the build **rewrites `dependencies.lock` as a side effect**, so the drift only surfaces in
  `git status` afterwards.

This shipped 1.0.9 on 5.3.1 against a 5.5.4 lock and cost a version plus an extra OTA cycle. The
check runs **before** the build (the build itself rewrites the lock). Intentional bumps:
`ESP32_ALLOW_IDF_MISMATCH=1`, then commit the lock change deliberately.

## 12. OTA reliability + distribution (issues #76 / #78 / #79)

Matter OTA runs over CHIP **BDX** (stop-and-wait, one ~1 KB block per UDP+MRP round-trip) so a
~1.5 MB image is minutes of chatty exchanges that stall on marginal Wi-Fi (observed hard at ~−67 dBm:
matter-server returns **error 11 "Target node did not process the update file"**). Three mitigations,
ported from the ESP32 esp-matter build to AmebaZ2:

- **#76: MRP tuning (survive deep fades).** AmebaZ2 has no ESP-IDF Kconfig, so CHIP fell back to the
  weak upstream MRP defaults (`MAX_RETRANS=4`, active-retry 300 ms, idle-retry 500 ms). `build` now
  idempotently injects overrides (`RETRANS=8`, active 500, idle 800, sender-boost 300, the ESP32
  values) into `connectedhomeip/src/platform/Ameba/CHIPPlatformConfig.h` via
  `apply_ota_hardening()`; canonical block + rationale in
  `firmware/src/sdk-edits/chip-ameba-ota-hardening.h`. `CHIPConfig.h` includes the platform config
  before `ReliableMessageProtocolConfig.h` applies its `#ifndef` defaults, so ours win. (The image is
  already built `-Os` here (chip core+main) so the ESP32 size lever was already in place; delta-OTA
  and a larger BDX block remain follow-ups.)
- **#78: HTTPS-OTA manual backup (break-glass).** Writing **`Identify.IdentifyTime = 88` on ep1**
  (`firmware/src/sdk-edits/matter_drivers.cpp`, Identify case in the uplink handler) spawns a task that
  fetches a plain-HTTP image from the Pi file server and applies it via the Realtek SDK's
  `http_update_ota()` (writes the idle A/B slot), then `ota_platform_reset()`. TCP's window/retransmit
  is far more robust than BDX on a lossy link. **Serve the build's `firmware_is.bin`** (correct FWHS
  serial, §11) as `HISENSE_OTA_RESOURCE`, e.g. `cp .../firmware_is.bin <docroot>/rac-ota.bin`;
  target host/port/path are compile-time macros in `matter_drivers.cpp`. Matter OTA stays primary.
- **#79: remote OTA distribution.** Set `OTA_RELEASE_BASE` in `ota-release.env` and `package` writes
  the manifest `otaUrl` as the release-asset URL
  (`$OTA_RELEASE_BASE/amebaz2-v<semver>/rac-v<int>.ota`) instead of `file:///…`. python-matter-server's
  OTA provider downloads an http(s):// `otaUrl` (checksum-verified) then re-serves it over BDX, so the
  big `.ota` lives in the GitHub release and only the small `.json` need be staged. CI
  (`.github/workflows/amebaz2-release.yaml`) already attaches `rac-v*.{ota,json}` on `release: published`
  (needs the self-hosted `sdk-builder` runner; until it exists, build+publish from the dev box).

## 13. Break-glass trigger that does not depend on Matter (issue #61)

`#78`'s `Identify = 88` trigger travels **over Matter**, so it is refused exactly when it is needed.
python-matter-server checks a client-side `node.available` flag *before* it contacts the device:

```python
if (node := self._nodes.get(node_id)) is None or not node.available:
    raise NodeNotReady(f"Node {node_id} is not (yet) available.")
```

That flag is set only after a **subscription** succeeds. A node whose subscriptions fail but whose
reads still work is therefore refused, even though the device would answer. On 2026-07-19 the
AmebaZ2 node was in exactly that state: console answering, A/C bus polling 293 frames, no faults,
and un-reflashable over the air because the only escape hatch sat on the broken transport.

**The listener.** A small authenticated TCP listener on `BREAKGLASS_PORT` (default 2324), compiled
into **both flavours**. Send the token, get `ok` and the device starts the same `#78` fetch; send
anything else and get a bare `no`.

```
printf 'TOKEN\r\n' | nc <device-ip> 2324
```

It is deliberately **not** a `:2323` diag-console command. That console is debug-flavour only and
unauthenticated by design, so a trigger living there would be absent from precisely the images most
likely to need it.

**Fails closed.** No `BREAKGLASS_TOKEN` in `ota-release.env` means the socket is never opened, and
the boot log says so instead of staying silent. There is deliberately no default token: a default in
a public repo is equivalent to no authentication. Set it in `ota-release.env` (gitignored); the
build injects it via `-D`, mirroring `HISENSE_OTA_URL`.

**Limits.** A plaintext secret over an unencrypted LAN socket that can start a firmware fetch. Real
improvement over an unauthenticated port, not strong authentication. And it only exists from the
next successful flash onward, so it cannot rescue an image that shipped without it.

## 14. Trust the device, not the tool (four cases in one session)

`ota-release.sh flash` reported the wrong outcome **four times** on 2026-07-19. The device's own
report was correct every time.

| tool said | reality |
|---|---|
| `declined: 11` x3, then silence | OTA had applied; device was running the new build |
| `FAILED: never sustained v10226` | device was on the newer v10227 (a later flash superseded it) |
| exit 0, no verdict line | v10227 booted fine |
| matter-server `avail=False` | device healthy, console answering instantly |

Verify against the device, in this order:

1. **The diag console** (`version` on `:2323`) is the most direct answer to "what is actually
   running", and it works when Matter does not.
2. **`read_attribute`**, not `get_node`, which returns cached attributes.
3. matter-server's node state is a **cache plus a client-side flag**. It can lag reality by a whole
   firmware version.

Corollary: `avail=False` does **not** mean the device is unreachable. Check the console before
concluding anything about the network. Pinging a link-local address also needs the right interface
(these devices sit on a tagged VLAN), so a failed ping from the wrong interface proves nothing.

## 15. After a structure-changing OTA, re-interview (not optional)

Adding or removing an endpoint or cluster changes the data model. matter-server keeps the **old**
cached model and will keep failing against it, including with TLV decode errors, until it is
re-interviewed:

```python
await ms_ws.call(ws, "interview_node", {"node_id": <id>}, "1", timeout=180)
```

Observed both directions in one session: after adding ep10, and again after reverting it, the node
stayed `avail=False` with a stale model (`sw` and endpoint list both wrong) until re-interviewed.
`interview_node` has **no** availability guard, which is why it works on a node that `write_attribute`
refuses.

## 16. A green build is not a working data model

The 2026-07-19 regression shipped with **every** existing gate passing: full clean build, contiguous
endpoint lint, host codec and Matter-map tests, and correct-looking generated `endpoint_config.h`
(right types, sizes, min/max entries, matching counts). None of them exercise a **subscription**,
which is what broke: reads worked, subscription priming reports failed with CHIP error `0x24`
"Invalid TLV tag", and Home Assistant lost the node.

Two theories were investigated and **refuted**, so do not re-tread them:

- the hand-written globals' `defaultValue` `""` vs `null` is inert. ZAP forces `External`
  attributes' defaults to `undefined` before codegen, so output is byte-identical.
- the `60` vs `61` cluster-count delta is correct, not an off-by-one: 60 server clusters plus one
  client-side OTA-requestor cluster on ep0.

Best-supported lead is upstream
[connectedhomeip#32273](https://github.com/project-chip/connectedhomeip/issues/32273): identical
signature while encoding global attributes during a wildcard subscription's **priming report** on a
stock example app. That fits the asymmetry seen here, since an interview issues many narrow reads
while a subscription does one wildcard expansion across every server cluster.

**Before re-landing a data-model change:** confirm `Subscription succeeded` in the matter-server log
and `avail=True` after a re-interview. This gate is now **automated in the flash path** (issue #64):
both `ota-release.sh flash` and `esp32-release.sh flash` treat the post-OTA re-interview as fatal,
poll the node over the websocket until `available` (~75 s timeout), and, when the matter-server log
is reachable from the release box, require `Subscription succeeded` in it. A build that cannot be
subscribed to now fails the flash step loudly instead of shipping. Pinning the exact culprit needs
verbose `CHIP:DMG` logging during a failing subscribe; the line before the error names the cluster
and attribute.

### Editing the `.zap` without the GUI

Scripted JSON edits do work for **standard** clusters (they resolve against stock ZCL metadata), and
a full build confirmed correct codegen: `FIXED_ENDPOINT_COUNT`, both clusters present in the
`.matter`, contiguity lint clean. But that build passed while shipping the model that broke
subscriptions, so treat GUI-free editing as **build-verified, not runtime-verified**. Manufacturer
clusters still require the GUI plus the `zzz_generated` edits. Cheap insurance either way: open the
`.zap` in the GUI once and plain-Save before building, which forces ZAP to re-derive all metadata in
one canonical pass.
