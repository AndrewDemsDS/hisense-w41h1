# The community tables: where the payload map started

Before anything had been captured on a W41H1 bus, the project took its first map of the command
and status payloads from two community projects for older Hisense modules. This page keeps what
was taken, says which of it survived, and lists the firmware's `ac_*` field names next to the
functions they turned out to be.

## Status of this page

History. Do not build from the tables on this page. The reference for the bus is
[03-rs485-ac-protocol.md](03-rs485-ac-protocol.md), which was confirmed on a real unit and found
several of the values below wrong for it. The byte-offset tables that once stood here were
removed when doc 03 replaced them.

The sources are [`pslawinski/esphome_airconintl`](https://github.com/pslawinski/esphome_airconintl)
(`messages.h`, `device_status.h`) and [`deiger/AirCon`](https://github.com/deiger/AirCon). Both
target the older AEH-W4A1 module. [`NOTICE.md`](../../NOTICE.md#acknowledgements-earlier-work-on-this-ac-bus)
records what this project took from each and which frames are still their bytes.

To build a replacement board today, use the project's own component:
[`firmware/esphome/README.md`](../../firmware/esphome/README.md) and
[`docs/guide/ESP32-Replacement-Build.md`](../../docs/guide/ESP32-Replacement-Build.md).

## What the community description said, and what holds

| Community description | On the W41H1 |
|---|---|
| RS-485, 9600 8N1, an ESP32 UART through a MAX485 to the 4-pin port, DE and RE from a GPIO | Holds. Wiring is in [`../hardware/pinouts.md`](../hardware/pinouts.md) |
| Frame `F4 F5`, a 2-byte big-endian length, payload, a 2-byte little-endian checksum, `F4 FB` | The tags hold. The length is one byte at offset 4 and the checksum is high byte first (doc 03, frame envelope) |
| Status frame of 82 bytes | 160 bytes |
| Room temperature as `(raw - 32) × 0.5556` | Whole degrees in the panel's unit |
| Fan command values `05`, `07`, `09` | `0B` to `13` in steps of two, auto `01` |
| Eco at status byte 35 bit 3 | Bit 2 |
| Purify at status byte 36 bit 5 | Unsettled: the stock capability table says bit 7 ([doc 03](03-rs485-ac-protocol.md#the-purify-bit-the-documents-and-the-code-disagree)) |
| Power on, power off and the status poll as literal frames | Still sent as published. No capture of them from the owner's bus exists |

## Value tables

From `deiger/AirCon`. These are semantic values, the numbers an application uses, not the bytes
on the wire. Doc 03 has the wire encoding.

| Function | Values | Today |
|---|---|---|
| Mode | FAN=0, HEAT=1, COOL=2, DRY=3, AUTO=4 | In use as the command index (`Mode` in the codec) |
| Fan speed | AUTO=0, LOWER=5, LOW=6, MEDIUM=7, HIGH=8, HIGHER=9 | `NOTICE.md` records that the source's fan table did not match this bus and was replaced. The codec's fan index also runs 5 to 9 for low to high, with 0 for auto and 1 for quiet |
| Power | OFF=0, ON=1 | |
| Temperature | whole °C or °F, a unit flag selects. The source notes that raw units may be half degrees | Whole degrees. The unit flag is status byte 26 bit 1 |
| Vertical swing | OFF=0, ON=1 | |
| Horizontal swing | OFF=0, ON=1 | No motor on the units measured |
| Sleep | STOP=0, 1, 2, 3, 4 | Four profiles: General, Old, Young, Kids |
| Eco | OFF=0, ON=1 | |
| Quiet (mute) | OFF=0, ON=1 | |
| 8 °C heat | OFF=0, ON=1 | Not implemented. See [14-open-questions.md](14-open-questions.md) |

## The firmware's `ac_*` names and their functions

The vendor firmware names fifteen capability fields. This was the first evidence of what a unit
can support. They are the flags of the ProductType reply, and their positions in that reply are in
doc 10 §5a. The status positions in the right column were the community's and are superseded by
doc 03.

| Firmware name | Function | Status position first assumed |
|---|---|---|
| `ac_cool_heat` | heat pump (cooling and heating) | |
| `ac_fan_mute` | quiet | byte 36 bit 2 (holds) |
| `ac_humidity` | humidity | bytes 22 and 23 (never confirmed) |
| `ac_8heat`, `ac_enable_8heat` | 8 °C heat | |
| `ac_power_save` | eco | byte 35 bit 3 (wrong, it is bit 2) |
| `ac_power_display` | panel display and dimmer | |
| `ac_purify` | ioniser | byte 36 bit 5 (unsettled) |
| `ac_swing_direction_8`, `ac_swing_follow` | louvre positions | byte 35 bits 6 and 7 are the plain swing flags |
| `ac_infinite_fan_speed` | stepless fan | byte 16 (the fan byte) |
| `ac_q_display`, `ac_ai`, `ac_dr` | quick display, smart mode, demand response | |

## Other community components

`akrabi/hisense_ac_esphome` and `zoffypal/esphome_hisence_ac` speak the same kind of bus and
were on the first reading list. Their payload maps were never checked against this unit. If you
use one of them on a W41H1, capture real frames first
([`../tools/README.md`](../tools/README.md)) and compare mode, temperature and fan against doc 03
before trusting a write.
