# User Guide

One script takes you from a fresh clone to a running A/C node: `firmware/scripts/dev.py`. It
checks your tools, installs the pinned ESPHome, runs the host tests, builds and flashes. It prints
every command before it runs it and asks before anything slow or destructive, so you can always see
what is happening and repeat a step by hand.

← back to [Home](Home) · details: [Build, Flash & Test](Build-Flash-Test) ·
[ESPHome Build](ESPHome-Build)

---

## Pick the hardware

There is one firmware, the `hisense_ac` ESPHome component. It reaches Home Assistant over the
ESPHome native API. What you choose is the board it runs on:

| # | Hardware | `--board` | Status | Why pick it |
|---|---|---|---|---|
| 1 | an ESP32-C3 SuperMini or classic ESP32 plus a 3.3 V RS-485 transceiver, in place of the module | `c3` (default), `classic` | runs on live units since 2026-08 | about 5 EUR of parts, flashed and recovered over USB, a bad update rolls back |
| 2 | the stock W41H1 module, through LibreTiny | `amebaz2-factory`, `amebaz2-sdk`, `amebaz2-native` | first run in an A/C on 2026-10-10 | nothing added to the unit, but no rollback, and a SOIC-8 clip is the only recovery |
| 3 | another chip ESPHome supports (ESP8266, RP2040) | none, use the example YAML | compiles, never run | you already have the board |

Start with the ESP32 board unless you have a reason not to. The stock module route is described in
[ESPHome Build](ESPHome-Build#the-stock-module-without-a-replacement-board): read its risk list
before you choose it. The trade-offs with measured figures are in
`firmware/docs/13-path-comparison.md`.

The ESP32 route needs the wiring from [Hardware & Wiring](Hardware-and-Wiring) and the GPIO
warnings in [ESP32 Replacement Board](ESP32-Replacement-Build).

Apple Home, Google Home and Alexa are reached through Home Assistant's own Matter or HomeKit
bridge. The Matter firmwares this project used to have are retired
([Matter Firmware (Retired)](Matter-Firmware-Retired)).

## What you need

A Linux x86_64 machine. The package names below are for Debian and Ubuntu; `doctor` tells you what
is still missing. If ESPHome is new to you, read [Build Environment](ESP32-Build-Environment)
first: it says where each tool comes from and gives the steps by hand.

| For | Install | Disk |
|---|---|---|
| the host tests | `sudo apt install git python3 g++` | |
| ESPHome | `sudo apt install pipx` (`fetch` installs the pinned ESPHome with it) | about 5 GB: ESPHome downloads its own toolchain into `~/.cache/esphome`, so the first build takes several minutes |

Then clone the repository:

```
git clone https://github.com/AndrewDemsDS/hisense-w41h1.git
cd hisense-w41h1
```

All commands below run from the repository root.

## The short version

```
python3 firmware/scripts/dev.py walk esphome [--board c3|classic] [--port /dev/ttyACM0]
```

`walk` asks before each step: check the tools (and offer to install ESPHome), host tests and the
config check, build, erase if the board is brand new, flash and logs, then print the staged
bring-up with its safety warnings. Answer N to skip a step. Everything below is the same flow, one
command at a time.

`--board` is `c3` (ESP32-C3 SuperMini, the default) or `classic` (ESP32-D0WDQ6). It picks the chip
and the pins, so no file needs editing to switch boards.

## Step by step

```
# read-only: is esphome at the pinned version?
python3 firmware/scripts/dev.py doctor esphome
# pipx install esphome==2026.7.4, create secrets.yaml
python3 firmware/scripts/dev.py fetch esphome
# host tests + esphome config
python3 firmware/scripts/dev.py test esphome
# esphome compile
python3 firmware/scripts/dev.py build esphome --board c3
# brand-new board only, asks you to type ERASE
python3 firmware/scripts/dev.py erase esphome --port /dev/ttyACM0
python3 firmware/scripts/dev.py flash esphome --board c3 --port /dev/ttyACM0
```

1. After `fetch`, fill in `firmware/esphome/secrets.yaml` (Wi-Fi and an API encryption key). It is
   gitignored.
2. `flash` builds, writes the board over USB and follows the logs.
3. Home Assistant discovers the node over mDNS; adopt it with the API key from `secrets.yaml`
   ([ESPHome Build](ESPHome-Build#adopt-it-in-home-assistant)). If the node cannot join Wi-Fi it
   opens a `hisense-ac-setup` hotspot to set it.
4. Later updates go over Wi-Fi from ESPHome itself ([ESPHome Build](ESPHome-Build#updates)).

More: [ESPHome Build](ESPHome-Build).

## Test without an A/C

`bench` flashes the firmware and runs `virtual_ac.py` on a second USB adapter that plays the A/C:

```
python3 firmware/scripts/dev.py bench esphome --board c3 --port /dev/ttyACM0 --sim-port /dev/ttyUSB0
```

Wiring and what passing output looks like: [Build, Flash & Test](Build-Flash-Test#bench-stage-no-ac).

## Then

`python3 firmware/scripts/dev.py next esphome` prints the remaining bring-up stages (real bus with
only A/B connected while USB-powered, then powered from the connector) and the warnings that go with
them. After that: [Everyday Control](Everyday-Control), and [Recovery & Reflash](Recovery-and-Reflash)
if something goes wrong.

## Command reference

| Command | Does |
|---|---|
| `walk esphome` | the guided flow, asks before each step |
| `doctor esphome` | read-only: tools on `PATH`, ESPHome at the pinned version, `secrets.yaml` present |
| `fetch esphome` | install the pinned ESPHome with pipx (asks first) |
| `test esphome` | `firmware/test/run_tests.sh`, then `esphome config` |
| `build esphome` | compile the firmware |
| `erase esphome --port P` | erase-flash, brand-new ESP32 boards only, needs typed `ERASE` |
| `flash esphome --port P` | flash over USB, then logs |
| `monitor esphome --port P` | serial logs only |
| `bench esphome --port P --sim-port S` | firmware against `virtual_ac.py`, no A/C |
| `next esphome` | the staged bring-up and its warnings |
| `convert esphome --board amebaz2-factory [--stage]` | for a stock module that still runs the retired Matter firmware: wrap the ESPHome image so that firmware installs it ([how](Converting-a-Stock-Module-to-ESPHome)) |
| `matter-slots <ip>`, `matter-backup <ip>` | read the slot state of such a unit, and save its inactive slot |

`python3 firmware/scripts/dev.py --help` prints the same list.
