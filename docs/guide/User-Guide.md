# User Guide

One script takes you from a fresh clone to a running A/C node: `firmware/scripts/dev.py`. It
checks your tools, fetches the pinned SDKs, runs the host tests, builds, flashes and ships OTA
updates for all three firmwares. It prints every command before it runs it and asks before anything
slow or destructive, so you can always see what is happening and repeat a step by hand.

← back to [Home](Home) · details: [Build, Flash & Test](Build-Flash-Test) ·
[OTA Updates](OTA-Updates)

---

## Pick a firmware

In order of preference:

| # | Target | Hardware | Reaches | Why pick it |
|---|---|---|---|---|
| 1 | `esphome` | an ESP32-C3 SuperMini or classic ESP32 plus a 3.3 V RS-485 transceiver | Home Assistant only | smallest toolchain, no commissioning, no matter-server, every fault bit is its own entity |
| 2 | `esp32` | the same ESP32 board and wiring | Matter (HA, Apple, Google, Alexa) | you need a controller other than Home Assistant |
| 3 | `amebaz2` | the stock W41H1 module, reflashed once over a SOIC-8 clip | Matter | no added hardware, but a ~15 GB proprietary SDK and a clip write |

If Home Assistant is your only controller, use ESPHome. If anything else must see the A/C, use the
ESP32 Matter build. Reflash the stock module only when you want to keep the original hardware.
The full trade-off, with measured figures, is `firmware/docs/13-path-comparison.md`.

Both ESP32 targets need the wiring from [Hardware & Wiring](Hardware-and-Wiring) and the GPIO
warnings in [ESP32 Replacement Build](ESP32-Replacement-Build).

## What you need

A Linux x86_64 machine. The package names below are for Debian and Ubuntu; `doctor` tells you what
is still missing for your target.

| For | Install | Disk |
|---|---|---|
| everything | `sudo apt install git python3 g++` (`g++` builds the host tests that `test` runs) | |
| `esphome` | `sudo apt install pipx` (`fetch` installs the pinned ESPHome with it) | about 5 GB: ESPHome downloads its own ESP-IDF into `~/.cache/esphome`, so the first build takes several minutes |
| `esp32` | ESP-IDF's and connectedhomeip's prerequisites: `sudo apt install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 gcc g++ pkg-config curl libdbus-1-dev libglib2.0-dev libavahi-client-dev python3-dev unzip libgirepository1.0-dev libcairo2-dev libreadline-dev libevent-dev` (`doctor esp32` checks the ones that fail the fetch) | about 10 GB for ESP-IDF and esp-matter |
| `amebaz2` | `sudo` rights: `fetch` installs its own host packages. A CH341A programmer and SOIC-8 clip for the first install | about 30 GB for the Realtek SDKs and connectedhomeip |

Then clone the repository with its submodule (the companion Home Assistant integration, which one
of the host tests checks against):

```
git clone --recurse-submodules https://github.com/AndrewDemsDS/hisense-w41h1.git
cd hisense-w41h1
```

All commands below run from the repository root. For OTA updates you also need
`firmware/scripts/ota-release.env`, copied from `ota-release.env.example` and filled in.

## The short version

```
python3 firmware/scripts/dev.py walk <target> [--board c3|classic] [--port /dev/ttyACM0]
```

`walk` asks before each step: check tools and SDK pins (and offer to fetch), host tests and lint,
build, erase if the board is brand new, flash and monitor, then print the staged bring-up with its
safety warnings. Answer N to skip a step. Everything below is the same flow, one command at a time.

`--board` is `c3` (ESP32-C3 SuperMini, the default) or `classic` (ESP32-D0WDQ6). It picks the chip
and the pins, so no file needs editing to switch boards.

