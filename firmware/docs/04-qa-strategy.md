# QA & testing strategy

How this firmware is validated without the physical A/C or the chip in hand, and where hardware is
still required. The commands and the list of tests are in
[`docs/guide/Testing-and-QA.md`](../../docs/guide/Testing-and-QA.md). This page is the reasoning.

The protocol is reverse-engineered, so the ground truth is our own sniffed frames. Every layer
below is built on that: captured bytes become golden vectors, and nothing is derived from another
model's documentation.

## The layers

| # | Layer | Proves | HW? |
|---|---|---|---|
| 1 | Host unit tests with a mocked HAL | builders and parsers produce and accept the right bytes, and mapping and retry decisions are right | no |
| 2 | Device simulation, golden-frame replay | the command and status loop against a modeled A/C | no |
| 2b | Host end-to-end | the hub and the entities do what the pure layers decided: entity states over the native API, bytes on the wire | no |
| 3 | Build checks | the YAML and schemas are valid on every board, and the stock-module image is laid out for its flash | no |
| 4 | Bench | a flashed board against the simulator on a USB adapter | a board |
| 5 | HIL | timing, RF, the real A/C | yes |

## Layer 1: host unit tests

Two bodies of code are tested on the host.

**The reference driver** (`firmware/test/reference-driver/`) is the project's first bus driver. It
ran on real units for months under the retired Matter builds, and it is frozen. It compiles
against a stub of the Realtek HAL (`firmware/test/hal_stub.h`, `stubinc/`). `test_codec.cpp` holds
it to captured frames, byte for byte.

**The ESPHome component's protocol sources** (`hisense_protocol.*`, `hisense_map.h`,
`hisense_bus.*`) have no ESPHome dependency by design, so they compile on the host too.
`test_esphome_codec_parity.cpp` runs every builder and parser of the port and of the reference
over the same inputs (exhaustive where the domain is small, the full command cartesian product,
seeded random frames for the parsers) and compares the results field by field. That comparison is
why a second copy of the codec is safe.

Things a codec test must include, each learned from a miss:

- a real **combined multi-field** command frame. The node always sends one, never a single-field
  frame.
- an actual **doubled-`0xF4`** stuffing round trip. A frame whose checksum never contains `0xF4`
  never exercises the stuff and un-stuff path.
- **fixpoint** checks on every map pair (status to command and back), which guard against a
  readback being sent out again as a new command.

## Layer 2: device simulation

`firmware/test/virtual_ac.py` models the indoor unit: it answers the handshake, serves 160-byte
status frames and applies command frames to its state. `run_tests.sh` checks it against the
independent frame decoder in `reverse-engineering/tools/`, in both directions, so the simulator
and the decoder cannot be wrong in the same way without a third party (the captures) noticing.

`test_esphome_bus.cpp` runs the component's bus scheduler against a simulated unit:
reply windows, hold-off, link loss, re-sends.

### Host end-to-end

Layer 1 stops below the hub. The hub and the entity classes (`hisense_ac.cpp`,
`hisense_climate.cpp`, `hisense_switch.cpp`, `hisense_select.cpp`) need ESPHome, and for a long
time nothing ran them but a board. ESPHome's `host` platform builds the whole component as a Linux
program, so `firmware/test/host_e2e/` does: the entity package unchanged, a pseudo-terminal for
the UART, the virtual A/C on the other end, and `aioesphomeapi` where Home Assistant would be.

Each check is one of two kinds: what an entity shows, or which frame the A/C model received.
Expected frames are written out in the test from the protocol document, not asked of the code
under test. The test reads nothing of the hub's internals, so it survives a restructuring of the
hub and fails when the behaviour moves.

What it costs is time. The host platform has no clock to turn forward, and the hub's rules are in
seconds (a 4 s hold-off, 10 s between special-mode writes, three sends before a command is given
up). The scenarios run side by side, each on its own node, which brings the default set to about
a minute. That is the reason the standing rule below still holds: a decision tested here takes
seconds per case, the same decision as a pure function takes microseconds and can be tested
exhaustively.

Two limits. The model answers at once, so nothing here says anything about DE timing, baud rate
or a slow unit. And the model is ours: it proves the firmware against our reading of the unit,
which the captures and the hardware test have to keep honest.

## Layer 3: build checks

`esphome config` validates every board file and every platform schema of the component without a
toolchain, in seconds. CI also compiles all three stock-module layouts for real, because that is
the one platform with a different compiler and integer widths, and because the build itself checks
that the image's slot addresses are those of the layout the file names.

ESPHome's own CI scripts run against the component too (`esphome-upstream-check.sh`): on the
pinned release in every pull request that changes the firmware, and on their development branch
every night.

## Layer 4: bench

`dev.py bench esphome` flashes a board and runs it against `virtual_ac.py` over a USB-TTL or
USB-RS485 adapter. It proves the wiring, the UART and DE timing on the real chip before any wire
touches the appliance.

## Layer 5: HIL

`firmware/test/hil_esphome_actuation.py` drives a node on a real A/C over the native API and
checks two properties for every control:

1. **Actuation.** The command lands and the entity changes state. A command that matches the
   current value is a no-op that proves nothing, so every step forces a real change.
2. **No collateral.** Most controls ride the combined command frame, which is rebuilt from the
   command shadow. If the shadow drifts, driving one control retunes the A/C. So after each step
   everything else must be where it was.

It also reads the bus counters: a readback that would pass on a dead bus is rejected, every wait
watches `Command retries` and waits longer when it moved, and the run ends with the counter
deltas. Retries are listed and are fine. A rise in `Failed commands` is a failed check. The
restore step leaves the unit off when the run itself switched it on (an earlier version put the
mode back after the power-off and left two units running, #175).

It is deliberately **not** part of `run_tests.sh`, which stays hardware-free. The panel display is
not asserted: the A/C reports no display state, so whether it lit remains a human observation.

For byte-level questions on a live bus there are two more tools: a tap decoded by
`reverse-engineering/tools/decode_ac_frames.py --port <tap>`, and `firmware/esp32-recon/`, a
console on the wire with a listen-only mode (`HIL-RUNBOOK.md` there).

## Standing rule: decisions do not live in glue

The ESPHome hub (`hisense_ac.cpp` and the entity classes) needs ESPHome's headers, so it cannot run
in Layer 1. It runs in the host end-to-end test, at the speed of its own timers. Any **decision** it makes (is this my own echo, should the shadow sync, is this command
confirmed, which special-mode frame goes next) is a pure function in `hisense_map.h` with a host
test. The glue only moves values.

This rule comes from a bug that escaped review: on an early build, a status readback was written
back as a command, which produced another readback, three to six times per user action. It lived
in a layer no host test could reach and was caught on hardware.

One known gap: `test_esphome_confirm.cpp` repeats the hub's command paths to test them, and that
copy can drift from the hub. The host end-to-end test runs the hub's own paths for the main cases
(a command ignored, lost, taken on a re-send, overtaken at the remote), not for every ordering the
pure test covers.
