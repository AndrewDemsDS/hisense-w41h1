# ESP32 Build Environment

From a clean Linux machine to a built and flashed ESP32 board, for both ESP32 firmwares: ESPHome
and esp-matter. This page covers the part the other pages assume: which tools you need, where they
come from, which directory each command runs in, and the same steps by hand for when you would
rather not use the wrapper script.

← back to [Home](Home) · siblings: [User Guide](User-Guide) ·
[Build, Flash & Test](Build-Flash-Test) · [ESPHome Build](ESPHome-Build) ·
[ESP32 Replacement Build](ESP32-Replacement-Build)

---

## What comes from where

None of the build tools live in this repository. The repository holds the firmware source and one
wrapper, `firmware/scripts/dev.py`, which installs and drives the tools below.

| Tool | Comes from | Version | Needed for |
|---|---|---|---|
| `esphome` | PyPI, installed with `pipx` | `2026.7.4` (the version CI validates against) | ESPHome |
| `idf.py`, compilers, `esptool` | Espressif's ESP-IDF, a git checkout plus its `install.sh` | `IDF_PIN` in `versions.env` | esp-matter |
| Matter SDK | Espressif's esp-matter, a git checkout plus its `install.sh` | `ESP_MATTER_PIN` in `versions.env` | esp-matter |
| `dev.py` | this repository | | both (optional) |

`versions.env` at the repository root is the single source of truth for the SDK pins, so the
by-hand commands below read the values from that file.

Two things about `idf.py` cost newcomers the most time:

- It is on `PATH` only after you source ESP-IDF's `export.sh` in the same shell. A new terminal
  starts without it.
- It runs only inside an ESP-IDF project directory. Here that is `firmware/esp32-matter/` (the
  Matter app) or `firmware/esp32-matter/smoketest/` (the bus monitor). Run from the repository root
  it stops with a missing `CMakeLists.txt` error.

ESPHome needs neither ESP-IDF nor esp-matter from you. It downloads its own toolchain on the first
build.

## Pick a path

| | ESPHome | esp-matter |
|---|---|---|
| Reaches | Home Assistant only | Matter: Home Assistant, Apple, Google, Alexa |
| You install | `pipx`, then `esphome` | about 30 host packages, ESP-IDF, esp-matter |
| Disk | about 5 GB (in `~/.cache/esphome`) | about 16 GB |
| After flashing | Home Assistant discovers it | commission it over Matter |

Hardware and wiring are the same for both: [Hardware & Wiring](Hardware-and-Wiring) and the pin
tables in [ESP32 Replacement Build](ESP32-Replacement-Build).

## Before either path

A Linux x86_64 machine. Package names are for Debian and Ubuntu.

```
sudo apt install git python3 g++
git clone --recurse-submodules https://github.com/AndrewDemsDS/hisense-w41h1.git
cd hisense-w41h1
```

`g++` builds the host tests. The submodule is the companion Home Assistant integration, which one
host test checks against.

Know which board you have, because every later command needs it:

| Board | `dev.py --board` | `idf.py set-target` | ESPHome `board` | TX / RX / DE |
|---|---|---|---|---|
| ESP32-C3 SuperMini | `c3` (the default) | `esp32c3` | `esp32-c3-devkitm-1` | 5 / 6 / 10 |
| classic ESP32 (DevKit, D0WDQ6) | `classic` | `esp32` | `esp32dev` | 19 / 18 / 4 |

The serial port is usually `/dev/ttyACM0` for a C3 SuperMini (native USB) and `/dev/ttyUSB0` for a
classic dev board (USB-UART bridge). If opening it fails with a permission error, your user lacks
access to the serial device; on Debian and Ubuntu that access comes from the `dialout` group.

## ESPHome

### With dev.py

From the repository root:

```
sudo apt install pipx
python3 firmware/scripts/dev.py fetch esphome
python3 firmware/scripts/dev.py test esphome
python3 firmware/scripts/dev.py erase esphome --port /dev/ttyACM0
python3 firmware/scripts/dev.py flash esphome --board c3 --port /dev/ttyACM0
```

`fetch` runs `pipx install esphome==2026.7.4` and creates `firmware/esphome/secrets.yaml` from the
example. Fill that file in before `test`: your Wi-Fi name and password, and a new API key. `erase`
is for a brand-new board only and asks you to type `ERASE`. `flash` builds, writes the board and
follows its log.

### By hand

