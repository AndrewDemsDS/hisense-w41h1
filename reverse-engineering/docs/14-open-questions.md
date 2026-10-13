# Open questions

What is not settled about the bus and the stock firmware, in one list, each with what would
settle it. If you have a unit that can answer one of these, the frames are what is needed: see
[Capturing frames](03-rs485-ac-protocol.md#capturing-frames) and
[`CONTRIBUTING.md`](../../CONTRIBUTING.md).

The component's sources mark the same points with `VERIFY`. To list the markers:

```
grep -rn VERIFY firmware/esphome/components/
```

## On the bus

Each row names the marker in the sources where one exists. The files are under
`firmware/esphome/components/hisense_ac/`.

| # | Question | What is known | What would settle it | Marker |
|---|---|---|---|---|
| 1 | Which status bit reports purify | Byte 36 bit 5 in the community map, byte 36 bit 7 in the stock capability table. The codec reads bit 7. Neither seen set ([doc 03](03-rs485-ac-protocol.md#the-purify-bit-the-documents-and-the-code-disagree)) | A status capture from a unit with a purifier, purify on and off | `hisense_protocol.h`, `purify_on` |
| 2 | Power-on together with auto, dry or fan (`9C`, `7C`, `1C` at command byte 18) | Cool `5C` and heat `3C` are the stock module's bytes. The other three follow from the field rule. The component sends them | A log at `VERY_VERBOSE` of a unit switched on from off in each of the three modes, with the status frames that follow | `hisense_protocol.h`, `AcCommand::power_on` |
| 3 | The explicit eco-off value `10` at command byte 33 | [Doc 03](03-rs485-ac-protocol.md#command-frame-byte-map-class-65-50-bytes) records it from the DI tap of 2026-07-08, and the capability rule computes the same value. The codec still marks it `VERIFY`. The tap's raw log is not in the repository | Eco switched off from Home Assistant on a unit with eco on, and the status bit read back | `hisense_protocol.h` and `.cpp`, `FEATURE_ECO_OFF` |
| 4 | Fan value `03` (quiet) at command byte 16 | Recorded by the DI tap. Sent by this project it put the unit on high. Quiet is reached through the mute flag | A DI tap of the stock module while the vendor app selects quiet, whole frame, to see what else the frame carries | `hisense_protocol.h`, `FAN_SPEED_QUIET`, and `hisense_entities.h` |
| 5 | The single-field frame's other bytes | Only the named byte (mute at 35, sleep at 17) and the marker at byte 31 are confirmed. The rest is the smallest frame that worked | A DI tap of the stock module sending quiet or a sleep profile | `hisense_protocol.cpp`, `build_single_field` |
| 6 | Vertical and horizontal swing in one command | Vertical is confirmed. The test units have no horizontal motor | A unit with a horizontal louvre: command both, read status byte 35 bits 6 and 7, and watch the louvres | `hisense_protocol.cpp`, `swing_byte` |
| 7 | The reply to a command frame | The stock module expects the command's first two payload bytes echoed and `01` after them. Not captured here. The component logs its verdict and decides nothing on it | A `VERY_VERBOSE` log that shows the reply to a `0x65` frame, for a command that took and for one that did not | `hisense_bus.h`, `CMD_REPLY_ACK_OFFSET` |
| 8 | Whether a unit ever raises request bit 3 (`08`) in its `0x1E` reply | The stock firmware handles it as reset and reconfigure. The units here raise bit 5 for the "77" gesture | A `DEBUG` log (it shows every change of the request bits) from another model during the gesture, or during a wired remote's reset | `hisense_protocol.h`, `LINK_REQ_RECONFIG` |
| 9 | Heating or cooling while in auto | With the compressor running in auto the component reports cooling, also when the unit heats. No status byte is known to tell the two apart | Status captures in auto, once heating and once cooling, compared byte by byte | `hisense_mapping.h`, `climate_action` |
| 10 | The `EC` placeholder in the outdoor and coil temperature after a power cut | Seen once, on one unit, 2026-10-10. How long it lasts, and whether both bytes leave it together, was not measured | A log of bytes 44 and 45 from mains power-on, on more than one unit | `hisense_telemetry.h` |
| 11 | The beeper bit with the bit clear | [Doc 03](03-rs485-ac-protocol.md#command-frame-byte-map-class-65-50-bytes) records that a unit took commands silently on 2026-10-10. The codec's comment still asks for the check | Nothing on the bus. The comment and the record have to be brought into line by the maintainer | `hisense_protocol.h`, `stamp_beep` |
| 12 | Class `67` | The stock module sends it after each ProductType exchange. Captured against a simulated A/C only | A tap on a stock module that is plugged into a real unit, for two minutes | none |
| 13 | The minute encoding of the timer bytes 30 and 31 | Two readings, not enough for a rule | A sweep of known durations set on the remote | none |
| 14 | Status byte 18 bit 7 | Not part of the mode. Its meaning is unknown | A capture of a frame with the bit set, and what the unit was doing | none |
| 15 | Status byte 38 | Reads `05` on both units measured. No record of the capability table claims it | A unit where it differs | none |
| 16 | The auxiliary heater relay bit (status byte 35 bit 4) | Position from the bus, never seen set: it needs cold weather or a defrost | A status capture in winter with the relay on | none |
| 17 | Fault bytes 40, 64 and 66 | Decoded from the stock capability table. Only two bits of byte 39 were seen firing, with induced faults (doc 10 §7.6) | A fault of the outdoor unit or the electronics, with a status capture | none |
| 18 | Commands for 8 °C heat, purify and the display unit | Predicted from the capability table: byte 37 `03` or `01`, byte 34 `C0` or `40`, byte 23 `03` or `01` (doc 10 §7.4b). The first driver's comments record an 8 °C heat test on 2026-07-19 and not the value that was sent | Each sent as a single field to a unit that reports the capability, and the status read back | none |
| 19 | The three literal frames (power on, power off, status poll) | Taken from the community project. They work on every unit here. No capture of them from the owner's bus exists, and parts of the power-off frame are not understood | A DI tap of the stock module while the vendor app switches the unit on and off | comments at `FRAME_ON`, `FRAME_OFF`, `STATUS_REQUEST` |

One marker is out of date and needs no measurement: `devtype_reply_usable` in
`hisense_protocol.h` says that no capture of a real device-type reply is in the repository. One
is now, from 2026-10-12, and it confirms the length and the checksum
([doc 03](03-rs485-ac-protocol.md#the-device-type-exchange)).

## Never run on a unit

These are not protocol questions. They are parts of the firmware and the tooling that have only
run on a host or on a bench without an A/C. The pages that own them say so in detail.

| What | Where it is described |
|---|---|
| Wi-Fi setup mode and the "77" gesture with the ESPHome firmware | [`firmware/esphome/README.md`](../../firmware/esphome/README.md#wi-fi-setup-mode-and-the-77-gesture) |
| The WS2812 status pixel of the ESP32-C3 SuperMini Plus board file | [`firmware/esphome/README.md`](../../firmware/esphome/README.md#status-led) |
| The clip flasher's one-slot writer (`ch341flash.py --layout ... --slot ...`) | [`docs/guide/Recovery-and-Reflash.md`](../../docs/guide/Recovery-and-Reflash.md) |

## In the stock firmware

These concern the vendor module's own code. None of them blocks the firmware of this project.

| Question | Where | What would settle it |
|---|---|---|
| Whether the stock module sends a command again when the reply fails its echo check, and what the cloud path strips per mode | doc 10 §5b-2 | Reading the queue consumer in the disassembly |
| The record count of the capability struct: the extractor loops over 69 records, the template has 67 | doc 10 §7.5 item 2 | Reading the allocation that fills `0x1000b8a8`, or a dump of that RAM |
| The `desc` word of ranged records (`t_temp`, `t_fan_speed`): where the range lives | doc 10 §7.5 item 3 | More disassembly of the extractor's callers |
| Which profile code (`100` to `129`) the units here resolve to. Not `199` | doc 11 §6 item 2 | Logging the ProductType payload against the decision tree, or reading `0x100096dc` on a stock module |
| What bytes 0, 1 and 3 of a capability record's first word mean | doc 11 §6 item 4 | A consumer has to be found first |
| Whether the 17 unconditional capability enables are ever cleared again | doc 11 §6 item 5 | A full trace of the writers |
| What the two other readers of the capability struct do (`0x9b6f1e3a`, `0x9b6f8150`) | doc 11 §6 item 6 | Decompiling them |
| Whether any writer can select the TLS mode without verification | doc 13, open questions | A writer of the value 6 to `0x10009050` has to be found |
| The UDP 5030 path gated on `0x65` and `0x66` | doc 13, open questions | Disassembly of the handler, or a probe of a stock module on an isolated network |
| Whether the stock "77" reset erases the Matter fabric storage | doc 13, open questions | A dump of a stock module before and after the gesture |
| How the bootloader chooses between two slots with the same serial | doc 13, open questions | Disassembly of the bootloader |
| Where the `HOTA=` SHA-256 argument is compared | doc 13, open questions | Disassembly |
| The exact pad register of `PA_17` | doc 10 §2.2 | Not recoverable from the flash dump: the code is in mask ROM |
