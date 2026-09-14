# Build, Flash and Test

Getting started from source, one section per firmware target. Every step goes through
`firmware/scripts/dev.sh`, which wraps the real scripts (`ota-release.sh`, `esp32-release.sh`,
`esp32-lint.sh`, the setup scripts) and prints each command before running it, so you can also
copy the commands and run them by hand.

← back to [Home](Home) · siblings: [Repo Map and Build Pipeline](Repo-Map-and-Build-Pipeline) ·
[Testing and QA](Testing-and-QA) · [ESP32 Replacement Build](ESP32-Replacement-Build) ·
[ESPHome Build](ESPHome-Build)

---

## Which target

| Target | Hardware | Reaches | Page |
|---|---|---|---|
| `amebaz2` | the stock W41H1 module, reflashed over a SOIC-8 clip | Matter (HA, Apple, Google, Alexa) | [Installing the Firmware](Installing-Custom-Firmware) |
| `esp32` | an ESP32-C3 SuperMini or classic ESP32 plus a 3.3 V transceiver | Matter | [ESP32 Replacement Build](ESP32-Replacement-Build) |
| `esphome` | the same ESP32 board and wiring | Home Assistant only | [ESPHome Build](ESPHome-Build) |

## Verification status

What each step has been run against. "Simulator" means `virtual_ac.py`, not an A/C.

| Step | amebaz2 | esp32 (C3) | esp32 (classic) | esphome |
|---|---|---|---|---|
| Host QA (`dev.sh test`) | CI, every push | CI, every push | CI, every push | CI, every push (plus `esphome config`) |
| `dev.sh build` on a real toolchain | yes, 2026-09-14 (v10332) | pending | untested | yes, C3, 2026-09-14 |
| `busmon` against the real bus | n/a | untested | hardware (2026-07-12) | n/a |
| Matter app against `virtual_ac.py` | n/a | simulator (2026-08-07) | untested | n/a |
| Node on a live A/C, USB-powered (stage 2) | hardware | untested on these pins | hardware | hardware |
| Powered from the connector (stage 3) | hardware | untested | see [ESP32 Replacement Build](ESP32-Replacement-Build) | outstanding |
| `dev.sh` flash / bench on hardware | n/a (clip) | untested | untested | untested |

Builds have been run through the wrapper; nothing has yet been flashed or benched through it on a
real board. If a step fails, the printed `$ ...` command is the thing to debug.

Two build failures seen while verifying, both with misleading symptoms:

- **`No module named 'matter'` in AmebaZ2 codegen, zero ninja steps.** The connectedhomeip
  pigweed venv is dead, usually because the host Python was upgraded underneath it. `ota-release.sh`
  now stops earlier with that diagnosis. Rebuild the venv with a Python the SDK supports first on
  `PATH` (3.11 worked): `cd <sdk>/connectedhomeip && rm -rf .environment && source scripts/bootstrap.sh`.
- **`make: *** [Makefile:49: is_matter] Error 2` with no visible cause.** The console shows only
  the tail of that step; the full output is in `/tmp/ota-ismatter.log`. Once it held
  `Segmentation fault` from the Realtek `arm-none-eabi-gcc` driver, crashing in its own license
  ("visa") check before compiling. An unchanged re-run passed, and the crash could not be
  reproduced on demand, so treat it as intermittent: re-run before debugging further.

## The guided flow

