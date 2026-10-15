# Stock command builders: what the vendor module puts in a `0x65` frame

How the vendor W41H1 firmware builds the command frame (class `0x65`) for every control, read
from the stock flash dump of 2026-08-19: one byte map per control, to check the values this
project's codec guessed against what the stock module sends. It answers, or narrows, open
questions 2, 3, 4, 5, 17 and 18 of [14-open-questions.md](14-open-questions.md).

Addresses are file offsets in `dumps/stock-w41h1-2026-08-19.bin` (fw1 slot, XIP = file +
`0x9b6dc000`). The dump is not in the repository. Every claim is tagged **[PROVEN]** (read from
the code at the offset cited) or **[INFERRED]** (deduced). Nothing on this page was run on a unit;
the hardware checks that remain are listed at the end.

## 1. The two builders

Two code paths in the stock module produce a `0x65` frame **[PROVEN]**:

| Path | Builder | Who calls it | Input |
|---|---|---|---|
| Cloud and app | `get_dev_control_cmd` at `0x1f75c` (name from its own log string, pool `0x1fa68`) | `cloud_process_control_cmd` at `0x1b7e8` (pool `0x1b8ac`), from the cloud JSON handler | a JSON object of `t_*` attributes (`"t_power":1`, `"t_fan_speed":9`, ...) |
| Matter | `matter_pack_devType_cmd` at `0x205c8` (pool `0x20758`) | only `matter_uart_process` at `0x25a48`, through the wrapper at `0x259d8` (`axt @ 0x205c8` gives one caller, `0x25a00`) | a kind (0 cooling setpoint, 1 heating setpoint, 2 system mode, 3 display unit) and one value |

Both hand a 33-byte payload (`0x21`) to the frame wrapper `0x2037c`, which writes the `F4 F5`
envelope and checksum, so the frame on the wire is the 50-byte command frame this project already
uses (`CMD_HEADER`, LEN `0x29`). Payload index N is frame byte 13 + N; the tables below use frame
bytes.

The Matter path is documented in [doc 10 §5b-2](10-stock-fw-init-and-comms.md): it only knows
power with cool or heat, the two setpoints and the display unit. The vendor app drives everything
else (fan, sleep, eco, mute, swing, 8 degree heat, purify, the panel display), so the rest of
this page is about the cloud path.

## 2. `get_dev_control_cmd`: the generic field writer [PROVEN]

`get_dev_control_cmd(json_text, &payloadA, &lenA, &payloadB, &lenB)` at `0x1f75c`:

1. Two 51-byte (`0x33`) global buffers are zeroed and both get `[0] = 0x65` (`0x1f79a`).
   Buffer B additionally gets `[1] = 0x03` (`0x1f7a0`): it is a second sub-command `65 03`,
   used only for attributes whose descriptor flags say so (see `t_fresh_air` below). Buffer A
   is the ordinary `65 00` command.
2. It logs `get_dev_control_cmd orig_command %s` (line 0x398) and `m_protocol_ver %x`
   (line 0x399, the word at `0x10000540`, which the class `0x07` version reply fills).
3. The JSON is parsed (`0x40610`). The device type byte at `0x1000931f` (frame byte 16 of the
   `0x0A` reply) selects the attribute table: `<= 1` or `0x36`/`0x37` takes the air-conditioner
   table (`0x1f822`), `0x15` takes a dehumidifier table (`0x1faae`), anything else produces no
   frame. The units here report device type `01` (doc 03), so the air-conditioner table applies.
4. For each of the 55 records in the RAM table at `[0x1000a844]` (12 bytes each: word, name
   pointer, 4-byte `desc`), if `desc[0] != 0` and the JSON has a key with that name
   (`0x4062e`), the value is read with `atoi` (`0x346b8`), truncated to 8 bits, and written
   with the rule at `0x1f8f2` to `0x1f93e`:

   ```
   byte   = 15 + desc[0]                      (payload index desc[0] + 2)
   shift  = desc[1] & 7
   mask   = {00,01,03,07,0f,1f,3f,7f}[desc[3] >> 3]       table at file 0x126ff0
   payload[byte] |= (value & mask) << shift
   payload[byte] |= 1 << (shift - 1)                     the "field is being set" marker
   ```

   Records with `desc[1] & 0xf8 != 0` are written into buffer B instead of A (`0x1f86c`).
   The two ORs are the same thing as the rule doc 10 §7.4b derived from the table alone,
   `((value << 1) | 1) << (shift - 1)`, with the mask added.
