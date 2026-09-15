# Build, Flash and Test

Getting started from source, one section per firmware target, in order of preference: ESPHome,
then ESP32 with Matter, then the stock AmebaZ2 module. Every step goes through
`firmware/scripts/dev.py`, which wraps the real scripts (`ota-release.sh`, `esp32-release.sh`,
`esp32-lint.sh`, the setup scripts) and prints each command before running it, so you can also
copy the commands and run them by hand. The task-oriented walkthrough is the
[User Guide](User-Guide); this page has the detail behind it.

← back to [Home](Home) · siblings: [User Guide](User-Guide) ·
[Repo Map and Build Pipeline](Repo-Map-and-Build-Pipeline) · [Testing and QA](Testing-and-QA) ·
[ESPHome Build](ESPHome-Build) · [ESP32 Replacement Build](ESP32-Replacement-Build)

---

## Which target

| Target | Hardware | Reaches | Page |
|---|---|---|---|
| `esphome` (preferred) | an ESP32-C3 SuperMini or classic ESP32 plus a 3.3 V transceiver | Home Assistant only | [ESPHome Build](ESPHome-Build) |
| `esp32` | the same ESP32 board and wiring | Matter (HA, Apple, Google, Alexa) | [ESP32 Replacement Build](ESP32-Replacement-Build) |
| `amebaz2` | the stock W41H1 module, reflashed over a SOIC-8 clip | Matter | [Installing the Firmware](Installing-Custom-Firmware) |

## Verification status

What each step has been run against. "Simulator" means `virtual_ac.py`, not an A/C.

| Step | esphome | esp32 (C3) | esp32 (classic) | amebaz2 |
|---|---|---|---|---|
| Host QA (`dev.py test`) | CI, every push (plus `esphome config`) | CI, every push | CI, every push | CI, every push |
| `dev.py build` on a real toolchain | yes, C3, 2026-09-14 | yes, 2026-09-14 (app 6% partition free; `busmon` too) | untested | yes, 2026-09-14 (v10332) |
| `busmon` against the real bus | n/a | untested | hardware (2026-07-12) | n/a |
| Matter app against `virtual_ac.py` | n/a | simulator (2026-08-07) | untested | n/a |
| Node on a live A/C, USB-powered (stage 2) | hardware | untested on these pins | hardware | hardware |
| Powered from the connector (stage 3) | outstanding | untested | see [ESP32 Replacement Build](ESP32-Replacement-Build) | hardware |
| `dev.py` flash / bench on hardware | untested | untested | untested | n/a (clip) |

Builds have been run through the wrapper; nothing has yet been flashed or benched through it on a
real board. If a step fails, the printed `$ ...` command is the thing to debug.

## The guided flow

```
python3 firmware/scripts/dev.py walk <target> [--board c3|classic] [--port /dev/ttyACM0]
```

It asks before each step: check tools and SDK pins, host QA and lint, build, (erase), flash and
monitor, then prints the staged bring-up with its safety warnings. Each step is also a
standalone command:

| Command | Does |
|---|---|
| `doctor <target>` | read-only: tools on `PATH`, SDK checkouts at the `versions.env` pins |
| `fetch <target>` | fetch the pinned SDKs (asks first; several GB) |
| `test <target>` | `firmware/test/run_tests.sh`, then the target's lint |
| `build <target>` | build the app |
| `erase <target> --port P` | erase-flash, brand-new boards only, needs typed `ERASE` |
| `flash <target> --port P` | flash, then monitor |
| `monitor <target> --port P` | serial console only |
| `bench <target> --port P --sim-port S` | firmware against `virtual_ac.py`, no A/C |
| `next <target>` | the staged bring-up and its warnings |
| `ota <amebaz2\|esp32> <step> [args]` | Matter OTA through the release scripts, see [OTA Updates](OTA-Updates) |

`--board` defaults to `c3`. It picks the IDF target (`esp32c3` or `esp32`) and, for ESPHome, the
board and pin substitutions passed to `esphome -s`.

## ESPHome

```
python3 firmware/scripts/dev.py fetch esphome      # pipx install esphome==2026.7.4, creates secrets.yaml
python3 firmware/scripts/dev.py test esphome       # host QA + esphome config
python3 firmware/scripts/dev.py flash esphome --board c3 --port /dev/ttyACM0
```

