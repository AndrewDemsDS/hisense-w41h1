# Build, Flash and Test

Getting started from source. Every step goes through `firmware/scripts/dev.py`, which prints each
command before running it, so you can also copy the commands and run them by hand. The
task-oriented walkthrough is the [User Guide](User-Guide); this page has the detail behind it.

← back to [Home](Home) · siblings: [User Guide](User-Guide) ·
[Repo Map and Build Pipeline](Repo-Map-and-Build-Pipeline) · [Testing and QA](Testing-and-QA) ·
[ESPHome Build](ESPHome-Build) · [ESP32 Replacement Board](ESP32-Replacement-Build)

---

## Which board

| `--board` | Hardware | Page |
|---|---|---|
| `c3` (default) | ESP32-C3 SuperMini plus a 3.3 V transceiver | [ESP32 Replacement Board](ESP32-Replacement-Build) |
| `classic` | classic ESP32 (D0WDQ6) plus a 3.3 V transceiver | [ESP32 Replacement Board](ESP32-Replacement-Build) |
| `amebaz2-factory`, `amebaz2-sdk`, `amebaz2-native` | the stock W41H1 module through LibreTiny, by the flash layout the unit has | [ESPHome Build](ESPHome-Build#the-stock-module-without-a-replacement-board) |

## Verification status

What each step has been run against. "Simulator" means `virtual_ac.py`, not an A/C.

| Step | ESP32 boards | stock module |
|---|---|---|
| Host QA (`dev.py test`) | CI, every push (plus `esphome config` on every board file) | CI, every push (plus a compile of all three layouts) |
| `dev.py build` on a real toolchain | yes, C3, 2026-09-14 | yes, all three layouts |
| Fresh clone on clean Ubuntu 24.04: `doctor`, `fetch`, `test`, `build` | yes, both boards, 2026-09-15 | not run |
| Node on a live A/C, USB-powered (stage 2) | hardware | n/a |
| Powered from the connector (stage 3) | hardware (units run this way) | hardware, factory layout only (2026-10-10) |
| `dev.py` flash / bench on hardware | untested | n/a (no serial flash through `dev.py`) |

If a step fails, the printed `$ ...` command is the thing to debug.

## The guided flow

```
python3 firmware/scripts/dev.py walk esphome [--board c3|classic] [--port /dev/ttyACM0]
```

It asks before each step: check the tools, host QA and the config check, build, (erase), flash and
logs, then prints the staged bring-up with its safety warnings. Each step is also a standalone
command:

| Command | Does |
|---|---|
| `doctor esphome` | read-only: tools on `PATH`, ESPHome at the pinned version, `secrets.yaml` present |
| `fetch esphome` | `pipx install esphome==2026.7.4`, create `secrets.yaml` (asks first) |
| `test esphome` | `firmware/test/run_tests.sh`, then `esphome config` |
| `build esphome` | `esphome compile` |
| `erase esphome --port P` | erase-flash, brand-new boards only, needs typed `ERASE` |
| `flash esphome --port P` | flash over USB, then logs |
| `monitor esphome --port P` | serial logs only |
| `bench esphome --port P --sim-port S` | firmware against `virtual_ac.py`, no A/C |
| `next esphome` | the staged bring-up and its warnings |

`--board` defaults to `c3`. Each value names one board file (`c3` is `w41h1-esp32c3.yaml`, `c3-plus`
is `w41h1-esp32c3-plus.yaml`, `classic` is `w41h1.yaml`), so `dev.py` builds the same image as
`esphome compile <that file>`. With
`--board amebaz2-factory`, `amebaz2-sdk` or `amebaz2-native` there is `test` and `build`. There is
no serial flash for the module through `dev.py`
([ESPHome Build](ESPHome-Build#the-stock-module-without-a-replacement-board)).

A stock module that still runs the retired Matter firmware is moved to ESPHome with a script of
its own, `firmware/scripts/matter-convert.py`: `convert --board amebaz2-factory [--stage]` wraps
the built ESPHome image as a Matter `.ota` that firmware installs, and `slots <ip>` and
`backup <ip> [out.bin]` are read-only probes of such a unit
([Converting a Stock Module](Converting-a-Stock-Module-to-ESPHome)).

ESPHome builds go through a PlatformIO build cache (`PLATFORMIO_BUILD_CACHE_DIR`, default
`~/.cache/w41h1-dev/pio-build-cache`). It speeds up the boards PlatformIO compiles (the stock
module, ESP8266, RP2040) and does nothing for the ESP32 boards, which ESPHome builds with ESP-IDF's
tools and ccache. Set the variable to an empty string to switch it off. The numbers and the two
caveats are in `firmware/esphome/README.md`, "Build cache".

## Build and flash

```
python3 firmware/scripts/dev.py fetch esphome      # pipx install esphome==2026.7.4, creates secrets.yaml
python3 firmware/scripts/dev.py test esphome       # host QA + esphome config
python3 firmware/scripts/dev.py flash esphome --board c3 --port /dev/ttyACM0
```

Fill in `firmware/esphome/secrets.yaml` (gitignored) before building. Each board has
a file of its own with its pins in it, and `--board` picks the file, so no YAML needs editing to
switch boards. `dev.py` uses the `esphome` on `PATH`; set `ESPHOME` to another install or to a
command with arguments (`ESPHOME="uvx esphome"`). Later updates go over the air with
`esphome run`, passing the same board overrides you flashed with: a C3 node given the classic
defaults receives an image for the wrong chip. Both command lines are in
[ESPHome Build](ESPHome-Build#updates).

## Bench stage: no A/C

`bench` flashes the normal image and starts `firmware/test/virtual_ac.py` on a second USB adapter
that plays the A/C's part, then follows the node's logs.

Two ways to wire it, no mains anywhere:

| Adapter | Connect |
|---|---|
| USB-TTL, **3.3 V** | board TX to adapter RX, board RX to adapter TX, GND to GND. No transceiver. DE is driven but unused. |
| USB-RS485 | the board's transceiver A to A, B to B, GND to GND. Exercises DE timing too. |

Board pins are C3 TX 5 / RX 6 / DE 10, classic TX 19 / RX 18 / DE 4.

```
python3 firmware/scripts/dev.py bench esphome --board c3 --port /dev/ttyACM0 --sim-port /dev/ttyUSB0
```

The simulator side first answers the handshake:

```
# virtual A/C up. initial: {'power': True, 'mode': 'cool', 'setpoint': 24, ...}
  [0x0A] handshake poll -> echoed slave reply
```

The pass is the `AC bus link` binary sensor turning on and the climate entity populating. A link
that stays off with the node sending is a wiring fault: TX and RX swapped, no common ground, or
(through a transceiver) DE not reaching the chip.

To look at the bus without Home Assistant in the way, `firmware/esp32-recon/` is a small ESP-IDF
tool for the same board: a console on the wire that decodes every frame, with a tap mode that only
listens.

## After the bench

`dev.py next esphome` prints the remaining stages: real bus with the board USB-powered and **only
A/B** connected (the ground-loop rule), then full integration powered from the connector. Its
warnings (3.3 V transceiver, the C3 DE pulldown, C3 USB pins, classic PSRAM pins) are the ones in
[ESP32 Replacement Board](ESP32-Replacement-Build).
