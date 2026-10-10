# Wiring `hisense_rs485` into the Matter room-air-conditioner example

> **Status: implemented** (Phases 1-3, builds clean). The actually-applied edits and rebuild
> recipe live in [`../sdk-edits/README.md`](../sdk-edits/README.md); the phase tracker is
> [`../../docs/01-expose-all-clusters.md`](../../docs/01-expose-all-clusters.md).

This replaces the SDK example's stub hardware I/O (DHT11 temp/humidity sensor + GPIO/PWM fan)
with real RS-485 TX/RX to a Hisense indoor unit, in
`component/common/application/matter/drivers/device/room_aircon_driver.cpp` and
`component/common/application/matter/examples/room_air_conditioner/matter_drivers.cpp`.

## 1-2. Uplink/downlink wiring

§1-2 used to carry a pre-implementation design sketch of the Matter attribute-write → RS-485 TX
uplink and the RS-485 status → Matter attribute downlink. The **real, applied edits now live in
the SDK tree**, captured at [`../sdk-edits/README.md`](../sdk-edits/README.md) (exact rebuild
recipe + the actual diffs): read that instead.

## 3. Matter <-> Hisense enum mapping

| Matter `SystemMode` | value | Hisense `HisenseMode` | value |
|---|---|---|---|
| Off | 0 | (power-off frame, no mode field) | -- |
| Cool | 3 | `HISENSE_MODE_COOL` | 2 |
| Heat | 4 | `HISENSE_MODE_HEAT` | 1 |
| FanOnly | 7 | `HISENSE_MODE_FAN` | 0 |
| Dry | 8 | `HISENSE_MODE_DRY` | 3 |
| Auto | 1 | `HISENSE_MODE_AUTO` | 4 (the command index: byte 18 = `0x90`). The *status* frame reports Auto as 5, and value 4 is skipped there |

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
- Hisense command frame: whole-degree integer, confirmed range 16-32 (Celsius samples) or 61-90
  (Fahrenheit samples) via `raw = value*2+1`. **No confirmed 0.5-degree-step encoding**: docs/05's
  "Raw units may be 0.5° steps" note is unconfirmed speculation, treat as whole degrees until a
  bench capture proves otherwise.
- Hisense status frame: setpoint (byte 19) and room temperature (byte 20) are direct integers in
  whole degrees C, hardware-confirmed. The reference project's `(raw - 32) * 0.5556` formula was
  wrong for this unit and is not used (RE docs/03, status-frame byte map).

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
a dropped reply just means the next poll re-requests status.)

**A command is sent once.** The bus task accepts a reply of any class as the answer to a queued
command frame and does not inspect it. With no reply inside the window it counts the frame in
`hisense_unanswered_command_count()` and moves on: nothing sends it again. Both Matter glues build
on this, so a write the unit misses is lost (issue #168). The ESPHome component has its own bus
scheduler (`firmware/esphome/components/hisense_ac/hisense_bus.*`) that keeps an unanswered frame
at the head of its queue for up to three sends and then checks the status. A design for the same
in this driver is written up in pull request #173 and is not implemented.

## 5b. Bus counters, link state and the beeper

Added in AmebaZ2 1.3.49 / ESP32 1.1.19 so the Matter builds report what the ESPHome build does.
All are declared in `hisense_rs485.h`.

| Call | Returns |
|---|---|
| `hisense_checksum_mismatch_count()` | `0x66` frames that passed framing and failed the checksum, since boot |
| `hisense_reply_timeout_count()` | reply windows that closed empty, any frame class |
| `hisense_unanswered_command_count()` | queued command frames among those |
| `hisense_link_loss_count()` | link-lost edges (five silent status polls in a row) |
| `hisense_link_is_up()` | false while the status poll is silent. True from boot until the first loss |
| `hisense_get_link_token(&hi, &lo)` | the A/C's device type and sub type. False until a DevType reply supplied them |

The counters are written by the bus task only. A reader on another task may see a value one
increment old, never a torn one.

`hisense_set_beeper(false)` makes the bus task clear the buzzer bit (frame byte 23 bit 2, the stock
`t_beep`) on every class `0x65` frame it sends from then on, through the pure
`hisense_stamp_beep()`, which also redoes the checksum. The default is on, and with it on every
frame is byte for byte what the builders produce. The driver does not persist the choice: the glue
reads its stored value (the ep11 OnOff attribute) and calls `hisense_set_beeper()` before the first
command. The decision of what to pass, including "a failed read means on", is
`matter_beeper_setting()` in `matter_aircon_map.h`.

## 6. `// VERIFY` checklist

The protocol-provenance log (pins/DE-RE/checksum/byte-stuffing, the 160B status-frame-length fix,
the full status/telemetry/command-frame byte maps, and the open "VERIFY" items) lives in
[`../../../reverse-engineering/docs/03-rs485-ac-protocol.md`](../../../reverse-engineering/docs/03-rs485-ac-protocol.md)
(Physical layer, Status-frame byte map, Diagnostic/telemetry byte map, Control-frame byte map,
and Open / uncatalogued sections), the protocol source of truth. Nothing here is unresolved
beyond what's tracked there.
