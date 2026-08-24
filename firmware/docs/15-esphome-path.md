# ESPHome path: a third firmware over the same codec (design)

Plan for issue #13: an ESPHome firmware for the ESP32 replacement board that exposes the full A/C
control surface over the native Home Assistant API, with no Matter stack, no commissioning, and no
companion HACS integration. It reuses `firmware/src/rs485-driver/` unchanged, so the protocol work
is not re-litigated; only the glue layer above it is new.

The design goal is **simplicity**, measured concretely: fewer moving parts between a user's A/C and
their HA dashboard, and less project-specific machinery to maintain. Parity with the existing
feature set is a hard constraint, not a goal to trade away.

## Scope

- **Target:** ESP32 and ESP32-C3, the same boards and wiring as `firmware/esp32-matter/`
  (see that README for the pin tables and the C3 GPIO warnings, which apply unchanged).
- **Not in scope:** AmebaZ2. ESPHome has no RTL8710C target in this project's plan, so the stock
  module keeps the Matter firmware. This path is a third option beside the two in
  [`13-path-comparison.md`](13-path-comparison.md), not a replacement for either.
- **Not in scope:** Matter. A user who needs Apple Home, Google Home, or Alexa should stay on the
  esp-matter build. ESPHome speaks to Home Assistant, and that is the whole trade.

## Why this is worth building

The RS-485 protocol is the expensive part of this project and it is already done, validated on
hardware, and covered by host golden tests. Everything above it exists only to move decoded values
into Home Assistant. On the Matter path that transport costs, today:

