# Testing and QA

Validate this firmware **without the physical A/C or the chip in hand**, and see where
hardware is still required. Full strategy: `firmware/docs/04-qa-strategy.md`.

← back to [Home](Home) · siblings: [Repo Map and Build Pipeline](Repo-Map-and-Build-Pipeline) ·
[Protocol Overview](Protocol-Overview)

---

## The layers

Standard embedded approach (mock the hardware, test each layer at the cheapest meaningful level,
keep real hardware as a thin top gate), adapted to a **reverse-engineered** protocol where our own
sniffed frames are the ground truth.

| # | Layer | Proves | HW? | Ours |
|---|---|---|---|---|
| 1 | Host unit tests | the codec, the mapping, the bus scheduler and the retry rules produce and accept the right bytes | no | `firmware/test/test_*.cpp` |
| 2 | Device simulation | the command and status loop against a modeled A/C | no | `firmware/test/virtual_ac.py` + sniffed golden frames |
| 2b | Host end-to-end | the hub and the entities themselves: what Home Assistant sees and which bytes reach the A/C | no | `firmware/test/host_e2e/` |
| 3 | Build checks | the YAML, every platform schema, and a real compile for the ESP32 boards and the stock module | no | `esphome config`, `esphome compile` in CI |
| 4 | Bench | the flashed node against the simulator on a USB adapter | a board | `dev.py bench esphome` |
| 5 | HIL, hardware in the loop | timing, RF, the real A/C | yes | `hil_esphome_actuation.py`, a tap on the bus |

## Running the host tests

```
firmware/test/run_tests.sh
```

It needs `g++` and `python3`, nothing else, and takes a few seconds. `dev.py test esphome` runs
it and then `esphome config`.

| Layer | Test | What it holds down |
|---|---|---|
| 1a | `test_codec.cpp` | the reference driver's builders and parsers against captured frames (golden bytes) |
| 1b | `test_reference_map.cpp` | the reference fan ladder, setpoint clamp, running state and power estimate, tied to the wire bytes |
| 1c | `test_esphome_map.cpp` | the reference ESPHome mapping: modes, fan names, presets, shadow sync |
| 1d | `test_esphome_codec_parity.cpp` | the component's codec (`hisense_protocol.*`, `hisense_map.h`) equals the reference driver: every builder and parser, byte for byte, over exhaustive and seeded random inputs |
| 1e | `test_esphome_bus.cpp` | the component's bus scheduler against a simulated A/C: timing, link loss, retries |
| 1f | `test_esphome_edge_cases.cpp` | shadow sync rules, boundaries, damaged byte streams |
| 1g | `test_esphome_confirm.cpp` | confirm and retry, power and mode in one frame |
| 1h | `test_esphome_uart_policy.cpp` | when the stock module's bus port is restarted |
| 2 | (inline in `run_tests.sh`) | `virtual_ac.py` and the frame decoder in `reverse-engineering/tools/` agree in both directions |
| 3 | `test_image_chain.py` | the AmebaZ2 image signing chain (`amebaz2_image.py`): walk, verify, re-sign, and the stale-trailer bug that bricked a unit |
| 4 | `test_dev_release.py` | `dev.py`: the conversion image re-signing, provider staging rules, the slot table against the layout files, the command line |

