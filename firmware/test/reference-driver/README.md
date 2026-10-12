# Reference driver (frozen)

This is the project's first RS-485 bus driver for the Hisense indoor unit: codec, bus task, DE
handling and link state machine. It ran on real A/Cs for months inside the two Matter firmwares,
which are now retired (tag `matter-final`, issue #182). Nothing ships from this directory any
more. It is kept for one reason: **it is the hardware-validated reference the ESPHome component's
codec is tested against.**

| File | What |
|---|---|
| `hisense_rs485.{h,cpp}` | the driver: frame builders and parsers, the bus task, the "77" handler. A C API written as C++11, against the Realtek SDK's `serial_*` / `gpio_*` HAL |
| `aircon_map_common.h` | the fan ladder, setpoint clamp and running-state derivation. What is left of the Matter builds' mapping header |
| `esphome_aircon_map.h` | the first ESPHome mapping (modes, fan names, presets, shadow sync), written before the component had its own |
| `power_estimate.h` | the calibrated power estimate (`firmware/docs/09-energy-monitoring.md`) |

## Who uses it

- `firmware/test/run_tests.sh`: `test_codec.cpp` (golden frames), `test_reference_map.cpp`,
  `test_esphome_map.cpp`, and `test_esphome_codec_parity.cpp`, which runs every builder and parser
  of this driver and of the component's port (`firmware/esphome/components/hisense_ac/`) over the
  same inputs and compares them. The HAL is stubbed by `../hal_stub.h` and `../stubinc/`.
- `firmware/esp32-recon/`: the bus diagnostic tool builds this driver for an ESP32 through its own
  HAL shim, so the reference can still be run on a wire.

## Rules

- **Do not develop here.** New behaviour goes into the ESPHome component.
- Change these files only to record a protocol fact that both sides must agree on (a corrected
  byte, a new field), together with the same change in the component, so the parity test keeps
  comparing two implementations of the same protocol. Reference the capture or RE doc.
- Comments in these files mention Matter clusters, endpoints and `matter_drivers.cpp`. They
  describe the builds this driver came from and are left as written. Where they say
  `INTEGRATION.md` they mean this file, and `UART_PINS.md` is now
  `firmware/docs/03-ac-uart-pins.md`.

The protocol source of truth is
[`reverse-engineering/docs/03-rs485-ac-protocol.md`](../../../reverse-engineering/docs/03-rs485-ac-protocol.md).
What follows are the driver-level facts that document does not carry.

## Hisense enums

| `HisenseMode` | value | note |
|---|---|---|
| `HISENSE_MODE_FAN` | 0 | |
| `HISENSE_MODE_HEAT` | 1 | |
| `HISENSE_MODE_COOL` | 2 | |
| `HISENSE_MODE_DRY` | 3 | |
| `HISENSE_MODE_AUTO` | 4 | the command index: byte 18 = `0x90`. The *status* frame reports Auto as 5, and value 4 is skipped there |

Power-off is a power frame with no mode field.

Fan, `HisenseFanSpeed`: `HISENSE_FAN_AUTO`=0, `HISENSE_FAN_QUIET`=1, `HISENSE_FAN_LOW`=5,
`HISENSE_FAN_MED_LOW`=6, `HISENSE_FAN_MID`=7, `HISENSE_FAN_MED_HIGH`=8, `HISENSE_FAN_HIGH`=9. Six
speeds, command byte 16 = index*2+1, all hardware-confirmed.

## Temperature units

- Hisense command frame: whole-degree integer, confirmed range 16-32 (Celsius samples) or 61-90
  (Fahrenheit samples) via `raw = value*2+1`. **No confirmed 0.5-degree-step encoding**: treat as
  whole degrees until a bench capture proves otherwise.
- Hisense status frame: setpoint (byte 19) and room temperature (byte 20) are direct integers in
  whole degrees C, hardware-confirmed. The reference project's `(raw - 32) * 0.5556` formula was
  wrong for this unit and is not used (RE docs/03, status-frame byte map).

## Bus timing

The reference implementation (`aircon_climate.h`) enforces, per TX:
- >=100ms since the last send before dequeuing the next message
- >=10ms of RX silence before sending (don't talk over an in-flight reply)
- a 1500ms idle window after every send before the next
- a 3000ms ACK timeout (treated as a soft failure, message dropped, bus reset
  to idle)

**This is now built into the driver.** `hisense_send_frame()` only ENQUEUES;
the driver's single bus task owns the UART and applies the first three timing
rules above (>=100ms gap, >=10ms RX-quiet, 1500ms post-send idle) before it
puts the next queued frame on the wire. So several callers can call
`hisense_send_frame()` freely and their bytes will never interleave. (The 3s ACK-timeout/soft-fail is not yet modeled;
a dropped reply just means the next poll re-requests status.)

**A command is sent once.** The bus task accepts a reply of any class as the answer to a queued
command frame and does not inspect it. With no reply inside the window it counts the frame in
`hisense_unanswered_command_count()` and moves on: nothing sends it again, so a write the unit
misses is lost (issue #168). The ESPHome component has its own bus
scheduler (`firmware/esphome/components/hisense_ac/hisense_bus.*`) that keeps an unanswered frame
at the head of its queue for up to three sends and then checks the status. This driver was never
given that.

## Bus counters, link state and the beeper

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
frame is byte for byte what the builders produce. The driver does not persist the choice: its
caller stores it and calls `hisense_set_beeper()` before the first command.
