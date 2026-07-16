# QA & testing strategy (W41H1 Matter firmware)

How this firmware is validated **without depending on the physical A/C or the
chip being in hand**, and where hardware is still required. It follows the
standard embedded/IoT test pyramid — mock the hardware, test each layer at the
cheapest meaningful level, keep real hardware as a thin top gate — adapted to a
**reverse-engineered** protocol where our own sniffed frames are the ground truth.

## The layers

| # | Layer | What it proves | Needs HW? | Ours |
|---|-------|----------------|-----------|------|
| 1 | Host unit / codec (mocked HAL) | encoder/parser produce/accept the right bytes | no | `test/test_codec.cpp` + `test/hal_stub.h` |
| 2 | Device sim + record/replay | full command↔status loop against a modeled A/C | no | `test/virtual_ac.py` + sniffed golden frames |
| 3 | Full-firmware sim (Renode) | the **real** driver code on the **real** core | no | `test/renode/` (scaffold) |
| 4 | Matter protocol (mapping test + loopback OTA sim) | cluster↔command mapping; OTA convert plumbing | partly | `test_matter_map` + `test/sim_ota_convert.sh` |
| 5 | HIL — hardware in the loop | timing, RF, the real A/C | yes | flash + HA + DI-tap sniffer |

Run layers 1–2 now: `firmware/test/run_tests.sh` (host-only, CI-friendly, exits
non-zero on any failure).

## Layer 1 — host unit tests with a mocked HAL

The bottom of the pyramid: mock the hardware-abstraction layer so driver logic
runs on a PC. `hal_stub.h` mocks the Ameba `serial_*` + FreeRTOS API; the *real*
`hisense_rs485.cpp` links against it and `test_codec.cpp` asserts every command
byte and every parsed status field against the **hardware-confirmed golden
values** (AUTO→`0x90`, fan `0x0B..0x13`, eco `0x30`, turbo `0x0C`, direct-°C
temps, the flag bits, checksum, F4-stuffing, and rejection of malformed frames).
36 assertions, milliseconds, no A/C. This is the regression gate for every codec
change.

## Layer 2 — device simulation + golden-frame record/replay

For a reverse-engineered protocol this is the highest-value technique: the frames
we captured off the real W41H1 are the **golden master**, and `virtual_ac.py` is
a software model of the indoor unit that speaks the validated bus — it answers
status-request polls with a 160-byte status frame and applies command frames to
its state. `run_tests.sh` round-trips it against `decode_ac_frames.py`
(simulator encodes → decoder reads back the same state; driver golden command
bytes → simulator mutates correctly). It also runs **interactively**: point it at
a PTY (`--pty`), a real serial port (`--port`, e.g. a USB-TTL loopback or the
DI/RO tap for on-hardware cross-checks), or a TCP socket (`--connect`, for Renode).

## Layer 3 — full-firmware simulation (Renode)

Renode runs unmodified Cortex-M firmware against emulated peripherals in CI — the
only no-chip way to exercise the driver's RX-IRQ/bus-task/TX-queue code on the
actual core. `test/renode/` has the platform scaffold (Cortex-M33 + memory map +
UART0 @ `0x40003000`) and a run script that bridges UART0 to `virtual_ac.py`.
**Recommended first target: a minimal bare-metal driver-test ELF**, not the full
Matter image (booting `flash_is.bin` needs the ROM/XIP/PMU modeled — see
`test/renode/README.md`).

## Layer 4 — Matter protocol QA

Two halves, one buildable without hardware and one needing a running device:

**4a — mapping unit test (built, no chip).** The Matter↔A/C translation is
extracted into a pure header (`firmware/src/rs485-driver/matter_aircon_map.h`)
that `matter_drivers.cpp` *actually uses*, and `test/test_matter_map.cpp` asserts
the whole table **end-to-end to the wire**: a Matter attribute value → mapping →
`hisense_build_command` → the hardware-confirmed byte (Auto→`0x90`, SpeedSetting
6→`0x13`, RockSetting up-down→byte32 `0xC0`, setpoint 2200→byte19 `0x2D`, …), plus
the reverse (status→SpeedCurrent/PercentCurrent/SystemMode). 39 assertions, host,
no chip — the offline equivalent of a chip-tool write. In `run_tests.sh`.

