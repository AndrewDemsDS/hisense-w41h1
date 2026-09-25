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

Needs `pyserial` for `--port`. It prints `# virtual A/C up. initial: {...}`, then one line per
handshake frame it echoes (`[0x0A] handshake poll -> echoed slave reply`) and a `[state] {...}`
line whenever a command changes its state. Status polls are answered silently.

## On-target bench: `smoketest/` against the simulator

`firmware/esp32-matter/smoketest/` is **not** a codec test (the golden vectors run on the host).
It builds `busmon`, the real driver plus the ESP-IDF HAL, logging each decoded status frame. Run it
against `virtual_ac.py` on a USB adapter with `dev.py bench esp32`. Wiring, passing output and the
failure signatures live in one place: [Build, Flash and Test](Build-Flash-Test#bench-stage-no-ac).

## C/C++ lint

Every C/C++ tree we own is held to one style and one check set, taken from ESPHome's so the
component and the shared driver read alike: `.clang-format` and `.clang-tidy` at the repo root,
narrowed per directory where a tree has a real constraint (`firmware/src/rs485-driver/` is a C API
built as C++11 for the Realtek SDK; `firmware/test/` is held to bug checks, not style). The
SDK-derived `firmware/src/sdk-edits/` is excluded and never reformatted.

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
  line with `NOLINT` is exempt; say why next to it. The C-API driver headers keep their macros
  (C includes them, and the HACS contract test parses `HISENSE_FEAT1_*` / `HISENSE_FAULT1_*`).
- **clang-tidy** runs on the host-compilable set only (driver, its headers, the host tests, the
  ESPHome codec port and bus scheduler), through a compile database the script generates from
  the same flags as `run_tests.sh` (`cpp-lint.sh compdb [DIR]` writes it for an editor). The
  esp32-matter and esp32-recon trees need ESP-IDF, and the rest of the ESPHome component needs
  ESPHome's headers, so those get clang-format and the custom rules, not clang-tidy.
- **`fix`** applies the whitespace rules, the clang-tidy fixes that cannot change behaviour
  (braces, `override`, redundant control flow and the like, only where the file's own
  `.clang-tidy` enables them), then clang-format.

The pre-commit hook runs `check --no-tidy` on staged C/C++ files (`CPP_LINT_TIDY=1` adds
clang-tidy) and only warns if the pinned tools are not installed yet.

## Beyond the host tests

The rest of the pyramid is described once, in `firmware/docs/04-qa-strategy.md`. What each part is
for, in short:

- **Layer 3, Renode** (`firmware/test/renode/`): a scaffold, **not runnable as-is**. The planned
  first target is a bare-metal driver-test ELF, not the full Matter image.
- **Layer 4b, chip-tool / CSA Test Harness**: needs a device; the test credentials let chip-tool
  commission it out of the box.
- **Layer 4c, OTA conversion sim** (`firmware/test/sim_ota_convert.sh`): proves the `.ota`
  packaging and the OTA transport on loopback, with no hardware and no real boot.
- **Layer 5, HIL**: the only layer covering RF, real bus timing and the physical unit. The DI-tap
  sniffer (`decode_ac_frames.py --port <tap>`) is the hardware assertion; the scripted HIL checks
  (`hil_display_actuation.py`, `hil_esphome_actuation.py`) stay out of `run_tests.sh` on purpose.
- **Standing rule for glue code**: `matter_drivers.cpp` cannot run on the host, so any glue
  *decision* goes into a pure, tested function in `matter_aircon_map.h`. The three concrete test
  requirements that follow from it are in docs/04.
