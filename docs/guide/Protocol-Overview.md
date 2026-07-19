# Protocol Overview

A developer's orientation to the Hisense A/C RS-485 bus protocol. Enough to know what the
frames look like and where the truth lives. This is **not** a byte reference. The bible is
`reverse-engineering/docs/03-rs485-ac-protocol.md`.

← back to [Home](Home) · siblings: [Repo Map and Build Pipeline](Repo-Map-and-Build-Pipeline) ·
[ESP32 Replacement Build](ESP32-Replacement-Build)

---

## ⚠️ The protocol is hardware-validated: trust the repo values

Framing, checksum, and every byte offset were sniff-confirmed against the **real W41H1 bus**
(command + status) by remote-button correlation. The offsets originally borrowed from the
older `AEH-W4A1` reference project were **wrong in several places** and are corrected in this
repo. **Do not re-derive or "fix" them from reference or other-model docs.** The executable
source of truth is the driver: `firmware/src/rs485-driver/hisense_rs485.cpp`
(`hisense_parse_status` / `hisense_build_command`) +
`firmware/src/rs485-driver/matter_aircon_map.h`.

## Physical layer

| | |
|---|---|
| Transport | RS-485, **9600 8N1** (auto-probed against 115200 by stock; the link is 9600) |
| Pins (module) | UART0 **TX = PA_14**, **RX = PA_13**; **DE = PA_17** |
| Role | Wi-Fi module is **bus master / sole initiator**; the A/C mainboard is a pure slave/responder |

Two facts about DE differ between the RE docs; both are correct in context. The A/C-protocol
bible (doc 03) notes the transceiver on the *stock module* is effectively auto-direction (no
`gpio_*` in the UART path it first analysed), while the deeper stock-firmware trace (doc 10)
proves the stock driver **does** bring up a DE line on **PA_17** and toggles it around every TX
(5 ms pre-guard, 1 ms post-guard). The ESP32 port and the corrected AmebaZ2 driver drive DE.
Bring-up ordering matters a lot, covered below.

## Frame envelope

The module↔A/C frame is `F4 F5` … `F4 FB` delimited. The **full** envelope the stock firmware
uses (doc 10 §3.2):

```
F4 F5 | DIR | CTRL | LEN | 00 00 | seqHi seqLo | FE 01 | 00 00 | CLASS | payload… | CKSUM | F4 FB
```