**4b — chip-tool / CSA Test Harness (needs a device).** Matter's official QA/cert
tool is the CSA **Test Harness** (Raspberry-Pi, PICS-driven, chip-tool underneath);
because the Phase-0 image uses CHIP default test creds (`0xFFF1`), it passes the
TH baseline and chip-tool commissions/controls it out of the box. For automated
regression, script **chip-tool** attribute reads/writes as YAML against the
commissioned device and assert the cluster surface behaves. (Add once flashed, or
run against a Linux CHIP app if you want it fully hardware-free.)

**4c — OTA conversion sim (built, no chip).** `firmware/test/sim_ota_convert.sh`
runs connectedhomeip's Linux `chip-ota-requestor-app` (device stand-in) +
`chip-ota-provider-app` + `chip-tool` on loopback: it commissions both, serves our
`.ota`, announces the provider, and asserts the real OTA sequence
(`QueryImage → UpdateAvailable → BDX → ApplyUpdateRequest`). This validates the
`.ota` packaging + OTA transport (the F2 stock→custom plumbing) with zero hardware;
it does not exercise the AmebaZ2 image processor / real boot.

## Layer 5 — HIL (the thin top, needs the chip)

Flash `built-images/flash_rac-stock-v1.bin` (Phase 0/1) via the CH341A clip,
commission in Home Assistant, then use the **DI-tap sniffer** as the hardware
assertion: `decode_ac_frames.py --port <tap>` confirms the firmware puts the
*correct bytes on the wire* for each Matter action, against the real A/C. This is
the only layer that covers RF, real bus timing, and the physical unit.

## Standing methodology — glue code is host-untestable as written

`matter_drivers.cpp` (the SDK-side Matter glue) needs CHIP headers to compile, so it cannot run
in Layer 1: echo-suppression, shadow-sync hold-off, and the re-entrant `Set()`-callback loop have
**zero** host coverage — only the pure functions in `matter_aircon_map.h` / `hisense_rs485.cpp`
are exercised by Layers 1–2. This is exactly how the downlink→readback→uplink feedback-loop bug
(HIL v5: HA setpoint/mode writes bounced back as 3–6 re-commands) slipped through review and had
to be caught on hardware instead of on a laptop. Standing rule for new glue code:

1. Extract any glue **decision** ("is this write my own downlink echo?", "should the shadow
   sync?") into a pure function in `matter_aircon_map.h`, then unit-test it there — don't leave
   decisions living only inside CHIP callback bodies.
2. Add **fixpoint/idempotence** tests on map pairs — they guard the whole feedback-loop bug class:
   `hisense_mode_to_matter(matter_mode_to_hisense(m))==m`, `matter_setpoint_to_c(sp*100)==sp`
   (16..32), `rock_to_swing(swing_to_rock(v,h))==(v,h)`.
3. Test a real **combined multi-field** command frame (the driver always sends one, never a
   single-field frame) and an actual **doubled-`0xF4`** stuffing round-trip — picking a frame
   whose checksum never contains `0xF4` means the stuff/un-stuff path is never exercised.

(Promoted from the 2026-07 bug-hunt review; the specific findings that motivated it are tracked
as GitHub issues.)

## Why we're well-positioned

The reverse-engineering already produced the two assets that are usually the
hardest part of RE-driver QA: a **validated codec** and a **golden corpus of real
frames**. That's what lets Layers 1–3 run entirely without the A/C or the chip,
so development and regression continue while hardware is unavailable — hardware
is needed only for the final HIL gate.

## References

Grounded in standard practice: mock-the-HAL host testing
([ElectronVector](http://www.electronvector.com/blog/when-you-want-to-unit-test-abstract-the-hardware)),
device-driver test automation + device simulation
([jumperiot](https://medium.com/jumperiot/how-to-automate-device-drivers-testing-in-iot-embedded-software-projects-44c164158f43)),
Renode full-firmware sim in CI
([Antmicro](https://antmicro.com/blog/2024/12/introducing-renode-vscode-extension),
[MDPI study](https://www.mdpi.com/2673-4591/79/1/52)), and the Matter Test Harness
([project-chip/certification-tool](https://github.com/project-chip/certification-tool)).
