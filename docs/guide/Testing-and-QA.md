# Testing and QA

What tests exist, what each one holds down, and the command that runs it. Nearly all of it runs
without an A/C or a board. Why the suite is shaped this way is in
`firmware/docs/04-qa-strategy.md`.

## The layers

| # | Layer | Proves | Hardware | Where |
|---|---|---|---|---|
| 1 | Host unit tests | the codec, the mapping, the bus scheduler, the retry rules and setup mode produce and accept the right bytes and make the right decisions | none | `firmware/test/test_*.cpp`, `test_*.py` |
| 2 | Device simulation | the command and status loop against a modelled A/C | none | `firmware/test/virtual_ac.py` |
| 2b | Host end-to-end | the hub and the entities as they ship: what Home Assistant sees and which bytes reach the A/C | none | `firmware/test/host_e2e/` |
| 3 | Build checks | the YAML, every platform schema, and a real compile of the board files | none | `esphome config`, `esphome compile`, in CI |
| 4 | Bench | a flashed board against the simulator on a USB adapter | a board | `dev.py bench esphome` |
| 5 | Hardware in the loop | timing, radio, the real A/C | a unit | `firmware/test/hil_esphome_actuation.py` |

## The commands

| Command | What it runs | Time | Needs |
|---|---|---|---|
| `firmware/test/run_tests.sh` | every host layer below | about a minute | `g++`, `python3`. With `esphome` installed it also runs the configuration matrix |
| `firmware/test/sanitize.sh` | every C++ host test again under AddressSanitizer and UBSan, with `g++` and `clang++` | about 25 s | a missing compiler is skipped |
| `firmware/test/fuzz/run.sh --smoke` | replays the fuzz seeds, corpus and regressions through every harness. Part of `run_tests.sh` | 10 to 20 s | `g++` or `clang++` |
| `firmware/test/fuzz/run.sh <seconds> [harness...]` | fuzzes each harness for that long | as given | `clang++` with libFuzzer |
| `firmware/test/coverage/run.sh` | line and branch coverage of the pure component code under the host tests | about 15 s | `gcovr` (fetched with `uvx`) |
| `firmware/test/coverage/mutate.py` | mutation testing of the same code: which changes to the source no test notices | minutes | `uvx` |
| `python3 firmware/test/test_esphome_config_matrix.py` | the configuration schema through `esphome config`. Part of `run_tests.sh` | seconds | `esphome` |
| `python3 firmware/test/host_e2e/run.py` | the host end-to-end scenarios | about two minutes | `esphome`, `g++` |
| `python3 firmware/scripts/dev.py test esphome` | `run_tests.sh`, then `esphome config` on one board file | | `esphome` |
| `firmware/scripts/cpp-lint.sh check` | clang-format, the custom rules and clang-tidy | | installs its pinned tools on first use |

The coverage and mutation scripts write to a temporary directory, never into the tree. Neither is
part of the default suite. `mutate.py` runs at most four mutants at once, each with a memory
limit and a timeout of its own, and prints those limits when it starts
(`firmware/docs/04-qa-strategy.md`).

## What run_tests.sh holds down

It prints a heading per layer, in the order of this table, and ends with `ALL QA LAYERS PASSED`.

