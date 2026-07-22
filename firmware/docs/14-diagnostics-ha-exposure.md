# Exposing all decoded diagnostics in Home Assistant (design)

Goal: surface every decoded diagnostic (compressor Hz, capability flags, link health, per-fault
detail) in Home Assistant, zero cloud. This supersedes the open-ended notes in `docs/01` and the
"telemetry gaps" tracked in issues #38/#39. Verified against the live Pi stack (HA 2026.6.4 +
`python-matter-server` 8.1.0), 2026-07-22.

## The four diagnostics and where they stand today

| diagnostic | decoded at | exposed today | count |
|---|---|---|---|
| compressor Hz | `hisense_rs485.cpp:714` (`buf[42]`) | `sensor.*_compressor_frequency` (HACS, #39), plus the coarse `ThermostatRunningState` -> `hvac_action` | 1 scalar |
| capability flags | `hisense_rs485.cpp:246` (`HisenseFeatures`) | `sensor.*_capabilities` (HACS, #39), plus the `:2323` console | **15** fields |
| per-fault detail | `HisenseFaults` (bytes 39/40/64/66) | `binary_sensor.*_faults` (HACS, #39, one PROBLEM sensor with per-bit attrs), plus the aggregate `BooleanState` on ep10 and `:2323` | **18** named bits |
| link health (#56) | `on_link()` liveness nulling | standard liveness attrs go `unavailable` in HA, mirrored by `binary_sensor.*_bus_link` (connectivity, HACS) | 1 bool |

Note the corrected counts: **18** fault bits (not 19) and **15** capability fields (not 14), per a
direct read of `hisense_rs485.h`. Size any packed bitmap off these.

## The crux: why the "obvious" path is a dead end here

The natural idea, register the mfg cluster `0xFFF1FC00` in `python-matter-server`'s
`custom_clusters.py` and let HA render it, does **not** work on this deployment, confirmed both by
source and by live inspection:

1. **HA no longer uses `python-matter-server` as its client.** HA 2026.6.4's `matter` integration
   pins `matter-python-client==0.7.1` (from the separate `matter-js/matterjs-server` project),
   which ships its own diverged `custom_clusters.py` (7 vendor clusters, none ours). The Pi's
   Matter controller is still the `python-matter-server` 8.1.0 docker container. These are two
   independently maintained files in two upstream projects. Patching the `matter-server`
   container's copy has zero effect on what HA renders.
2. **HA's native rendering needs a hardcoded schema.** `homeassistant/components/matter` creates
   entities only from hardcoded `MatterDiscoverySchema` objects referencing specific typed
   cluster/attribute classes. `discovery.py` has **no** generic "unknown custom cluster ->
   diagnostic entity" fallback. A vendor cluster with no matching schema yields zero entities.
3. So **native** HA entities from `0xFFF1FC00` require two out-of-repo PRs (a cluster class in
   `matter-python-client` **and** a `MatterDiscoverySchema` in `home-assistant/core`). Not
   shippable from this repo. The "free" standard-cluster alternatives were checked against the
   *installed* `matter/sensor.py` and are also dead ends: `ElectricalPowerMeasurement.Frequency`,
   `GeneralDiagnostics.Active*Faults`, and `OperationalError.errorStateLabel` all have **no**
   discovery schema (or map only 4 spec error IDs). Do not re-tread these.

## The clean path (no upstream PR, empirically proven)

matter-server stores **any** device-reported attribute keyed by a plain numeric path,
`"<endpoint>/<cluster_id_dec>/<attr_id>"`, with **no** cluster registration required. Verified
live: nodes 14/35/62 already carry the Hisense cluster's Eco/Turbo/Mute/SleepProfile at
`"<ep>/4294048768/{0,1,2,3}"` (`0xFFF1FC00` = `4294048768`) in the matter-server node store,
despite that cluster never being registered anywhere. `CompressorHz` (16) is now compiled in and
read live (v1.1.4, #38/#82); `OutdoorTemp` (17) stays **absent** from every node's `attributeList`
(declared in the XML, not compiled into the `.zap`).

So the answer: **compile the attributes into the existing cluster, then read them raw from
matter-server's WebSocket API in our own HACS integration.** This bypasses both the Pi-side and
HA-side custom-cluster machinery entirely.

## Recommended architecture (phased)

**Phase 0 (done):** outdoor temp (ep2), coil temp (ep8), aggregate fault (ep10), ActivePower
already ship via standard clusters. Keep as-is.

**Phase 1 (cheapest, ship first, ~zero firmware/GUI):**
- **link health** -> an HA Template Binary Sensor (or one small class in `hisense-unified-ac`)
  watching an existing entity's state `not in (unavailable, unknown)`. The #56 nulling already
  drives the availability transition. No firmware, no GUI, no Pi change.
- **`Thermostat.FeatureMap` cool/heat** -> `Set()` the `kHeating`/`kCooling` bits from
  `cool_heat` in `matter_drivers.cpp`. The FeatureMap is already `included`/RAM-backed in the
  `.zap` (no GUI change), and it is a genuine native `hvac_modes` improvement.

**Phase 2 (done, v1.1.4, #38/#82):** on the **existing** ep1
`HisenseAircon` cluster (`0xFFF1FC00`, already attached):
- tick `included` on the already-declared `CompressorHz` (`0x0010`);
- add two new `bitmap32` attributes: `Features1` (`0x0012`, the 15 capability fields) and
  `Faults1` (`0x0013`, the 18 fault bits);
- mirror in the `HisenseAircon-ClusterId.h` / `zzz_generated` edit; wire the writes in
  `matter_drivers.cpp` at the existing decode sites (features `:246`, compressor Hz `:714`, faults
  `~1602`). Because these attach to an already-attached cluster, the contiguous-endpoint boot-crash
  rule does not newly apply; a standard full rebuild + boot-check on **both** flavours (AmebaZ2 +
  ESP32) still applies. Verify the raw values land in the matter-server node store before writing
  any HA-facing code.

**Phase 3 (done, #39, shipped in `hisense-unified-ac` PR #85):** in
`integrations/hisense-unified-ac`:
- add `matter-python-client` as a `manifest.json` requirement (the same client HA already runs
  against this matter-server, so it is proven interoperable);
- new `config_flow` fields (`matter_ws_url`, `node_id` per device);
- a coordinator: `read_attribute()` once + `subscribe_events(ATTRIBUTE_UPDATED)` thereafter for the
  raw paths (no polling needed, firmware reports on change via the existing wildcard subscription);
- `sensor.py` (Compressor frequency, unit Hz, `state_class=measurement`; Capabilities, a count with
  per-bit attributes) and `binary_sensor.py` (one Faults sensor, `device_class=problem`, per-bit
  detail in attributes; Bus link, `device_class=connectivity`), decoding `Faults1` / `Features1`
  client-side. The bit map in `const.py` stays in lockstep with `hisense_rs485.h`, and a host-side
  test asserts agreement (`firmware/test/test_diag_contract.py`, wired into `run_tests.sh`).

**Phase 4 (optional, only if native `matter`-integration entities are specifically wanted):** file
a `matter-python-client` cluster-class PR + a matching `home-assistant/core` `MatterDiscoverySchema`
PR. Not required; Phases 1-3 already cover all four diagnostics end to end with full fidelity.

## Task split (who does what)

**ZAP GUI (manual):** open `room-air-conditioner-app.zap` (and the ESP32 esp-matter app for
parity) via the `run_zaptool.sh` recipe in CLAUDE.md; on the **existing** ep1 `Hisense Aircon`
cluster, tick `CompressorHz` (`0x0010`) included and add the two `bitmap32` attributes `Features1`
(`0x0012`) + `Faults1` (`0x0013`), matching the storage pattern of Eco/Turbo/Mute/SleepProfile. No
new endpoints. Re-capture into `firmware/src/sdk-edits/` per `sdk-edits/README.md`.

**Firmware:** the `matter_drivers.cpp` write glue for the three new attributes + the Phase-1
FeatureMap `Set()`; build/package/flash both flavours; re-interview both nodes.

**Pi side:** **no** matter-server or HA-core change. Only the standard post-OTA re-interview so
matter-server's descriptor read picks up the new attributes, plus a normal HACS install of the
updated integration into the bind-mounted `custom_components/`. Do not edit any container's
`site-packages` (not bind-mounted, wiped on image update).

## Hard blockers (documented so they are not re-attempted)

- Native `homeassistant/components/matter` rendering of any `0xFFF1FC00` attribute = two upstream
  PRs (`matter-python-client` + `home-assistant/core`). Out of scope for Phases 1-3.
- `ElectricalPowerMeasurement.Frequency`, `GeneralDiagnostics.Active*Faults`, and
  `OperationalError.errorStateLabel` have no discovery schema on the installed HA -> no cheaper
  native route than the mfg-cluster path.
- One-`BooleanState`-endpoint-per-fault-bit would be up to 18 new endpoints (GUI + rebuild +
  reflash + re-commission x18, each forced into a semantically-wrong device class). The single
  `Faults1` bitmap + HACS decode gets full per-bit fidelity for zero new endpoints instead.
