# RS-485 A/C protocol (the bus reference)

This page is the reference for the serial bus between the Wi-Fi module and the A/C's indoor-unit
mainboard: the frame envelope, the message classes, the link bring-up, and the byte maps of the
status and command frames. It is the page the firmware's codec
([`hisense_protocol.h`](../../firmware/esphome/components/hisense_ac/hisense_protocol.h),
[`hisense_protocol.cpp`](../../firmware/esphome/components/hisense_ac/hisense_protocol.cpp)) is
checked against. Where the two disagree, the disagreement is written down here.

The disassembly that backs the envelope and the stock module's behaviour is in
[`10-stock-fw-init-and-comms.md`](10-stock-fw-init-and-comms.md). What is still unsettled is
collected in [`14-open-questions.md`](14-open-questions.md).

## How each fact is known

Every table below says where a value comes from. Four sources exist:

| Label | Meaning |
|---|---|
| firmware | Read from the vendor module's firmware by disassembly. The dump is not in the repository. Addresses are in doc 10. |
| bus, with a date | Seen on a real unit of the owner's: a tap on the stock module's transceiver, a remote button pressed while the status frame was logged, or a reply logged by this project's firmware. |
| community | Taken from an earlier project, [`pslawinski/esphome_airconintl`](https://github.com/pslawinski/esphome_airconintl) or [`deiger/AirCon`](https://github.com/deiger/AirCon). [`NOTICE.md`](../../NOTICE.md#acknowledgements-earlier-work-on-this-ac-bus) lists what came from each. |
| inferred | Follows from a rule that holds for other values. Not seen on the wire. |

The units measured are wall-mounted Hisense splits that report device type `01 01`. Offsets are
absolute frame offsets, counted from the first `F4` as byte 0, after the doubled `F4` bytes have
been removed.

## Physical layer

| Item | Value | Source |
|---|---|---|
| Bus | RS-485, half duplex, through a UM3352E transceiver on the module | board ([01-hardware.md](01-hardware.md)) |
| Line settings | 9600 baud, 8 data bits, no parity, 1 stop bit | firmware (`serial_baud(0x2580)`, `serial_format(8, None, 1)`, doc 10 §2.1) |
| Module pins | UART0, TX `PA_14`, RX `PA_13` | firmware (doc 10 §2.1) |
| Direction line | `PA_17` drives the transceiver's DE: high to send, low to receive | firmware (doc 10 §1.3), then hardware 2026-07-06 |
| DE timing of the stock module | DE high, 5 ms, send, 1 ms, DE low, under a mutex | firmware (doc 10 §4.4) |
| Receive | interrupt driven into a buffer, one frame complete on `F4 FB` | firmware (doc 10 §4.3) |
| Reply window | 500 ms per transaction | firmware (doc 10 §4.6) |

The firmware also carries the string `baudrate parameter must be B9600 %d, B115200 %d`. The stock
module probes both rates at start-up, alternating until the A/C answers the device-type request
(doc 10 §4.1). Every unit seen here answers at 9600.

An earlier version of this page said the transceiver switches direction by itself and that no
direction pin exists. That was wrong. It came from a first static pass that missed the `PA_17`
set-up, and it cost several days: bytes reached the transceiver's DI pin and never the bus. The
record of that pass is [`firmware/docs/03-ac-uart-pins.md`](../../firmware/docs/03-ac-uart-pins.md).

What this project's firmware does with these facts (DE held 5 ms before the first byte and 25 ms
after the last, a 1000 ms bus cycle, the same 500 ms window) is in
[`hisense_bus.h`](../../firmware/esphome/components/hisense_ac/hisense_bus.h). The first driver's
timing rules are in
[its README](../../firmware/test/reference-driver/README.md#bus-timing).

## Frame envelope

Source: firmware. The parser is at runtime `0x9b6f2e00` (file `0x22e00`) with the image mapped at
base `0x9b6d0000`, and the builder sites are in doc 10 §3.2.

```
F4 F5 | DIR | CTRL | LEN | 00 00 | T1 T2 | FE 01 | 00 00 | CLASS | payload ... | CK_HI CK_LO | F4 FB
 0  1    2     3      4    5  6    7  8     9 10   11 12    13      14 ...        n-4   n-3    n-2 n-1
```

| Offset | Field | Value |
|---|---|---|
| 0, 1 | start tag | `F4 F5` |
| 2 | direction | `00` module to A/C, `01` A/C to module. The stock parser rejects a received frame whose byte 2 is not `01` |
| 3 | control | `40` on every frame seen. Bits 6 and 7 select the checksum width (`00` 1 byte, `40` 2, `80` 2, `C0` 4). Bit 5 would make LEN a 16-bit big-endian value. Bit 0 would put one extra byte in front of LEN |
| 4 | LEN | the whole frame is LEN + 9 bytes long |
| 5, 6 | reserved | `00 00` |
| 7, 8 | device type and sub-type | `00 00` in the first frame of a session, then the pair the A/C reported (see [Link bring-up](#link-bring-up-and-heartbeat)) |
| 9, 10 | marker | `FE 01` |
| 11, 12 | pad | `00 00` |
| 13 | class | see [Message classes](#message-classes) |
| 14 onwards | payload | class specific. Payload index N of doc 10 is frame byte 13 + N |
| n-4, n-3 | checksum | 16-bit sum, high byte first |
| n-2, n-1 | end tag | `F4 FB` |

This is the layout of a frame from the module. A frame from the A/C has the same length rule,
class byte, checksum and end tag, and its bytes 5 to 12 are laid out differently: the status reply
opens `F4 F5 01 40 97 01 00 FE 01 01 01 01 00 66`, with `FE` at byte 7. Use the absolute offsets
of the byte maps below for A/C frames and do not derive them from the table above.

The checksum is the sum of bytes 2 to n-5 inclusive (direction through the last payload byte),
truncated to 16 bits. It is a plain sum, not an XOR and not a CRC (firmware: the Thumb loop
`ldrb r2,[r3,#1]!; add r1,r2`, width helper at `0x9b6f0958`).

Frame lengths seen, all LEN + 9 (bus):

| Frame | LEN | Length |
|---|---|---|
| link bring-up requests `0x0A`, `0x07`, ProductType request | `0B` | 20 |
| status poll | `0C` | 21 |
| device-type reply | `0D` | 22 |
| `0x1E` heartbeat and its reply | `13` | 28 |
| ProductType reply | `24` | 45 |
| command `0x65` | `29` | 50 |
| status reply | `97` | 160 |

The first status decoder here assumed the 82-byte status frame of the community project and
dropped every frame. The W41H1's status frame is 160 bytes.

### Parser facts

From the stock parser's disassembly:

- A frame of length 3 or less is rejected.
- Bytes 0 and 1 have to be `F4 F5`, otherwise the log says "invalid frame".
- The end tag is read at the position LEN gives. If it is not `F4 FB` the log says "no end tag".
- A checksum mismatch logs "crc error" and the frame is dropped before any handler sees its
  payload.
- A received frame has to carry `01` at byte 2, otherwise "link resp error".

### Byte stuffing

A `F4` inside the frame, between the direction byte and the checksum, is sent twice. The end tag
is the only `F4` followed by `FB` (firmware, stuffing helper at `0x9b6f7854`). The first check of
this rule here was a sample frame of the community project: its 16 °C frame has the checksum
`01 F4`, which is on the wire as `01 F4 F4 F4 FB`. The component's builders double a `F4`
checksum byte, and its receiver removes any doubled `F4`.

## Message classes

Source: firmware for the builders and handlers (doc 10 §3.3), bus where noted.

| Class | Name | Direction | Purpose |
|---|---|---|---|
| `0A` | device type | request and reply | first frame of a session. The A/C reports what it is |
| `07` | version | request and reply | the mainboard's protocol version. The stock module gates some bytes on it (doc 10 §6.6) |
| `1E` | link heartbeat | request about once a second, and reply | the module reports its Wi-Fi state. The A/C's reply carries request bits ([the "77" request](#recommission-77-the-request-bits-in-the-0x1e-reply)) |
| `66` sub `00` | status | poll and 160-byte reply | the unit's state |
| `66` sub `40` | ProductType | request and 45-byte reply | capability flags ([11-model-capability-map.md](11-model-capability-map.md)) |
| `65` | command | module to A/C | set power, mode, setpoint, fan and the rest |
| `1D` | reconfigure, OTA | A/C to module | the A/C can answer the device-type request with it. Not seen on a unit here |
| `67` | device ID | request and 60-byte reply | payload `67 00 00`. Sent by the stock module once after each ProductType exchange; the reply carries a 23-character device id ([below](#the-device-id-exchange-class-67)) |

The stock firmware's error strings name three reply kinds, `link resp`, `net resp` and
`trans resp`. They are nested checks on the reply's sub-header, not three classes.

On `67`: an earlier statement in doc 10 that this class never appears on the bus was wrong and is
corrected there (doc 10 §3.3). The component does not send it, and the units run normally without
it. What a real unit answers has not been captured; the layout the stock module expects is below.

### The device-ID exchange (class 67)

Source: the stock image of 2026-08-19, read with rizin (`dumps/issue-110-class-0x67-analysis.md`,
local only). The module's init routine `uart_ctl_process_main` sends `67 00 00` right after the
ProductType query, with a 3 s window and one retry, and ignores the result. The reply handler,
`handle_deviceID_cmd_result` (named by its own log string), accepts a reply only when:

| Frame byte | Content |
|---|---|
| whole frame | 60 bytes, LEN `33` (inner payload of 43 bytes from byte 13) |
| 13 | class `67` |
| 16, 17 | `10 24` |
| 31 to 53 | the device id, 23 ASCII bytes. The module folds `A` to `Z` to lowercase |
| 54, 55 | 16-bit sum of bytes 16 to 53, high byte first (a second checksum, inside the payload) |
| 56, 57 | the frame checksum |

Anything else is logged ("invalid device id length or code format", "device id checksum
mismatch") and dropped. The id goes to the module's cloud identity (`g_device_id`,
`set_device_id_to_flash`). `firmware/test/virtual_ac.py` answers the query in this layout
(`--device-id`), so a stock module on the bench sees the same exchange as on a unit. Bytes 5 to 12
of that reply copy the status reply; a real unit's reply has not been seen and may differ there.

The module is the only initiator. The A/C sends nothing unasked.

## Link bring-up and heartbeat

The A/C ignores the status poll until the module has brought the link up: a device-type request,
a version request, then a `0x1E` heartbeat about once a second.

The three request frames were captured from the stock module's transmit line (the transceiver's
DI pin) on 2026-07-06. The heartbeat was byte-identical in 22 frames in a row, so it has no
counter.

```
0A  F4 F5 00 40 0B 00 00 00 00 FE 01 00 00 0A 04 00 01 58 F4 FB
07  F4 F5 00 40 0B 00 00 01 01 FE 01 00 00 07 01 00 01 54 F4 FB
1E  F4 F5 00 40 13 00 00 01 01 FE 01 00 00 1E 00 00 B0 80 20 00 00 00 40 00 03 02 F4 FB
```

### The device-type exchange

The request is the `0A` frame above: bytes 7 and 8 are `00 00` because nothing is known yet.

A real reply, captured on 2026-10-12 from a unit driven by this project's firmware on an ESP32-C3
board:

```
F4 F5 01 40 0D 01 00 FE 01 00 00 01 00 0A 04 01 01 01 01 60 F4 FB
```

| Offset | Bytes | Meaning |
|---|---|---|
| 2 | `01` | from the A/C |
| 4 | `0D` | LEN 13, so 22 bytes in all |
| 9, 10 | `00 00` | not the device type. See below |
| 13 to 15 | `0A 04 01` | class, sub-type, and `01` for "reply" |
| 16, 17 | `01 01` | device type and sub-type |
| 18, 19 | `01 60` | checksum: the bytes 2 to 17 add up to `0x0160` |
| 20, 21 | `F4 FB` | end tag |

So this reply follows the same length rule and the same checksum rule as every other frame. Until
this capture the repository held no real device-type reply, and its length and checksum were
derived from the stock parser. The same capture holds a `0x1E` reply of 28 bytes with the checksum
`02 BD` and a status frame of 160 bytes with the checksum `05 4E`. Both verify. Their full bytes
are not reproduced here.

The stock handler checks payload `0A 04 01`, then stores the device type (frame byte 16) and the
sub-type (frame byte 17). Every later frame from the module carries that pair in bytes 7 and 8.
That is why the captured `07` and `1E` frames above carry `01 01` there.

The pair has to come from bytes 16 and 17 of the reply. Bytes 9 and 10 of the same reply are
`00 00`. A build that stamped those instead (2026-07-16) had every frame rejected by the A/C and
never got a status frame. Status replies do carry `01 01` at bytes 9 and 10, which is why an
earlier look at them suggested the two were the same. The full account is in doc 10 §4.5.

Device types the stock firmware knows: `00` and `01` (air conditioner), `15` (dehumidifier), `36`
and `37` (A/C variants). See [11-model-capability-map.md](11-model-capability-map.md).

## What the firmware does on receive

This section describes the component, not the vendor module. The code is `FrameAssembler` and the
parsers in [`hisense_protocol.cpp`](../../firmware/esphome/components/hisense_ac/hisense_protocol.cpp),
and `BusScheduler::frame_received_` in
[`hisense_bus.cpp`](../../firmware/esphome/components/hisense_ac/hisense_bus.cpp).

- The assembler accepts a frame only when byte 1 is `F5`, byte 2 is `01` (which also drops the
  echo of the node's own frames), LEN fits the 200-byte buffer, and the frame ends `F4 FB` at
  LEN + 9. It removes doubled `F4` bytes.
- A byte that breaks a frame is looked at again as a possible start, and a later `F4 F5` inside a
  frame that turns out to be cut short is followed as a second candidate. Noise in front of a
  reply does not cost the reply.
- The checksum is judged once per frame, in the scheduler. A frame that fails is counted in the
  "Checksum errors" sensor, logged at WARN, and not parsed.
- A device-type reply is adopted only when its checksum is good and the pair is not `00 00`. A
  damaged reply is a failed try and the request goes out again 500 ms later. It does not count as
  having heard the unit.
- A parser reads a payload byte only when that byte lies in front of the 4-byte trailer (checksum
  and end tag). The minimum lengths that follow are 49 bytes for a status frame, 40 for a
  ProductType reply (44 for its extended tier) and 22 for a device-type reply. In a shorter frame
  the same offsets hold checksum bytes or the end tag.
- Only a status frame with a good checksum keeps the link alive. Five status polls in a row
  without one and the link is declared lost.
- The request bits of a `0x1E` reply are read only from a frame with a good checksum.

## Status frame: byte map (class `66` sub `00`, 160 bytes)

Source: bus. On 2026-07-07 and the days after, each control was toggled on the unit's remote
while the 160-byte status reply was logged, and the bytes that changed were noted. The first
offsets came from the community project. Several were wrong for this unit and are corrected here.
The parser is `parse_status()` in the codec.

| Byte | Field | Encoding | Notes |
|---|---|---|---|
| 16 | fan speed | `01` auto, `02` quiet, `0A` `0C` `0E` `10` `12` low, medium-low, medium, medium-high, high | The community project had three speeds |
| 17 | sleep profile | `00` off, `02` General, `04` Old, `06` Young, `08` Kids (profile × 2) | All four seen by rotating the profile on the remote, 2026-08-19 (doc 10 §7.4c) |
| 18 | power and mode | bits 2 and 3: run, non-zero when the unit is on. Bits 4 to 6: mode | The stock capability table defines the mode as three bits. Until 2026-10 the driver read four and took a frame with bit 7 set for an unknown mode. Auto is reported as 5 or 6. Smart mode changes the low bits. What bit 7 carries is unknown |
| 19 | setpoint | whole degrees in the panel's unit | `16` hex is 22 °C |
| 20 | room temperature | whole degrees in the panel's unit | Not the community project's `(raw - 32) × 0.5556`. Seen together with byte 19 on a capture: cool, set 22, room 21 |
| 26 | flags | bit 7: smart mode (the remote's SMART button). Bit 1: the panel shows Fahrenheit | The Fahrenheit bit was confirmed on 2026-07-19 by switching a remote to °F |
| 30 | on timer | relative countdown | See [Timer](#timer-bytes-30-and-31) |
| 31 | off timer | relative countdown, `00` when none | |
| 35 | flags 1 | bit 7 vertical swing, bit 6 horizontal swing, bit 4 auxiliary heater relay, bit 2 eco, bit 1 turbo | Eco is bit 2 here where the community project had bit 3. Turbo was not in the community project |
| 36 | flags 2 | bit 2 quiet (mute). Purify: see below | Quiet toggled on the bench |
| 37 | display | bit 7 follows the remote's DIMMER button | Bit 3 is the filter indicator per the stock capability table (doc 10 §7.4a), not seen set |
| 39, 40, 64, 66 | fault bytes | one bit per fault | Decoded from the stock capability table, two bits confirmed by induced faults on 2026-07-22 (doc 10 §7.3 to §7.6) |

Horizontal swing: the test units have no horizontal louvre motor. The status bit toggles and
nothing moves (confirmed 2026-07-07). The component has the option `supports_horizontal_swing`,
off by default, for a unit that has one.

Auxiliary heater relay: the bit read 0 during normal heat-pump heating (compressor at 50 Hz, warm
air). It is expected in cold weather or defrost and could not be provoked at 33 °C outside. The
position is from the bus correlation and the meaning from the stock firmware's names.

Buttons that leave no trace in the status frame, checked by pressing them while logging: iFeel
(the remote sends its own temperature reading to the unit, not over this bus) and Clock.

### The purify bit: the documents and the code disagree

Two positions are on record for the status bit that reports purify, and no unit with a purifier
was available to settle it.

| Position | Where it comes from |
|---|---|
| byte 36 bit 5 (`0x20`) | The community project's map, carried into the first version of this table and into [05-esp32-replacement.md](05-esp32-replacement.md). On the test units the bit is always 0 |
| byte 36 bit 7 (`0x80`) | The stock capability table: the `t_purify` record decodes to status byte 36 bit 7 (doc 10 §7.4d). The same decode rule reproduces six positions that were confirmed on the bus |

The codec reads bit 7 (`purify_on` in `parse_status()`) and marks it `VERIFY`. Bit 7 has the
better evidence. Neither has been seen set. The ProductType flag `ac_purify` reads 0 on the units
measured (doc 11 §5.1), so the feature is absent there. A status capture from a unit with a
purifier, taken with purify on and off, settles it.

### Telemetry bytes

Source: bus, 2026-07-07, by comparing three states: cooling hard (compressor at 60 Hz), off, and
heating hard (50 Hz). A byte that reverses between cooling and heating is a coil temperature. One
that goes to zero when the compressor stops is a load reading.

| Byte | Field | Cool, off, heat |
|---|---|---|
| 41, 43 | compressor frequency commanded | 60, 0, 55 |
| 42 | compressor frequency actual | 60, 0, 50 |
| 44 | outdoor temperature, °C | confirmed against a weather station |
| 45 | outdoor coil temperature, °C | 33, 25, 17 |
| 50 | bus voltage, about 220 | 218, 222, 218 |
| 55 (repeated at 145) | current reading A | 18, 0, 22 |
| 56 (repeated at 144) | current reading B | 12, 0, 38 |
| 60 | current or discharge reading | 44, 0, 54 |
| 71 | a counter or current seen only when cooling | 2, 0, 0 |

Bytes 144 and 145 repeat bytes 56 and 55. The current bytes are not calibrated to amperes. How
the firmware turns byte 55 into a power estimate is in
[`firmware/docs/09-energy-monitoring.md`](../../firmware/docs/09-energy-monitoring.md).

Right after mains power returns, bytes 44 and 45 both read `EC` (-20 °C) until the unit has real
readings. This was seen once, on one unit, on 2026-10-10. The firmware's rule for it is in
[`hisense_telemetry.h`](../../firmware/esphome/components/hisense_ac/hisense_telemetry.h).

### Timer (bytes 30 and 31)

The remote's timer is relative. The remote works out the time remaining against its own clock and
sends that. The same target time gave different byte values when set at different times of day,
so the A/C never knows the time of day. The value shows in the status frame for about three polls
when the timer is confirmed on the remote, and then both bytes return to `00`. Byte 30 is the
on timer and byte 31 the off timer.

The encoding of the minutes is not known. A one-hour on timer read `0B` in byte 30 and `01` in
byte 31, and an earlier mixed test read `8B` and `15`. A sweep of known durations would settle
it. The firmware does not bridge the timer: Home Assistant automations do the same job.

## Command frame: byte map (class `65`, 50 bytes)

The layout of this frame and its first values came from the community project
(`esphome_airconintl`, `messages.h`). On 2026-07-08 a tap on the stock module's DI pin, while the
vendor app drove the unit, corrected several of them. The stock firmware's capability table later
explained every value by one rule (doc 10 §7.4b):

```
value = ((logical << 1) | 1) << (shift - 1)        00 = leave this field alone
```

The low bit of each field says "this field is being set", which is why the command values are
odd-looking.

| Byte | Field | Values | Source |
|---|---|---|---|
| 16 | fan | `01` auto, `0B` low, `0D` medium-low, `0F` medium, `11` medium-high, `13` high (index × 2 + 1) | bus, DI tap 2026-07-08. The community values `05` `07` `09` were wrong here |
| 16 | fan, quiet | `03` | The DI tap recorded it. Sent as a fan value by this project it put the unit on high. See below |
| 17 | sleep | profile × 2 + 1: `03` `05` `07` `09`, off `01` | bus, 2026-08-19 (doc 10 §7.4c) |
| 18 | mode | (index × 2 + 1) in the high nibble: fan `10`, heat `30`, cool `50`, dry `70`, auto `90` | community for the numbering, auto `90` from the DI tap |
| 18 | power | bits 2 and 3: `0C` switch on, `04` switch off, `00` leave alone | firmware (doc 10 §5b-2), see below |
| 19 | setpoint | degrees × 2 + 1 | community, matched |
| 23 | beeper | bit 2 set: the unit beeps when it takes the frame | firmware, then heard on a unit 2026-10-10 |
| 23 | display unit | `05` Celsius, `07` Fahrenheit, in the stock Matter path | firmware (doc 10 §5b-2) |
| 31 | marker | `01` in every command | bus, 2026-08-19: a frame without it is accepted and ignored |
| 32 | vertical swing | `C0` swing, `40` hold | community, matched |
| 33 | eco | `30` on, `10` off | bus, DI tap 2026-07-08. The first driver sent `34` |
| 33 | turbo | `0C` on, `04` off | bus, DI tap 2026-07-08. The first driver sent `5C` |
| 35 | quiet (mute) | `30` on, `10` off | bus, DI tap 2026-07-08 |
| 36 | panel display | `C0` on, `40` off, `00` leave alone | bus, bench 2026-07-19 |

Status and command disagree on auto: the command is index 4 (`90`) and the status reports 5 or 6.

### Where the three literal frames come from

Three frames are sent byte for byte as the community project published them: power on (`on[]`),
power off (`off[]`) and the status poll (`request_status[]`). No capture of these three from the
owner's bus is on record. They are in use because every unit this firmware runs on answers the
poll and switches on and off on them. The stock firmware explains the 16-byte header and the poll
field by field, and its power values agree with the two power frames (next section). Several
bytes of the power-off frame are not understood, so it is kept as published.
[`NOTICE.md`](../../NOTICE.md#acknowledgements-earlier-work-on-this-ac-bus) has the list.

What was captured on the owner's bus: the three link frames from the stock module's DI pin
(2026-07-06), the command corrections of the table above (DI tap, 2026-07-08), the status byte
map (remote buttons, from 2026-07-07), and the device-type reply (2026-10-12).

### Power travels in the mode byte

The stock module puts power-on (`0C`) into the same byte as the mode: `5C` is cool and on, `3C`
is heat and on, `04` is off (firmware, doc 10 §5b-2). It never sends a power frame followed by a
mode frame. The component sends power and mode this way since 2026-10. For auto, dry and fan the
same rule gives `9C`, `7C` and `1C`. The stock Matter path never sends those three, and they have
not been compared with a capture.

### Quiet is the mute flag, not a fan speed

The DI tap of 2026-07-08 lists `03` at byte 16 for quiet. When this project sent `03` there as a
fan speed, the unit went to high. Quiet is reached through the mute flag at byte 35, after which
the status reports fan `02`. The codec keeps `FAN_SPEED_QUIET` with a `VERIFY` marker and the
component never sends it from Home Assistant.

### Single-field frames

Quiet and the sleep profile go out as a minimal frame: the header, `04` at byte 23, `01` at byte
31, and the one field. Only the named byte is confirmed. On 2026-08-19 it was found that byte 17
in the combined frame cancels sleep and cannot select a profile, and that the minimal frame
selects every profile once byte 31 is `01`. The account is in doc 10 §7.4c.

### Horizontal swing

The community encoding (byte 32 bits 4 and 5, plus `14` at byte 37) has never moved anything on
the test units, which have no motor for it. It is kept for a unit that has one. Vertical and
horizontal in one frame is not confirmed.

### Which comfort modes can be on together

Source: bus, 2026-07-23, through this project's firmware (command sent, status read back) and
checked against the vendor app on the same unit. The A/C enforces this itself: for a pair that
cannot coexist, the mode switched on second wins and the first reads back 0.

| | quiet | sleep | turbo |
|---|---|---|---|
| eco | yes (eco 1, mute 1, fan `02`) | yes | no (byte 33 carries one of them) |
| quiet | | no | no |
| sleep | | | no |

Commands that arrive close together are dropped. Quiet needed about 6 s to take. A 3 s gap, or
quiet followed at once by eco, lost a command. Turbo followed by quiet can leave both off for a
moment. The component sends special-mode frames 10 s apart for this reason.

After quiet is switched on and off again, the fan is left at `0A` (low) and not `01` (auto).

### Command replies

The stock module accepts a reply to a command when it repeats the command's first two payload
bytes and carries `01` in the third (firmware, doc 10 §4.2). That layout has not been captured on
this bus. The component logs the verdict and decides nothing on it. What tells a command that
took from one that did not is the next status frame
([`hisense_bus.h`](../../firmware/esphome/components/hisense_ac/hisense_bus.h)).

## ProductType (class `66` sub `40`)

The A/C answers the ProductType request with 45 bytes of capability flags. The flag positions are
in doc 10 §5a, and what they mean for a unit is in
[11-model-capability-map.md](11-model-capability-map.md).

## Recommission ("77"): the request bits in the `0x1E` reply

On the remote, the horizontal airflow button pressed six times makes the unit beep and show "77"
on its panel. The vendor manual describes it as the way to reset the module's Wi-Fi. No frame
class exists for it. The mainboard sets a request bit in its reply to the module's heartbeat, and
the panel shows "77" because the module says it is provisioning.

These bits are in use today. The component's setup-request tracker reads them, and the
`wifi_setup_mode` component opens the node's Wi-Fi hotspot on a request. Today's behaviour is
described in
[`firmware/esphome/README.md`](../../firmware/esphome/README.md#wi-fi-setup-mode-and-the-77-gesture).
Setup mode and the gesture have not been run on a unit with the ESPHome firmware yet. The bus
facts below were measured with the earlier firmwares.

### Inbound: what the A/C asks for

The stock handler `handle_num30_cmd_result` (`0x9b6f2778`) reads payload byte 4 of the reply,
which is frame byte 17 (firmware):

| Bit | Mask | What the stock module does |
|---|---|---|
| 5 | `20` | smart-config pairing, then a reboot (`0x9b6f27de`) |
| 3 | `08` | reset and reconfigure: `hs_reset_wifi`, `enter_provisioning_reset`, 2 s, reboot (`0x9b6f27f4`) |
| 0 | `01` | store: the A/C hands the module 8 or 2 bytes of a binding token |
| 6 | `40` | an action code |

A reply of class `1D` in place of `1E` is the request to reboot for an update of the mainboard.
The handler also reads payload bytes 5 and 11 (doc 10 §6.5).

Bus, 2026-07-20: the gesture sets bit 5, not bit 3. The reply carries `20` at byte 17 for one
reply, about one second, and then `00` again. Each press that completes the gesture makes one
pulse. Bit 3 has not been seen from a unit.

`enter_provisioning_reset` (`0x9b6f2358`) writes the module's state as follows and the caller
then reboots (firmware):

```
wifi_set_connect_status(3)      disconnected
wifi_set_control_status(0)
wifi_set_wifi_prov_status(1)    provisioning
wifi_set_work_status(5)
wifi_set_wifi_route_status(0)
report_restart()                the new state goes out in the next heartbeat
```

### Outbound: what the module reports

The module reports its own state in every heartbeat (firmware, builder `0x9b6f225c`):

```
frame 16 = 0x80 | (scan not finished << 6) | (connect << 4) | (control << 2)
frame 17 = 0x80 | (self test << 5) | (scan << 4) | (provisioning << 3)
frame 18 = (work << 5) | (route << 1)        only when the A/C's protocol version allows it
frame 22 |= 0x40
```

While pairing, frame 17 has bit 3 set and frame 16 has bits 4 and 5 set (connect is 3). Once the
module has an address, frame 16 is `B0`, frame 17 bit 3 is clear, frame 18 is `22` and frame 22
has `40`. That is the "connected" heartbeat captured from the stock module, shown above.

Bus, 2026-07-09: the panel shows "77" exactly while frame 17 bit 3 is set in the module's
heartbeat. Setting the bit lit "77" and clearing it cleared the panel. Nothing else has to be
sent. So the panel is driven by the module, and the same bit position means "reconfigure" in one
direction and "I am provisioning" in the other.

### How this project's firmware uses the bits

The component takes bit 5 at once, because one reply is all a unit sends. Bit 3 has to hold for
three replies in a row. After a request is taken, and after the "77" indication is switched off,
bit 5 is ignored for 10 s, because the press that ends the mode makes a pulse of its own. The
code is `link_request_bits()` and the `SetupRequestTracker` in the codec, and the action
`hisense_ac.set_setup_indicator` sets the outbound bit.

### History: the Matter firmwares and "77"

> **Retired.** This section describes how the project's two Matter firmwares used the "77"
> request. They were retired on 2026-10-12, and the last tree with them is the tag `matter-final`.
> See [`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md). The
> current use of the request is in
> [`firmware/esphome/README.md`](../../firmware/esphome/README.md#wi-fi-setup-mode-and-the-77-gesture).

The Matter firmwares reacted to either request bit by opening a Matter commissioning window
(`OpenBasicCommissioningWindow`, discriminator 3840) while staying commissioned. The fabric was
not wiped. Repeated pulses were collapsed into one window. Horizontal swing was not advertised
over Matter, because the units have no motor for it (the `RockSupport` attribute was `0x02`,
vertical only). This was proven on hardware. Two things
were never checked: replacing the fabric on a real re-pair, and whether the "exit 77" frame of
the first driver clears the panel after the window times out.

The outbound bit followed the commissioning window. Until 2026-07-27 it was set only on the
remote's gesture, so a window opened in another way (at first boot with no fabric, or through the
Administrator Commissioning cluster) left the panel dark while the device could be paired. From
that date the firmware drove the bit from the commissioning window manager's open and close
notifications, with one sync at driver start because the first-boot window is already open by
then.

Verified on hardware on 2026-07-27, on one unit running the AmebaZ2 Matter firmware 1.3.31, for a
window opened through the Administrator Commissioning cluster:

| Step | The firmware's `link` console command reported | Panel |
|---|---|---|
| no window | `OUTBOUND prov_status=0` | off |
| window open | `OUTBOUND prov_status=1` | shows 77 |
| window closed | `prov_status=0` again | clears |

For how the vendor firmware's own Matter interface is commissioned, see
[02-matter-local-control.md](02-matter-local-control.md).

## Capturing frames

Tap the UM3352E on the stock module ([pinout](../hardware/pinouts.md)): pin 4 (DI) carries what
the module sends, pin 1 (RO) what the A/C answers, pin 5 is ground. A USB RS-485 adapter on the
A and B lines sees both directions. The module has to be plugged into the A/C, because the
transceiver is powered from the A/C's 5 V. If the logic level at RO or DI is 5 V, bring it down
to 3.3 V with a divider before a 3.3 V adapter.

[`tools/sniff.py`](../tools/sniff.py) logs frames with timestamps,
[`tools/decode_ac_frames.py`](../tools/decode_ac_frames.py) checks and decodes them, and
[`tools/map_diff.py`](../tools/map_diff.py) reports which status bytes changed while one control
was toggled. The tools are described in [`tools/README.md`](../tools/README.md).

## History: how this page started

The first version of this page was written before anything had been captured. These notes are
kept because they show what was known from the firmware's strings alone.

- The strings `aircond get cmd crc error` and `aircond get cmd error: invalid frame` showed that
  frames have a header, a length and a checksum. `BC_cmd_task` handles the board communication,
  and `dev_ota_uart_process_cmd` shows that an update of the mainboard can go over the same UART.
- The firmware names these capability fields: `ac_cool_heat`, `ac_fan_mute`,
  `ac_infinite_fan_speed`, `ac_humidity`, `ac_purify`, `ac_power_save`, `ac_power_display`,
  `ac_q_display`, `ac_swing_direction_8`, `ac_swing_follow`, `ac_8heat`, `ac_enable_8heat`,
  `ac_ai`, `ac_dr`, `ac_trans_102_64`. They turned out to be the ProductType flags (doc 10 §5a).
  `ac_trans_102_64` was first read as a hint at block sizes of 102 and 64 bytes. It is the flag
  that selects the generic profile `199` (doc 11 §4).
- The vendor firmware bridges its Matter thermostat to the bus: `SystemMode` to the mode and
  power, `LocalTemperature` from the room temperature, `OccupiedCoolingSetpoint` and
  `OccupiedHeatingSetpoint` to the setpoint. What it sends for each is in doc 10 §5b-2.
- The plan then was to flash one of the community ESPHome components
  (`pslawinski/esphome_airconintl`, `akrabi/hisense_ac_esphome`, `zoffypal/esphome_hisence_ac`),
  which target the older AEH-W4A1 module on the same kind of bus, and to reconcile the
  differences with captures. The project wrote its own component in the end. The community
  tables are discussed in [05-esp32-replacement.md](05-esp32-replacement.md).
- The first driver (kept as a frozen test reference in
  [`firmware/test/reference-driver/`](../../firmware/test/reference-driver/README.md)) had open
  items that were listed here. They are closed: the explicit eco and turbo off values are sent
  (`10`, `04`), the display has the three values of the table above, sleep goes out as a
  single-field frame, and the 10 s poll interval inherited from a sensor stub became the 1 s bus
  cycle of the stock module.