| Machinery on the esp-matter path | Why it exists | ESPHome equivalent |
|---|---|---|
| `main/app_main.cpp` (1961 lines) | endpoint/cluster construction, attribute I/O, echo guards, commissioning, OTA hooks, watchdogs | ~400 line hub component + entity classes |
| `main/diag_console.cpp` (584 lines) + the `:2323` debug flavour | no other way to see decoded state on a deployed node | `logger:` + diagnostic entities, always on, no second flavour |
| `scripts/esp32-release.sh` (441 lines) | delta-OTA base archiving (#82), version int discipline (#77), staged provider push | `esphome run` |
| `scripts/esp32-lint.sh` | keeps `PROJECT_VER` and `sdkconfig` in sync | not applicable |
| `ElectricalPowerMeasurementDelegate.{h,cpp}` | CHIP delegate so EPM reads route correctly | three `sensor:` declarations |
| `integrations/hisense-unified-ac` (separate repo, submodule) + `test_diag_contract.py` | Matter cannot render a manufacturer cluster in HA without two upstream PRs, see [`14-diagnostics-ha-exposure.md`](14-diagnostics-ha-exposure.md) | native entities, no bitmap, no cross-repo contract |
| Test attestation, VID `0xFFF1`/PID `0x8001`, the uncertified-device caveat | Matter requires credentials | none |
| Commissioning, BLE, fabric management, the "77" recommission flow | Matter onboarding | Wi-Fi credentials in the YAML, or the AP fallback |

That is about 3700 lines of ESP32-specific firmware and tooling (`app_main.cpp`, `diag_console.cpp`,
the EPM delegate, and the two scripts), plus a second repository, in service of transport. The
ESPHome path deletes all of it and keeps the 2824 lines of driver and mapping code (plus the HAL)
that encode the reverse-engineering work.

The diagnostics story is the sharpest example. Doc 14 documents that surfacing the 18 fault bits and
15 capability flags natively in HA is blocked behind PRs to two upstream projects, which is why the
firmware packs them into `Features1`/`Faults1` bitmaps that a custom HACS integration unpacks, with
a host test pinning the bit layout across two repositories. In ESPHome, each bit is a
`binary_sensor` with `entity_category: diagnostic`. The bitmaps, the integration, and the contract
test all stop being necessary.

## Non-negotiables

1. **The driver is reused unchanged.** `hisense_rs485.{h,cpp}`, `matter_aircon_map.h`, and
   `power_estimate.h` are shared source, exactly as the esp-matter path reuses them. No fork, no
   "ESPHome flavoured" copy of the codec. Any protocol fix must land once and reach all three
   firmwares.
2. **The HAL is reused unchanged.** `firmware/esp32-matter/components/hisense_hal/` is already a
   plain ESP-IDF component with no Matter dependency, and it carries the DE timing that took a
   multi-day debug to find. ESPHome builds ESP32 targets on ESP-IDF, so it compiles as-is.
3. **ESPHome's `uart:` component is not used.** The HAL owns the port through
   `uart_driver_install()`. Handing the port to ESPHome would mean rewriting the HAL against
   `esphome::uart::UARTDevice` and re-validating DE assert and release timing against a real
   mainboard. The YAML therefore declares pins on the component, not a `uart:` block. This also
   answers the DE question in issue #13: DE is required, it is already handled, and
   `HISENSE_RS485_HW_MODE` in `Kconfig.projbuild` documents the peripheral-driven alternative that
   ESPHome's own RS-485 support uses, still unvalidated against this A/C.
4. **Parity is verified against the inventory below**, not asserted.

## Entity inventory (the parity contract)

Every capability the esp-matter node exposes today, and where it lands in ESPHome. Endpoint numbers
refer to `firmware/esp32-matter/main/app_main.cpp`.

| Today (Matter) | ESPHome | Driver source |
|---|---|---|
| ep1 OnOff | `climate` mode `OFF` vs any other | `hisense_build_power_frame()` |
| ep1 Thermostat SystemMode | `climate` mode auto/cool/heat/dry/fan_only | `HisenseCommand.mode`, `state.mode` |
| ep1 Occupied{Cooling,Heating}Setpoint | `climate` target temperature, 16 to 32, step 1 | `state.setpoint_c` |
| ep1 LocalTemperature | `climate` current temperature | `state.indoor_temp_c` |
| ep1 ThermostatRunningState | `climate` action | `hisense_to_running_state()` |
| ep1 Thermostat FeatureMap gating | YAML: omit `heat` from `supported_modes` | `matter_thermostat_featuremap()` |
| ep1 FanControl FanMode/Percent/Speed | `climate` fan mode: auto plus 6 custom speeds | `hisense_fan_raw_to_*()` |
| ep1 FanControl Rock | `climate` swing mode off/vertical/horizontal/both | `state.vswing_on`, `state.hswing_on` |
| ep1 EPM ActivePower/Voltage/Current | 3 `sensor` (power W, voltage V, current A) | `power_estimate.h` |
| not exposed today | `sensor` energy kWh, `state_class: total_increasing` | `hisense_energy_add/mwh()` |
| ep1 mfg `0x0010` CompressorHz | `sensor` compressor frequency | `state.compressor_freq` |
| ep1 mfg `0x0012` Features1 (15 fields) | 15 diagnostic `binary_sensor` plus a `text_sensor` summary | `HisenseFeatures` |
| ep1 mfg `0x0013` Faults1 (18 bits) | 18 diagnostic `binary_sensor` | `HisenseFaults` |
| ep2 TemperatureMeasurement | `sensor` outdoor temperature | `state.outdoor_temp_c` |
| ep8 TemperatureMeasurement | `sensor` coil temperature | `state.coil_temp_c` |
| ep3 OnOff Eco | `switch` | `HISENSE_FEATURE_ECO` / `ECO_OFF` |
| ep4 OnOff Quiet | `switch` | `hisense_build_mute_frame()` |
| ep5 OnOff Turbo | `switch` | `HISENSE_FEATURE_TURBO` |
| ep6 ModeSelect Sleep profile | `select` with 5 options | `hisense_build_sleep_frame()` |
| ep7 ContactSensor aux heat | `binary_sensor` | `state.heat_relay_on` |
| ep9 OnOff panel display | `switch` | `HisenseDisplay` tri-state |
| ep10 BooleanState aggregate fault | `binary_sensor`, `device_class: problem` | `HisenseFaults.any` |
| Thermostat C/F unit (#5) | diagnostic `switch` or `select` | `state.temp_unit_f` |
| link health nulling (#56) | `binary_sensor`, `device_class: connectivity`, plus NaN on stale sensors | `hisense_set_link_cb()` |
| `:2323` `token` | diagnostic `text_sensor` | `hisense_get_link_token()` |
| `:2323` `busstats`, checksum counter | diagnostic `sensor` | `hisense_checksum_mismatch_count()` |
| `:2323` `bootreason` | diagnostic `text_sensor` | `esp_reset_reason()` |
| `:2323` `poll`, `watch`, `raw` | `logger:` at DEBUG, entity history in HA | driver callbacks |
| `:2323` `decode`, `tx`, `selftest` | optional `api:` user services, debug YAML only | `hisense_build_command_override()` |
| Identify=77 recommission (#69) | `on_recommission:` automation trigger, plus `wifi:` AP fallback | `hisense_set_recommission_cb()` |
| Identify=88 HTTPS-OTA break-glass (#61, #104) | native `ota:` plus `safe_mode` | dropped |

Two entries change character rather than disappearing, and both should be called out in the README
so nobody reads parity as identity:

- **Capability gating (#72, #102)** stops being runtime firmware logic and becomes a YAML decision.
  On the Matter path the firmware hides eco/quiet/display when the `0x66/40` ProductType reply says
  the unit lacks them, because a Matter node's endpoint list is fixed once commissioned. In ESPHome
  a user does not declare the entities their unit does not have, and the capability
  `text_sensor` tells them which those are. Same outcome, no firmware state machine. The component
  should still log a warning when a declared entity contradicts the reported capability.
- **The "77" flow** loses its Matter meaning (there is no fabric to swap). It stays wired as an
  automation trigger so a user can bind it to whatever they want, and the driver still clears the
  request with `hisense_send_exit_77()` so the A/C does not sit in the mode (#69).

## Architecture

```
firmware/esphome/
  README.md                     bring-up, wiring pointer, parity notes
  w41h1.yaml                    reference config (substitutions for board + pins)
  w41h1-debug.yaml              !include of the above plus verbose logger + the debug services
  secrets.yaml.example
  components/hisense_ac/
    __init__.py                 hub schema: pins, poll interval, DE mode; codegen
    climate.py sensor.py binary_sensor.py switch.py select.py text_sensor.py
    hisense_ac.{h,cpp}          hub: driver init, callbacks, publish scheduling
    hisense_climate.{h,cpp}     the climate entity
    (shared driver sources, see Phase 0)
```

Three design points decide whether this stays simple:

**Bus task to loop hand-off.** The driver runs its own FreeRTOS task and fires
`hisense_status_cb_t` from it. ESPHome entity publishes must happen on the main loop task. The hub
keeps one `HisenseState` snapshot plus a dirty flag under a small mutex, the callback writes it, and
`loop()` drains and publishes. No queues, no locks held across a publish. This is also why the
esp-matter path's re-entrant `Set()` callback loop does not exist here: ESPHome's `control()` is
only ever called by Home Assistant, never by our own `publish_state()`, so the entire class of
downlink to readback to uplink feedback bugs that
`docs/guide/Testing-and-QA.md` warns about is structurally absent. Per-field
echo guards are not needed. A short command hold-off still is, so a poll that predates a user
command cannot visibly revert it in HA.

**Mapping stays pure and host-tested.** Do not reimplement the fan ladder or setpoint clamping in
the component. `matter_aircon_map.h` is Matter-named but its fan table, setpoint helpers and running
state derivation are protocol logic, not Matter logic, so reuse them directly. Add
`firmware/src/rs485-driver/esphome_aircon_map.h` for the mode enum mapping only, expressed as plain
integer constants documented as mirroring ESPHome's `ClimateMode`, and `static_assert` those
constants against the real enum inside the component's `.cpp`. That keeps the mapping host-testable
in a new `firmware/test/test_esphome_map.cpp` while making an upstream enum change a compile error
rather than a silent wrong mode. Map Hisense AUTO to `CLIMATE_MODE_HEAT_COOL` so HA renders it as
the A/C's own auto rather than a scheduler.

**Packaging answers issue #13's open question with one source, two access modes.** The component
lives in-tree. The repo's own YAML loads it with `external_components: source: {type: local, path:
components}`; end users load the identical directory with `type: git` plus `path:`, pointing at this
repo. No second repository, no mirror to keep in sync.

## Phases

**Phase 0, how shared sources reach the ESPHome build. RESOLVED (ESPHome 2026.7.4).** The answer is
the option this plan originally listed last: the component's `__init__.py` registers
`firmware/esp32-matter/components/hisense_hal` and `hisense_rs485` as **local ESP-IDF components**
via `add_idf_component(name=..., path=...)`. They are already proper IDF components with the right
include dirs, so they compile in place, unmodified, with zero copies and no sync step.

The symlink and sync-copy options were tried first and both fail for the same reason. Symlinks do
get followed when ESPHome copies component sources into the build tree, so the `.cpp` files compile
fine, but the driver includes its HAL headers with angle brackets (`<platform_stdlib.h>`), and on
the ESP-IDF framework **ESPHome forwards only `-D` and `-W` compiler flags** (see
`framework_helpers.get_project_compile_flags`), so `-I` cannot put the component directory on the
include path. The generated `src` component registers `INCLUDE_DIRS "." "esphome"` and nothing else,
which is why a copied-in header at `src/esphome/components/hisense_ac/platform_stdlib.h` is present
but unreachable. Anyone tempted to retry the symlink route will get a build that compiles the glue
and then fails on the driver's first angle include.

Two consequences worth carrying forward: pin configuration works by `-D` (`PinNames.h` now guards
each `PA_*` define with `#ifndef` so YAML wins and esp-matter keeps its per-target defaults), and
the ESPHome path takes a build-time dependency on the `esp32-matter` directory layout. If the HAL
ever moves to a shared location, both firmwares update together.

**Phases 1 to 3 are implemented and building** (834 KB flash, 45.5% of the app partition, 47.6 KB
RAM, 55% of the partition free, so the ESP32 delta-OTA machinery is unnecessary), and the firmware
has since run on a live A/C. Phase 4 is partly banked, Phase 5 is most of the way through, and the
open items are named under each below.

**Phase 1, hub plus climate. DONE.** Hub component, driver init, the loop hand-off, and the `climate`
entity covering power, mode, setpoint, fan, swing, current temperature and action. Bench-validated
against `firmware/test/virtual_ac.py` over a USB-TTL adapter, which is stage 1 of the staged
bring-up rule in the ESP32 README. Exit criterion: HA drives every field of the climate card against
the simulator, and `test_esphome_map.cpp` passes in `run_tests.sh`.

**Phase 2, the rest of the control surface. DONE.** Eco, Quiet, Turbo, display switches, sleep `select`,
C/F diagnostic. Exit criterion: the inventory table's control rows all check out against the
simulator.

**Phase 3, telemetry and diagnostics. DONE.** Outdoor, coil, compressor, power, voltage, current, energy,
aux heat, the 18 fault bits, the 15 capability flags, bus link, checksum counter, boot reason.
Exit criterion: every row of the inventory table has a live entity, and the capability flags match
what the debug node's `features` console command reports on the esp-matter build.

**Phase 4, hardware validation. IN PROGRESS.** The node has run on a live `CF35LR03G` and been
driven end to end from Home Assistant: every field of the climate card, the eco / turbo / quiet /
display switches, and all four sleep profiles. Two protocol bugs surfaced here rather than on the
bench, and both fixes landed in the shared driver, so all three firmwares carry them:

- Byte 36 rides every COMBINED frame and `0x00` means "display on", not "leave this field alone",
  so driving eco, turbo or quiet re-lit a panel the user had switched off. The hub now carries the
  display state in the command shadow.
- `hisense_build_single_field()` built from a zeroed buffer and never wrote `frame[31] = 0x01`, the
  marker every combined command sets. Both single-field frames were accepted on the wire and
  silently ignored, which is why mute and sleep read as unreachable on all three firmwares.
  Fixed 2026-08-19, story in [`07-stock-parity-gaps.md`](07-stock-parity-gaps.md).

`firmware/test/hil_esphome_actuation.py` drives the node over the native API and checks two
properties per control, actuation and no collateral change, snapshotting and restoring the unit's
state around the run. It needs real hardware, so it stays outside `run_tests.sh`.

Outstanding: stage 3 of the bring-up procedure (powered from the A/C connector's 5 V instead of
USB, and closed up), plus a DI-tap sniffer pass confirming the frames on the wire, which is the
same Layer 5 gate the other two paths pass. Do not skip the ground-loop warning.

**Phase 5, docs and CI. MOSTLY DONE.** Landed: [`firmware/esphome/README.md`](../esphome/README.md),
the ESPHome column in [`13-path-comparison.md`](13-path-comparison.md), the `ESPHome-Build` guide
page for the docs site, and `esphome config` as a hardware-free CI step in `.github/workflows/qa.yaml`
(pinned to esphome 2026.7.4, and checked to fail on a renamed option rather than to merely run).

Outstanding: replace the stale `reverse-engineering/esphome/w41h1-esp32.yaml`, which still points at
the third-party `airconintl` component whose byte map is unvalidated for this unit, with a pointer
to `firmware/esphome/`. An `esphome-vX.Y.Z` tag build stays optional; ESPHome has no delta-OTA base
and no software version gate, so it needs none of `esp32-release.sh`.

## Testing

Layers 1 and 2 of the QA pyramid are unchanged and already cover the codec and the simulator
round-trip, because the driver is the same object under test. The additions are
`test_esphome_map.cpp` in `run_tests.sh` for the new mode mapping, and `esphome config` as a YAML and
schema lint that needs no toolchain. Nothing in the ESPHome path needs the Matter OTA conversion
simulator, the `.zap` contiguity check, or the software version comparison, so those stay scoped to
the paths that need them.

## What is lost

- **Every controller that is not Home Assistant.** Matter is the only path to Apple Home, Google
  Home, and Alexa. This is the entire trade and it should be the first line of the README.
- **The stock module.** ESPHome does not run on the RTL8710C here, so the ESPHome path always means
  replacement hardware and a physically opened unit.
- **Project identity.** The repo's headline is a Matter firmware. The ESPHome path is a lower
  barrier alternative for the ESP32 board, and the docs should frame it that way rather than as the
  recommended default, at least until it has run on a real A/C for a while.
- **Runtime capability gating** becomes a YAML decision, as described above.

## Deliberately not attempted

- LibreTiny as a route to ESPHome on the AmebaZ2 module. It nominally covers the Realtek ambz2
  family, but nothing in this project has tested it, the module's flash budget and the recovery
  story are both tight, and a wrong guess here bricks a unit whose stock image is only recoverable
  with a clip. If anyone wants it, it is a separate spike with its own hardware, not a phase of this
  plan.
- Rewriting the HAL against ESPHome's `uart:` component, for the reasons in non-negotiable 3.
- Any Matter or HACS interoperability for this path. A user picks one transport.