```
sudo apt install pipx
pipx install esphome==2026.7.4
pipx ensurepath
```

Open a new terminal so `esphome` is on `PATH`, then:

```
cd firmware/esphome
cp secrets.yaml.example secrets.yaml
python3 -c "import base64,os; print(base64.b64encode(os.urandom(32)).decode())"
```

Put the printed key in `secrets.yaml` as `hisense_ac__encryption_key`, and set `wifi_ssid` and
`wifi_password`. The file is gitignored. Then validate, and build and flash:

```
esphome config w41h1.yaml

# classic ESP32: the defaults in w41h1.yaml
esphome run w41h1.yaml --device /dev/ttyUSB0

# ESP32-C3 SuperMini: override the board and pins
esphome -s board esp32-c3-devkitm-1 -s tx_pin 5 -s rx_pin 6 -s de_pin 10 run w41h1.yaml --device /dev/ttyACM0
```

`esphome config` needs no toolchain and finishes in seconds, so run it first. The first
`esphome run` downloads ESP-IDF into `~/.cache/esphome` and takes several minutes. Later updates go
over Wi-Fi with the same `esphome run` line and no `--device`. Keep passing the C3 overrides to a
C3 node: without them it receives an image for the wrong chip.

Home Assistant then finds the node over mDNS. Adopt it with the API key from `secrets.yaml`
([Commissioning & HA Setup](Commissioning-and-HA-Setup#esphome-build-adopt-it)).

## esp-matter

### Host packages

ESP-IDF's and connectedhomeip's prerequisites, as one line:

```
sudo apt install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 gcc g++ pkg-config curl libdbus-1-dev libglib2.0-dev libavahi-client-dev python3-dev unzip libgirepository1.0-dev libcairo2-dev libreadline-dev libevent-dev
```

Install these first. A missing `libusb-1.0` fails ESP-IDF's `install.sh` only after it has
downloaded every toolchain, and missing `curl` or glib fails esp-matter's install deep inside
connectedhomeip's bootstrap.

The two SDKs must share one Python interpreter, and esp-matter's install does not resolve on Python
3.14. On a host whose `python3` is 3.14 or newer, use `dev.py`: it runs both installs under a
uv-managed Python 3.12 (`uv python install 3.12` if you have none) or under the interpreter you name
with `ESP_PYTHON=/path/to/python3.12`.

### With dev.py

From the repository root:

```
python3 firmware/scripts/dev.py doctor esp32
python3 firmware/scripts/dev.py fetch esp32
python3 firmware/scripts/dev.py test esp32
python3 firmware/scripts/dev.py build esp32 --board c3
python3 firmware/scripts/dev.py erase esp32 --port /dev/ttyACM0
python3 firmware/scripts/dev.py flash esp32 --board c3 --port /dev/ttyACM0
```

`doctor` is read-only and lists what is missing. `fetch` clones ESP-IDF into `~/esp/esp-idf` and
esp-matter into `~/esp/esp-matter` at the pinned versions and runs both installers; set `IDF_PATH`
and `ESP_MATTER_PATH` first to use other locations or existing checkouts. `build`, `erase` and
`flash` source both environments themselves, so you never source `export.sh` on this route.

### By hand

These are the commands `fetch` and the hosted CI build run. Start in the repository root so the
pins come from `versions.env`:

```
. ./versions.env
echo "$IDF_PIN $ESP_MATTER_PIN"
```

ESP-IDF, with the toolchains for both chips:

```
git clone -b "$IDF_PIN" --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh esp32,esp32c3
. ./export.sh
```

esp-matter, in the same shell so its installer uses ESP-IDF's Python environment:

```
git init -q ~/esp/esp-matter
cd ~/esp/esp-matter
git remote add origin https://github.com/espressif/esp-matter.git
git fetch --depth 1 origin "$ESP_MATTER_PIN"
git checkout -q FETCH_HEAD
git submodule update --init --depth 1
(cd connectedhomeip/connectedhomeip && ./scripts/checkout_submodules.py --platform esp32 linux --shallow)
./install.sh --no-host-tool
```

`--no-host-tool` skips building `chip-tool` and `chip-cert`, which the firmware build does not use.
The shallow fetch of one commit is what keeps the download to a few GB.

Then, in every new shell before building:

```
. ~/esp/esp-idf/export.sh
. ~/esp/esp-matter/export.sh
```

Build, from the project directory:

```
cd firmware/esp32-matter
idf.py set-target esp32c3
idf.py build
```

Use `esp32` in place of `esp32c3` for a classic board. `set-target` wipes that directory's
`sdkconfig` and `build/`, so run it once per board type. The image is
`build/hisense_ac_matter.bin`.

Flash and watch the console, with the board on USB:

```
idf.py -p /dev/ttyACM0 erase-flash
idf.py -p /dev/ttyACM0 flash monitor
```

Run `erase-flash` once on a brand-new board and never on a commissioned one. A vendor test image
can leave a Wi-Fi config in NVS that makes every commissioning attempt fail with
`CHIP Error 0x000000AC`, and `flash` alone does not touch NVS. On a commissioned node the same
command wipes its fabric and Wi-Fi settings.

Next is commissioning: [Commissioning & HA Setup](Commissioning-and-HA-Setup).

A plain `idf.py build` is a development build. An image meant to go out over the air has to come
from `dev.py ota esp32 release`, which archives the delta base first
([OTA Updates](OTA-Updates#esp32-delta-ota)).

## Check the board without an A/C

`firmware/test/virtual_ac.py` is a software model of the indoor unit. It runs on your computer,
attached to a second USB serial adapter, and answers the board the way the A/C mainboard would. It
lets you see the firmware handshake and decode status before any wire touches the appliance.

`firmware/esp32-matter/smoketest/` is a second, smaller ESP-IDF project called `busmon`: the real
bus driver with no Matter stack, logging every decoded frame to the USB console. It is the esp-matter
bench firmware. The ESPHome build needs no equivalent, because its normal image already logs the
bus.

One command flashes the bench firmware and starts the simulator:

```
python3 firmware/scripts/dev.py bench esphome --board c3 --port /dev/ttyACM0 --sim-port /dev/ttyUSB0
python3 firmware/scripts/dev.py bench esp32   --board c3 --port /dev/ttyACM0 --sim-port /dev/ttyUSB0
```

`--port` is the board and `--sim-port` is the adapter. The simulator needs pyserial
(`sudo apt install python3-serial`). The wiring for a 3.3 V USB-TTL or a USB-RS485 adapter and the
output that counts as a pass are in [Build, Flash & Test](Build-Flash-Test#bench-stage-no-ac).

## When it fails

| Symptom | Cause | Fix |
|---|---|---|
| `idf.py: command not found` | ESP-IDF's `export.sh` was not sourced in this shell | source both `export.sh` files, or use `dev.py` |
| `idf.py` reports a missing `CMakeLists.txt` | run from the repository root | `cd firmware/esp32-matter` |
| CMake stops with `ESP_MATTER_PATH not set` | only ESP-IDF's `export.sh` was sourced | source esp-matter's `export.sh` too |
| ESP-IDF's `install.sh` fails at the end, on openocd or libusb | `libusb-1.0-0` missing | install the host packages, then `dev.py fetch esp32` again |
| esp-matter's `install.sh` fails at "Installing pip requirements" | glib or `curl` missing | the same |
| `pw: command not found` on a second esp-matter install | connectedhomeip's environment is already bootstrapped | `./install.sh --no-host-tool --no-bootstrap`, which `dev.py fetch esp32` picks by itself |
| `pip` refuses with "externally-managed" during esp-matter's install | ESP-IDF's `export.sh` was not sourced first, so `python3` is the distro's | source it, then run `install.sh` again |
| a build breaks after a host Python upgrade | the SDK virtual environments point at the removed interpreter | `dev.py doctor esp32` names the dead one |
| commissioning fails instantly with `CHIP Error 0x000000AC` | stale vendor NVS on a new board | erase the flash once, then flash again |
| `esphome: command not found` after `pipx install` | `~/.local/bin` is not on `PATH` yet | `pipx ensurepath`, then a new terminal |

If a `dev.py` step fails, it has already printed the exact command as a `$ ...` line. Run that line
by hand to debug it.

## After the first flash

Do not wire a freshly flashed board straight into the A/C. Follow the three stages in
[ESP32 Replacement Build](ESP32-Replacement-Build#staged-bring-up-never-leave-the-ac-in-an-unknown-state):
bench, then the real bus with only A and B connected while the board is on USB power, then powered
from the connector. `python3 firmware/scripts/dev.py next esp32` (or `next esphome`) prints them
with their warnings. The rule that matters most at stage 2: while the board is on USB, do not also
connect the A/C's ground or 5 V.