| Layer | Test | What it holds down |
|---|---|---|
| 1a | `test_codec.cpp` | the reference driver's builders and parsers against golden frames |
| 1b | `test_reference_map.cpp` | the reference fan ladder, setpoint clamp, running state and power estimate |
| 1c | `test_esphome_map.cpp` | the reference driver's first ESPHome mapping of modes, fan names and presets (`reference-driver/esphome_aircon_map.h`) |
| 1d | `test_esphome_codec_parity.cpp` | the component's codec equals the reference driver, byte for byte, over exhaustive and seeded random inputs |
| 1e | `test_esphome_bus.cpp` | the bus scheduler against a simulated A/C: timing, link loss, retries |
| 1f | `test_esphome_edge_cases.cpp` | shadow sync rules, boundaries, damaged byte streams |
| 1g | `test_esphome_confirm.cpp` | each confirm and retry decision, then the real `CommandTracker` (the class the hub uses) against a model of a unit that loses frames and overrides values |
| 1h | `test_esphome_entities.cpp`, `test_esphome_tables.py` | the climate request as a plan, what a status frame shows, the switches and the sleep select. The Python schema tables against the C++ constants they name |
| 1i | `test_esphome_setup_request.cpp` | the remote's setup request: debounce, lockout, the heartbeat flag |
| 1j | `test_esphome_uart_policy.cpp` | when the stock module's bus port is restarted |
| 1k | `test_esphome_setup_edges.cpp` | every ordering of the two request bits, the lockout, link loss |
| 1l | `test_esphome_bus_stress.cpp` | scheduler edge cases and stress: the clock wrap, damaged replies, seeded random runs |
| 1m | `test_esphome_codec_gaps.cpp`, `test_esphome_map_gaps.cpp` | the gaps that coverage and mutation testing found |
| 1n | `test_esphome_setup_mode.cpp` | the decisions of Wi-Fi setup mode |
| 1o | `fuzz/run.sh --smoke` | the fuzz corpus, the known failures and framing edge cases, under ASan and UBSan |
| 1p | `test_esphome_config_matrix.py` | valid configurations validate with no warning, invalid ones (`firmware/esphome/tests/invalid/`) are rejected with the expected message, and every board file gives the same entities. Skipped without `esphome` |
| 2 | inline | `virtual_ac.py` and the frame decoder in `reverse-engineering/tools/` agree in both directions |
| 3 | `test_image_chain.py` | the stock module's image signing chain (`amebaz2_image.py`): walk, verify, re-sign |
| 4 | `test_dev_release.py`, `test_matter_convert.py` | `dev.py` (build cache, board files, command line) and `matter-convert.py` |
| 5 | `version-lint.py`, `test_version_lint.py` | the firmware version, the changelog and the ESPHome pin agree, and the lint fails on trees that have to fail |
| 6 | `test_dev_edges.py`, `test_scripts_edges.py`, `test_flasher.py`, `test_hil_restore.py`, `test_mutate_limits.py` | edge cases of the tooling: `dev.py`, the scripts, the commit hook, the lint lists, the clip flasher's checks, the limits `mutate.py` puts on a mutant |

To run one C++ test alone, compile it from `firmware/test/` as `run_tests.sh` does:

```
g++ -std=c++11 -Wall -Istubinc -I. -Ireference-driver \
    test_codec.cpp reference-driver/hisense_rs485.cpp -o test_codec && ./test_codec
```

### The reference driver

`firmware/test/reference-driver/` is the project's first bus driver, which ran on real units for
months. It is frozen. The component carries its own port of the codec, and layer 1d compares the
port with that hardware-validated original on every push. Change the reference only to add a
protocol fact both sides must agree on, and say so in the commit.

## The virtual A/C

`firmware/test/virtual_ac.py` speaks the validated bus: it answers status polls with a 160-byte
status frame and applies command frames to its state. It attaches to a pseudo-terminal (`--pty`),
a serial port (`--port`, which needs `pyserial`) or a TCP socket (`--connect`). It prints
`# virtual A/C up. initial: {...}`, one line per handshake frame it answers, and a `[state] {...}`
line whenever a command changes its state.

