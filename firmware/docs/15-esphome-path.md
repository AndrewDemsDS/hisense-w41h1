# ESPHome path: a third firmware over the same codec (design)

Plan for issue #13: an ESPHome firmware for the ESP32 replacement board that exposes the full A/C
control surface over the native Home Assistant API, with no Matter stack, no commissioning, and no
companion HACS integration. It reuses `firmware/src/rs485-driver/` unchanged, so the protocol work
is not re-litigated; only the glue layer above it is new.

The design goal is **simplicity**, measured concretely: fewer moving parts between a user's A/C and
their HA dashboard, and less project-specific machinery to maintain. Parity with the existing
feature set is a hard constraint, not a goal to trade away.

> **Status, October 2026.** This is the original design. Two of its decisions were later reversed
> so the component can go to upstream ESPHome, which cannot depend on files outside the component.
> The component now carries its own port of the codec (held equal to the shared driver by
> `firmware/test/test_esphome_codec_parity.cpp`) and talks through ESPHome's `uart:` component.
> The route described under Phase 0, registering the shared driver and its HAL as local ESP-IDF
> components, has been removed. The unit's auto mode is also now `CLIMATE_MODE_AUTO`, not the
> `CLIMATE_MODE_HEAT_COOL` this plan chose. [`firmware/esphome/README.md`](../esphome/README.md)
> describes the component as it is today. With no ESP-IDF dependency left, the same component now
> also builds for the stock module's own MCU. That build has run on one module, with the factory
> flash layout, since 2026-10-10:
> [The stock module through LibreTiny](#the-stock-module-through-libretiny).

## Scope

- **Target:** ESP32 and ESP32-C3, the same boards and wiring as `firmware/esp32-matter/`
  (see that README for the pin tables and the C3 GPIO warnings, which apply unchanged).
- **Not in scope (in this plan):** AmebaZ2. ESPHome has no RTL8710C target in this project's plan, so the stock
  module keeps the Matter firmware. A LibreTiny build was added later and has run on one module
  (see the section near the end). This path is a third option beside the two in
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
| `scripts/dev.py ota esp32` release steps | delta-OTA base archiving (#82), version int discipline (#77), staged provider push | `esphome run` |
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
| ep1 FanControl FanMode/Percent/Speed | `climate` fan mode: auto, low, medium_low, medium, medium_high, high (quiet is a preset) | `hisense_fan_raw_to_*()` |
| ep1 FanControl Rock | `climate` swing mode off/vertical/horizontal/both | `state.vswing_on`, `state.hswing_on` |
| ep1 EPM ActivePower/Voltage/Current | 3 `sensor` (power W, voltage V, current A) | `power_estimate.h` |
| not exposed today | `sensor` energy kWh, `state_class: total_increasing` | `hisense_energy_add/mwh()` |
| ep1 mfg `0x0010` CompressorHz | `sensor` compressor frequency | `state.compressor_freq` |
| ep1 mfg `0x0012` Features1 (15 fields) | 15 diagnostic `binary_sensor` plus a `text_sensor` summary | `HisenseFeatures` |
| ep1 mfg `0x0013` Faults1 (18 bits) | 18 diagnostic `binary_sensor` | `HisenseFaults` |
| ep2 TemperatureMeasurement | `sensor` outdoor temperature | `state.outdoor_temp_c`, unknown while the unit sends its placeholder (below) |
| ep8 TemperatureMeasurement | `sensor` coil temperature | `state.coil_temp_c`, same rule |
| ep3 OnOff Eco | `switch`, and the `climate` presets | `HISENSE_FEATURE_ECO` / `ECO_OFF` |
| ep4 OnOff Quiet | `switch`, and the `climate` presets | `hisense_build_mute_frame()` |
| ep5 OnOff Turbo | `switch`, and the `climate` presets | `HISENSE_FEATURE_TURBO` |
| ep6 ModeSelect Sleep profile | `select` with 5 options, and the `climate` presets | `hisense_build_sleep_frame()` |
| wrapper preset (hisense-unified-ac) | `climate` preset, same 13 names | `esphome_preset_plan()` |
| ep7 ContactSensor aux heat | `binary_sensor` | `state.heat_relay_on` |
| ep9 OnOff panel display | `switch` | `HisenseDisplay` tri-state |
| ep10 BooleanState aggregate fault | `binary_sensor`, `device_class: problem` | `HisenseFaults.any` |
| ep11 OnOff beeper (label "Beeper", default on, persisted) | `switch` beeper | `hisense_set_beeper()`, `hisense_stamp_beep()` |
| ep1 mfg `0x0014` to `0x0017` bus counters | 4 diagnostic `sensor` (checksum errors, reply timeouts, unanswered commands, link losses) | `hisense_*_count()` |
| ep1 mfg `0x0018` LinkToken | diagnostic `text_sensor` | `hisense_get_link_token()` |
| ep1 mfg `0x0019` BusLink | `binary_sensor`, `device_class: connectivity` | `hisense_link_is_up()` |
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

Stage 3 of the bring-up procedure (powered from the A/C connector's 5 V instead of USB, and closed
up) is done: the ESPHome units run that way. Outstanding: a DI-tap sniffer pass confirming the
frames on the wire, which is the same Layer 5 gate the other two paths pass. Do not skip the
ground-loop warning.

**Outdoor and coil temperature after a power cut.** Right after mains power returns, the unit
fills both bytes (44 and 45) with `0xEC`, which decodes to -20 C, until it has real readings. Seen
on one unit on 2026-10-10: both sensors at exactly -20, then 35 and 26 C the same afternoon. The
component publishes that pair as unknown. The rule is in `hisense_map.h`
(`outdoor_temps_measured`) and is marked VERIFY, since it rests on one observation. It is narrow
on purpose, because -20 C is also a temperature: both bytes must be `0xEC` in the same frame, and
only until a real pair has been seen since the bus link came up. After that, -20 is published as
a reading. What it still hides is a unit at rest in exactly -20 C weather, from the moment the
node starts until either reading moves. How long the placeholder lasts, and whether it can return
while the unit stays powered, was not measured. The two Matter builds publish the bytes as they
are, so they show -20 C in that window.

**Wi-Fi power saving is off.** `packages/node.yaml` sets `power_save_mode: none`. ESPHome's
default on ESP32 is `light`, where the radio sleeps between beacons. On an ESP32-C3 node with a
weak signal (about -77 dBm) that default gave 65 % ping loss, an API connection that dropped about
once a minute, and commands that did not reach the unit. With `none`: 100 of 100 pings and a
command taken in 0.5 s (2026-10-10). The esp-matter build forces the radio on for the same reason
(`firmware/esp32-matter/main/app_main.cpp`). The setting is a substitution, `wifi_power_save`, for
a board whose supply cannot carry the extra current.

**Phase 6, special modes as climate presets. IMPLEMENTED, awaiting hardware validation.** A
climate group syncs only `climate` attributes, so eco, quiet, turbo and sleep were invisible to it
as switches and a select. The climate entity now offers them as presets with the exact names the
`hisense-unified-ac` wrapper gives the Matter builds (`none`, `eco`, `quiet`, `turbo`,
`eco_quiet`, `sleep_<profile>`, `eco_sleep_<profile>`), so units on either firmware agree.

The decisions are pure functions in `esphome_aircon_map.h`, host-tested in
`test_esphome_map.cpp`, and port the interlocks the wrapper measured on a live A/C:

- which states exist: eco pairs with quiet or with any sleep profile, quiet never with sleep, turbo
  with nothing (`k_esphome_presets`);
- naming a transient state the same way the wrapper does: exact row, else turbo, eco, quiet, sleep
  (`esphome_preset_detect`);
- the write order: a sleep clear first, byte33 and mute clears, then sets, and a sleep profile last,
  re-sent after any byte33 write because eco after sleep drops the profile (`esphome_preset_plan`);
- refusing a fan speed while turbo, quiet or sleep owns the fan (`esphome_fan_request_allowed`).

The A/C swallows a special-mode command that arrives within about 8 s of the previous one, so the
hub owns a small paced queue (`HISENSE_SPECIAL_SETTLE_MS`, 10 s) that presets, the eco / turbo /
quiet switches and the sleep select all feed. The climate entity keeps showing the requested preset
until the queue drains and the command hold-off expires, so a group mirroring it never copies an
intermediate state. Exit criterion: every preset set from Home Assistant reads back as itself on a
live A/C, from each starting preset.

**Phase 7, confirm and retry. IMPLEMENTED, awaiting hardware validation.** Hardware runs on two
units failed about one check in 52, each time with `unanswered_commands` at exactly 1: the unit
missed one frame and nothing sent it again. The stock module verifies the reply to every command
(`reverse-engineering/docs/10-stock-fw-init-and-comms.md`, section 4.6) and packs power and mode
into one frame (section 5b-2). The component now does both:

- the scheduler sends a command frame that got no reply again in the next cycle, in front of the
  rest of the queue, three sends at most and only while the unit answers its status poll;
- the hub keeps the last request until a status frame four seconds later agrees with it, sends it
  again when it does not (twice at most, once for special modes, which also wait out the 10 s
  pacing), then gives up with a warning and a `failed_commands` count;
- a newer command replaces the one being checked, and a link loss drops it;
- a mode request to a powered-down unit is one frame with the power-on bits in byte 18. A unit that
  is still off at the check gets the previous pair of frames on the re-send.

Every decision is a pure function in `hisense_map.h` (`command_resend_allowed`,
`intent_expected_fields`, `confirm_decision`, `pending_begin`, `resend_frames`, `special_plan`),
tested in `test_esphome_confirm.cpp` together with scenarios that run them against a model of a
unit that loses frames and overrides values. The stock reply layout (echo of the first two payload
bytes, then 1) has not been captured on this bus, so the reply verdict is logged at DEBUG and not
acted on. Exit criterion: the hardware test passes on both units with `failed_commands` at 0, and
one captured reply to a command settles the acknowledgement layout.

**Phase 5, docs and CI. MOSTLY DONE.** Landed: [`firmware/esphome/README.md`](../esphome/README.md),
the ESPHome column in [`13-path-comparison.md`](13-path-comparison.md), the `ESPHome-Build` guide
page for the docs site, and `esphome config` as a hardware-free CI step in `.github/workflows/qa.yaml`
(pinned to esphome 2026.7.4, and checked to fail on a renamed option rather than to merely run).

The stale `reverse-engineering/esphome/w41h1-esp32.yaml` is gone, replaced by a README in that
directory pointing at `firmware/esphome/` and recording why the third-party `airconintl` config it
carried is not what to hand a user (its payload byte map is unvalidated for this unit). The RE
README and `05-esp32-replacement.md` now point at the in-repo firmwares too.

Outstanding: an `esphome-vX.Y.Z` tag build, which stays optional. ESPHome has no delta-OTA base and
no software version gate, so it needs none of the `dev.py ota esp32` release steps.

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
- **The stock module.** Until the LibreTiny build below has passed a hardware test, the ESPHome
  path means replacement hardware and a physically opened unit.
- **Project identity.** The repo's headline is a Matter firmware. The ESPHome path is a lower
  barrier alternative for the ESP32 board, and the docs should frame it that way rather than as the
  recommended default, at least until it has run on a real A/C for a while.
- **Runtime capability gating** becomes a YAML decision, as described above.

## The stock module through LibreTiny

Added October 2026. The component stopped depending on ESP-IDF when it gained its own codec and
moved onto ESPHome's `uart:`, so the same YAML and the same C++ now build for the module's own
MCU, the Realtek RTL8710C (AmebaZ2 family), through ESPHome's LibreTiny platform (`rtl87xx:`).
With it, the ESPHome path needs no replacement board.

**Status: the factory layout runs on one module in an A/C, since 2026-10-10. The sdk and native
layouts compile and have not run on hardware.**

### Hardware results, 2026-10-10

One module with the factory layout, in an A/C, converted over the air from the Matter firmware
(the procedure is `docs/guide/Converting-a-Stock-Module-to-ESPHome.md`).
Measured that day:

| What | Result |
|---|---|
| Boot | the image boots under the factory bootloader and joins Wi-Fi |
| Layout guard | `Flash layout` reported `ok`: running slot 2, next write address `0x010000`, the Matter image left intact in slot 1 |
| Delivery | the LibreTiny application image, re-signed with the next FWHS serial and wrapped as a Matter `.ota`, was applied by the Matter firmware's normal OTA path |
| ESPHome to ESPHome updates | two, with `esphome upload` (`firmware.uf2`). The running slot went 2, 1, 2 and the node was back within about 10 s each time |
| Reboot | no hang, with the workaround flags in the image. One mains power cycle also came back clean |
| Bus | status frames decode and commands are answered. The reply verdict logs `Command answered (class 0x65, echo, ack)`, so the echo-plus-ACK layout the component marks VERIFY is seen on this unit |
| **Receiver** | **stopped twice** and stayed stopped until a power cycle. A cause was found and is fixed (next paragraphs) |
| A second module, read with the clip | also the factory layout, with the vendor firmware (serial 100) in both slots and nothing in `0x3D0000..0x3D8000` |
| A second module, converted | link up for about a minute, then its bus went silent at the pin within four minutes and stayed silent (see "The silent unit") |

**The receiver stall.** Twice the bus link dropped and did not come back until the unit was power
cycled: once within the first minute of the first boot after the conversion, and once right after
the first command sent to a unit that had been idle for five minutes. The unit obeyed that command
and later ones, so transmit kept working. Only receive was dead, and the node reported the link
as lost while still controlling the unit. 115 forced preference writes to flash did not reproduce
it, and neither did repeated commands.

**A cause: LibreTiny's receive buffer loses count.** LibreTiny 1.13.0 keeps received bytes in the
Arduino `RingBufferN`. The reader, on the main loop, ends with `_numElems--`. The receive
interrupt ends with `_numElems++`. The decrement is a load, a subtract and a store, and the
interrupt is not masked around it. When the interrupt lands between the load and the store, the
store writes the old count minus one and the byte the interrupt just stored is never counted. It
stays in the buffer. From then on `available()` is one short, every frame is read one byte late
and one byte short, and the frame assembler never completes a frame: reply timeouts with no
checksum error, while transmit works. Nothing puts the count right again except a new buffer.
The same race is reported against the Arduino core API (arduino/ArduinoCore-API issue 195).

Measured on a module, with UART0 in internal loopback so that the real interrupt and the real
buffer are used and nothing reaches the bus:

| Reader | Baud | Bytes | Times the count fell behind |
|---|---|---|---|
| no lock, one read at a random time within 127 us | 115200 | 142558 | 21, the first after 0.8 s. 21 bytes behind at the end |
| interrupts masked around the read (PRIMASK) | 115200 | 142551 | 0 |
| ESPHome `InterruptLock` around the read | 115200 | 142485 | 0 |
| no lock, reads in groups every 2 to 10 ms, as the main loop reads | 9600 | 233034 | 5, the first after 59 s: one a minute |
| ESPHome `InterruptLock`, same reader | 9600 | 233287 | 0 |
| no lock, one read within 127 us of each byte | 9600 | 178420 | 0: every read is over before the next byte |

No byte was lost or reordered in any run: the bytes arrive, the count is wrong. The 9600 baud
run with grouped reads is the bus's own case, with bytes arriving all the time: one loss a
minute. On the bus, bytes arrive for roughly a quarter of each second (a 160-byte status frame
and the shorter replies), which gives a loss every three to four minutes. The unit that showed
the stalls, once the restart below was in place, dropped its link at random intervals with a
median of 169 s, each drop ending 0.4 to 1.5 s later with the restart, with no checksum errors.

**The fix.** On LibreTiny the component reads the port under ESPHome's `InterruptLock`
(`HisenseAC::bus_read`), one buffered byte at a time, a few microseconds each. The other
platforms are not touched: their UART drivers do not use this buffer.

**The recovery, now a fallback.** Closing and reopening the port (`Serial0.end()`,
`Serial0.begin()`) makes a new buffer, which is why it brought the link back. With the receive
interrupt switched off on purpose, the link was reported lost after 5 silent polls and was back
0.6 s after the restart. `packages/amebaz2.yaml` still restarts the port when the hub reports the
link lost and every 15 s while it stays down, as a net for a stall with some other cause. It logs
a warning each time and counts the restarts in a `Serial port restarts` diagnostic sensor. It no
longer fires while the hub holds the transceiver in transmit: closing the port takes the TX pin
away from the UART, and with DE high that puts a break on the pair. The timing rule is a plain
function with a host test (`amebaz2/w41h1_uart_policy.h`,
`firmware/test/test_esphome_uart_policy.cpp`). It lives in the board package because `Serial0` is
LibreTiny's: ESPHome's uart component can reload a port's settings only on ESP32 and ESP8266.
If the counter stays at 0 through a soak on a unit that answers, the restart can go.

**DE timing, measured.** With an API client and Home Assistant connected, over about 150 frames:
the write blocks for up to 12.5 ms (LibreTiny waits for room in the 16-byte transmit FIFO, so it
returns with up to 16 bytes still to send), and DE fell 23.9 to 25.9 ms after the last stop bit.
The 25 ms hold is kept to within 1 ms: the deadline is counted from before the write and
includes the frame's wire time, and the loop runs every 1 to 5 ms while DE is high. The longest
single pass of the main loop in the same period was 53 ms, so a hold of up to about 75 ms is
possible. That would cost one reply, and the link is reported lost only after five in a row. The time the A/C takes to answer could not be
measured (the unit this was timed on did not answer at all).

**The silent unit.** A second module, in another A/C, ran ESPHome with the link up for about a
minute, then lost the link five times within two and a half minutes, the fifth time for good.
The first timeout came 1.1 s after Home Assistant connected. This is not the buffer fault and no
firmware state explains it:

- The receive pin never went low: 0 of 2.3 million samples taken in the 120 ms after our frames,
  0 bytes in the receive FIFO, no framing, break or overrun flag, with the receive interrupt
  enabled throughout.
- Our side looks right: the TX pin toggles during a frame (low in 71 % of samples), the baud
  divisor gives 9604 baud, DE is low when idle (read back as an input)
  and is driven around every frame. 16 bytes left the transmit FIFO in 16.66 ms.
- It made no difference whether DE fell 25 ms or 0.95 ms after the last stop bit.
- About 100 port restarts, eight software resets and one power-on reset did not bring a reply.
  Nor did five minutes without transmitting, twice.

What is left is outside the chip: the transceiver, the wiring, or the A/C not answering. Whether
that unit still obeys commands was not checked. It needs someone at the unit: a command with the
beeper on, a scope or a second transceiver on the pair, and the same module on the unit that
works.

Not measured: the sdk and native layouts, any run longer than an afternoon, an interrupted or
failed update, the return to the Matter firmware, and the image without the reboot workaround.

### What exists

- `firmware/esphome/w41h1-amebaz2-factory.yaml`, `-sdk.yaml` and `-native.yaml`: one board file
  per flash layout a module can have (next section). All three use board `cr3l`, UART0 on PA14
  (TX) and PA13 (RX) and DE on PA17, the module's own wiring, and share
  `packages/amebaz2.yaml`. They include the same entity package as the ESP32 board files and add
  one diagnostic text sensor, `Flash layout`, the serial port recovery described above and its
  `Serial port restarts` sensor.
- Board `cr3l` is a Tuya module with the same chip class. It is used because it is the LibreTiny
  board definition that names PA13 and PA14 as UART0. ESPHome's LibreTiny UART only accepts a
  port's fixed hardware pins, and `generic-rtl8720cm-4mb-1712k` does not define them. Its flash
  layout is not used: each board file replaces it with a file from `firmware/esphome/amebaz2/`.
- The logger stays on LibreTiny's default port, UART2 (TX on PA16), which is the module's log
  console pad.
- CI compiles all three on every push (`esphome-amebaz2` in `.github/workflows/qa.yaml`) and
  fails if the component warns.

### Which flash layout a unit has

There is no default layout, and the choice is not a preference. It has to be the layout already
in the module's flash.

LibreTiny takes the two slot addresses from the build: where an update is written, which image
counts as valid, and which one to invalidate once the update is in. The module's bootloader takes
them from the partition table in flash. If the image was built for other addresses than the table
holds, the first ESPHome to ESPHome update is written inside the wrong region and the running
image is then invalidated. Neither slot boots after that, and the unit needs the clip.

The fleet has two layouts, and LibreTiny brings a third:

| Layout | Board file | FW1 | FW2 | Settings (kvs) | A unit has it when |
|---|---|---|---|---|---|
| factory | `w41h1-amebaz2-factory.yaml` | `0x010000` + `0x170000` | `0x190000` + `0x170000` | `0x3D0000` | it went from the vendor firmware to this project's firmware over the air, so it kept the factory partition table |
| sdk | `w41h1-amebaz2-sdk.yaml` | `0x00C000` + `0x1AC000` | `0x1B8000` + `0x1AC000` | `0x3D0000` | its flash was first written with this repo's clip image (`flash_rac-integrated-*.bin`), which carries the Realtek SDK's partition table |
| native | `w41h1-amebaz2-native.yaml` | `0x010000` + `0x1AC000` | `0x1BC000` + `0x1AC000` | `0x3F8000` | it was written from scratch with LibreTiny's own partition table and bootloader |

LibreTiny's stock layout for `cr3l` is the native row. It is right only for a module that was
given LibreTiny's partition table and bootloader with the clip. On a converted unit it is the
brick described above, and its settings area at `0x3F8000..0x3FFFFF` also lies over data the
Matter firmware and the Realtek SDK keep at the top of flash (the tail of the DCT, UART settings
at `0x3FB000`, the Bluetooth FTL at `0x3FC000`). The factory and sdk layouts put the settings at
`0x3D0000`, above the second slot in both.

To tell which layout a unit has:

- **Its history.** Converted over the air from the vendor firmware and never clipped: factory.
  First written with this repo's clip image: sdk.
- **Ask the Matter firmware** (1.3.9 or later) for the address of the inactive slot:
  `dev.py ota amebaz2 revert --backup <unit-ip>` prints `addr=`. `0x10000` or `0x190000` is the
  factory layout, `0xC000` or `0x1B8000` is the sdk layout. The command only reads.
- **A clip dump.** The second firmware image starts at `0x190000` (factory), `0x1B8000` (sdk) or
  `0x1BC000` (native).

Two guards keep a wrong choice from going unnoticed. Both are in
`firmware/esphome/amebaz2/w41h1_slots.h`:

- At build time, the header checks that the image's slot and settings addresses are exactly those
  of the layout the board file names. A layout file that is missing, misnamed or not applied
  fails the build. So does a build of the package with no layout named.
- At run time, the image asks the Realtek SDK where the bootloader's partition table puts the next
  update (`sys_update_ota_prepare_addr()`) and compares that with its own two slots. The result is
  logged at boot and published as the `Flash layout` sensor, for example
  `factory ok slot=2 next=0x010000 fw1_sn=... fw2_sn=...`. On `MISMATCH` the image also switches
  its own update server off, so the bricking update cannot be started. ESPHome's safe mode does
  not run that check, so a unit in safe mode still accepts an update.

The run-time check has run on one module, with the factory layout, and reported `ok` there. It
has not run on the sdk or native layout, and a mismatch has not been provoked on hardware.

### What was checked without hardware

| Check | Result |
|---|---|
| `esphome config` and `esphome compile`, ESPHome 2026.7.4, LibreTiny 1.13.0, all three layouts | pass. Flash 604,881 bytes: 40.1 % of the factory layout's 1,507,328 byte slot, 34.5 % of the 1,753,088 byte slot of the other two. Static RAM 13,369 of 262,144 bytes |
| Layout applied | each build's slot and settings addresses are those of its layout (compile-time check), and the application image is built for the first slot's address (`0x010000`, `0x00C000`, `0x010000`) |
| Warnings from `components/hisense_ac/` | none, also with every option of every platform declared (`tests/build.rtl87xx-ard.yaml`) |
| UART write blocks (LibreTiny waits on the TX FIFO, about 1 ms per byte) | host test: DE still falls 25 ms after the last byte, the cycle stays at 1 s, no reply is missed |
| Receive buffer | LibreTiny's is 256 bytes and filled from an interrupt. A status frame, the longest the unit sends, is 160 bytes |
| Reboot workaround links | `arch_restart()` in the built image branches to `sys_reset` (disassembly of the ELF) |
| DE pin bring-up order | the RE notes ([`10-stock-fw-init-and-comms.md`](../../reverse-engineering/docs/10-stock-fw-init-and-comms.md)) show `gpio_init(PA_17)` faulting before the scheduler runs. ESPHome calls `setup()` from the main task, after it |

### Open risks

1. **Platform stability.** LibreTiny rates RTL8720C 2 out of 5. Its feature table for 1.13.0 lists
   Wi-Fi, digital I/O, flash I/O and the watchdog as untested on this family and OTA as not
   implemented, although the OTA code is there and users report it working. On this module Wi-Fi,
   the DE pin, flash writes and two updates worked on the day of the test. The serial
   receive buffer loses count without a lock (risk 12).
2. **Reboot hang.** LibreTiny 1.13.0 restarts the chip with a CPU-only reset. On other RTL8720C
   modules that leaves it dark after a restart or an OTA until power has been off for about
   30 seconds (libretiny-eu/libretiny issue 396). The fix, a watchdog system reset, is proposed in
   pull request 397 and is in no release. The board file applies the same change with two linker
   flags (`--wrap=lt_reboot`, `--defsym=__wrap_lt_reboot=sys_reset`), the workaround confirmed
   in that issue on two other modules (a Tuya WBR3 and a Xiaomi one). On this module, with the flags
   in the image, two updates and one mains power cycle came back without a hang. The image has not
   run without them, so whether this module needs them is not known.
3. **No automatic rollback.** The ESP32 bootloader goes back to the previous image when a new one
   fails to boot. Nothing does that here. ESPHome's safe mode covers an image that boots and then
   crash-loops. An image that does not get that far needs the clip.
4. **First flash.** With the factory or sdk layout the module keeps the bootloader and partition
   table it has, and only the application image is new: it goes into a firmware slot. With the
   native layout the partition table, the bootloader and the application are all LibreTiny's, by
   SOIC-8 clip on the GD25Q32 or by UART download mode: UART2 (PA15 RX, PA16 TX), with PA0 held at
   3.3 V through reset and PA13 not pulled low. Whether PA0 and PA15 can be reached on the W41H1
   board is not known. A unit that runs this project's Matter firmware needs neither: the Matter
   firmware installs the application image over the air (`dev.py convert`, the guide linked
   above). That is how the one tested module got it.
5. **Calibration data.** Flash `0x1000..0x3FFF` holds per-module data, including the flash
   controller's calibration table at `0x1040` that the boot code reads. The full-flash image
   LibreTiny builds is blank there apart from two bytes at `0x1028`. Written whole with a clip, it
   erases that data. A clip write has to keep the region from the module's own dump. Whether
   LibreTiny's bootloader needs its two bytes is not known.
6. **Flash pin select.** The RE notes warn that a bootloader which selects the flash D2 line on
   PA17 would drive the DE pin from the flash controller. The stock bootloader does not.
   LibreTiny replaces the bootloader, and what its one selects has not been checked. Scope PA17
   during boot with the module off the A/C bus.
7. **DE while the chip boots.** PA17's level between reset and `setup()` is whatever LibreTiny's
   boot leaves it at. A high DE holds the A/C's bus. Read back as an input on a running module,
   PA17 is low even with the chip's pull-up switched on, so the board pulls DE down while nothing
   drives it. What the boot code does with the pin before `setup()` has not been measured.
8. **Loop blocking.** A command frame holds the main loop for about 50 ms. ESPHome warns above
   50 ms ("took a long time for an operation"), so the log may show that warning once or twice.
   The Wi-Fi stack runs in its own tasks and is not held. ESPHome's own components, the API
   among them, wait for the write. DE is released within 1 ms of its 25 ms hold, timed in the
   firmware ("DE timing, measured" above). A scope has not been on it.
9. **Settings storage.** ESPHome's preferences go to LibreTiny's key-value area, 32 KiB at the
   address the layout gives: `0x3D0000` for factory and sdk, `0x3F8000` for native. `0x3D0000` is
   above the second slot in both layouts. The converted unit booted and ran with the area as the
   Matter firmware had left it, and a second module read with the clip had nothing there under
   the vendor firmware. What the area held on the converted unit, and how FlashDB treated it on
   first boot, was not looked at. The native
   address must not be used on a unit that may go back to the Matter firmware without a full
   clip restore: it overwrites data that firmware keeps at the top of flash.
10. **Going back.** Returning to the stock or the Matter firmware is a full clip write of a dump
    taken before the first flash. Take the dump and verify it first. A return over the air has
    not been tried. After the first ESPHome update the Matter image is gone from the other slot.
11. **The wrong layout.** Covered above. The build-time guard is tested (a wrong or missing
    layout file fails the build). The run-time guard has reported `ok` on one correct unit. It
    has not been seen to report a mismatch on hardware.
12. **The receiver stall.** Described under the hardware results. One cause is found and
    prevented. The restart stays for any other, and a `Serial port restarts` sensor that keeps
    climbing on a unit that answers means the port is failing in a way that is not understood.
13. **A unit that stops answering.** One of two converted modules went silent on the bus within
    four minutes and nothing in the firmware brought it back ("The silent unit" above). Until the
    cause is known, a conversion can end with a unit that has to go back to the Matter firmware
    with the clip.

### What is left to test

The first module went straight into a live unit, so the bench steps were skipped and are still
worth doing on a spare module: restart ten times, update five times, cut power in the middle of
one update, and watch DE on a scope for the settle and drain times and for its level during boot,
against `virtual_ac.py` on the bus pads.

Beyond that: the sdk and native layouts, a soak of days on the converted unit with the restart
counter watched (it should now stay at 0), the cause of the silent unit, the time the A/C takes
to answer (DE fall to first received byte), `hil_esphome_actuation.py` against this build, and a
return to the Matter firmware.

## Deliberately not attempted

- LibreTiny as a route to ESPHome on the AmebaZ2 module was on this list in the original plan. It
  has since been built and has run on one module (the section above). The reasons it was
  left out still describe the risk: nothing in this project has run it, and a wrong guess leaves
  a unit that only a clip recovers.
- Rewriting the HAL against ESPHome's `uart:` component, for the reasons in non-negotiable 3.
- Any Matter or HACS interoperability for this path. A user picks one transport.
