# RS-485 A/C protocol (ESP32 replacement path)

The dongle talks to the A/C indoor-unit mainboard over **RS-485** via the UM3352E
transceiver. Replacing the dongle with an **ESP32 + RS-485 transceiver** running ESPHome
lets you control the A/C locally without the OEM module at all.

## What the firmware tells us

- **Baud:** the UART supports **B9600** and **B115200** (`baudrate parameter must be
  B9600 %d, B115200 %d`). The A/C link is most likely **9600 8N1** (typical for these
  buses); confirm by sniffing.
- **Framing:** commands are framed and integrity-checked —
  `aircond get cmd crc error`, `aircond get cmd error: invalid frame`. So expect a frame
  header, length, payload, and a checksum/CRC trailer.
- **Comm task:** `BC_cmd_task` handles board communication; `dev_ota_uart_process_cmd`
  suggests OTA can also flow over the UART.

## Frame format (decoded from firmware disassembly)

Recovered by disassembling the A/C-bus parser. The firmware image is XIP-mapped at
**base `0x9b6d0000`** (`runtime = file_offset + base`); the parser is at runtime
`0x9b6f2e00` (file `0x22e00`) and the status-struct handler at `0x9b6f0e60`.

The module↔A/C frame is **`F4 F5` … `F4 FB`** delimited:

```
off  field        value / meaning
──────────────────────────────────────────────────────────────────────
 0   STX1         0xF4                         start marker 1
 1   STX2         0xF5                         start marker 2
 2   TYPE         message class (0x01 = the A/C data path that was decoded;
                                 other values route to link/net handlers)
 3   CTRL         control/flags byte:
                    bit0 (0x01)  -> an extra byte precedes the length field
                    bit5 (0x20)  -> LEN is 16-bit big-endian (else 8-bit)
                    bit6-7(0xC0) -> flags (purpose TBD)
 4.. LEN          payload length: 1 byte, or 2 bytes big-endian if CTRL bit5 set
     PAYLOAD      LEN bytes (A/C command/status; the ac_* fields ride here)
 N   CKSUM        checksum over the frame; WIDTH = CTRL & 0xC0 (0x00→1 byte,
                  0x40→2, 0x80→2, 0xC0→4), big-endian running sum of the
                  payload. All ac_* command/status frames use CTRL=0x40 →
                  2-byte checksum. (failure => "crc error")
 N+1 ETX1         0xF4                         end marker 1
 N+2 ETX2         0xFB                         end marker 2
```

Parser facts (verbatim from the disassembly):
- Rejects frames with length ≤ 3.
- `byte[0]==0xF4 && byte[1]==0xF5` or it's "invalid frame".
- Reads the end tag at `header+LEN`: `byte==0xF4 && next==0xFB` or it's "no end tag".
- Checksum mismatch => "crc error". Algorithm CONFIRMED from the parser: a big-endian
  running **sum** of the payload (Thumb `ldrb r2,[r3,#1]!; add r1,r2` loop), width chosen
  by CTRL & 0xC0 (helper `0x9b6f0958`: 0x00→1, 0x40→2, 0x80→2, 0xC0→4 bytes). Not XOR, not
  a CRC. All `ac_*` command/status frames use CTRL=0x40 → 2-byte checksum.
- **Byte-stuffing**: a checksum byte equal to the `0xF4` marker is escaped by doubling it
  (confirmed via the reference's `temp_16_C` frame: checksum `0x01F4` is emitted on the wire
  as `01 F4 F4 F4 FB`). The builder in `hisense_rs485.cpp` stuffs an `0xF4` checksum byte on
  TX; the bus task un-stuffs on RX.

### Physical layer (resolved from the stock firmware dump, no bench needed)

- **Pins**: `PA_14` (TX) / `PA_13` (RX), UART0, **8N1, 9600 baud** — read straight out of the
  stock `uart_ctl_process_main` bring-up (see `UART_PINS.md`).
- **DE/RE**: **none** — the transceiver is auto-direction (no `gpio_*` calls anywhere in the
  UART init or TX window). The driver does no direction control.
- **RX is interrupt-driven**: the stock firmware uses `serial_irq_handler`/
  `serial_irq_set(RxIrq)` into a ring buffer (same pattern `hisense_rs485.cpp` uses) — no
  polling, no busy-spin `serial_getc()`.