5. Indices 0, 4, 5 and 6 of the table have special handling before the generic write (a `tbh`
   jump table at `0x1f886`, entries in the local evidence note). Section 4 lists them.
6. Length: buffer A is sent as 33 bytes if its last non-zero byte is at payload index `0x1b` or
   lower (frame byte 40), else as 51 bytes (`0x1f94e` to `0x1fb5c`). Buffer B, when it holds
   anything, is 33 bytes. An all-zero buffer is not sent at all.
7. Beep (`0x1f96e` to `0x1f992`): the JSON key `t_beep` is read, default 1 when absent, and
   `payload[0x0a] |= value << 2`, which is frame byte 23 bit 2. So every app command carries
   `04` at byte 23 unless the app sends `t_beep: 0`. If only buffer B is non-empty, the beep
   lands in B at payload index 4 bit 7 instead (`0x1faa4`).

`cloud_process_control_cmd` (`0x1b7e8`) then queues each non-empty buffer as a separate item
of type `0xc9` (`0x1b83e`). The queue consumer in `uart_ctl_process_main` wraps it with `0x2037c`
(`0x269c0`) and sends it through `0x24a50` with a 3000 ms window (`0xbb8` at `0x24a72`).

### The template in flash

The RAM table is filled from a template of 12-byte records in flash. The air-conditioner
template starts at `0x129e10` (`t_work_mode`) and runs to `0x129f6c`; a second block at
`0x12a08c` holds the dehumidifier records (`t_humidity`, `t_pump`, `t_anion`, with other
offsets). The order of the first records in RAM is **[INFERRED]** to be the template order,
because the special cases keyed on index 0, 4, 5 and 6 only make sense for `t_work_mode`,
`t_fan_speed`, `t_fan_speed_s` and `t_fanspeedCV`, which are exactly those records in the
template.

## 3. Byte map per control [PROVEN from the records and the writer]

Values are what the writer produces for the attribute value in the "value" column. Frame bytes.
`desc` as stored (little-endian word, so `desc[0]` is its lowest byte).

