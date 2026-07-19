# Testing and QA

Validate this firmware **without the physical A/C or the chip in hand**, and see where
hardware is still required. Full strategy: `firmware/docs/04-qa-strategy.md`.

← back to [Home](Home) · siblings: [Repo Map and Build Pipeline](Repo-Map-and-Build-Pipeline) ·
[Protocol Overview](Protocol-Overview)

---

## The 5-layer pyramid

Standard embedded/IoT approach (mock the hardware, test each layer at the cheapest meaningful
level, keep real hardware as a thin top gate), adapted to a **reverse-engineered** protocol
where our own sniffed frames are the ground truth.

| # | Layer | Proves | HW? | Ours |
|---|---|---|---|---|
| 1 | Host unit / codec (mocked HAL) | encoder/parser produce/accept the right bytes | no | `test/test_codec.cpp` + `test/hal_stub.h` |
| 2 | Device sim + record/replay | full command↔status loop vs a modeled A/C | no | `test/virtual_ac.py` + sniffed golden frames |
| 3 | Full-firmware sim (Renode) | the **real** driver on the **real** core | no | `test/renode/` (scaffold, see below) |
| 4 | Matter protocol (mapping test + OTA sim) | cluster↔command mapping; OTA plumbing | partly | `test_matter_map` + `test/sim_ota_convert.sh` |
| 5 | HIL, hardware in the loop | timing, RF, the real A/C | yes | flash + HA + DI-tap sniffer |

## Running the host tests

Layers 1–2 (and the Layer-4 mapping test) run host-only, CI-friendly, exit non-zero on failure:

```
firmware/test/run_tests.sh
```

It builds and runs three things:
- **Layer 1a: codec golden regression** (`test_codec.cpp`): 36 assertions of every command
  byte and parsed status field against hardware-confirmed golden values (AUTO→`0x90`, fan
  `0x0B..0x13`, eco `0x30`, checksum, F4-stuffing, malformed-frame rejection).
- **Layer 1b / 4a: Matter↔A/C mapping** (`test_matter_map.cpp`): 39 assertions end-to-end to
  the wire: a Matter attribute value → mapping → `hisense_build_command` → the confirmed byte,
  plus the reverse (status → `SpeedCurrent`/`SystemMode`). The offline equivalent of a chip-tool
  write.
- **Layer 2: virtual A/C round-trip**: `virtual_ac.py` (a software model of the indoor unit)
  encodes → `decode_ac_frames.py` reads back the same state; driver golden command bytes →
  simulator mutates correctly.

Run a single layer by compiling its `.cpp` directly, e.g.:

```
g++ -std=c++11 -Wall -Istubinc -I. -I../src/rs485-driver \
    test_codec.cpp ../src/rs485-driver/hisense_rs485.cpp -o test_codec && ./test_codec
```

(swap `test_codec.cpp` → `test_matter_map.cpp` for Layer 1b).

## The virtual A/C simulator

`virtual_ac.py` speaks the validated bus: it answers status-request polls with a 160-byte
status frame and applies command frames to its state. Beyond the round-trip self-check it runs
**interactively**: point it at a PTY (`--pty`), a real serial port (`--port`, e.g. a USB-TTL
loopback or the DI/RO tap for on-hardware cross-checks), or a TCP socket (`--connect`, for
Renode). Develop against it before touching the real bus.

## Layer 4 extras

- **4b: chip-tool / CSA Test Harness** (needs a device): because the image uses CHIP default
  test creds (`0xFFF1`), chip-tool commissions/controls it out of the box.
- **4c: OTA conversion sim** (built, no chip): `firmware/test/sim_ota_convert.sh` runs CHIP's
  Linux `chip-ota-requestor-app` + `chip-ota-provider-app` + `chip-tool` on loopback,
  commissions both, serves our `.ota`, and asserts the real OTA sequence
  (`QueryImage → UpdateAvailable → BDX → ApplyUpdateRequest`). Validates the `.ota` packaging +
  OTA transport with zero hardware; it does **not** exercise the AmebaZ2 image processor / real
  boot.

## Renode (Layer 3): scaffold, not runnable as-is

`firmware/test/renode/`
is a **spike, NOT runnable as-is**: `hisense_qa.resc` references a `driver_test.elf` that
isn't built or committed, and RTL8710C is not a built-in Renode platform. What's there: an
`rtl8710c.repl` platform (Cortex-M33 + the linked memory map + UART0 @ `0x40003000`) and a run
script that bridges UART0 to `virtual_ac.py` over TCP. The recommended first target is a
**minimal bare-metal driver-test ELF** (links `hisense_rs485.cpp` + a startup + a thin UART
shim), *not* booting the full Matter image (that needs ROM/XIP/PMU/BLE modeled, a multi-day job).
Until then, use Layers 1–2 + the OTA sim for turnkey no-hardware coverage. See
`firmware/test/renode/README.md`.

## HIL gate: the thin top

Layer 5 is the only layer covering RF, real bus timing, and the physical unit: flash via the
CH341A clip, commission in Home Assistant, then use the **DI-tap sniffer**
(`decode_ac_frames.py --port <tap>`) as the hardware assertion. It confirms the firmware puts
the *correct bytes on the wire* for each Matter action against the real A/C.

**HIL-gate philosophy:** there is usually **no chip access in-session**, so do all
chip-independent work and defer flash/commission. Layers 1–4 run without the A/C or chip
precisely because the reverse-engineering already produced the two hardest RE-driver-QA assets:
a **validated codec** and a **golden corpus of real frames**.

## Standing rule: glue code is host-untestable as written

`matter_drivers.cpp` needs CHIP headers to compile, so it can't run in Layer 1: echo-suppression,
shadow-sync hold-off, and the re-entrant `Set()`-callback loop have **zero** host coverage. This
is how the downlink→readback→uplink feedback-loop bug slipped review and had to be caught
on hardware. So for new glue code:

1. Extract any glue **decision** ("is this write my own echo?", "should the shadow sync?") into
   a pure function in `matter_aircon_map.h` and unit-test it there.
2. Add **fixpoint/idempotence** tests on map pairs
   (`hisense_mode_to_matter(matter_mode_to_hisense(m))==m`, etc.); they guard the whole
   feedback-loop bug class.
3. Test a real **combined multi-field** command frame and a **doubled-`0xF4`** stuffing
   round-trip (a frame whose checksum never contains `0xF4` never exercises the stuff path).

Specific findings that motivated this are tracked in the project's issue tracker.
