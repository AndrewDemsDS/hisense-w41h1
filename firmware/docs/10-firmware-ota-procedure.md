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

**Command inventory** (token + colon suffix; plain token = the `#78` HTTPS fetch above). All ship
in BOTH flavours, gated only by the token being set at build time:

- `<token>:slots` (>= 1.3.8): report both slots' FWHS serials + the running index (§17)
- `<token>:revert` (>= 1.3.8): boot the other slot if it is strictly older (§17 Path 1)
- `<token>:backup` (>= 1.3.9): stream the inactive (stock) slot's raw image (§17)
- `<token>:wipekv` (>= 1.3.16): factory-reset the Matter KV (formats both Matter DCT regions,
  `0x3E0000`/`0x3ED000`) and reboot. The cure for the "previously cloud-paired stock unit"
  commissioning wedge: see [docs/12 §"SendTrustedRootCert wedge"](12-ota-convert-stock-unit.md).
  Wlan fast-reconnect data is untouched, but the fresh KV has no CHIP network config, so the
  device comes back **uncommissioned in BLE commissioning mode with Wi-Fi down**: plan to
  re-commission over BLE (`chip-tool pairing code-wifi ... --bypass-attestation-verifier 1`
  from a laptop in range), then hand off to HA via `open-commissioning-window`.

  ⚠️ A CH341A clip-copied DCT byte range is **not** equivalent to `:wipekv`. Writing in a
  known-good post-wipe DCT capture via the clip left one unit deterministically wedged at
  `SendTrustedRootCert` (IM `0x0501`) even though every byte matched; running the firmware's own
  `:wipekv` (which formats both DCT regions with the device's own DCT layer) fixed it
  immediately. Prefer this command whenever the device is reachable; fall back to the clip only
  when it is not.

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

## 17. Reverting to stock without opening the case (issue #19)

Two OTA-only paths back to the stock ConnectLife firmware. Path 1 was **proven on hardware
2026-07-21** (office unit, stock sw 2): stock → OTA-convert to 1.3.8 → `revert --flip` →
stock boots and rejoins ConnectLife by itself → re-convert to 1.3.8 (full round trip).
Feature-map evidence: `reverse-engineering/docs/13`.

### Triage first: is it bricked, or did it just leave our fabric?

**Network silence alone is not evidence of a brick.** Two healthy-unit cases look identical to a
brick if you only watch the network:

- **After any stock revert (Path 1 or Path 2), the unit leaves our Matter fabric** and rejoins
  ConnectLife on its own network (its own Wi-Fi profile, its own cloud); it is simply invisible
  to us on our VLAN, whether or not that network is reachable from here.
- **After converting a unit with a prior custom life back to custom**, the surviving Matter DCT
  (`0x3E0000`/`0x3ED000`) leaves `FabricCount() != 0`, so connectedhomeip's `Server.cpp:520-534`
  takes the "already commissioned" branch and explicitly disables BLE advertising; there is no
  window to scan for (see [docs/12 step 7](12-ota-convert-stock-unit.md)).

The actual discriminator is the flash **Quad-Enable (QE) bit**. A bootloader rejection routes
through `boot_load`'s shared failure sink into `hal_flash_return_spi`, which **clears** QE; a
healthy boot leaves it **set** (full mechanism in
[`reverse-engineering/analysis/bootloader.md`](../../reverse-engineering/analysis/bootloader.md)).
Check, in order:

1. **QE bit via the CH341A**, read-only, no soldering: settles it with zero network access.
2. **`<token>:slots`** over break-glass, on any network the device might answer on: any reply at
   all (`ok: fw1_sn=<u> fw2_sn=<u> cur=<idx>`) proves the firmware is alive and both slots readable.
3. **A fresh CH341A dump**, reading the FWHS headers/signatures of both app slots directly, if
   break-glass is also unreachable.

**Path 2 is now hardware-confirmed** (2026-07-26/27, office unit): a repackaged stock image built
with the inner-HMAC fix booted, read back as VID `5004` / PID `13825` / `softwareVersion 2`. It
booted from **FW2**, confirming `boot_load` picks a slot by signature and serial only and does not
care which physical slot holds the image (do not add slot guards to the tooling).

### Path 1: slot-flip (no payload, preferred when it applies)

A stock→custom OTA conversion writes only the inactive slot, so the stock image stays intact
and signature-valid in the other slot until a **second** custom OTA overwrites it. Custom
firmware ≥ 1.3.8 has two break-glass commands (same listener as §13, token + colon suffix):

- `<token>:slots` → `ok: fw1_sn=<u> fw2_sn=<u> cur=<idx>` (FWHS serials of both slots)
- `<token>:revert` → invalidates the **running** image's signature
  (`sys_update_ota_set_boot_fw_idx`) and resets; the bootloader falls back to the other slot

```
ota-release.sh revert --flip <unit-ip> [--force]
```

The script queries `:slots` first and refuses unless the other slot's serial is below
`SERIAL_BASE` (stock carries serial 100; custom serials are `SERIAL_BASE + versionInt`), so a
flip onto an older **custom** image needs `--force`. Guard inside the firmware too: revert
refuses when the other slot is not older. Returning to custom afterwards = re-run the docs/12
conversion (stock's dormant OTA Requestor).

**Why it is safe for the cloud binding:** the regions stock needs stay byte-intact under the
custom firmware (constant-scanned against the deployed image): Wi-Fi profile `0x2FF000`,
cloud config + dkey `0x3DB000`, device identity `0x3DD000`. A reverted unit rejoins
ConnectLife as itself, no re-provisioning. The Matter DCT areas (`0x3E0000`/`0x3ED000`) are
the clobbered ones, and those only cost the (anyway replaced) stock commissioning.

**Caveat, virgin units:** slot 2 in the factory dump is not S2292 but an Aug-2023 MP-test
build (`S1798.MP_TEST_VERSION_SE`, no Matter). The running stock slot (S2292) is what a
conversion preserves, so a first-generation convert flips back to S2292 as intended; just do
not treat "the other slot" as interchangeable before checking `:slots`.

### Path 2: repackage the stock app as a Matter OTA (root-caused 2026-07-25, hardware-confirmed 2026-07-26/27)

**Status: the 2026-07-21 brick is explained, the recipe is fixed, and a repackaged image has
booted on real hardware (office unit: VID `5004` / PID `13825` / `softwareVersion 2` read off the
device, booted from FW2).**

What happened: a repackaged payload (stock backup + serial patch + re-HMAC + re-sum, every check
green, written byte-perfect to flash, verified by a post-mortem clip dump) left the unit dark, with
the GD25Q32 QE bit found CLEARED and cleared again after a manual re-set plus power cycle.

Root cause (issue #75): the bootloader verifies a **second, inner HMAC** that nothing in the old
recipe recomputed:

```
# for sub-image i whose header is at H (the first is at H = 0xE0):
S     = u32le(img[H])         # segment SIZE at H+0x00 -- NOT next_img
END   = H + 0x60 + S
START = 0 if i == 0 else H
img[END : END+0x20]  ==  HMAC-SHA256(partition hash_key, img[START:END])
next header = H + u32le(img[H+4])          # RELATIVE; 0xFFFFFFFF terminates

# sub-image 0 is at H = 0xE0, so its trailer is at u32le(img[0xE0]) + 0x140, and it is the
# only span that reaches the serial at +0xF4. `img[0xE0]` is the SIZE field. Calling it
# `next_img` (as this doc used to) and reading img[0xE4] instead puts the trailer 0x2E0 bytes
# too late and makes every genuine image look corrupt. Detail: reverse-engineering/docs/13.
```

The hashed span starts at image offset 0, so it covers the serial at `+0xF4`. Patching the serial
invalidated it while leaving the manifest signature and byte-sum trailer perfectly valid, which is
why every host-side check passed. `boot_load` compares the trailer, prints `"Hash Result
Incorrect!"`, and falls into its shared failure sink, which clears the flash QE bit and returns -1;
the caller then hangs forever, with **no fall-back to the other slot** (which is why a perfectly
valid custom image in FW2 did not rescue the unit). Symptom matched exactly.

Verified across 29 real images: the relationship holds on every genuine image and fails on exactly
the four `rac-stock-v*-payload.bin` files the old `--repackage` produced. `ota-release.sh` now
recomputes the inner HMAC and **self-checks it on every archived image before building**, so this
class of failure cannot ship silently again. Full analysis:
[`reverse-engineering/analysis/bootloader.md`](../../reverse-engineering/analysis/bootloader.md).

Correction to the old note here: "the SAME stock bytes with the factory signature boot fine on the
same unit" was **not** a controlled comparison. Every observed successful stock boot had effectively
one valid candidate slot (the clip recovery erased FW2; Path 1's flip invalidates it), whereas the
failure had two. That difference is real but is not the cause; the inner HMAC is.

**Confirmed on hardware 2026-07-27** (office unit): a repackaged stock image booted, and the
device reported VID 5004 / PID 13825 / sw 2. Recovery if one ever does fail is still the CH341A
clip. `--repackage` now self-checks every sub-image trailer, and `--apply` re-verifies the
payload before staging, so the #75 class cannot ship silently again.

#### Silence is not a brick (this cost hours, twice, on 2026-07-26/27)

A unit that has gone quiet is almost never bricked. Two expected states look identical from the
network:

- **After a revert to stock**, the unit leaves our Matter fabric and associates to its own
  network. Invisible to us by design.
- **After converting a unit that had a prior custom life**, the Matter DCT survives the round
  trip, so `FabricCount() != 0` and connectedhomeip explicitly DISABLES BLE advertising. No
  first-boot window ever opens, so BLE scanning finds nothing no matter how long you look.

Check in this order, cheapest first:

1. `<token>:slots` over the break-glass listener. An answer proves the unit is alive and tells
   you which slot booted.
2. If you have a clip on anyway, read the flash **QE bit**: `python3 firmware/flasher/ch341_sr.py`
   (SR2 bit 1). A bootloader rejection routes through `boot_load`'s shared failure sink and
   **clears QE**; a healthy boot leaves it **set**. This is the definitive discriminator and it
   was readable the whole time on both occasions.
3. Read the app slots out of a dump and walk the sub-image chain (§17 rule above). A valid chain
   in the booted slot plus QE set means the firmware is fine and the problem is elsewhere.

`boot_load` is **slot-agnostic**: the image base is `0x98000000 + slot_start` from the partition
table and the virtual base is taken verbatim from the section header. Stock has booted fine from
FW2. **Never add a "stock must live in FW1" guard to any revert or convert path** -- it would
refuse a case that is proven to work.

Recovery recipe that worked (clip): write the unit's own dump (per-unit data preserved) with
fw1 replaced by the ORIGINAL stock slot bytes (from a `revert --backup` capture, factory
signature) and fw2 erased to 0xFF; `ch341flash-full.py` re-sets QE at the end. Boots stock,
ConnectLife rejoins.

⚠️ That recipe rewrites the **app slots** (fw1/fw2), not the Matter DCT. If a unit instead needs
its DCT reset (the SendTrustedRootCert wedge, docs/12), do not clip-copy a known-good DCT byte
range in as a substitute: one unit treated that way was left deterministically wedged at
`SendTrustedRootCert` even though every byte matched. Use the firmware's own `<token>:wipekv`
(§13) whenever the device is reachable; it formats the DCT with the device's own layer instead of
foreign bytes copied in from elsewhere.

What still holds from the host work:

For units whose stock slot is already overwritten. Needs a stock dump of any W41H1 (per-unit
data is **not** required: OTA writes only the app slot, and the `0x0` system data, Wi-Fi
profile, dkey and identity live outside it). The stock image's acceptance criteria as decoded
so far (docs/13): bytes `0:32` = `HMAC-SHA256(partition hash_key @ flash 0x140,
image[0xE0:0x140])`, plus a 4-byte byte-sum trailer at EOF. No app-level cryptographic
signature. So:

```
ota-release.sh revert --repackage <stock-dump.bin>   # carve fw1 @0x10000, patch serial @+0xF4,
                                                     # re-HMAC (incl. the #75 inner HMAC), re-sum,
                                                     # wrap as rac-stock-v<N>.ota
ota-release.sh revert --apply --ip <unit-ip>         # confirm, stage on the Pi, update_node,
                                                     # then CLASSIFY the outcome (see below)
ota-release.sh revert --slots <unit-ip>              # read-only slot probe (triage; changes nothing)
```

`--repackage` first re-verifies the recipe byte-exact against every archived
`firmware_is-v*.bin` and the dump's unpatched fw1, and dies loudly on any mismatch. The
revert int is `max(version.txt, .released-version) + 1` and the patched serial follows the
§11 rule (`SERIAL_BASE + int`), so the bootloader accepts the "older" stock payload.
`.released-version` is left alone so the next custom OTA still has to beat the last custom int.

**`--apply` verdicts.** It prints a confirmation first (target `NODE_ID` and where it came
from, the node's live identity read back through matter-server, which slot the image lands in,
and that the unit leaves this fabric), requires you to type `revert node <id>`, and then
classifies the outcome into exactly one of three verdicts:

| verdict | exit | evidence |
|---|---|---|
| `REVERTED` | 0 | the unit reports softwareVersion **4** (vendor 5004) on three sustained fresh reads, or on the 180 s re-check |
| `NOT REVERTED` | 3 | the node answers on a custom softwareVersion, or the break-glass listener answers `:slots`. Only the custom firmware serves that, so the module is alive and the OTA simply never took |
| `AMBIGUOUS` | 4 | silence on every channel we own |

A fabric drop is **no longer** treated as success. It was until 2026-07-27, and that is exactly
how two healthy units got called bricks: stock leaves our fabric AND joins its own
factory-provisioned network, so a healthy reverted unit and a bootloader-rejected module are
both invisible to us. Absence is not evidence. On `AMBIGUOUS` the script prints the triage
list: the ConnectLife app (a reverted unit reappears there by itself), `revert --slots <ip>`
(read-only; an answer proves the custom firmware is still running), then the flash **QE bit**
via `firmware/flasher/ch341_sr.py` (cleared = the bootloader rejected the image and hung, set =
it did not), then the app slots read out of a clip dump. Pass `--ip <unit-ip>` so the
break-glass probe can run at all; without it the best verdict the script can reach is
`AMBIGUOUS`.

**Recommended journey.** Right after the FIRST OTA conversion (docs/12), while the stock
image still sits intact in the inactive slot, fetch a copy of it once and keep the file:

```
ota-release.sh revert --backup <unit-ip>    # needs custom firmware >= 1.3.9 (:backup command)
```

`--backup` streams the inactive slot over the break-glass listener and saves it only after
three checks pass (serial < `SERIAL_BASE`, HMAC, bytesum trailer). After that, any number of
custom OTAs is safe: even once a second custom OTA overwrites the stock slot,
`ota-release.sh revert --repackage <backup>` + `ota-release.sh revert --apply` restores
stock over the air. Mind the **version-consumption rule**: each repackaged revert image
carries serial `SERIAL_BASE + max(version.txt, .released-version) + 1`, burning one fleet
version number, so `--repackage` bumps `version.txt` past the int it just used (commit the
bump). A later custom OTA at or below that int would tie the bootloader and boot stock.

### What does not work (investigated, dead ends)

- **Downloading a public stock image.** The only firmware URL compiled into stock
  (`download.hismarttv.com/Content/WifiDeviceVersionFile/<id>.bin`) serves an older
  module generation (zero 4 KB blocks in common with the W41H1 dump). The device never polls
  for versions; file IDs only exist in the cloud API, harvestable by MITM-ing the phone app.
- **Spoofing the cloud to push HOTA.** Gateway TLS is pinned-CA `VERIFY_REQUIRED` (the
  VERIFY_NONE path is unreachable dead code), and jcmd v5 is AES-256-CBC + HMAC-SHA256 keyed
  by the per-device dkey. 
- **Remote stock-dump capture.** Before 1.3.9 no flash readback path existed in either
  firmware; the CH341A clip dump was the only capture route. 1.3.9 adds the `:backup`
  break-glass command (Path 2 journey above), which reads back the inactive slot over the air.