| Control | JSON attribute | record | `desc` | byte | shift | mask | value -> wire |
|---|---|---|---|---|---|---|---|
| mode | `t_work_mode` | `0x129e10` | `1c030503` | 18 | 5 | `07` | fan 0 `10`, heat 1 `30`, cool 2 `50`, dry 3 `70`, auto 4 `90` |
| power | `t_power` | `0x129e1c` | `0b030303` | 18 | 3 | `01` | on `0C`, off `04` |
| setpoint | `t_temp` | `0x129e28` | `38040104` | 19 | 1 | `7f` | degrees × 2 + 1 (`2D` for 22) |
| display unit | `t_temp_type` | `0x129e34` | `090b0108` | 23 | 1 | `01` | Celsius 0 `01`, Fahrenheit 1 `03` |
| fan speed | `t_fan_speed` | `0x129e40` | `39010101` | 16 | 1 | `7f` | index × 2 + 1: auto 0 `01`, 1 `03`, low 5 `0B`, 6 `0D`, 7 `0F`, 8 `11`, high 9 `13` |
| fan speed (alt) | `t_fan_speed_s` | `0x129e4c` | `38000101` | 16 | 1 | `7f` | as above, plus byte 40 (see §4) |
| fan percent | `t_fanspeedCV` | `0x129e58` | `383b0119` | 40 | 1 | `7f` | `((min(v,100) & 0xfe) << 1) | 1` |
| louvre angle | `t_swing_angle` | `0x129e64` | `19130110` | 31 | 1 | `07` | angle × 2 + 1: 0 `01` ... 7 `0F` |
| swing direction | `t_swing_direction` | `0x129e70` | `18450120` | 47 | 1 | `07` | v × 2 + 1 |
| swing follow | `t_swing_follow` | `0x129e7c` | `1441021a` | 41 | 2 | `03` | `(v << 2) | 2` |
| tms | `t_tms` | `0x129e88` | `0e450520` | 47 | 5 | `01` | on `30`, off `10` |
| temp compensate | `t_temp_compensate` | `0x129e94` | `240b0408` | 23 | 4 | `0f` | `(v << 4) | 8` |
| vertical swing | `t_up_down` | `0x129ea0` | `0f140711` | 32 | 7 | `01` | on `C0`, off `40` |
| horizontal swing | `t_left_right` | `0x129eac` | `0e140511` | 32 | 5 | `01` | on `30`, off `10` |
| sleep | `t_sleep` | `0x129eb8` | `39020102` | 17 | 1 | `7f` | profile × 2 + 1: off `01`, `03` `05` `07` `09` |
| fresh air | `t_fresh_air` | `0x129ed0` | `0e661f03` | 18 of the `65 03` frame | 7 | `01` | on `C0`, off `40` (buffer B, flags `0x18`) |
| eco | `t_eco` | `0x129ef4` | `0a140512` | 33 | 5 | `01` | on `30`, off `10` |
| mute (quiet) | `t_fan_mute` | `0x129f00` | `0a150514` | 35 | 5 | `01` | on `30`, off `10` |
| turbo | `t_super` | `0x129f0c` | `09140312` | 33 | 3 | `01` | on `0C`, off `04` |
| demand response | `t_demand_response` | `0x129f18` | `0f44072c` | 59 | 7 | `01` | on `C0`, off `40` (51-byte payload) |
| dal | `t_dal` | `0x129f24` | `0e44052c` | 59 | 5 | `01` | on `30`, off `10` |
| talr | `t_talr` | `0x129f30` | `0d44032c` | 59 | 3 | `01` | on `0C`, off `04` |
| 8 degree heat | `t_8heat` | `0x129f48` | `083e0116` | 37 | 1 | `01` | on `03`, off `01` |
| purify | `t_purify` | `0x129f54` | `0f150713` | 34 | 7 | `01` | on `C0`, off `40` |
| panel display | `t_dimmer` | `0x129f60` | `0f160715` | 36 | 7 | `01` | on `C0`, off `40` |
| beep | `t_beep` (not a record) | code `0x1f98c` | | 23 | 2 | | on `04`, off `00`, default on |

Records with `desc[0] = 0` (`f_votage`, `f_cool_qvalue`, `f_heat_qvalue`) are status-only and
skipped by the writer (`0x1f84e`).

Two consequences:

- **A stock command contains only the fields the JSON names.** Byte 23 is the one exception
  (beep, default on). There is no marker at byte 31 unless the app sends `t_swing_angle`
  (`01` is `t_swing_angle: 0`), and no `01` filler at byte 16 unless it sends `t_fan_speed: 0`.
- **Combining fields is an OR into the same bytes.** Eco and turbo share byte 33, vertical and
  horizontal swing share byte 32, power and mode share byte 18: `t_power: 1` with
  `t_work_mode: 2` gives `5C`, the value the Matter path hard-codes.

## 4. The special cases in the writer [PROVEN]

Index 0, `t_work_mode` (`0x1f8a8`):

- If the value is 5 and the device type is `0x37`: byte 33 `|= 0x30` (eco on). Not reachable on
  a device type `01` unit. The Matter builder carries the same dead test at `0x2063c`.
- If `m_protocol_ver <= 0x211`: byte 17 `|= 01`, 32 `|= 01`, 33 `|= 14`, 34 `|= 05`,
  41 `|= 40` (`0x1f8ca` to `0x1f8f0`). These are the "legacy" bytes doc 10 §5b-2 found in the
  Matter builder; the cloud path adds them to a mode write only.

Index 4, `t_fan_speed` (`0x1f9e4`), skipped when the product type string at `0x10009374`
is `199` (`0x1f9e4`, string at file `0x1245a7`):

