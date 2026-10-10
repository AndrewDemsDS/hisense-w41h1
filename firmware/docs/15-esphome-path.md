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
> also builds for the stock module's own MCU. That build has not run on hardware:
> [The stock module through LibreTiny](#the-stock-module-through-libretiny-compiles-hardware-test-pending).

## Scope

- **Target:** ESP32 and ESP32-C3, the same boards and wiring as `firmware/esp32-matter/`
  (see that README for the pin tables and the C3 GPIO warnings, which apply unchanged).
- **Not in scope (in this plan):** AmebaZ2. ESPHome has no RTL8710C target in this project's plan, so the stock
  module keeps the Matter firmware. A LibreTiny build was added later and is untested on hardware
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
| ep2 TemperatureMeasurement | `sensor` outdoor temperature | `state.outdoor_temp_c` |
| ep8 TemperatureMeasurement | `sensor` coil temperature | `state.coil_temp_c` |
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

Outstanding: stage 3 of the bring-up procedure (powered from the A/C connector's 5 V instead of
USB, and closed up), plus a DI-tap sniffer pass confirming the frames on the wire, which is the
same Layer 5 gate the other two paths pass. Do not skip the ground-loop warning.

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

## The stock module through LibreTiny (compiles, hardware test pending)

Added October 2026. The component stopped depending on ESP-IDF when it gained its own codec and
moved onto ESPHome's `uart:`, so the same YAML and the same C++ now build for the module's own
MCU, the Realtek RTL8710C (AmebaZ2 family), through ESPHome's LibreTiny platform (`rtl87xx:`).
If it works on hardware, the ESPHome path no longer needs a replacement board.

**Nothing here has run on a module.**

### What exists

- `firmware/esphome/w41h1-amebaz2.yaml`: board `cr3l`, UART0 on PA14 (TX) and PA13 (RX), DE on
  PA17, the module's own wiring. It includes the same two packages as the ESP32 board files, so
  the entities are identical.
- Board `cr3l` is a Tuya module with the same chip class and flash layout (4 MB, two 1712 KiB app
  slots). It is used because it is the LibreTiny board definition that names PA13 and PA14 as
  UART0. ESPHome's LibreTiny UART only accepts a port's fixed hardware pins, and
  `generic-rtl8720cm-4mb-1712k` does not define them.
- The logger stays on LibreTiny's default port, UART2 (TX on PA16), which is the module's log
  console pad.
- CI compiles the file on every push (`esphome-amebaz2` in `.github/workflows/qa.yaml`) and fails
  if the component warns.

### What was checked without hardware

| Check | Result |
|---|---|
| `esphome config` and `esphome compile`, ESPHome 2026.7.4, LibreTiny 1.13.0 | pass. Flash 599,425 of 1,753,088 bytes (34.2 %), static RAM 13,337 of 262,144 bytes |
| Warnings from `components/hisense_ac/` | none, also with every option of every platform declared (`tests/build.rtl87xx-ard.yaml`) |
| UART write blocks (LibreTiny waits on the TX FIFO, about 1 ms per byte) | host test: DE still falls 25 ms after the last byte, the cycle stays at 1 s, no reply is missed |
| Receive buffer | LibreTiny's is 256 bytes and filled from an interrupt. A status frame, the longest the unit sends, is 160 bytes |
| Reboot workaround links | `arch_restart()` in the built image branches to `sys_reset` (disassembly of the ELF) |
| DE pin bring-up order | the RE notes ([`10-stock-fw-init-and-comms.md`](../../reverse-engineering/docs/10-stock-fw-init-and-comms.md)) show `gpio_init(PA_17)` faulting before the scheduler runs. ESPHome calls `setup()` from the main task, after it |

### Open risks

1. **Platform stability.** LibreTiny rates RTL8720C 2 out of 5. Its feature table for 1.13.0 lists
   Wi-Fi, digital I/O, flash I/O and the watchdog as untested on this family and OTA as not
   implemented, although the OTA code is there and users report it working.
2. **Reboot hang.** LibreTiny 1.13.0 restarts the chip with a CPU-only reset. On other RTL8720C
   modules that leaves it dark after a restart or an OTA until power has been off for about
   30 seconds (libretiny-eu/libretiny issue 396). The fix, a watchdog system reset, is proposed in
   pull request 397 and is in no release. The board file applies the same change with two linker
   flags (`--wrap=lt_reboot`, `--defsym=__wrap_lt_reboot=sys_reset`), the workaround confirmed
   in that issue on two other modules (a Tuya WBR3 and a Xiaomi one). On this module it is unproven. A module inside an A/C that needs a
   30 second mains cut after every update is not usable, so this is the first thing to test.
3. **No automatic rollback.** The ESP32 bootloader goes back to the previous image when a new one
   fails to boot. Nothing does that here. ESPHome's safe mode covers an image that boots and then
   crash-loops. An image that does not get that far needs the clip.
4. **First flash.** Either the SOIC-8 clip on the GD25Q32, or UART download mode: UART2 (PA15 RX,
   PA16 TX), with PA0 held at 3.3 V through reset and PA13 not pulled low. Whether PA0 and PA15 can
   be reached on the W41H1 board is not known.
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
   boot leaves it at. A high DE holds the A/C's bus. It has not been measured.
8. **Loop blocking.** A command frame holds the main loop for about 50 ms. ESPHome warns above
   50 ms ("took a long time for an operation"), so the log may show that warning once or twice.
   The Wi-Fi stack runs in its own tasks and is not held. ESPHome's own components, the API
   among them, wait for the write. Whether DE is released on time with the rest of ESPHome
   sharing the loop is a scope measurement.
9. **Settings storage.** ESPHome's preferences go to LibreTiny's key-value area at `0x3F8000`.
   On a module that ran the stock firmware that region holds the vendor's data, and how FlashDB
   treats it on first boot has not been seen.
10. **Going back.** Returning to the stock or the Matter firmware is a full clip write of a dump
    taken before the first flash. Take the dump and verify it first.

### A hardware test, in order

Off the A/C, on the bench supply: flash, confirm it joins Wi-Fi and Home Assistant, press restart
ten times, update over the air five times, cut power in the middle of one update. Then the bench
against `virtual_ac.py` on the bus pads, watching DE on a scope for the settle and drain times and
for its level during boot. Only then a live unit, with `hil_esphome_actuation.py`.

## Deliberately not attempted

- LibreTiny as a route to ESPHome on the AmebaZ2 module was on this list in the original plan. It
  has since been built and is waiting for a hardware test (the section above). The reasons it was
  left out still describe the risk: nothing in this project has run it, and a wrong guess leaves
  a unit that only a clip recovers.
- Rewriting the HAL against ESPHome's `uart:` component, for the reasons in non-negotiable 3.
- Any Matter or HACS interoperability for this path. A user picks one transport.
