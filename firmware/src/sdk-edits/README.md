# SDK edits: the Matter integration (Phases 1–3)

The firmware is the Realtek AmebaZ2 `room_air_conditioner` Matter example with the Hisense
RS-485 driver wired in. The integration lives in the SDK tree (`~/ameba-dev`), not the app repo:
this directory captures the changed files and documents the in-place edits so the whole thing is
reproducible after a fresh `setup.sh`. Base build: `make room_air_conditioner_port && make
is_matter` in `ameba-rtos-z2/project/realtek_amebaz2_v0_example/GCC-RELEASE`.

## Files here (copies of modified SDK files)

- `matter_drivers.cpp` → `.../examples/room_air_conditioner/matter_drivers.cpp`,
  the glue: uplink (Matter write → `HisenseCommand`/frame), downlink
  (status → attributes), init (RS-485 driver + poll, no DHT/PWM), the mfg-cluster
  handler, the three no-op mfg-cluster server callbacks, and the **#78 HTTPS-OTA
  break-glass** (Identify=88 → `http_update_ota()` → reboot; host/port/path are the
  `HISENSE_OTA_*` macros; serve the build's `firmware_is.bin` for the FWHS serial).
- `chip-ameba-ota-hardening.h` → **appended** to
  `connectedhomeip/src/platform/Ameba/CHIPPlatformConfig.h` by `dev.py ota amebaz2 build`
  (`apply_ota_hardening()`, idempotent + self-healing; marker `HISENSE_OTA_HARDENING`).
  The **#76 MRP tuning** (RETRANS 4→8, active 300→500, idle 500→800, sender-boost 300)
  so the long BDX OTA survives marginal Wi-Fi. This file is the canonical copy; the SDK
  header is a derived target, never hand-edit it.
- `room-air-conditioner-app.zap` → same path, endpoint config (Phase 2 fan/swing
  /setpoint feature flags + Phase 3 Hisense Aircon cluster on ep1). First authored in the ZAP
  GUI; changed since with `firmware/scripts/zap_edit.py` (see "Editing the data model" below).
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
| `common/port/matter_lwip.c` | IPv6 SLAAC on the station netif, after the DHCPv6 start: `netif_set_ip6_autoconfig_enabled(&xnetif[0], 1)` plus a `tcpip_callback` to `nd6_restart_netif` (router solicitation). lwIP creates netifs with autoconfig off and the SDK only runs DHCPv6, so without this the node is link-local only and unreachable from a controller on another VLAN. `scripts/apply-matter-edits.sh` step 5. |
| `project/amebaz2/Makefile.include.matter` | `-DINET_CONFIG_ENABLE_IPV4=1` (the SDK ships `=0`). The same line sets the GN arg `chip_inet_config_enable_ipv4`. With IPv4 off, minimal mDNS listens on IPv6 only. An AP that builds multicast-to-unicast from IGMP membership (UniFi Multicast Enhancement) delivers no IPv6 mDNS, so a restarted controller can't resolve the node. `scripts/apply-matter-edits.sh` step 6. |
| `connectedhomeip/src/app/zap-templates/zcl/zcl.json` | `+ "hisense-aircon-cluster.xml"` in `xmlFile` |
| `connectedhomeip/src/app/zap_cluster_list.json` | `+ "HISENSE_AIRCON_CLUSTER": []` in Server + Client Directories (ember-only) |
| `connectedhomeip/zzz_generated/.../ids/Clusters.h` | `+ #include <clusters/HisenseAircon/ClusterId.h>` |
| `connectedhomeip/zzz_generated/.../callback.h` | `+` decls for `emberAfHisenseAiron{Init,Shutdown}Callback` + `MatterHisenseAirconClusterServerShutdownCallback` |

The three callback **definitions** (no-op) are in `matter_drivers.cpp`.

## Edits `dev.py ota amebaz2 build` applies on every build

These are transforms in `firmware/scripts/dev.py` (`apply_sdk_edits()`), not files or patches.
The table lists the main ones.
Each one is idempotent, reports whether it changed anything, and stops the build if the expected
marker is missing afterwards. They exist because the files are Realtek's and cannot be vendored,
so a fresh or restored SDK silently loses a hand edit. Host tests for the transforms are in
`firmware/test/test_dev_release.py`.

| File | Change |
|------|--------|
| `connectedhomeip/src/platform/Ameba/CHIPPlatformConfig.h` | the MRP tuning block from `chip-ameba-ota-hardening.h` (above) |
| `core/matter_interaction.cpp` | DownlinkTask stack 1024 to 4096 words |
| `examples/room_air_conditioner/example_matter_room_air_conditioner.cpp` | example init task stack 2048 to 8192 words |
| `drivers/matter_drivers/mode_select/ameba_mode_select_manager.cpp` | a null-span guard in the options lookup |
| same file | **the sleep profiles.** Realtek's example table (three coffee options on endpoint 1) becomes Off, General, Old, Young, Kids as modes 0 to 4 on endpoint 6. Added in 1.3.50 |
| `GCC-RELEASE/application.is.matter.mk`, `application.is.mk` | build clock and builder identity pinned, for a reproducible image |

The sleep profile row is the one that bit. It used to be a manual step that
`scripts/apply-matter-edits.sh` only printed a reminder for. The SDK copy on the build machine
went back to the stock table, endpoint 6 had no entry, and every image built from it shipped an
empty `SupportedModes` list on ep6 while `CurrentMode` kept working (#181). The mode value is the
Hisense sleep profile number, and the five labels are the ones the ESP32 build creates in code.
`apply-matter-edits.sh` now only reports the state of that file.

## Manufacturer cluster notes (`0xFFF1FC00`)

A truly-custom cluster (not one CHIP ships) needs its Id + callback decls in the SDK's
pre-baked `zzz_generated` tables. The clean way is `zap_regen_all.py` (heavy, regenerates the
whole SDK); the targeted edits above are the minimal equivalent and build cleanly. Attributes
are ember-RAM stored; writes reach the uplink handler via the global attribute-change callback
(raw cluster/attr ids `0xFFF1FC00` / `0x0000-3`, `0x0010-11`), read-back via
`emberAfWriteAttribute(...)` since a custom cluster has no generated `::Set` accessors.

Attribute ids (the XML is the definition, `HisenseAircon-ClusterId.h` the C++ names, and
`firmware/test/test_zap_edit.py` fails when the XML, the header, `matter_aircon_map.h` and the
`.zap` disagree):

| id | name | type | access | meaning |
|---|---|---|---|---|
| `0x0000` to `0x0003` | Eco, Turbo, Mute, SleepProfile | boolean x3, int8u | read/write | mirrors of the ep3/4/5/6 controls |
| `0x0010` | CompressorHz | int8u | read | compressor frequency |
| `0x0011` | OutdoorTemp | int8s | read | declared, not enabled (ep2 carries it) |
| `0x0012`, `0x0013` | Features1, Faults1 | int32u | read | packed capability and fault bits (docs/14) |
| `0x0014` | ChecksumErrors | int32u | read | 0x66 frames with a bad checksum, since boot |
| `0x0015` | ReplyTimeouts | int32u | read | reply windows that closed empty, since boot |
| `0x0016` | UnansweredCommands | int32u | read | command frames the unit never answered, since boot |
| `0x0017` | LinkLosses | int32u | read | link-lost edges, since boot |
| `0x0018` | LinkToken | int16u | read | A/C device type (high byte) and sub type; 0 until learned |
| `0x0019` | BusLink | boolean | read | true while status polls are answered |

## To rebuild from a fresh SDK

1. Re-apply the in-place edits above (or keep the SDK tree).
2. If the ZAP GUI isn't used, the Hisense Aircon cluster must still be enabled on
   endpoint 1 in the `.zap` via `run_zaptool.sh` (a hand-added cluster block is not
   endpoint-counted by codegen, see docs/01).
3. `make room_air_conditioner_port && make is_matter`.

`dev.py ota amebaz2 build` re-applies the MRP OTA-hardening append (`apply_ota_hardening`) on every run,
so a fresh/reinstalled SDK self-heals, no manual step for `chip-ameba-ota-hardening.h`.

## Editing the data model

Build traps, the mandatory full-clean, the OTA-serial rule, and the ZAP dep-tracking bug (why a
regenerated `endpoint_config.h` ships stale unless `attribute-storage.cpp` is rebuilt), plus the
`zap_regen_all.py` warning, are canonical in
[`firmware/docs/10-firmware-ota-procedure.md`](../../docs/10-firmware-ota-procedure.md). See there;
don't duplicate them here.

### Without the GUI: `firmware/scripts/zap_edit.py`

This is the supported route for adding an attribute, changing a default or storage option, and
appending an endpoint. The `.zap` is JSON in a fixed layout (2-space indent, no trailing
newline), so the tool's output is byte-identical to a GUI save for everything it leaves alone.
Attribute names and types come from the ZCL XML that ZAP loads, never from the command line.

```bash
python3 firmware/scripts/zap_edit.py list
# a standard attribute that was not enabled, with a default:
python3 firmware/scripts/zap_edit.py add-attribute --endpoint 1 --cluster Thermostat \
    --attribute MinSetpointDeadBand --default 0
# an attribute of the manufacturer cluster (add it to hisense-aircon-cluster.xml and
# HisenseAircon-ClusterId.h first):
python3 firmware/scripts/zap_edit.py add-attribute --endpoint 1 --cluster "Hisense Aircon" \
    --attribute ChecksumErrors
# a new endpoint: copy an existing one's type, then adjust the copy
python3 firmware/scripts/zap_edit.py clone-endpoint --from 9
python3 firmware/scripts/zap_edit.py set-attribute --endpoint 11 --cluster On/Off \
    --attribute OnOff --default 1 --storage NVM
# then, with the SDK present, before building:
python3 firmware/scripts/zap_edit.py check
```

`clone-endpoint` only appends (the next free id, its own endpoint type), so endpoints stay
contiguous and no existing endpoint moves. An endpoint also needs its number in
`matter_drivers.cpp`, a UserLabel there, and the same endpoint created in the same position in
`firmware/esp32-matter/main/app_main.cpp`.

`check` copies the `.zap` next to the SDK example and runs the `GENERATE_ZAP` steps of the SDK
Makefile (both `generate.py` passes, `codegen.py`, `zap_cluster_list.py`), the same commands
`dev.py ota amebaz2 build` reaches through `make`. It fails when a step fails, when the
generated endpoint array differs from the `.zap`, when an attribute enabled in the `.zap` is
missing from the generated `.matter` (a hand-added block that codegen ignores), or when ZAP
prints a warning of a kind the committed `.zap` does not already print. `--keep DIR` saves the
generated files and both logs.

The warning test compares kinds, with the endpoint number taken out, because this model already
prints 45 kinds of compliance warning on every generation. The switch endpoints are On/Off
plug-in units without Groups, Scenes Management or the Lighting feature (on purpose: Home
Assistant would grow a "power-on behaviour" entity per switch), and the root endpoint lacks four
items a newer specification made mandatory. A new plug-in endpoint repeats the warnings its
siblings already have and passes. Any other warning fails the check.

The tool does not cover removing or reordering endpoints, or enabling a cluster that is not on
the endpoint yet. Use the GUI for those. Neither route replaces the runtime check: a model can
generate and build cleanly and still break subscriptions (docs/10 §16).

Two details specific to this integration (not repeated in docs/10):
- **Open the `.zap` GUI:** `ZAP_INSTALL_PATH=~/ameba-dev/connectedhomeip/.environment/cipd/packages/zap run_zaptool.sh <app>.zap`
  (toolchain + `activate.sh` must be on PATH first, see docs/10 §4). Re-capture the `.zap` into
  this dir after editing.
- **FeatureMap caveat:** some Thermostat endpoints carry FeatureMap as `EXTERNAL_STORAGE` (fed by an
  app callback, not the compiled default), a `.zap` default bump won't move those. Check which
  endpoint carries the feature.
