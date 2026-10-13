# firmware/test

The tests of the firmware and its tooling. What each layer proves, what it cannot, the
known-failure convention and how CI runs it all are in
[`firmware/docs/04-qa-strategy.md`](../docs/04-qa-strategy.md). This page is the map of the
directory.

| Path | What | Run |
|---|---|---|
| `run_tests.sh` | every host layer, no hardware | `firmware/test/run_tests.sh` |
| `sanitize.sh` | the C++ tests under ASan and UBSan, with `g++` and `clang++` | `firmware/test/sanitize.sh` |
| `test_*.cpp`, `test_*.py` | the tests themselves. `test_common.h`, `hal_stub.h`, `stubinc/`, `edge_harness.py` and `image_fixtures.py` are what they share | through `run_tests.sh` |
| `fuzz/` | libFuzzer harnesses for the codec and the bus scheduler, their seeds, corpus and regressions | `fuzz/run.sh --smoke` |
| `coverage/` | line and branch coverage (`run.sh`) and mutation testing (`mutate.py`) of the pure code. Run the mutation script under a memory cap | `coverage/run.sh` |
| `host_e2e/` | the hub and the entities as a Linux program against a model of the A/C | `python3 host_e2e/run.py` |
| `virtual_ac.py` | a software model of the indoor unit, also used on the bench | `python3 virtual_ac.py --pty` |
| `hil_esphome_actuation.py` | the actuation check on a real A/C. **It drives a unit.** Not part of `run_tests.sh` | `python3 hil_esphome_actuation.py <host>` |
| [`reference-driver/`](reference-driver/) | the frozen first bus driver, the reference of the parity test | not run by itself |

The scripts document their own options at the top of each file.