One caveat on Layer 1g: the test does not include the hub, it repeats the hub's glue, and that
copy can drift. The [host end-to-end test](#the-host-end-to-end-test) runs the same cases through
the hub itself.

Run a single layer by compiling its `.cpp` directly, from `firmware/test/`:

```
g++ -std=c++11 -Wall -Istubinc -I. -Ireference-driver \
    test_codec.cpp reference-driver/hisense_rs485.cpp -o test_codec && ./test_codec
```

### The reference driver

`firmware/test/reference-driver/` is the project's first bus driver, the one that ran on real
units under the retired Matter builds. It is frozen. The ESPHome component carries its own port of
the codec, because ESPHome's code rules and the old driver's platform contract cannot share a
file. Layer 1d is what makes a second copy safe: the port is compared with the hardware-validated
original on every push. Change the reference only to add a protocol fact both sides must agree on,
and say so in the commit.

## The virtual A/C simulator

`virtual_ac.py` speaks the validated bus: it answers status-request polls with a 160-byte
status frame and applies command frames to its state. Beyond the round-trip self-check it runs
**interactively**: point it at a PTY (`--pty`), a real serial port (`--port`, e.g. a USB-TTL
loopback or a tap for on-hardware cross-checks), or a TCP socket (`--connect`). Develop against
it before touching the real bus.

Needs `pyserial` for `--port`. It prints `# virtual A/C up. initial: {...}`, then one line per
handshake frame it echoes (`[0x0A] handshake poll -> echoed slave reply`) and a `[state] {...}`
line whenever a command changes its state. Status polls are answered silently.

`dev.py bench esphome` flashes a board and runs it against the simulator. Wiring and passing
output live in one place: [Build, Flash and Test](Build-Flash-Test#bench-stage-no-ac).

## The host end-to-end test

`firmware/test/host_e2e/` runs the component as it ships, hub and entities included, with no
board. `host.yaml` includes `packages/hisense-ac.yaml` unchanged and builds it for ESPHome's `host`
platform: a Linux program whose UART is a pseudo-terminal. `run.py` puts the virtual A/C on the
other end, connects over the native API with `aioesphomeapi` as Home Assistant does, and checks
two things for every action: what the entities show, and which bytes reached the A/C.

```
python3 firmware/test/host_e2e/run.py              # build, then the default scenarios
python3 firmware/test/host_e2e/run.py --full       # the long ones too
python3 firmware/test/host_e2e/run.py --list       # every scenario and what it takes
python3 firmware/test/host_e2e/run.py holdoff      # one scenario
python3 firmware/test/host_e2e/run.py --coverage   # which lines and functions the scenarios reach
```

It needs `esphome` and `g++`. The API client comes with esphome, so the script runs itself under
the Python esphome is installed in. Without esphome it prints that it was skipped. The first
build downloads PlatformIO's `native` platform, which is why this is not part of `run_tests.sh`.
CI runs it in the `esphome-host-e2e` job of the QA workflow whenever the firmware or the test
changed, and the nightly workflow runs `--full`.

| Scenario | What it holds down |
|---|---|
| `boot` | the handshake, the device type learned and stamped into every frame, a state from every entity, and the entity list against `entities.golden.json` |
| `telemetry`, `faults` | sensors, the -20 C placeholder, the climate action, fault and capability bits |
| `climate_modes`, `climate_values`, `fahrenheit` | the command frame for every mode, setpoint (rounding, limits), fan step and swing, byte for byte |
| `switch_*`, `presets` | eco, turbo, quiet, sleep, display and beeper, the 10 s pacing between special-mode frames |
| `holdoff` | a status that contradicts a command does not move the entity for 4 s |
| `retry_main`, `retry_special`, `deaf_unit` | a command the unit ignores or does not hear: the re-sends, then the give-up and the entity reverting |
| `queue_full`, `link_never_up`, `link_loss` | dropped and refused frames, a silent unit, the link sensor and the counters |
| `setup_request` | the remote's setup gesture fires `on_setup_request` once, `set_setup_indicator` shows in the heartbeat |
| `damaged_traffic` | bad checksums, truncated replies and noise are counted and never shown |
| `presets_all`, `deaf_unit_forever`, `periodic` | `--full` only: every preset, nine frames to a unit that never answers, the once-a-minute refresh |

Things to know before changing it:

- **The timers are real.** The hub runs its 1 s bus cycle, 4 s hold-off and 10 s special-mode
  pacing on the wall clock, and the host platform has no clock to turn forward. Each scenario has
  its own program, pseudo-terminal, API port and temporary directory, so they run side by side:
  four of them run at a time (`--jobs`). The default set takes about two minutes, `--full` about three.
- **The entity list is a golden file.** A changed name, object id, unit, device class, category,
  icon or climate trait fails `boot`. After a change that is meant, run
  `run.py --update-golden boot` and commit `entities.golden.json` with it.
- **Known failures.** A defect the test found or reproduces is a `known_failure(...)` in
  `scenarios.py`: green while the defect is there, red on the day it is fixed, with a message
  that says to turn it into a plain check.
- **The A/C** is `ac_model.py`: `virtual_ac.py` plus what a test has to switch on (a silent or
  deaf unit, damaged replies, the setup request bits, fault bytes).

## C/C++ lint

Every C/C++ tree we own is held to one style and one check set, taken from ESPHome's:
`.clang-format` and `.clang-tidy` at the repo root, narrowed per directory where a tree has a real
constraint (`firmware/test/reference-driver/` is a C API written as C++11, and `firmware/test/` is
held to bug checks, not style).

```
firmware/scripts/cpp-lint.sh check [PATH...]   # what CI runs: clang-format, custom rules, clang-tidy
firmware/scripts/cpp-lint.sh fix   [PATH...]   # applies the fixable part, then check reports the rest
```

- **clang-format 13.0.1 and clang-tidy 22.1.8**, ESPHome's pins. Another clang-format major
  formats differently, so the script refuses other versions and installs the pinned wheels into
  `~/.cache/hisense-w41h1/cpp-lint` on first use (`CLANG_FORMAT` / `CLANG_TIDY` override).
- **Custom rules** (`cpp-lint.py`) are the portable part of ESPHome's `ci-custom.py`: no
  integer-constant `#define` where `constexpr` works, braces around a lone `ESP_LOG` body, no
  `byte`, `sprintf`, `scanf` or `std::bind`, inclusive language, `#pragma once`, whitespace. A
  line with `NOLINT` is exempt; say why next to it. The reference driver's C-API headers keep
  their macros, because C includes them.
- **clang-tidy** runs on the host-compilable set only (the reference driver, its headers, the host
  tests, the ESPHome codec port and bus scheduler), through a compile database the script
  generates from the same flags as `run_tests.sh` (`cpp-lint.sh compdb [DIR]` writes it for an
  editor). The esp32-recon tree needs ESP-IDF, and the rest of the ESPHome component needs
  ESPHome's headers, so those get clang-format and the custom rules, not clang-tidy.
- **ESPHome's own gates.** `firmware/scripts/esphome-upstream-check.sh` copies the `hisense_ac`
  component and its tests into a checkout of `esphome/esphome` and runs the scripts their CI runs:
  `ci-custom.py`, `build_codeowners.py`, ruff, pylint and `test_build_components.py`. By default
  it checks against the ESPHome release this repo builds with, and CI requires that run to pass.
  `ESPHOME_UPSTREAM_REF=dev` checks against their development branch instead. The nightly
  workflow runs that and does not block on it, because their rules can ask for things the pinned
  release does not have yet.
- **`fix`** applies the whitespace rules, the clang-tidy fixes that cannot change behaviour
  (braces, `override`, redundant control flow and the like, only where the file's own
  `.clang-tidy` enables them), then clang-format.

The pre-commit hook runs `check --no-tidy` on staged C/C++ files (`CPP_LINT_TIDY=1` adds
clang-tidy) and only warns if the pinned tools are not installed yet.

## Beyond the host tests

- **Build checks in CI**: `esphome config` on every board file and on the upstream-shaped
  component tests, a real compile of all three stock-module layouts, which also fails when
  the image's slot addresses are not those of the layout the file names, and a real compile of
  the three ESP32 board files and of the hub with no entity platform. Both compile jobs fail on
  a compiler warning from the components, and both are skipped when a change touches nothing they
  build from. The ESP8266 and RP2040 example files are compiled every night.
- **Fuzzing in CI**: pull requests replay the corpus (`fuzz/run.sh --smoke`, inside
  `run_tests.sh`). The nightly workflow fuzzes each harness for ten minutes, keeps what it finds
  for the next night, and uploads the reproducer when a harness stops on a crash.
- **Version and changelog**: `run_tests.sh` ends with `firmware/scripts/version-lint.py` and its
  own test (`test_version_lint.py`), which feeds the lint trees that have to fail. CI runs the
  same lint with the history, for the rule that a component or package change comes with a
  changelog entry ([Repo Map](Repo-Map-and-Build-Pipeline#version-and-releases)).
- **HIL**: the only layer covering RF, real bus timing and the physical unit.
  `firmware/test/hil_esphome_actuation.py` stays out of `run_tests.sh` on purpose. It drives every
  control over the native API, checks that nothing else moved, and ends with the bus counter
  deltas: retries are listed, a rise in `Failed commands` fails the run. It restores the state it
  found, also when a step fails, on Ctrl-C and at its hard timeout, reads the unit back and fails
  if anything is not as found, and leaves the unit off if it had to switch it on. A unit that is
  running when it connects is taken to be in use and is not touched without `--allow-running`.
- **A tap on the bus**: `reverse-engineering/tools/decode_ac_frames.py --port <tap>` decodes what
  is on the wire, and `firmware/esp32-recon/` is a console on the bus for byte-level questions.
- **Standing rule for glue code**: the ESPHome hub only runs in the host end-to-end test, on real
  timers, where one case costs seconds. So any glue *decision* goes into a pure, tested function
  in `hisense_map.h`. A feedback-loop bug once escaped review because it lived in a layer no test
  could reach.