Fill in `firmware/esphome/secrets.yaml` (gitignored) before building. `w41h1.yaml` keeps the
classic ESP32 defaults; `--board c3` overrides `board`, `tx_pin`, `rx_pin` and `de_pin` on the
command line, so the YAML never needs editing to switch boards. `dev.py` uses the `esphome` on
`PATH`; set `ESPHOME` to point at another install. Later updates go over the air with
`esphome run`, passing the same board overrides you flashed with: a C3 node given the classic
defaults receives an image for the wrong chip. Both command lines are in
[OTA Updates](OTA-Updates#esphome-updates) (`dev.py ota` is Matter-only).

## ESP32 (esp-matter)

```
python3 firmware/scripts/dev.py fetch esp32        # ESP-IDF v5.5.4 + esp-matter at the pinned SHA
python3 firmware/scripts/dev.py test esp32
python3 firmware/scripts/dev.py build esp32 --board c3
python3 firmware/scripts/dev.py flash esp32 --board c3 --port /dev/ttyACM0
```

- **Where things live.** ESP-IDF defaults to `~/esp/esp-idf` and esp-matter to `~/esp/esp-matter`;
  set `IDF_PATH` / `ESP_MATTER_PATH` to use existing checkouts. `fetch` follows esp-matter's own
  documented procedure (shallow submodules, `checkout_submodules.py --platform esp32 linux`).
- **Python version.** esp-matter's install does not resolve on Python 3.14. The ESP-IDF env and
  the esp-matter venv must share one interpreter; `dev.py doctor esp32` checks that. After a failed
  install, move `esp-matter/connectedhomeip/connectedhomeip/.environment` aside before retrying: a
  half-built venv fails the next run with `pw: command not found`.
- **Where `idf.py` runs.** In `firmware/esp32-matter/` for the Matter app and in
  `firmware/esp32-matter/smoketest/` for `busmon`. Each has its own `sdkconfig` and `build/`.
  `dev.py` only calls `idf.py set-target` when the configured target differs, because that call
  wipes both.
- **Brand-new board.** Run `dev.py erase esp32 --port P` once before the first flash. A vendor
  test image can leave a Wi-Fi config in NVS that makes every commissioning attempt fail with
  `CHIP Error 0x000000AC` (full story in `firmware/esp32-matter/README.md`). Never erase a
  commissioned node: that wipes its fabric.
- **Dev build vs release.** `dev.py build` is a plain `idf.py build`. An image that goes out over
  OTA must come from `dev.py ota esp32 release`, which runs `esp32-release.sh` and archives the
  delta base first ([OTA Updates](OTA-Updates#esp32-delta-ota)).

## AmebaZ2 (stock module)

```
python3 firmware/scripts/dev.py fetch amebaz2      # firmware/setup.sh then scripts/setup.sh, ~15 GB
python3 firmware/scripts/dev.py test amebaz2       # host QA + ota-release.sh lint
python3 firmware/scripts/dev.py build amebaz2      # ota-release.sh build: full clean, FWHS serial, verify
```

`dev.py flash amebaz2` does not write anything. It prints the two real paths, because both carry
risks a wrapper should not hide: a first install is a SOIC-8 clip write
([Installing the Firmware](Installing-Custom-Firmware), dump the chip first), and a commissioned
node takes `dev.py ota amebaz2 release`. Read `firmware/docs/10-firmware-ota-procedure.md` before
the first OTA.

Two AmebaZ2 build failures seen while verifying, both with misleading symptoms:

- **`No module named 'matter'` in AmebaZ2 codegen, zero ninja steps.** The connectedhomeip
  pigweed venv is dead, usually because the host Python was upgraded underneath it. `ota-release.sh`
  now stops earlier with that diagnosis. Rebuild the venv with a Python the SDK supports first on
  `PATH` (3.11 worked): `cd <sdk>/connectedhomeip && rm -rf .environment && source scripts/bootstrap.sh`.
- **`make: *** [Makefile:49: is_matter] Error 2` with no visible cause.** The console shows only
  the tail of that step; the full output is in `/tmp/ota-ismatter.log`. Once it held
  `Segmentation fault` from the Realtek `arm-none-eabi-gcc` driver, crashing in its own license
  ("visa") check before compiling. An unchanged re-run passed, and the crash could not be
  reproduced on demand, so treat it as intermittent: re-run before debugging further.

## Bench stage: no A/C

`bench` flashes the bus-facing firmware and starts `firmware/test/virtual_ac.py` on a second USB
adapter that plays the A/C's part. For `esphome` it flashes the normal image and follows its logs;
for `esp32` it flashes `smoketest/` (`busmon`, a live bus monitor: the real driver's handshake,
poll and status parse, logging every decoded frame).

Two ways to wire it, no mains anywhere:

| Adapter | Connect |
|---|---|
| USB-TTL, **3.3 V** | board TX to adapter RX, board RX to adapter TX, GND to GND. No transceiver. DE is driven but unused. |
| USB-RS485 | the board's transceiver A to A, B to B, GND to GND. Exercises DE timing too. |

Board pins are C3 TX 5 / RX 6 / DE 10, classic TX 19 / RX 18 / DE 4.

```
python3 firmware/scripts/dev.py bench esphome --board c3 --port /dev/ttyACM0 --sim-port /dev/ttyUSB0
python3 firmware/scripts/dev.py bench esp32   --board c3 --port /dev/ttyACM0 --sim-port /dev/ttyUSB0
```

On ESPHome the pass is the `AC bus link` binary sensor turning on and the climate entity
populating. On `esp32`, the simulator side first answers the handshake:

```
# virtual A/C up. initial: {'power': True, 'mode': 'cool', 'setpoint': 24, ...}
  [0x0A] handshake poll -> echoed slave reply
```

then `busmon` logs one decoded frame per second, with `frames` and `RX` both climbing:

```
I busmon: A/C #1: power=1 mode=... set=24C indoor=25C outdoor=32C fan=0x.. comp=..Hz ...
W busmon: t=3s: frames=3 TX=... RX=... | rx_task_up=1 ...
```

`frames=0` with `TX` climbing and `RX=0` is a wiring fault: TX and RX swapped, no common ground, or
(through a transceiver) DE not reaching the chip.

## After the bench

`dev.py next <target>` prints the remaining stages: real bus with the board USB-powered and **only
A/B** connected (the ground-loop rule), then full integration powered from the connector. Its
warnings (3.3 V transceiver, the C3 DE pulldown, C3 USB pins, classic PSRAM pins) are the ones in
[ESP32 Replacement Build](ESP32-Replacement-Build).