```
firmware/scripts/dev.sh walk <target> [--board c3|classic] [--port /dev/ttyACM0]
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

`--board` defaults to `c3`. It picks the IDF target (`esp32c3` or `esp32`) and, for ESPHome, the
board and pin substitutions passed to `esphome -s`.

## ESP32 (esp-matter)

```
firmware/scripts/dev.sh fetch esp32        # ESP-IDF v5.5.4 + esp-matter at the pinned SHA
firmware/scripts/dev.sh test esp32
firmware/scripts/dev.sh build esp32 --board c3
firmware/scripts/dev.sh flash esp32 --board c3 --port /dev/ttyACM0
```

- **Where things live.** ESP-IDF defaults to `~/esp/esp-idf` and esp-matter to `~/esp/esp-matter`;
  set `IDF_PATH` / `ESP_MATTER_PATH` to use existing checkouts. `fetch` follows esp-matter's own
  documented procedure (shallow submodules, `checkout_submodules.py --platform esp32 linux`), with
  `install.sh --no-host-tool`: chip-tool and the other host tools are not needed to build firmware.
- **Python version.** esp-matter's install does not resolve on Python 3.14. `dev.sh` uses
  `ESP_PYTHON` if set, otherwise on a 3.14+ host a uv-managed 3.12 (`uv python install 3.12`), for
  both the ESP-IDF env and the esp-matter venv; they must share one interpreter. `dev.sh doctor
  esp32` checks that. After a failed install, move
  `esp-matter/connectedhomeip/connectedhomeip/.environment` aside before retrying: a half-built
  venv fails the next run with `pw: command not found`.
- **Where `idf.py` runs.** In `firmware/esp32-matter/` for the Matter app and in
  `firmware/esp32-matter/smoketest/` for `busmon`. Each has its own `sdkconfig` and `build/`.
  `dev.sh` only calls `idf.py set-target` when the configured target differs, because that call
  wipes both.
- **Brand-new board.** Run `dev.sh erase esp32 --port P` once before the first flash. A vendor
  test image can leave a Wi-Fi config in NVS that makes every commissioning attempt fail with
  `CHIP Error 0x000000AC` (full story in `firmware/esp32-matter/README.md`). Never erase a
  commissioned node: that wipes its fabric.
- **Dev build vs release.** `dev.sh build` is a plain `idf.py build`. An image that goes out over
  OTA must come from `esp32-release.sh`, which archives the delta base first
  ([OTA Updates](OTA-Updates#esp32-delta-ota)).

## ESPHome

```
firmware/scripts/dev.sh fetch esphome      # esphome==2026.7.4 via uv (or pipx), creates secrets.yaml
firmware/scripts/dev.sh test esphome       # host QA + esphome config
firmware/scripts/dev.sh flash esphome --board c3 --port /dev/ttyACM0
```

Fill in `firmware/esphome/secrets.yaml` (gitignored) before building. `w41h1.yaml` keeps the
classic ESP32 defaults; `--board c3` overrides `board`, `tx_pin`, `rx_pin` and `de_pin` on the
command line, so the YAML never needs editing to switch boards. Later updates go over the air with
plain `esphome run w41h1.yaml`.

## AmebaZ2 (stock module)

```
firmware/scripts/dev.sh fetch amebaz2      # firmware/setup.sh then scripts/setup.sh, ~15 GB
firmware/scripts/dev.sh test amebaz2       # host QA + ota-release.sh lint
firmware/scripts/dev.sh build amebaz2      # ota-release.sh build: full clean, FWHS serial, verify
```

`dev.sh flash amebaz2` does not write anything. It prints the two real paths, because both carry
risks a wrapper should not hide: a first install is a SOIC-8 clip write
([Installing the Firmware](Installing-Custom-Firmware), dump the chip first), and a commissioned
node takes `ota-release.sh package`, `stage`, `flash`. Read `firmware/docs/10-firmware-ota-procedure.md`
before the first OTA.

## Bench stage: no A/C

`bench` flashes the bus-facing firmware and starts `firmware/test/virtual_ac.py` on a second USB
adapter that plays the A/C's part. For `esp32` it flashes `smoketest/` (`busmon`, a live bus
monitor: the real driver's handshake, poll and status parse, logging every decoded frame); for
`esphome` it flashes the normal image and follows its logs.

Two ways to wire it, no mains anywhere:

| Adapter | Connect |
|---|---|
| USB-TTL, **3.3 V** | board TX to adapter RX, board RX to adapter TX, GND to GND. No transceiver. DE is driven but unused. |
| USB-RS485 | the board's transceiver A to A, B to B, GND to GND. Exercises DE timing too. |

Board pins are C3 TX 5 / RX 6 / DE 10, classic TX 19 / RX 18 / DE 4.

```
firmware/scripts/dev.sh bench esp32 --board c3 --port /dev/ttyACM0 --sim-port /dev/ttyUSB0
```

Passing output. The simulator side first answers the handshake:

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
(through a transceiver) DE not reaching the chip. On ESPHome the equivalent pass is the `AC bus
link` binary sensor turning on and the climate entity populating.

## After the bench

`dev.sh next <target>` prints the remaining stages: real bus with the board USB-powered and **only
A/B** connected (the ground-loop rule), then full integration powered from the connector. Its
warnings (3.3 V transceiver, the C3 DE pulldown, C3 USB pins, classic PSRAM pins) are the ones in
[ESP32 Replacement Build](ESP32-Replacement-Build).
