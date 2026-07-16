# Wiring `hisense_rs485` into the Matter room-air-conditioner example

> **Status: implemented** (Phases 1-3, builds clean). The actually-applied edits and rebuild
> recipe live in [`../sdk-edits/README.md`](../sdk-edits/README.md); the phase tracker is
> [`../../docs/01-expose-all-clusters.md`](../../docs/01-expose-all-clusters.md).

This replaces the SDK example's stub hardware I/O (DHT11 temp/humidity sensor + GPIO/PWM fan)
with real RS-485 TX/RX to a Hisense indoor unit, in
`component/common/application/matter/drivers/device/room_aircon_driver.cpp` and
`component/common/application/matter/examples/room_air_conditioner/matter_drivers.cpp`.

## 1-2. Uplink/downlink wiring (superseded design sketch)

§1-2 used to carry a future-tense design sketch (proposed diffs for the Matter
attribute-write → RS-485 TX uplink and the RS-485 status → Matter attribute downlink) written
before the integration was implemented. The **real, applied edits now live in the SDK tree**,
captured at [`../sdk-edits/README.md`](../sdk-edits/README.md) (exact rebuild recipe + the
actual diffs) — read that instead; this file no longer carries a separate sketch of it.

## 3. Matter <-> Hisense enum mapping

| Matter `SystemMode` | value | Hisense `HisenseMode` | value |
|---|---|---|---|
| Off | 0 | (power-off frame, no mode field) | -- |
| Cool | 3 | `HISENSE_MODE_COOL` | 2 |
| Heat | 4 | `HISENSE_MODE_HEAT` | 1 |
| FanOnly | 7 | `HISENSE_MODE_FAN` | 0 |
| Dry | 8 | `HISENSE_MODE_DRY` | 3 |
| (Auto -- not in this SDK example's SystemMode set) | -- | `HISENSE_MODE_AUTO` | **5** (CONFIRMED on hardware; value 4 is skipped) |

Fan: use `FanControl::mapPercentToMode()`'s existing percent thresholds,
translated to `HisenseFanSpeed` (`HISENSE_FAN_AUTO`=0/off,
`HISENSE_FAN_QUIET`=1, `HISENSE_FAN_LOW`=5, `HISENSE_FAN_MED_LOW`=6,
`HISENSE_FAN_MID`=7, `HISENSE_FAN_MED_HIGH`=8, `HISENSE_FAN_HIGH`=9 -- SIX
speeds, command byte16 = index*2+1, all hardware-confirmed) rather than
introducing Matter's own `FanModeEnum`
ordinal directly -- the two enums don't line up 1:1 (Matter has
Off/Low/Medium/High/On/Auto/Smart; Hisense has Auto/Mute/Low/Med/Max).

## 4. Temperature units

- Matter: `int16`, **hundredths of a degree C** everywhere (`LocalTemperature`,
  `OccupiedCoolingSetpoint`, `OccupiedHeatingSetpoint`, `MeasuredValue`).
- Hisense command frame: whole-degree integer, confirmed range 16-32 (Celsius
  samples) or 61-90 (Fahrenheit samples) via `raw = value*2+1`. **No
  confirmed 0.5-degree-step encoding** -- docs/05's "Raw units may be 0.5°
  steps" note is unconfirmed speculation; treat as whole degrees until a
  bench capture proves otherwise.
- Hisense status frame: **VERIFY** -- the reference project decodes both
  `indoor_temperature_setting` and `indoor_temperature_status` via
  `(raw - 32) * 0.5556`, i.e. as if `raw` were a plain Fahrenheit integer,
  which is a DIFFERENT raw encoding than the command side's `2n+1` scheme.
  This driver ports that formula as-is (see `hisense_parse_status()`); do not
  trust the resulting `indoor_temp_c`/`setpoint_c` numbers until a real
  capture confirms which encoding the status frame actually uses.

Always round Matter's hundredths-of-a-degree to the nearest whole degree
before calling `hisense_build_command()` (see the `OccupiedCoolingSetpoint`
snippet above); always multiply Hisense's whole-degree readback by 100
before calling `matter_driver_set_measured_temp_cb()`.

## 5. Bus timing

The reference implementation (`aircon_climate.h`) enforces, per TX:
- >=100ms since the last send before dequeuing the next message
- >=10ms of RX silence before sending (don't talk over an in-flight reply)
- a 1500ms idle window after every send before the next
- a 3000ms ACK timeout (treated as a soft failure, message dropped, bus reset
  to idle)

**This is now built into the driver.** `hisense_send_frame()` only ENQUEUES;
the driver's single bus task owns the UART and applies the first three timing
rules above (>=100ms gap, >=10ms RX-quiet, 1500ms post-send idle) before it
puts the next queued frame on the wire. So callers -- the poll task and the
Matter uplink handler -- can both call `hisense_send_frame()` freely and their
bytes will never interleave. (The 3s ACK-timeout/soft-fail is not yet modeled;
a dropped reply just means the next poll re-requests status. Add it to the bus
task if you want explicit retry.)

## 6. `// VERIFY` checklist

This section used to carry the protocol-provenance log (pins/DE-RE/checksum/byte-stuffing,
the 160B status-frame-length fix, the full status/telemetry/command-frame byte maps, and the
open "VERIFY" items). That log duplicated the protocol source of truth and has been moved
there — see
[`../../../reverse-engineering/docs/03-rs485-ac-protocol.md`](../../../reverse-engineering/docs/03-rs485-ac-protocol.md)
(Physical layer, Status-frame byte map, Diagnostic/telemetry byte map, Control-frame byte map,
and Open / uncatalogued sections). Nothing here is unresolved beyond what's tracked there.