- When the device type is `<= 1`: if `m_protocol_ver > 0x214` and the product type is not one
  of `100`, `101`, `104`, `105` (`0x1f6bc`), nothing special happens and the generic rule
  applies. Otherwise the value is remapped through the table at file `0x126b7b`
  (5 -> 2, 6 -> 2, 7 -> 3, 8 -> 4, 9 -> 4, others unchanged, `0x1f734`): the old three-speed
  ladder `05` `07` `09` that the community project published.
- Then, if `m_protocol_ver <= 0x211`: byte 35 `|= 20` when the value is 1 (mute on), byte 35
  `|= 10` always, byte 17 `|= 01`, byte 33 `|= 14` (`0x1fa28` to `0x1fa48`).

Index 5, `t_fan_speed_s` (`0x1f9c2`): unless the product type is `199`, byte 40 `|= 01 | (pct
<< 1)` with pct from the table at file `0x126b75` (5 -> 5, 6 -> 10, 7 -> 50, 8 -> 80, 9 -> 90,
`0x1f70c`), then continues as `t_fan_speed`.

Index 6, `t_fanspeedCV` (`0x1fa4a`): value clamped to 100 and rounded down to even.

The W41H1 units here get fan `0B` for low on the bus (DI tap 2026-07-08, doc 03), so on them the
remap does not run: **[INFERRED]** their `m_protocol_ver` is above `0x214` and their product
type is not in `{100, 101, 104, 105}`, which also means none of the `<= 0x211` legacy bytes are
sent to them.

## 5. The power frames and the status poll

Stock power-on from the app, `{"t_power": 1}` only **[PROVEN]**: payload `65 00`, byte 18
`0C`, byte 23 `04`, everything else `00`. As a 50-byte frame:

```
F4 F5 00 40 29 00 00 01 01 FE 01 00 00 65 00 00 00 00 0C 00 00 00 00 04 00 00 00 00 00 00
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 DF F4 FB
```

This is `FRAME_ON` in `hisense_protocol.cpp`, byte for byte, including the checksum `01 DF`.

Stock power-off, `{"t_power": 0}` **[PROVEN]**: byte 18 `04`, byte 23 `04`, everything else
`00`. `FRAME_OFF` in the codec carries more: `01` at 17, `01` at 24, `01` at 29, `01` at 32,
`55` at 33, `01` at 35. None of those come from the stock writer for a `t_power` write. Bytes
17 and 32 and the `14` inside `55` match the legacy block of §4, which runs only for
`m_protocol_ver <= 0x211` and only on a mode or fan write; `01` at 24, 29 and 35 and the `41`
inside `55` are not produced by any path of the writer. The community frame therefore looks like
a capture from an older unit with more attributes in the JSON, not the minimal stock frame.

The two frames the vendor app sends in practice are **not** settled here: the app may put more
attributes into the JSON than `t_power`, and the stock builder would then carry them. Only a
DI tap while the app switches the unit on and off shows that (open question 18 stays open on
that point; the stock values for the bytes themselves are now known).

Status poll **[PROVEN]**: the builder at `0x20514` allocates 5 bytes, writes `66` and four
zero bytes, and wraps 4 bytes. The frame is `... 66 00 00 00 01 B3 F4 FB`, which is
`STATUS_REQUEST` in the codec.

## 6. The single-field frames

The codec's `build_single_field()` sends the header, `04` at byte 23, `01` at byte 31 and the
one field. The stock equivalent of a single attribute write is the header, `04` at byte 23
(beep default) and the one field: **no byte 31** (§3). The `01` at byte 31 that the bench of
2026-08-19 found necessary (doc 10 §7.4c) is `t_swing_angle: 0` in stock terms, which the app
evidently sends with every command. Whether the A/C needs byte 31 or merely needs any second
field is not something the dump can answer.

So, for the two frames the codec sends:

| Frame | Codec | Stock for the same single attribute |
|---|---|---|
| mute on / off | byte 35 `30` / `10`, byte 23 `04`, byte 31 `01` | byte 35 `30` / `10`, byte 23 `04` |
| sleep profile | byte 17 profile × 2 + 1, byte 23 `04`, byte 31 `01` | byte 17 profile × 2 + 1, byte 23 `04` |

## 7. What this settles for the codec's `VERIFY` markers