| Field | Meaning |
|---|---|
| `F4 F5` / `F4 FB` | start / end markers |
| `DIR` | `0x00` module→A/C, `0x01` A/C→module (RX rejects `!=1`) |
| `CTRL` | `0x40` normal. bit6 → 2-byte checksum; bit5 → 16-bit BE length; bit0 → extra pre-LEN byte |
| `LEN` | payload count; **total frame = LEN + 9** (this fixed the old fixed-82 assumption that dropped every 160-byte frame) |
| `seqHi/Lo` | the A/C's **device-type / sub-type**, `00 00` until the DevType reply answers, that unit's code after ([below](#the-seqhilo-bytes-are-a-device-type-not-a-session-token)) |
| `CLASS` | message class (below) |
| `CKSUM` | **big-endian running SUM** over the payload, width from `CTRL & 0xC0` (`0x40` → 2 bytes). **Not XOR, not a CRC.** |

**Checksum + byte-stuffing:** all `ac_*` frames use `CTRL=0x40` → 2-byte checksum. A checksum
byte equal to the `0xF4` marker is **escaped by doubling it** (`… F4 → … F4 F4 …`). The
builder stuffs on TX; the bus task un-stuffs on RX. A frame whose checksum never contains
`0xF4` never exercises this path, a known test trap (see [Testing and QA](Testing-and-QA)).

## The `seqHi/Lo` bytes are a device-type, not a session token

Read this before touching the framing. It cost us a working A/C.

Those two envelope bytes look like a per-session token, and the stock disassembly can be read that way. They are the attached unit's **device-type / sub-type**: a static identifier for that model. The module sends `00 00` until the A/C answers the DevType (`0x0A`) handshake, then sends the code that reply carries, on every later frame.

Read the code from the DevType reply's **inner payload** `[3]/[4]` (frame bytes `[16]/[17]`). Do **not** read that reply's envelope `[9]/[10]`.

Measured on a live unit, both values from the same `0x0A` reply:

```
device-type  inner [3]/[4] : 01 01   <- send this
session tok  envel [9]/[10]: 00 00   <- NOT this
```

We shipped a build that stamped the envelope `[9]/[10]` instead. It read `00 00`, treated that as "no token yet", and sent `00 00` on every frame after the handshake. The A/C rejected all of them: the link never came up, no status arrived, and the unit needed an OTA to recover.

A `0x66` **status** reply *does* carry `01 01` at `[9]/[10]`, which is what makes the token reading look right. The DevType reply is the one that differs, and it is the frame that matters.

`01 01` is one model's code. A different Hisense model answers with a different code, which is why the driver learns it rather than hardcoding it. Details and the per-model capability map: `reverse-engineering/docs/11-model-capability-map.md`.

## Message classes

| Class | Name | Purpose |
|---|---|---|
| `0x0A` | DevType | **First** handshake; identify device type, open session. Must complete before status/command frames are honored. |
| `0x07` | Version | Exchange MCU/proto version; gates legacy-compat fields |
| `0x1E` (num30) | Link/status heartbeat | ~1 Hz. Module reports its Wi-Fi/commissioning state; A/C replies with **request bits**; this is the "77" channel (below) |
| `0x66` sub `00` | Status poll | Live A/C state → the 160-byte status reply |
| `0x66` sub `40` | ProductType | Model code + decoded feature bit-flags |
| `0x65` | Control / set | The command write (fan/mode/temp/…) |
| `0x1D` (num29) | Reconfig / OTA | A/C demands re-provisioning / OTA reboot |

> Provenance: only the **envelope** is firmware-confirmed from the stock binary. The
> stock module treats temps/fan/compressor as a bit-packed + transparent block it forwards to
> cloud without field-decoding. The **byte-per-field** maps below were confirmed a different
> way: on the live bus, by toggling each control and diffing the reply. Both are in doc 03.

## Status vs command maps (summary)

Two directions, two different frames. Full tables (with the confirmed hex per field) in doc 03.

**Status: 160-byte class-`0x66` reply** (hardware-confirmed by remote-button diffing):

| Byte | Field |
|---|---|
| 16 | fan speed (six speeds, more than the reference's three) |
| 18 | mode nibble (AUTO = 5 in *status*) |
| 19 | setpoint °C (direct integer) |
| 20 | room temp °C (direct integer, **not** the W4A1 `(raw-32)*0.5556`) |
| 35 | flags1: vswing / hswing / aux-heat / eco / turbo |
| 36 | flags2: purify / mute |
| 41–45 | compressor freq + outdoor/coil temp (telemetry) |

**Command: 50-byte class-`0x65` frame** (DI-tap-confirmed on the stock dongle):

| Byte | Field | Note |
|---|---|---|
| 16 | fan | ref's `0x05/07/09` were WRONG; `fan*2+1` |
| 18 | mode | AUTO = `0x90` in *command*, which differs from status nibble 5 |
| 19 | temp | `2n+1`, whole °C |
| 32 | vertical swing | `0xC0` swing / `0x40` fixed |
| 33 | eco (`0x30`/`0x10`) / turbo (`0x0C`/`0x04`) | ref values were wrong |

**Horizontal swing is N/A**: no motor on this unit; the bit toggles but nothing moves. Only
vertical swing is advertised over Matter. Left in the builder as dead code for a hypothetical
H-swing variant.

## Matter ↔ RS-485 mapping

The bridge translates Matter thermostat/fan attributes to A/C fields. The mapping is
extracted into the pure, host-tested `firmware/src/rs485-driver/matter_aircon_map.h`
that `matter_drivers.cpp` uses.

| Matter attribute | A/C concept |
|---|---|
| `SystemMode` / `Mode` | off / cool / heat / auto |
| `LocalTemperature` | indoor temp sensor |
| `OccupiedCoolingSetpoint` / `OccupiedHeatingSetpoint` | cool / heat target |
| fan / swing / eco extras | `ac_fan_*`, `ac_swing_*`, `ac_power_save`, … |

## The "77" recommission mechanism

There is **no dedicated reset frame**. The remote's *Horizon Airflow ×6 → "77"* is a
remote→mainboard key sequence; the mainboard signals the module by setting a **request bit in
its reply to the ~1 Hz `0x1E` heartbeat**. Static RE inferred bit3 (`0x08`); the **hardware
truth** is that "77" pulses **`payload[4] = 0x20` (bit5, smart-config)** for a single ~1 Hz
frame, then clears. Our driver keys on `0x08|0x20` (edge-triggered, repeat pulses collapsed by
a pending-guard) → `matter_driver_on_recommission` → `OpenBasicCommissioningWindow` advertises
a beacon **while staying commissioned** (fabric not wiped). Conversely, the A/C lights "77" iff
the module reports `prov_status` in its **outbound** `0x1E`, so the display tracks our pairing
state. Both directions are **proven on hardware**. Full bitfields + the outbound builder in
doc 03's "Recommission" section.

> ⚠️ **Bring-up ordering.** Initialize the DE GPIO (PA_17) and UART from a **task**,
> after the RTOS scheduler and SDK HAL are up, never from a static ctor or pre-scheduler
> Matter init. Doing it early faults through unpopulated HAL tables (this was the v4 boot
> fault). The exact stock ordering (mutex first, persistent `gpio_t`, DE toggle sequence) and
> the root-cause analysis are in
> `reverse-engineering/docs/10-stock-fw-init-and-comms.md`.

## The two deep references

- **`reverse-engineering/docs/03-rs485-ac-protocol.md`** is the protocol bible: full frame
  format, checksum, every confirmed byte map, telemetry block, timer encoding, and the "77"
  bitfields.
- **`reverse-engineering/docs/10-stock-fw-init-and-comms.md`** covers stock-firmware init,
  bus bring-up, DE-GPIO PA_17 ordering, the link state machine, the transaction primitive,
  and the DE-vs-flash pin analysis.
