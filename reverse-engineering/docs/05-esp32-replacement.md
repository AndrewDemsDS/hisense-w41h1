# ESP32 replacement: command ↔ A/C function mapping

> **SUPERSEDED, byte offsets below are community-derived, not hardware-confirmed for this
> unit.** [`03-rs485-ac-protocol.md`](03-rs485-ac-protocol.md) has the **real, W41H1-bus-
> confirmed** status byte map (offsets 18/19/26/30/31/35/37), pinned by toggling each remote
> control and diffing the live status reply, and it found several of the community offsets
> below (esp. the status byte-35/36 flag bits) **wrong** for this unit. Treat this doc's byte
> tables as a *starting point only*; use §03 as the source of truth for anything already
> covered there. This doc's unique value, the ESP32 build path, physical wiring, and the
> firmware `ac_*` field cross-reference, is still current and kept below.

Goal: an **ESP32 + RS-485 transceiver** that plugs into the A/C's 4-pin port in place of
the W41H1 and drives every function locally. This is the full "what to send / what you get
back" map.

Sources: the frame format is **confirmed from the W41H1 firmware disassembly**
([`03-rs485-ac-protocol.md`](03-rs485-ac-protocol.md)); the payload byte layout and value
tables below are from the tested community implementations (`pslawinski/esphome_airconintl`
`messages.h`/`device_status.h`, `deiger/AirCon`), which speak the identical Hisense A/C
bus, **not yet reconciled against this unit's confirmed status map in §03**. Offsets marked
*(verify)* should be confirmed against your unit with a capture, the framing is identical,
but a specific model can shift a field.

## Physical / link

- **RS-485, 9600 8N1.** ESP32 UART → MAX485 (or reuse a UM3352) → A/C 4-pin port
  (`5V, GND, A, B`). Set the transceiver DE/RE from a GPIO (half-duplex).
- Frame: `F4 F5 [LEN(2, big-endian)] [payload] [CKSUM(2, little-endian)] F4 FB`.

## COMMAND frame (ESP32 → A/C): "set" packet, and STATUS frame (A/C → ESP32): "report" packet

Byte-offset tables for both frame directions **removed here, see
[`03-rs485-ac-protocol.md`](03-rs485-ac-protocol.md#status-frame-byte-map--hardware-confirmed-160b-class-0x66-reply)**
for the hardware-confirmed offsets (status byte map pinned by remote-button correlation on
the real W41H1 bus) plus the general frame format (`F4 F5` … `F4 FB`, length, checksum). The
community tables that used to live here disagreed with §03 in several places (notably the
status byte-35/36 flag bit assignments) and were the wrong source for this unit.

## Value tables (semantic encodings)

Mode, fan, etc. use these values on the bus (from `deiger/AirCon`, matching the firmware):

| Function | Values |
|---|---|
| **Mode** | FAN=0, HEAT=1, COOL=2, DRY=3, AUTO=4 |
| **Fan speed** | AUTO=0, LOWER=5, LOW=6, MEDIUM=7, HIGH=8, HIGHER=9 |
| **Power** | OFF=0, ON=1 |
| **Temperature** | integer °C or °F; unit flag selects (status byte 26 / cmd temp bytes). Raw units may be 0.5° steps, verify. |
| **Vertical swing** | OFF=0, ON=1 |
| **Horizontal swing** | OFF=0, ON=1 |
| **Sleep** | STOP=0, 1, 2, 3, 4 |
| **Eco / power-save** | OFF=0, ON=1 |
| **Quiet / mute** | OFF=0, ON=1 |
| **8°C heat** | OFF=0, ON=1 |

## Firmware `ac_*` field ↔ function

The W41H1 firmware's status fields line up with the above (confirms the unit supports them):

| firmware `ac_*` | function |
|---|---|
| `ac_cool_heat` | fast cool/heat |
| `ac_fan_mute` | quiet / mute (status b36.2) |
| `ac_humidity` | humidity (status 22/23) |
| `ac_8heat` / `ac_enable_8heat` | 8 °C heat |
| `ac_power_save` | eco (status b35.3) |
| `ac_power_display` | panel LED / dimmer |
| `ac_purify` | ionizer / clean (status b36.5) |
| `ac_swing_direction_8` / `ac_swing_follow` | swing (status b35.6/7) |
| `ac_infinite_fan_speed` | stepless fan (status 16) |
| `ac_q_display` / `ac_ai` / `ac_dr` | quick-display / AI / demand-response |

## Recommended build

Don't hand-roll the frame builder, flash **`pslawinski/esphome_airconintl`** (or
`akrabi/hisense_ac_esphome`) which already implements this map, then wire the ESP32 as in
[`../esphome/w41h1-esp32.yaml`](../esphome/w41h1-esp32.yaml). Verify against your unit by
capturing a few real frames ([`../tools/sniff.py`](../tools/sniff.py)) and checking the
mode/temp/fan bytes match the tables above before trusting writes.
