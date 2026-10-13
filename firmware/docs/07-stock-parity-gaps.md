# 07: What the stock module does that this firmware does not

The one list of features the vendor firmware has and where this firmware stands on each: whether
the value is decoded, whether a command exists, and whether Home Assistant sees it. Use it to
find out if a feature of your unit is reachable, and what is still to be reverse-engineered.

Columns: **DEC** = decoded from the bus, **CTRL** = a command builder exists, **EXP** = exposed
to Home Assistant. Capabilities come from the unit's ProductType reply (`0x66/40`). The bit map
is in `reverse-engineering/docs/10-stock-fw-init-and-comms.md`, section 5a
(`handle_producttype_cmd_result @0x9b6f0c4c`). The entities are described in
[`docs/guide/Entities-and-Diagnostics.md`](../../docs/guide/Entities-and-Diagnostics.md).

## Matrix

| Stock feature | DEC | CTRL | EXP | In the ESPHome firmware |
|---|:--:|:--:|:--:|---|
| Power on/off | ✅ | ✅ | ✅ | `climate` mode off or not off |
| Mode (cool/heat/dry/fan/auto) | ✅ | ✅ | ✅ | `climate` mode |
| Setpoint | ✅ | ✅ | ✅ | `climate` target temperature, 16 to 32 °C |
| Fan (6 discrete speeds) | ✅ | ✅ | ✅ | `climate` fan mode |
| Eco / power-save | ✅ | ✅ | ✅ | `eco` switch and presets |
| Turbo / boost | ✅ | ✅ | ✅ | `turbo` switch and preset |
| Mute / quiet | ✅ | ✅ | ✅ | `quiet` switch and presets. Fixed 2026-08-19 (below) |
| Sleep profile (4) | ✅ | ✅ | ✅ | `sleep_profile` select and presets. Fixed 2026-08-19, all four profiles verified |
| Aux/PTC heat relay | ✅ | – | ✅ | `aux_heat` binary sensor (read-only status) |
| Outdoor + coil temp | ✅ | – | ✅ | two sensors |
| Power (V/I/W) | ✅ | – | ✅ | three sensors, an estimate ([`09-energy-monitoring.md`](09-energy-monitoring.md)) |
| Vertical swing on/off | ✅ | ✅ | ✅ | `climate` swing mode. Horizontal swing is decoded and commanded the same way behind `supports_horizontal_swing`, for a unit that has the motor |
| Display / panel on/off | cap only | ✅ | ✅ | `display` switch. Write-only: the A/C reports no display state. The dimmer *level* (`ac_power_display`) still needs new RE |
| Beeper (buzzer on a command) | – | ✅ | ✅ | `beeper` switch, default on, stored on the node. Off clears `t_beep` (command frame byte 23 bit 2) on every frame the node sends. The A/C reports no beeper state. Heard to silence a unit on an ESPHome node (maintainer's check, 2026-10-10) |
| 8 °C frost-guard heat | ✅ cap | ❌ | ✅ ro | capability bit `ac_8heat` (byte 26 `0x80`) read as `capability_heat_8c`. `ac_enable_8heat` (byte 39 `0x04`) read as `capability_enable_8heat` when the reply is long enough. No control frame is known. `reverse-engineering/docs/05-esp32-replacement.md` marks it likely absent on this unit |
| Purify / ionizer | ✅ cap | ❌ | ✅ ro | `ac_purify` (byte 23 `0x08`) read as `capability_purify`. The live `purify_on` bit (status byte 36 `0x20`) is always 0 on this unit. No builder |
| AI / smart | ✅ cap | ❌ | ✅ ro | `capability_ai` (byte 28 `0x40`). Capability only |
| Demand-response | ✅ cap | ❌ | – | a 2-bit field (byte 35). Decoded, no entity: it is not a boolean |
| Infinite / stepless fan | ✅ cap | ⚠️ | ✅ ro | `capability_infinite_fan` (byte 25 `0x08`). The firmware drives 6 discrete speeds only |
| 8-position louvre aim | ✅ cap | ⚠️ | ✅ ro | `capability_swing_dir_8` and `capability_swing_follow`, and on/off swing is decoded. There is no per-position command |
| Fresh-air / dew | – | – | – | no `ac_*` flag exists: not in the stock feature set, nothing to mirror |

`✅ ro` means the capability flag is readable in Home Assistant as a `capability_*` binary sensor
when the configuration declares it. No control frame is reverse-engineered for any of those rows,
so none can be driven. Several are argued to be physically absent on this unit. Each needs its
control frame found first (issue #52), with the tap procedure in
[`firmware/esp32-recon/HIL-RUNBOOK.md`](../esp32-recon/HIL-RUNBOOK.md).

## Behaviour gaps against the stock module

Differences in how the node talks to the unit, found in the 2026-10 re-read of the stock image
(`reverse-engineering/docs/10-stock-fw-init-and-comms.md`, sections 4.6 and 5b-2):

| Stock behaviour | ESPHome firmware |
|---|---|
| Power and mode in one `0x65` frame (byte 18 = mode bits with `0x0C`) | yes, from 2026-10-10 (#179), with a fallback to two frames |
| A command is checked against the reply | read and logged, not acted on. The reply layout is taken from the vendor firmware and marked `VERIFY`. One stock module logged a reply that agrees with it on 2026-10-10. No reply has been captured byte for byte |
| A command is checked against the next status and sent again | yes (#179) |
| Cloud pairing after the "77" gesture (smart-config, the binding token) | not implemented. The gesture is reported as `on_setup_request`, and the shipped configuration opens the Wi-Fi setup hotspot on it |

The retired Matter builds had none of the first three (issue #168).

## Mute and sleep: one missing byte (2026-08-19, FIXED)

Quiet and sleep did not work on any firmware of this project until 2026-08-19.

The single-field builders build from a zeroed buffer: header, `f[23]=0x04`, and the one named
byte. Every combined command also writes `frame[31] = 0x01`. Every control riding the combined
frame worked. Both single-field frames were accepted on the wire and silently ignored. Adding
that marker to the single-field builder fixed both, with no change at any call site.

Verified on a live `CF35LR03G`: all four sleep profiles select (`sleep_raw` 2/4/6/8, and 0 for
off), the Sleep select entity reads every option back, and mute engages with `fan_raw 0x02`.

What made it hard to see is worth remembering. From `sleep_raw = 0`, "selected a profile" and
"ignored" are the same observation, and byte 17 in the combined frame *does* act, but only to
cancel. Every probe from a zero start therefore looked like silence. Setting a profile on the
A/C's own remote first, then commanding a different one, is what separated the two cases.

## Capability names that were wrong (issue #83, resolved)

Two field-naming bugs, fixed 2026-07-16/18. Both were labels only (not used for control), so
neither changed behaviour:

- `HisenseFeatures.purify`/`.q_display` were misnamed. They became `heat_8c` (byte 26 `0x80` =
  `ac_8heat`) and `purify` (byte 23 `0x08` = `ac_purify`) per the Ghidra decode
  (`reverse-engineering/docs/10-stock-fw-init-and-comms.md`, section 5a). The byte reads were
  always correct.
- `ac_q_display` (byte 39 `0x40`), `ac_enable_8heat` (byte 39 `0x04`) and `ac_trans_102_64`
  (byte 38 `0x08`) were undecoded. They were added as `q_display`, `enable_8heat` and
  `trans_102_64`. They live on payload `[0x19]`/`[0x1A]`, which the unit only sends when the
  reply is long enough (frame length above 39), and are gated by `ext_valid`. So **a `0` with
  `ext_valid == false` means "unknown", not "absent"**.

Verified on hardware on 2026-07-18: the `0x66/40` reply is 45 bytes, so the extended tier is
reachable, and `ac_q_display` reads 1 on this unit, a capability that was invisible while the
field name held `ac_purify`. `ac_enable_8heat` and `ac_trans_102_64` read 0. The base-tier flags
match the 2026-07-16 capture unchanged (`reverse-engineering/docs/11-model-capability-map.md`,
section 5.1).

## History: how the Matter builds exposed this

> **Retired: the Matter firmwares.** This section describes how the two Matter builds carried
> the rows above. They are not in the tree any more. The last tree with them is the tag
> `matter-final`. Current: the entities in the matrix. See
> [`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md).

- The endpoint for each row (ep1 OnOff, Thermostat and FanControl, ep3 to ep6 for the special
  modes, ep7 aux heat, ep9 display, ep11 beeper) is in the parity inventory of
  [`15-esphome-path.md`](15-esphome-path.md#the-parity-inventory).
- Capabilities, faults and the compressor frequency were attributes of a manufacturer cluster
  (`0xFFF1FC00`: `CompressorHz` `0x0010`, `Features1` `0x0012`, `Faults1` `0x0013`, the bus
  counters, `LinkToken` and `BusLink` at `0x0014` to `0x0019` from AmebaZ2 1.3.49 and ESP32
  1.1.19). The `hisense-unified-ac` HACS integration decoded them (#82, #39), from its 1.6.0 for
  the counters.
- The full 16 to 32 °C setpoint range needed `MinSetpointDeadBand` = 0, from AmebaZ2 1.3.49 and
  ESP32 1.1.19. The beeper switch shipped in the same versions and was not listened to on a
  Matter node.
- Vertical swing (FanControl `RockSetting` on ep1) and the display switch (ep9) shipped under #19
  on both builds.
- On AmebaZ2 the sleep select's option list came from an SDK edit that the build applied from
  1.3.50. Some earlier builds shipped it empty.
- The mute and sleep fix above reached both Matter builds through the shared driver
  (`matter_drivers.cpp` and `app_main.cpp` called the same two builders). Neither Matter image
  was rebuilt to confirm it on hardware.
- The 2026-07-18 verification of the extended capability tier was read with the `features`
  command on the ESP32 Matter build's `:2323` console (firmware 1.0.10).