- **TX pacing / no interleave**: sends are serialized and paced by a single bus task (rules in
  [`INTEGRATION.md` §5](../../firmware/src/rs485-driver/INTEGRATION.md#5-bus-timing)); no more
  back-to-back `hisense_tx_frame()`.
- **TX tail timing**: `hisense_tx_raw()` waits ~3 ms (≥2 byte-times) after the last
  `serial_putc()` so the shift register drains before the next send.

### Message classes

The parser distinguishes three response/command classes, matching the error strings
`link resp` / `net resp` / `trans resp`:
- **LINK** — module↔A/C link/handshake.
- **NET** — network/commissioning status relayed to the A/C.
- **TRANS** (transparent) — the actual A/C control + status payload; this is where the
  `ac_*` fields live (see below). `ac_trans_102_64` hints at fixed transparent block
  sizes (102 / 64 bytes).

> The framing (F4 F5 / F4 FB, LEN, CTRL-selected checksum width) AND the checksum
> algorithm are firmware-confirmed. The byte-level encoding of the `ac_*` status fields has
> since been **hardware-confirmed on the real W41H1 bus** by remote-button correlation — see
> [Status-frame byte map](#status-frame-byte-map--hardware-confirmed-160b-class-0x66-reply)
> below and `hisense_rs485.cpp`. (The offsets originally borrowed from the AEH-W4A1 reference
> were wrong in several places and are corrected there.)

## A/C feature fields (from firmware)

The protocol carries these A/C capability/state fields (control surface):

```
ac_cool_heat            ac_fan_mute            ac_infinite_fan_speed
ac_humidity             ac_purify              ac_power_save
ac_power_display        ac_q_display           ac_swing_direction_8
ac_swing_follow         ac_8heat / ac_enable_8heat   ac_ai
ac_dr                   ac_trans_102_64
```

## Status-frame byte map — hardware-confirmed (160B class-`0x66` reply)

These offsets were confirmed on the **real W41H1 bus** by toggling each control on the
remote and diffing the 160-byte A/C→module status reply (`f4 f5 01 … 66 …`). The
authoritative parse lives in [`hisense_rs485.cpp`](../../firmware/src/rs485-driver/hisense_rs485.cpp)
(`hisense_parse_status`); this table records the **remote-button correlation** that backs it,
including fields the driver does not currently consume.

**Frame length**: 160 bytes, not the reference project's 82 — LEN-driven off `byte[4]`
(total = `byte[4] + 9`), confirmed across every frame class. The old fixed-82 assumption
dropped every status frame; the parser + RX task + `HISENSE_RX_BUF_SIZE` (was 128, too small
for 160) are all sized for 160 now.

| Byte(s) | Field | Encoding (confirmed) | Notes |
|---|---|---|---|
| 16 | fan speed | `01`=auto, `02`=quiet, `0A/0C/0E/10/12`=low/med-low/mid/med-high/high | six speeds — more than the reference's three |
| 17 | sleep profile | `0x00` off / `0x02` General / `0x04` Old / `0x06` Young / `0x08` Kids | = profile × 2; new field |
| 18 | mode nibble | see driver (`0x28`/`0x2a`/`0x48`…); **AUTO = 5** (nibble), value 4 skipped | high nibble = mode; SMART/AI toggles the low bits |
| 19 | setpoint | direct integer °C | e.g. `0x16` = 22 °C set |
| 20 | current/room temp | direct integer °C | **NOT** the W4A1 `(raw-32)*0.5556` formula; e.g. `0x15` = 21 °C room. Confirmed together with byte19 on a real capture: COOL / 22 / 21 / auto / no-swing |
| 26 | AI / SMART flag | bit `0x80` | SMART button sets it (with the byte18 nibble) → HA `heat_cool` |
| 30 | **ON-timer** | relative countdown | non-transient? see **Timer** below |
| 31 | **OFF-timer** | relative countdown | `0x00` = disabled |
| 35 | flags1 | bit7 `0x80`=vswing, bit6 `0x40`=hswing, bit4 `0x10`=aux/PTC heat relay, bit2 `0x04`=eco, bit1 `0x02`=turbo | vswing real; **hswing vestigial — no motor on this unit** (bit toggles, nothing moves; confirmed 2026-07-07, not advertised over Matter — `RockSupport` must be `0x02`, vertical only); eco bit position corrects the reference's bit3; turbo is a new field not in the reference; heat-relay bit verified 0 during normal heat-pump HEAT (compressor 50 Hz, warm air) — only fires in cold/defrost, unconfirmable at 33 °C outdoor but position/meaning understood |
| 36 | flags2 | bit5 `0x20`=purify, bit2 `0x04`=mute | purify: feature **absent** on this unit (no app/remote control), bit always 0; mute bench-toggled |
| 37 | display dimmer | bit `0x80` | DIMMER button; display-only, no A/C effect |

**Remote buttons with NO bus signature** (remote-local functions — verified by pressing them
while logging and seeing zero status change): **iFeel** (remote sends its own temp reading
out-of-band, not via this bus) and **Clock** (sets the remote's own time display).

### Diagnostic/telemetry byte map (status frame, hardware-confirmed)

Mapped 2026-07-07 by a three-state correlation (hard-cool 60 Hz → off 0 Hz → hard-heat 50 Hz):
a byte that **reverses** between cool/heat is a coil temp; one that **zeros** when the
compressor stops (without reversing) is a load/current reading. None are control fields —
current bytes would need a clamp meter to calibrate to amps, but the block structure is fully
known; no unknown bytes remain in this range.

| Byte | Field | Signature (cool→off→heat) |
|---|---|---|
| 41, 43 | compressor **target/commanded** freq | 60→0→55 (leads actual @42's 50) |
| 42 | compressor **actual** freq | 60→0→50 |
| 44 | outdoor temp | direct °C, confirmed vs a weather station |
| 45 | outdoor/condenser coil temp | 33→25→17 °C (reverses; 25 ≈ ambient/outdoor) |
| 50 | DC-bus voltage or stable ref (~220) | 218→222→218 (barely moves) |
| 55 (= 145) | phase current / power A | 18→0→22 |
| 56 (= 144) | phase current / power B | 12→0→38 |
| 60 | active current / discharge reading | 44→0→54 |
| 71 | cooling-only counter/current | 2→0→0 |

`@144/@145` are an exact duplicate of `@55/@56` (the frame echoes the current block).

### Control-frame (command) byte map — DI-tap-confirmed on the stock dongle (2026-07-08)

Command frames are 50 B (`byte[4]=0x29`), 2-byte checksum — matching the builder. Sniffing the
stock dongle's own app-driven commands corrected several values the driver had inherited from
the reference project:

| Byte | Field | Confirmed command values | Notes |
|---|---|---|---|
| 16 | fan | `0x01/03/0B/0D/0F/11/13` = auto/quiet/low/med-low/mid/med-high/high | reference's `0x05/07/09` were WRONG; enum renumbered so `fan*2+1` stays correct |
| 17 | sleep | `profile*2+1` (`0x03/05/07/09`), off `0x01` | |
| 18 | mode | AUTO = `0x90` (index 4) | STATUS reports AUTO as nibble 5 — the two genuinely differ; supersedes the earlier inferred `0xB0` |
| 19 | temp | `2n+1`, whole °C | already matched the reference |
| 32 | vertical swing | `0xC0` swing / `0x40` fixed | matched builder |
| 33 | eco | `0x30` on / `0x10` off | was `0x34` |
| 33 | turbo | `0x0C` on / `0x04` off | was `0x5C` |
| 35 | mute | `0x30` on / `0x10` off | noted for when exposed |

**Horizontal-swing command: N/A — no motor on this unit** (confirmed 2026-07-07). The
reference-inherited encoding (byte32 bits4-5 + byte37=`0x14`) is dead code on the W41H1; never
sent because `RockLeftRight` is not advertised. Left in the builder only for a hypothetical
H-swing-capable variant.

### Open / uncatalogued

- The class **`0x1E`** link/heartbeat frame (28 B) is uncatalogued on the command/status path —
  see the [Recommission ("77")](#recommission-77--how-the-stock-firmware-does-it-firmware-re-f1)
  section below for its request/reply bitfields, which **are** decoded.
- **Combined command frames**: `hisense_build_command()` packs many fields into one frame.
  Multi-field frames are legal (the reference's `mode_*` frames are combined), but the
  companion bytes those set (`[31]/[32]/[35]`) aren't reproduced by this driver yet — bench-test
  one field at a time, then combined.
- **Turbo-off/eco-off**: only `HISENSE_FEATURE_NONE` (baseline `0x04`) is wired; the
  reference's explicit off bytes (`0x54`/`0x14`) aren't reachable. Confirm `0x04` clears an
  active turbo/eco state, or add explicit off-toggles.
- **`display_on=false`** maps to "leave alone" (`0x00`), not "explicit off" (`0x40`) — confirm
  the wanted behavior.
- **`sleep` level**: written as `0x00` = "don't touch" (was `0x01`, which forced sleep-level-0).
  Not yet exposed as a Matter field.
- **Poll interval** (10 s) is carried over from the DHT11 stub, not derived from the bus;
  tighten once bench timing is known (reference polls ~5 s).

### Timer (byte 30 / byte 31)

The on/off timer is **relative, not absolute** — the remote computes *time-remaining* against
its **own** clock and sends that; the A/C never knows wall-clock time (confirmed: the same
target time yielded different byte values when set at different times of day). The value is
**transient in the status stream** — it appears for only ~3 poll frames when the timer is
confirmed on the remote, then byte30/31 return to `0x00` (the unit latches the schedule
internally; the status frame only announces the *set event*). Byte30 = ON slot, byte31 = OFF
slot. The exact minute-encoding was **not** fully pinned (a clean 1-hour ON test read
byte30 = `0x0b`, byte31 = `0x01`; an earlier mixed test read `0x8b`/`0x15`) — pinning it would
need a controlled sweep of known durations, which is **not worth doing**: scheduling is handled
natively and far more flexibly by **Home Assistant automations** on the Matter climate entity,
so the onboard timer is not bridged.

## Matter ↔ RS-485 mapping

The module bridges Matter thermostat attributes to the A/C fields:

| Matter attribute | A/C concept |
|------------------|-------------|
| `SystemMode` / `Mode` | off / cool / heat / … (`ac_cool_heat`) |
| `LocalTemperature` | indoor temp sensor |
| `OccupiedCoolingSetpoint` | cool target |
| `OccupiedHeatingSetpoint` | heat target |
| fan / swing / eco extras | `ac_fan_*`, `ac_swing_*`, `ac_power_save`, … |

## Recommission ("77") — how the stock firmware does it (firmware-RE, F1)

Static RE of `dumps/w41h1_dump1.bin` (RTL8710C Thumb, XIP base `0x9b6d0000`). Answers "what does
the module receive on 77, and what does it send back after commissioning" **without** a bench sniff
(later bench-confirmed — see below). For the evergreen "how do I open the commissioning window"
how-to, see [`02-matter-local-control.md`](02-matter-local-control.md#opening-the-commissioning-window-reset).

### There is no dedicated "reset" frame — it's a request bit in the `0x1E` reply

The remote's *Horizon Airflow ×6 → "77"* is a **remote→mainboard** key sequence; the mainboard does
**not** emit a new frame class to the module. Instead it sets a **request bit in its reply to the
module's ~1 Hz num30 (class `0x1E`) heartbeat**. The module's `handle_num30_cmd_result`
(`0x9b6f2778`) loads the A/C reply's **`payload[4]`** (frame byte 17, the class-`0x1E` request
bitfield) into `r8` and tests: **[PROVEN]**

| `payload[4]` bit | mask | meaning (module action) | site |
|---|---|---|---|
| bit5 | `0x20` | smart-config / ESPTouch pairing path | `tst.w r8,#0x20` @ `0x9b6f27de` → `0x9b6f2946` |
| **bit3** | **`0x08`** | **reset / reconfigure → re-provision (this is the "77" recommission)** | `tst.w r8,#8` @ `0x9b6f27f4` → reset chain |
| bit0 | `0x01` | STORE: A/C hands the module 8/2 binding-token bytes (`memcpy 0x100096d0`) | `tst.w r8,#1` @ `0x9b6f2858` |
| bit6 | `0x40` | action code (`ubfx r4,r8,#6,#1`) | `0x9b6f27f0` |

(The `0x1D`/num29 sibling, checked first at `0x9b6f2782`, is the **OTA reboot** — `0x9b6fa510(6,1)`.)

On **bit3**: log → `hs_reset_wifi` (`0x9b6eceb4`) → **`enter_provisioning_reset` (`0x9b6f2358`)** →
`DelayUs(0x7d0`=2000 ms`)` → **reboot `0x9b6fa510(6,2)`**. So "77" = *enter provisioning, then reboot*.

### `enter_provisioning_reset` (`0x9b6f2358`) — the exact state writes **[PROVEN]**

```
wifi_set_connect_status(3)   ; 0x9b6f0424  -> disconnected
wifi_set_control_status(0)   ; 0x9b6f0470
wifi_set_wifi_prov_status(1) ; 0x9b6f02f0  -> ENTER PROVISIONING
wifi_set_work_status(5)      ; 0x9b6f0348
wifi_set_wifi_route_status(0); 0x9b6f03a0
report_restart()             ; 0x9b6f7ce0  -> pushes the new state onto the next 0x1E
b.w commissioning_entry      ; 0x9b6eef3c
```
Then the caller reboots (`0x9b6fa510(6,2)`); the module comes back up with Wi-Fi creds cleared and
in provisioning mode → BLE Matter commissioning window open. Wi-Fi/creds erasure + the actual Matter
window-open happen across that reboot (in the connectedhomeip provisioning path), not inline.

### What the module SENDS BACK to the A/C — the outbound `0x1E` (builder `0x9b6f225c`) **[PROVEN]**

The module reports its own commissioning/Wi-Fi state in **every** ~1 Hz `0x1E` it sends. Same byte
offsets, opposite meaning to the inbound request bits:

```
wire[16]=payload[3] = 0x80 | (!recv_num30<<6) | (connect<<4) | (control<<2)
wire[17]=payload[4] = 0x80 | (selftest<<5) | (scan<<4) | (prov<<3)   ; prov here = OUR prov_status
wire[18]=payload[5] = (work<<5) | (route<<1)        ; only if AC-proto-ver<=0x214
wire[22]=payload[9] |= 0x40                          ; always
```
- **During pairing** (right after 77): `prov=1` → `payload[4]` bit3 set outbound; `connect=3` →
  `payload[3]` bits4+5.
- **After commissioning + IP** (`wifi_server_main` got-IP branch `0x9b6fc0b6`: work=1,route=1,prov=0,
  connect=0): `payload[4]` bit3 clears, `payload[5]` = `work@b5|route@b1` (0x22), `payload[3]` = 0xB0,
  `payload[9]` bit6 (0x40) set. **That combination is the on-the-wire "commissioned & online" signal**
  the A/C reads to update its display/state.

### Implication for our custom firmware (F1 implementation, no sniff needed to start)

1. In our `handle_num30`-equivalent, when the A/C reply's `payload[4]` has **bit3 (0x08)** set (and/or
   bit5 0x20), trigger a factory re-commission: erase the Matter fabric(s) + Wi-Fi creds and
   `OpenBasicCommissioningWindow()` (or reboot into commissioning). This is what stock does; our fabric
   is connectedhomeip, so use its APIs rather than the stock Wi-Fi state machine.
2. Keep reporting our commissioning state back in the outbound `0x1E` (prov bit while uncommissioned;
   `work`/`route` + byte[9] `0x40` once joined) so the A/C's "77"→pairing→connected UX still works.

**Hardware-confirmed:** pressing "77" makes the A/C's inbound `0x1E` reply set
**`payload[4] = 0x20` (bit5, smart-config)** — NOT bit3 (`0x08`) as the static RE above inferred.
It is a **one-frame pulse**: `0x00 → 0x20` for a single ~1 Hz reply, then back to `0x00`
(re-pulses on each press). Our driver keys on `0x08|0x20` (edge-triggered; the
`s_recommission_pending` guard collapses repeat pulses into one window), so "77" → bus `0x20` →
`matter_driver_on_recommission` → `OpenBasicCommissioningWindow` advertises a commissioning beacon
(disc 3840) **while staying commissioned** (fabric not wiped). **[PROVEN on HW.]** Still open: the
fabric-**swap** on a real re-pair, and whether `hisense_send_exit_77()` clears the A/C's "77"
display after the window times out.

**Hardware-confirmed (outbound):** the A/C lights **"77" iff the module reports prov_status=1**
in its OUTBOUND `0x1E` (`payload[4]`/byte17 bit3 = `0x08`) — injecting the bit shows "77", clearing
it clears the display. The firmware sets it while the commissioning window is open
(`hisense_set_provisioning(true)`) and clears it on pair/timeout, so the A/C "77" display tracks
our pairing state. **[PROVEN on HW.]**

## Capturing the real frames

The exact byte layout has to come from the wire. Use
[`tools/sniff.py`](../tools/sniff.py):

- Tap **UM3352 pin 4 (DI)** → USB-TTL RX, **pin 5 (GND)** → GND, to capture what the
  dongle **sends** to the A/C. Tap **pin 1 (RO)** for the A/C's replies. Or tap the A/B
  pair with a USB-RS485 adapter to see both directions.
- The dongle must be **plugged into the A/C and powered** (the transceiver is 5 V from the
  A/C; on the bench it's dead). If tapping RO/DI at 5 V logic, drop it to 3.3 V with a
  divider before a 3.3 V-only adapter.
- Trigger known actions (power, mode, temp ±, fan, swing) and note the timing to map bytes
  → commands.

## Existing implementations to build on

The A/C-side Hisense protocol is already implemented by community ESPHome projects (target
the older `AEH-W4A1` but speak the same A/C bus, so a good starting point):

- `pslawinski/esphome_airconintl` — drop-in ESPHome replacement for the Hisense/Aircon
  International dongle (same 4-pin port, RS-485, 5 V).
- `akrabi/hisense_ac_esphome`
- `zoffypal/esphome_hisence_ac`

Plan: flash one of these to an ESP32 + MAX485, wire to the A/C's 4-pin port
(A/B/5V/GND), and reconcile any W41H1-specific differences against the frames captured
with `sniff.py`.