A flashed board against it is the bench stage:
[Build, Flash and Test](Build-Flash-Test#bench-stage-no-ac).

## The host end-to-end test

`firmware/test/host_e2e/` runs the component as it ships, hub and entities included, with no
board. `host.yaml` includes `packages/hisense-ac.yaml` unchanged and builds it for ESPHome's
`host` platform: a Linux program whose UART is a pseudo-terminal. `run.py` puts the virtual A/C
on the other end, connects over the native API as Home Assistant does, and checks two things for
every action: what the entities show, and which bytes reached the A/C.

```
python3 firmware/test/host_e2e/run.py              # build, then the default scenarios
python3 firmware/test/host_e2e/run.py --full       # the long ones too
python3 firmware/test/host_e2e/run.py --list       # every scenario and how long it takes
python3 firmware/test/host_e2e/run.py holdoff      # one scenario
python3 firmware/test/host_e2e/run.py --coverage   # which lines of the hub and entities the scenarios reach
python3 firmware/test/host_e2e/run.py --tidy       # clang-tidy over the hub and the entities
firmware/test/host_e2e/soak.sh 10                  # the default set ten times, to find a flaky check
```

Without `esphome` it prints that it was skipped. The first build downloads PlatformIO's `native`
platform, which is why it is not part of `run_tests.sh`.

- The timers are real. The hub runs its 1 s bus cycle, 4 s hold-off and 10 s special-mode
  pacing on the wall clock. Scenarios run four at a time (`--jobs`). The default set takes about
  two minutes, `--full` about three.
- The entity list is a golden file. A changed name, object id, unit, device class, category,
  icon or climate trait fails the `boot` scenario. After a change that is meant, run
  `run.py --update-golden boot` and commit `entities.golden.json`.
  [Entities and Diagnostics](Entities-and-Diagnostics) is written from that file.
- Known failures. A defect the test reproduces is a `known_failure(...)` in `scenarios.py`:
  green while the defect is there, red on the day it is fixed.

## Hardware in the loop

`firmware/test/hil_esphome_actuation.py <host>` drives every control of a real node over the
native API, checks that nothing else moved, and ends with the bus counter deltas: retries are
listed, and a rise in `Failed commands` fails the run. It stays out of `run_tests.sh` on purpose.

It restores the state it found, also when a step fails, on Ctrl-C and at its hard timeout
(`--timeout`). It reads the unit back and fails if anything is not as found. A unit that is
running when it connects is taken to be in use and is not touched without `--allow-running`. The
API key comes from `--key` or from `secrets.yaml`.

For byte-level questions on a live bus, `reverse-engineering/tools/decode_ac_frames.py --port <tap>`
decodes what is on the wire, and `firmware/esp32-recon/` is a console on the bus.

## C/C++ lint

Every C/C++ tree here is held to ESPHome's style and checks: `.clang-format` and `.clang-tidy`
at the repository root, narrowed per directory where a tree has a real constraint.

```
firmware/scripts/cpp-lint.sh check [PATH...]   # what CI runs
firmware/scripts/cpp-lint.sh fix   [PATH...]   # applies the fixable part, then reports the rest
```

- clang-format 13.0.1 and clang-tidy 22.1.8 are ESPHome's pins. The script refuses other
  versions and installs the pinned ones into `~/.cache/hisense-w41h1/cpp-lint` on first use
  (`CLANG_FORMAT` and `CLANG_TIDY` override).
- The custom rules (`cpp-lint.py`) are the portable part of ESPHome's `ci-custom.py`. A line with
  `NOLINT` is exempt. Say why next to it.
- clang-tidy runs on what compiles on a host: the reference driver, the host tests, the codec
  port and the bus scheduler. The hub and the entities need ESPHome's headers, so they get
  clang-tidy through `host_e2e/run.py --tidy`.
- `firmware/scripts/esphome-upstream-check.sh` copies the `hisense_ac` component and its tests
  into a checkout of `esphome/esphome` and runs the scripts their CI runs. By default it checks
  against the ESPHome release this repository builds with. `ESPHOME_UPSTREAM_REF=dev` checks
  against their development branch.

## What CI runs

Every push and pull request gets the lints, `run_tests.sh`, the sanitizer run and
`esphome config` on every board file. A change to the firmware adds real compiles, the host
end-to-end test and ESPHome's own gates. A nightly run fuzzes every harness for ten minutes,
compiles the example board files and runs the long end-to-end scenarios. The jobs, one by one:
[Repo Map and Build Pipeline](Repo-Map-and-Build-Pipeline#continuous-integration-github-actions).

## The rule for glue code

The hub only runs in the host end-to-end test, on real timers, where one case costs seconds. So
a decision (is this write my own echo, is this command confirmed, which special-mode frame goes
next) is written as a pure function in one of the component's pure headers, with a host test. A
feedback-loop bug once escaped review because it lived in a layer no test could reach.
