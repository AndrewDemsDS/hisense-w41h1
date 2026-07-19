# SDK edits: the Matter integration (Phases 1–3)

The firmware is the Realtek AmebaZ2 `room_air_conditioner` Matter example with the
Hisense RS-485 driver wired in. The integration lives in the SDK tree
(`~/ameba-dev`), not the app repo, this directory captures the changed files +
documents the in-place edits so the whole thing is reproducible after a fresh
`setup.sh`. Base build: `make room_air_conditioner_port && make is_matter` in
`ameba-rtos-z2/project/realtek_amebaz2_v0_example/GCC-RELEASE`.

## Files here (copies of modified SDK files)

- `matter_drivers.cpp` → `.../examples/room_air_conditioner/matter_drivers.cpp`,
  the glue: uplink (Matter write → `HisenseCommand`/frame), downlink
  (status → attributes), init (RS-485 driver + poll, no DHT/PWM), the mfg-cluster
  handler, the three no-op mfg-cluster server callbacks, and the **#78 HTTPS-OTA
  break-glass** (Identify=88 → `http_update_ota()` → reboot; host/port/path are the
  `HISENSE_OTA_*` macros; serve the build's `firmware_is.bin` for the FWHS serial).
- `chip-ameba-ota-hardening.h` → **appended** to
  `connectedhomeip/src/platform/Ameba/CHIPPlatformConfig.h` by `ota-release.sh build`
  (`apply_ota_hardening()`, idempotent + self-healing; marker `HISENSE_OTA_HARDENING`).
  The **#76 MRP tuning** (RETRANS 4→8, active 300→500, idle 500→800, sender-boost 300)
  so the long BDX OTA survives marginal Wi-Fi. This file is the canonical copy; the SDK
  header is a derived target, never hand-edit it.
- `room-air-conditioner-app.zap` → same path, endpoint config (Phase 2 fan/swing
  /setpoint feature flags + Phase 3 Hisense Aircon cluster on ep1, GUI-authored).
- `hisense-aircon-cluster.xml` → `connectedhomeip/src/app/zap-templates/zcl/data-model/chip/`,
  the `0xFFF1FC00` manufacturer cluster definition.
- `HisenseAircon-ClusterId.h` → `connectedhomeip/zzz_generated/app-common/clusters/HisenseAircon/ClusterId.h`,
  the cluster Id (hand-created; `zap_regen_all.py` would generate the full set).

Also part of the driver (in `firmware/src/rs485-driver/`, copied alongside
matter_drivers into the example dir): `hisense_rs485.{h,cpp}`, `matter_aircon_map.h`.

## In-place edits (diffs, not copyable as whole files)

| File | Change |
|------|--------|
| `common/include/platform_opts_matter.h` | `CONFIG_EXAMPLE_MATTER_CHIPTEST=0`, `CONFIG_EXAMPLE_MATTER_ROOM_AIR_CONDITIONER=1` (selects the example; SDK ships defaulting to chiptest) |
| `.../make/room_air_conditioner/lib_chip_room_air_conditioner_main.mk` | `+ SRC_CPP += .../hisense_rs485.cpp` |
| `core/matter_events.h` | `+ kEventType_Downlink_Aircon_Status` enum value |
| `connectedhomeip/src/app/zap-templates/zcl/zcl.json` | `+ "hisense-aircon-cluster.xml"` in `xmlFile` |
| `connectedhomeip/src/app/zap_cluster_list.json` | `+ "HISENSE_AIRCON_CLUSTER": []` in Server + Client Directories (ember-only) |
| `connectedhomeip/zzz_generated/.../ids/Clusters.h` | `+ #include <clusters/HisenseAircon/ClusterId.h>` |
| `connectedhomeip/zzz_generated/.../callback.h` | `+` decls for `emberAfHisenseAiron{Init,Shutdown}Callback` + `MatterHisenseAirconClusterServerShutdownCallback` |

The three callback **definitions** (no-op) are in `matter_drivers.cpp`.

## Manufacturer cluster notes (`0xFFF1FC00`)

A truly-custom cluster (not one CHIP ships) needs its Id + callback decls in the
SDK's pre-baked `zzz_generated` tables. The clean way is `zap_regen_all.py` (heavy,
regenerates the whole SDK); the targeted edits above are the minimal equivalent and
build cleanly. Attributes are ember-RAM stored; writes reach the uplink handler via
the global attribute-change callback (raw cluster/attr ids `0xFFF1FC00` / `0x0000-3`,
`0x0010-11`), read-back via `emberAfWriteAttribute(...)` since a custom cluster has no
generated `::Set` accessors.

## To rebuild from a fresh SDK

1. Re-apply the in-place edits above (or keep the SDK tree).
2. If the ZAP GUI isn't used, the Hisense Aircon cluster must still be enabled on
   endpoint 1 in the `.zap` via `run_zaptool.sh` (a hand-added cluster block is not
   endpoint-counted by codegen, see docs/01).
3. `make room_air_conditioner_port && make is_matter`.

`ota-release.sh build` re-applies the MRP OTA-hardening append (`apply_ota_hardening`) on every run,
so a fresh/reinstalled SDK self-heals, no manual step for `chip-ameba-ota-hardening.h`.

## Editing the data model

Build traps, the mandatory full-clean, the OTA-serial rule, and the ZAP dep-tracking bug (why a
regenerated `endpoint_config.h` ships stale unless `attribute-storage.cpp` is rebuilt), plus the
`zap_regen_all.py` warning, are canonical in
[`firmware/docs/10-firmware-ota-procedure.md`](../../docs/10-firmware-ota-procedure.md). See there;
don't duplicate them here.

Two details specific to this integration (not repeated in docs/10):
- **Open the `.zap` GUI:** `ZAP_INSTALL_PATH=~/ameba-dev/connectedhomeip/.environment/cipd/packages/zap run_zaptool.sh <app>.zap`
  (toolchain + `activate.sh` must be on PATH first, see docs/10 §4). Re-capture the `.zap` into
  this dir after editing.
- **FeatureMap caveat:** some Thermostat endpoints carry FeatureMap as `EXTERNAL_STORAGE` (fed by an
  app callback, not the compiled default), a `.zap` default bump won't move those. Check which
  endpoint carries the feature.
