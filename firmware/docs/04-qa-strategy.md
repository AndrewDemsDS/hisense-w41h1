# 04: How the firmware is tested

For a contributor who wants to know which test to run, what a green run proves and what it
cannot. Everything up to the bench runs without an A/C and without a board.

```bash
firmware/test/run_tests.sh                    # every host layer, about a minute
firmware/test/sanitize.sh                     # the C++ tests again under ASan and UBSan, two compilers
python3 firmware/test/host_e2e/run.py         # the hub and the entities as a Linux program, about two minutes
```

The guide has the short version for a first contribution:
[`docs/guide/Testing-and-QA.md`](../../docs/guide/Testing-and-QA.md). The structure of the code
under test is in [`01-component-architecture.md`](01-component-architecture.md).

## What the tests are measured against

The protocol is reverse-engineered, so there is no specification to test against. The ground
truth is, in this order: frames taken off the bus of a real unit, the vendor module's firmware,
and what a unit does when it is sent a frame. Three literal frames (power on, power off and the
status poll) are the bytes published by the community project `pslawinski/esphome_airconintl`.
Units act on them, and the vendor firmware explains the poll, but no capture of those three from
this project's own bus is on record
([`NOTICE.md`](../../NOTICE.md#acknowledgements-earlier-work-on-this-ac-bus)). A test that pins
them pins the published bytes.

## The layers

| Layer | Where | Proves | Cannot prove | Runs |
|---|---|---|---|---|
| Host unit tests | `firmware/test/test_*.cpp`, `test_*.py` | builders and parsers produce and accept the right bytes. Mapping, retry, scheduling and setup-mode decisions are right over their input domains | anything about timing on a chip, or that our reading of the unit is right | `run_tests.sh`, every push |
| Sanitizers | `firmware/test/sanitize.sh` | the same tests touch no memory out of bounds and hit no undefined behaviour, under `g++` and `clang++` | paths the tests do not reach | every push |
| Fuzzing | `firmware/test/fuzz/` | the codec and the scheduler hold their properties on inputs nobody wrote | properties nobody stated | smoke on every push, ten minutes per harness every night |
| Coverage and mutation | `firmware/test/coverage/` | which lines and branches the host tests reach, and which changes to the code no test notices | nothing by itself: it points at missing tests | coverage on pushes to `main`, mutation by hand |
| Configuration matrix | `firmware/test/test_esphome_config_matrix.py` | the schemas accept what they should and refuse what they should, with the right message, and every board file gives the same entities | that the generated code compiles | `run_tests.sh` when `esphome` is installed |
| Device simulation | `firmware/test/virtual_ac.py` | the simulator and the independent frame decoder agree in both directions | that either matches a unit | `run_tests.sh` |
| Host end-to-end | `firmware/test/host_e2e/` | the hub and the entity classes do what the pure layers decided: entity states over the native API, bytes on the wire | DE timing, baud rate, a slow unit, anything the model does not model | when the firmware or the test changed, the long scenarios every night |
| Build checks | `esphome config`, `esphome compile` | the YAML and schemas are valid on every board, the images build without a warning from our components, the stock-module image is laid out for its flash | that an image runs | every push (config), on firmware changes (compile) |
| Bench | `dev.py bench` | a flashed board against the simulator: wiring, the UART and DE on the real chip | the A/C | by hand, needs a board |
| Hardware | `firmware/test/hil_esphome_actuation.py`, a tap on the bus | timing, radio, the real unit | | by hand, needs a unit |

## Host tests: `run_tests.sh`

It needs `g++`, `python3` and a compiler with AddressSanitizer for the fuzz replay (`clang++` or
`g++`). With `esphome` on the path it also runs the configuration matrix. It takes about a minute.
The layers, in the order they run, with the label each prints:

| Label | Test | What it holds down |
|---|---|---|
| 1a | `test_codec.cpp` | the reference driver's builders and parsers against golden frames |
| 1b | `test_reference_map.cpp` | the reference fan ladder, setpoint clamp, running state and power estimate |
| 1c | `test_esphome_map.cpp` | the reference driver's first ESPHome mapping (`reference-driver/esphome_aircon_map.h`) |
| 1d | `test_esphome_codec_parity.cpp` | the component's codec equals the reference driver: every builder and parser over exhaustive, cartesian and seeded random inputs |
| 1e | `test_esphome_bus.cpp` | the bus scheduler against a simulated unit: reply windows, DE timing, hold-off, link loss, re-sends, a blocking write, a coarse loop |
| 1f | `test_esphome_edge_cases.cpp` | shadow sync rules, boundaries, damaged byte streams |
| 1g | `test_esphome_confirm.cpp` | each confirm and retry decision, then the command tracker against a model of a unit that loses frames and overrides values |
| 1h | `test_esphome_entities.cpp` | the climate request as a plan, what a status frame shows, the switches, the sleep select |
| 1h | `test_esphome_tables.py` | the option tables in the Python schemas against the C++ constants they name. It runs under the 1h heading and prints one of its own |
| 1i | `test_esphome_setup_request.cpp` | the remote's setup request: debounce, lockout, the heartbeat flag |
| 1j | `test_esphome_uart_policy.cpp` | when the stock module's bus port is restarted |
| 1k | `test_esphome_setup_edges.cpp` | every ordering of the two request bits, the lockout, link loss |
| 1l | `test_esphome_bus_stress.cpp` | the scheduler through the clock wrap, damaged replies and seeded random runs |
| 1m | `test_esphome_codec_gaps.cpp`, `test_esphome_map_gaps.cpp` | the lines, branches and mutants that coverage and mutation testing found untested |
| 1n | `test_esphome_setup_mode.cpp` | Wi-Fi setup mode: every phase against every event and input, the credential rules, the status display, every deadline across the clock wrap |
| 1o | `fuzz/run.sh --smoke` | the fuzz corpus, the regressions and the hand-written framing edge cases replay clean, and every known failure still fails |
| 1p | `test_esphome_config_matrix.py` | the configuration matrix (below). Skipped when `esphome` is not installed |
| 2 | inline Python | `virtual_ac.py` and `reverse-engineering/tools/decode_ac_frames.py` agree in both directions |
| 3 | `test_image_chain.py` | the stock module's image signing chain (`amebaz2_image.py`): walk, verify, re-sign, and the stale-trailer defect that bricked a unit. Real images under `firmware/built-images/` and `dumps/` are checked when present (both are gitignored) |
| 4 | `test_dev_release.py` | `dev.py` (build cache, board files, the command line) |
| 5 | `version-lint.py`, `test_version_lint.py` | the version, the changelog and the ESPHome pin agree, and the lint fails on trees that have to fail |
| 6 | `test_dev_edges.py`, `test_scripts_edges.py`, `test_flasher.py`, `test_hil_restore.py`, `test_mutate_limits.py` | edge cases of the tooling: `dev.py`, the lint scripts, the hook, the clip flasher against a chip that is a bytearray, the restore path of the hardware test, the limits `mutate.py` puts on a mutant |

Run one test by compiling it the way the script does, from `firmware/test/`:

```bash
g++ -std=c++17 -Wall -I../esphome/components/hisense_ac test_esphome_confirm.cpp \
    ../esphome/components/hisense_ac/hisense_protocol.cpp -o test_esphome_confirm && ./test_esphome_confirm
```

Things a codec test must include, each learned from a miss:

- a real combined multi-field command frame. The node always sends one for the main fields.
- a doubled-`0xF4` round trip. A frame whose checksum never contains `0xF4` never exercises the
  stuffing.
- fixpoint checks on every map pair (status to command and back), which guard against a readback
  going out again as a new command.

## Sanitizers: `sanitize.sh`

Builds every C++ host test with `-fsanitize=address,undefined -fno-sanitize-recover=all`, once
with `g++` and once with `clang++`, from the same sources and flags as `run_tests.sh`. A report
aborts the test, so "ok" means the test's own checks passed and no out-of-bounds access,
use-after-free, leak, signed overflow, bad shift or misaligned access happened on the way. A
compiler that is not installed is reported as skipped. A `test_*.cpp` that is missing from the
script's table fails the run, so a new test cannot be forgotten. It takes about 25 s and is not
part of `run_tests.sh`. `CXXS="clang++" firmware/test/sanitize.sh` runs one compiler.

## Fuzzing: `fuzz/`

Seven libFuzzer harnesses, one per `fuzz_*.cpp`: the frame assembler, the parsers, the setup
request tracker, the builders, a build and parse round trip, a differential run of the component
against the reference driver, and the bus scheduler driven by a script of events. Each file says
at its top what its input is and which properties it checks.

```bash
firmware/test/fuzz/run.sh --smoke           # replay seeds, corpus and regressions. 10 to 20 s, no libFuzzer needed
firmware/test/fuzz/run.sh 600 bus parsers   # fuzz these two for ten minutes each (clang++ with libFuzzer)
firmware/test/fuzz/run.sh --coverage        # what the corpus reaches in the product sources
firmware/test/fuzz/run.sh --merge           # fold new inputs back into corpus/<harness>.hex
```

Without harness names the timed form runs all seven in parallel. New inputs go to
`fuzz/build/corpus/`, crash reproducers to `fuzz/build/artifacts/<harness>/`. The corpus in the
tree is text, one input per line (`corpus/<harness>.hex`), and `seeds/` and `regressions/` are
written by `make_seeds.py`, which documents each one byte by byte.

What it cannot do: a harness checks only the properties written into it. Two properties do not
hold today and are recorded as [known failures](#known-failures).

## Coverage and mutation testing: `coverage/`

```bash
firmware/test/coverage/run.sh [--baseline] [--out DIR]
```

Line and branch coverage of the pure code under the host tests, with `gcov` and `gcovr`: a text
summary, the uncovered lines and partly taken branches (`uncovered.py`), and an HTML report. It
measures `hisense_protocol.*`, the four map parts, `hisense_commands.h`, `hisense_entities.h`,
`hisense_bus.*`, `w41h1_uart_policy.h` and `setup_mode_logic.h`. `--baseline` leaves out the gap
tests, to see what they add. It needs `gcovr` (fetched through `uvx` at the version CI pins, or
set `GCOVR`), takes about 15 s and writes nothing into the tree.

```bash
python3 firmware/test/coverage/mutate.py [--target NAME ...] [--sample N] [--seed S] [--jobs J] [--mem-limit MB] [--out DIR]
```

Mutation testing changes the product code a little at a time (`universalmutator`, through `uvx`)
and runs the host tests against each change. A mutant that does not compile is not counted. One
whose object code is identical to the original is not counted. One that a test fails on, or that
runs into the 20 s timeout, is killed. One that every test passes is a survivor, and a survivor
is a missing test. The score is killed over killed plus survivors. Targets are `protocol_cpp`,
`protocol_h`, `mapping_h`, `special_h`, `confirm_h`, `telemetry_h`, `commands_h`, `entities_h`
and `uart_h`. `--survivors-of LABEL` re-runs only the survivors of an earlier run, and
`--sanitize` builds the tests with the sanitizers, about three times slower. A run takes minutes.

**The script fences each mutant in.** A mutant is broken code on purpose: a flipped loop
condition can spin while it prints, and a changed size can allocate without bound. So every
mutated test binary runs with these limits, which the script prints when it starts:

| Limit | Default | How |
|---|---|---|
| memory | 512 MB per test binary (`--mem-limit MB`) | a `systemd-run --user --scope` with `MemoryMax` and no swap, so the kernel kills that one process. Where no user scope can be started, `RLIMIT_AS` on the process |
| time | 20 s | the process is killed |
| output | none is kept | stdout and stderr go to `/dev/null` and are never read into the script |
| files | 16 MB, and no core file | `RLIMIT_FSIZE`, `RLIMIT_CORE` |
| workers | 4 at once (`--jobs J`) | the run is refused when `--jobs` times `--mem-limit` is more than a quarter of the machine's memory |

A mutant stopped by a limit is killed, like one that fails a check. With `--sanitize` and no
systemd scope there is no memory limit, because the sanitizers reserve more address space than
`RLIMIT_AS` could allow: the script says so in a warning, and that run belongs in a container or
a scope with a limit of its own. The compilers are not limited. Each worker runs one at a time.
`firmware/test/test_mutate_limits.py` feeds the runner a program that allocates without bound,
one that prints without end and one that writes a file without end, and is part of
`run_tests.sh`.

It is not in CI and not in `run_tests.sh`. What it found is in the two gap tests (layer 1m),
where each block names the line or the mutant it is there for.

What coverage and mutation cannot say: a covered line is a line that ran, not a line whose
result was checked, which is why the mutation run exists. And neither sees the hub or the entity
classes, which only the host end-to-end test reaches (`run.py --coverage` prints what its
scenarios reach).

## Configuration matrix: `test_esphome_config_matrix.py`

Everything goes through `esphome config`, on copies in a temporary directory with the
placeholder secrets. Three checks:

1. Valid configurations, generated as a matrix: every optional entity on and off, the hub with no
   platform, each platform alone, the climate options at their extremes, two hubs on two UARTs,
   every flash layout, the status LED and the pixel alone and together. Each has to validate
   with no warning.
2. Invalid configurations, one file each under `firmware/esphome/tests/invalid/`. Each has to be
   rejected with the message its `# expect:` line gives.
3. The board files: every one gives the same entities with the same names and ids (35 of them as
   of 2026-10-13).

`--full` adds one run per single entity, `--show <name>` prints what `esphome` says for one file,
and `--codegen` also generates the C++ (it fetches libraries from the network, so it is not in
the default run). Without `esphome` the test prints that it was skipped, unless `--require` is
given.

## Device simulation: `virtual_ac.py`

It models the indoor unit: it answers the handshake, serves 160-byte status frames and applies
command frames to its state. `run_tests.sh` checks it against the independent frame decoder in
`reverse-engineering/tools/`, in both directions, so the simulator and the decoder cannot be
wrong in the same way without the golden frames noticing. It also runs on its own, on a
pseudo-terminal (`--pty`) or a serial port (`--port`), which is what the bench uses.

## Host end-to-end: `host_e2e/`

The pure tests stop below the hub. The hub and the entity classes (`hisense_ac.cpp`,
`hisense_climate.cpp`, `hisense_switch.cpp`, `hisense_select.cpp`) need ESPHome. ESPHome's `host`
platform builds the whole component as a Linux program, so `host_e2e/` does: the entity package
unchanged (`host.yaml` includes `packages/hisense-ac.yaml`), a pseudo-terminal for the UART, a
model of the A/C on the other end (`ac_model.py`: `virtual_ac.py` plus the faults a test needs)
and `aioesphomeapi` where Home Assistant would be.

```bash
python3 firmware/test/host_e2e/run.py              # build, then the default scenarios
python3 firmware/test/host_e2e/run.py --full       # the long ones too, about three minutes
python3 firmware/test/host_e2e/run.py --list       # the scenarios and what each takes
python3 firmware/test/host_e2e/run.py boot faults  # only these
python3 firmware/test/host_e2e/run.py --no-build   # the binary from the last build
python3 firmware/test/host_e2e/run.py --coverage   # build with gcov, print what the scenarios reached
python3 firmware/test/host_e2e/run.py --update-golden boot   # rewrite entities.golden.json
python3 firmware/test/host_e2e/run.py --tidy       # clang-tidy over the hub and the entities, no scenarios
firmware/test/host_e2e/soak.sh 10                  # the default set ten times in a row, to find a flaky check
```

Each check is one of two kinds: what an entity shows, or which frame the model received.
Expected frames are written out in the test from the protocol document and are not asked of the
code under test. The test reads nothing of the hub's internals, so it survives a restructuring
of the hub and fails when the behaviour moves. The entity list is a golden file
(`entities.golden.json`): a changed name, id, unit, class or icon fails the `boot` scenario.

What it costs is time. The host platform has no clock to turn forward, and the hub's rules are in
seconds. Each scenario runs its own program on its own pseudo-terminal and API port, four at a
time (`--jobs`), which brings the default set to about two minutes. That cost is the reason the
rule in [`01-component-architecture.md`](01-component-architecture.md#the-rule-a-decision-is-a-pure-function-with-a-test)
still holds.

Two limits. The model answers at once, so nothing here says anything about DE timing, baud rate
or a slow unit. And the model is ours: it proves the firmware against our reading of the unit,
which the golden frames and the hardware test have to keep honest.

It needs `esphome` and a C++ compiler, and the first build fetches PlatformIO's `native`
platform, which is why it is not part of `run_tests.sh`. Without `esphome` it prints that it was
skipped, unless `--require` is given.

## Build checks

`esphome config` validates every board file, every upstream-shaped component test and every
platform schema without a toolchain, in seconds. The compiles are real: the three ESP32 board
files and the hub with no entity platform, and all three stock-module layouts, because that is
the one platform with a different compiler and integer widths and because the build itself checks
that the image's slot addresses are those of the layout the file names.
`firmware/scripts/esphome-compile-check.sh` is the gate: it fails on a compiler warning from this
repository's components and on an image that does not carry the firmware version.

ESPHome's own CI scripts run against `hisense_ac` too
(`firmware/scripts/esphome-upstream-check.sh`).

## Bench and hardware

`dev.py bench --port <board> --sim-port <adapter>` flashes a board and runs it against
`virtual_ac.py` over a USB-TTL or USB-RS485 adapter. It proves the wiring, the UART and DE on the
real chip before any wire touches the appliance. The procedure and the passing output are in
[`docs/guide/Build-Flash-Test.md`](../../docs/guide/Build-Flash-Test.md).

`firmware/test/hil_esphome_actuation.py <host> [--key <noise_psk>]` drives a node on a real A/C
over the native API and checks two properties for every control:

1. **Actuation.** The command lands and the entity changes state. A command that matches the
   current value is a no-op that proves nothing, so every step forces a real change.
2. **No collateral.** Most controls ride the combined command frame, which is rebuilt from the
   command shadow. If the shadow drifts, driving one control retunes the A/C. So after each step
   everything else must be where it was.

It also reads the bus counters: a readback that would pass on a dead bus is rejected, every wait
watches `Command retries` and waits longer when it moved, and the run ends with the counter
deltas. Retries are listed and are fine. A rise in `Failed commands` is a failed check.

What protects the unit: a unit that is running when the test connects is taken to be in use and
nothing is sent to it (`--allow-running` tests it as it is, `--power-on` is for a unit you
switched off). The state found at the start is restored in a `finally`, also when a step raises,
on Ctrl-C and at the hard timeout (`--timeout`, 25 minutes by default), and the unit is read back
afterwards: a field that is not as it was found is a failed check. A unit the run itself switched
on is left off. An earlier version put the mode back after the power-off and left two units
running (#175). `test_hil_restore.py` tests that restore path on the host.

It is not part of `run_tests.sh`, which stays hardware-free. The panel display is not asserted:
the A/C reports no display state, so whether it lit is for a person at the unit to see. The
recorded results are in [`15-esphome-path.md`](15-esphome-path.md#hardware-validation-on-esp32-boards).

For byte-level questions on a live bus there are two more tools: a tap decoded by
`reverse-engineering/tools/decode_ac_frames.py --port <tap>`, and
[`firmware/esp32-recon/`](../esp32-recon/README.md), a console on the wire with a listen-only
mode.

## Known failures

A defect that a test found and that is not fixed yet stays in the suite as a known failure. The
rule is the same everywhere: the check states the behaviour the code should have, it passes the
run while the defect is there, and it fails the run on the day the defect is gone. The marker
cannot outlive the defect, and nobody has to remember to remove it.

| Where | Form | As of 2026-10-13 |
|---|---|---|
| C++ host tests | `KNOWN(cond, ...)` in `test_esphome_setup_edges.cpp`, `KNOWN_DEFECT(cond, ...)` in `test_esphome_map_gaps.cpp`. Printed as `known` or `KNOWN DEFECT`, counted in the test's last line | 1: a unit that asks with bit `0x08` cannot end setup mode with the gesture |
| Python tooling tests | `known_failure(fixed, msg, defect)` in `edge_harness.py`. Printed as `xfail` | none open |
| Host end-to-end | `r.known_failure(fixed, msg, defect)` in `host_e2e/harness.py` | none open |
| Fuzz harnesses | a line in `fuzz/regressions/KNOWN_FAILURES` with a reproducer, and the property behind `FUZZ_STRICT` in the harness. The smoke run replays the reproducer against the default build (must pass) and the strict build (must fail) | 2: the assembler delivers a frame whatever its checksum (by design, the scheduler judges it), and `0xF4` in a frame body is not doubled on transmit |
| Sanitizers | an entry in the `KNOWN` table of `sanitize.sh`, which switches one check off for one test | none |
| Invalid configurations | `# accepted:` and the reason in a file under `tests/invalid/`. The file has to validate | 4, all by design with a warning |

To convert an entry when the defect is fixed:

1. Fix the code. The run now fails at the marker, with a message that says so.
2. Turn the marker into a plain check: `KNOWN(...)` or `KNOWN_DEFECT(...)` becomes `CHECK(...)`
   with the same condition, `known_failure(fixed, ...)` becomes `check(fixed, ...)`. For a fuzz
   entry, delete the line in `KNOWN_FAILURES`, remove the `FUZZ_STRICT` exclusion in the harness
   so the property is checked by default, and keep the reproducer under `regressions/` as an
   ordinary input. For an `# accepted:` file, replace the line with `# expect:` and the message.
3. Leave a comment that says what the check was a known failure of. `scenarios.py` has several
   that read "Was a known failure ... until ...".

`FUZZ_STRICT=1 firmware/test/fuzz/run.sh 60 builders` fuzzes with the known failures switched on,
to look at one or to check a fix.

## How CI runs it

`.github/workflows/qa.yaml`, on every push and pull request to `main` and `dev`:

| Job | Runs | When |
|---|---|---|
| `host-qa` | shellcheck, ruff, `cpp-lint.sh check`, `run_tests.sh`, `sanitize.sh`, `esphome config` on every board file and every `tests/build*.yaml`. The one required check | always |
| `version` | `version-lint.py` with the history: a change under `components/` or `packages/` needs a changelog entry | always |
| `lint-extra` | actionlint, zizmor, `docs/guide/lint.py`, cppcheck on the pure code, the ESPHome pin | always |
| `esphome-esp32` | a compile of the ESP32 board files and the hub-only configuration, through `esphome-compile-check.sh` | when the firmware changed |
| `esphome-amebaz2` | a compile of all three stock-module layouts | when the firmware changed |
| `esphome-upstream` | ESPHome's own gates on the pinned release | when the firmware changed |
| `esphome-host-e2e` | `host_e2e/run.py --require`, then clang-tidy and coverage of the hub as a report | when the firmware or the test changed |
| `coverage` | `coverage/run.sh`, as a table in the job summary. Never blocking | pushes to `main` |

`.github/workflows/nightly.yaml`, at 02:17 UTC, by hand, and on a pull request that changes what
one of its jobs builds:

| Job | Runs |
|---|---|
| `fuzz` | every harness for ten minutes (one minute on a pull request), one runner each. The corpus is kept between nights, and a reproducer is uploaded when a harness stops |
| `boards` | the board files no pull request compiles (ESP8266, RP2040) |
| `esphome-upstream-dev` | ESPHome's gates against their `dev` branch. A failure is a warning, not a red run |
| `host-e2e-full` | every host end-to-end scenario, the long ones included |
| `esp32-recon` | the bus diagnostic tool builds with ESP-IDF v5.5.5 |

`.github/workflows/esphome-release.yaml` runs the version check, the host tests and a compile of
every board file before it publishes a release
([`CONTRIBUTING.md`](../../CONTRIBUTING.md#releasing)).

The pre-commit hook (`git config core.hooksPath firmware/.githooks`) runs `run_tests.sh` when
the firmware, the tests or the tooling are staged, `cpp-lint.sh check --no-tidy` on staged C and
C++ files, and the prose gate on staged Markdown.