| Marker | File | Verdict |
|---|---|---|
| `FAN_SPEED_QUIET = 1` (cmd `03`) | `hisense_protocol.h` | Confirmed as an encoding: `t_fan_speed: 1` gives `03` at byte 16 and the writer has no other way to make `03`. **Still open as a control**: whether the app's quiet is `t_fan_speed: 1`, `t_fan_mute: 1` (byte 35 `30`) or both cannot be read from the dump. The bench result (`03` alone -> high) says the A/C does not treat fan 1 as quiet; the mute byte is the control (question 4) |
| `FEATURE_ECO_OFF = 0x10` | `hisense_protocol.h`, `.cpp` | Confirmed: `t_eco: 0` gives byte 33 `10`, the generic rule with no special case (question 3) |
| `AcCommand::power_on` (`9C`, `7C`, `1C`) | `hisense_protocol.h` | Confirmed by construction: `t_power: 1` ORs `0C` into byte 18 and `t_work_mode` ORs `(mode × 2 + 1) << 4` into the same byte, so auto gives `9C`, dry `7C`, fan `1C`. Not sent by the Matter path, and no capture (question 2 narrows to "the bytes are right, the A/C's reaction is unmeasured") |
| `build_single_field` other bytes | `hisense_protocol.cpp` | Contradicted in one byte: stock writes no `01` at byte 31 for a single attribute. Byte 23 `04` matches (beep default). The rest, all zero, matches (question 5) |
| `swing_byte` combined | `hisense_protocol.cpp` | Confirmed as bytes: `t_up_down` and `t_left_right` OR into byte 32 (`C0` or `40`, plus `30` or `10`), so vertical swing with horizontal swing is `F0`, vertical hold with horizontal off is `50`. The codec's horizontal "off" is `00` (field absent), which stock would write as `10` when the app names it. The `14` at byte 37 the codec adds for horizontal is **not stock**: byte 37 is `t_8heat`, and nothing in the writer puts `14` there for the device type `01` table (question 6; the A/C's reaction still unmeasured) |
| `purify_on` status bit | `hisense_protocol.h` | Status side, not a command: the record says status byte 36 bit 7, command byte 34 `C0` / `40`, as doc 10 §7.4d had (question 1 unchanged) |
| `FRAME_ON` comment | `hisense_protocol.cpp` | Confirmed: identical to the stock `{"t_power": 1}` frame (question 18, on side) |
| `FRAME_OFF` comment | `hisense_protocol.cpp` | Contradicted: stock `{"t_power": 0}` is byte 18 `04`, byte 23 `04`, nothing else. The extra bytes of the community frame are not stock output for this unit type (question 18, off side) |
| `STATUS_REQUEST` comment | `hisense_protocol.h` | Confirmed: `66 00 00 00` from the builder at `0x20514` |
| 8 degree heat, purify, display unit (question 17) | none | Confirmed as stock bytes: byte 37 `03` / `01`, byte 34 `C0` / `40`, byte 23 `03` / `01` (`t_temp_type` shares byte 23 with the beep bit and `t_temp_compensate`). Hardware reaction unmeasured |

## 8. What was not established

- What the vendor app puts in the JSON for each button. The builder is generic; the
  app decides the attribute set. A DI tap with the app is still the only way to see a complete
  stock frame per control.
- The value of `m_protocol_ver` on the units here. It is inferred above `0x214` from the fan
  ladder seen on the bus, not read.
- Whether the A/C accepts a single-attribute frame without byte 31. The bench of 2026-08-19
  says no for the mute and sleep frames; this page only says the stock writer does not add it.
- The RAM table's record order beyond the first seven (inferred from the template).
- The queue consumer's retry on a failed reply check (doc 10's open item, not touched here).

## 9. Tooling

rizin 0.8.2 through the REA MCP server (`execute_rizin_command`, `e asm.bits=16; e asm.cpu=cortex`,
`aap; aar` over `0x10000..0x60000` for `axt`) and python3 pointer scans for the string references
and the template records. The raw listings and pool words are in `dumps/single-field-builders-analysis.md`,
kept with the dump and not published.