## 1. ESPHome (recommended)

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
   ([Commissioning & HA Setup](Commissioning-and-HA-Setup#esphome-build-adopt-it)). If the node
   cannot join Wi-Fi it opens a `hisense-ac-setup` hotspot to set it.
4. Later updates go over Wi-Fi from ESPHome itself
   ([OTA Updates](OTA-Updates#esphome-updates)). `dev.py ota` is Matter-only.

More: [ESPHome Build](ESPHome-Build).

## 2. ESP32 with Matter

```
python3 firmware/scripts/dev.py doctor esp32
# ESP-IDF + esp-matter at the pinned versions
python3 firmware/scripts/dev.py fetch esp32
# host tests + esp32-lint
python3 firmware/scripts/dev.py test esp32
python3 firmware/scripts/dev.py build esp32 --board c3
# brand-new board only
python3 firmware/scripts/dev.py erase esp32 --port /dev/ttyACM0
python3 firmware/scripts/dev.py flash esp32 --board c3 --port /dev/ttyACM0
```

1. `IDF_PATH` and `ESP_MATTER_PATH` default to `~/esp/esp-idf` and `~/esp/esp-matter`; set them to
   reuse existing checkouts.
2. Erase a brand-new board once before the first flash: stale vendor NVS makes commissioning fail
   with `CHIP Error 0x000000AC`. **Never erase a commissioned node**, that wipes its fabric.
3. Commission it into Home Assistant: [Commissioning & HA Setup](Commissioning-and-HA-Setup).
4. `dev.py build` is a development build. Updates that go out over the air use `dev.py ota`
   (below), which archives the delta base first.

More: [ESP32 Replacement Build](ESP32-Replacement-Build).

## 3. AmebaZ2 (stock module)

```
python3 firmware/scripts/dev.py doctor amebaz2
# firmware/setup.sh then scripts/setup.sh, ~15 GB
python3 firmware/scripts/dev.py fetch amebaz2
# host tests + ota-release.sh lint
python3 firmware/scripts/dev.py test amebaz2
# full clean, FWHS serial, verify
python3 firmware/scripts/dev.py build amebaz2
# prints the clip and OTA paths, writes nothing
python3 firmware/scripts/dev.py flash amebaz2
```

The first install is a SOIC-8 clip write, which `dev.py` deliberately does not do for you. Dump the
whole chip first, then follow [Installing the Firmware](Installing-Custom-Firmware). Prebuilt
images are on the GitHub Releases page if you would rather not build.

## Updating over the air (Matter targets)

Once a node is commissioned, updates never need a cable:

```
# host tests + tools + link quality
python3 firmware/scripts/dev.py ota <amebaz2|esp32> preflight
# build, package, stage, flash
python3 firmware/scripts/dev.py ota amebaz2 release --bump --flash
# the same for ESP32, as a delta patch
python3 firmware/scripts/dev.py ota esp32 release --flash
# read the version the node is running
python3 firmware/scripts/dev.py ota <amebaz2|esp32> verify
```

The single steps (`build`, `package`, `stage`, `flash`) are available the same way. Extra arguments
pass straight to the release script. Read [OTA Updates](OTA-Updates) before the first one:
the retry behaviour and the version rules are not optional.

## Test without an A/C

`bench` flashes the bus-facing firmware and runs `virtual_ac.py` on a second USB adapter that plays
the A/C:

```
python3 firmware/scripts/dev.py bench esphome --board c3 --port /dev/ttyACM0 --sim-port /dev/ttyUSB0
python3 firmware/scripts/dev.py bench esp32   --board c3 --port /dev/ttyACM0 --sim-port /dev/ttyUSB0
```

Wiring and what passing output looks like: [Build, Flash & Test](Build-Flash-Test#bench-stage-no-ac).

## Then

`python3 firmware/scripts/dev.py next <target>` prints the remaining bring-up stages (real bus with
only A/B connected while USB-powered, then powered from the connector) and the warnings that go with
them. After that: [Everyday Control](Everyday-Control), and [Recovery & Reflash](Recovery-and-Reflash)
if something goes wrong.

## Command reference

| Command | Does |
|---|---|
| `walk <target>` | the guided flow, asks before each step |
| `doctor <target>` | read-only: tools on `PATH`, SDK checkouts at the `versions.env` pins |
| `fetch <target>` | fetch the pinned SDKs or tools (asks first) |
| `test <target>` | `firmware/test/run_tests.sh`, then the target's lint |
| `build <target>` | build the app |
| `erase <target> --port P` | erase-flash, brand-new ESP32 boards only, needs typed `ERASE` |
| `flash <target> --port P` | flash, then monitor (AmebaZ2: prints the paths only) |
| `monitor <target> --port P` | serial console only |
| `bench <target> --port P --sim-port S` | firmware against `virtual_ac.py`, no A/C |
| `next <target>` | the staged bring-up and its warnings |
| `ota <amebaz2\|esp32> <step> [args]` | `preflight`, `verify`, `build`, `package`, `stage`, `flash`, `release`, `publish`, `tag` |

`python3 firmware/scripts/dev.py --help` prints the same list.
