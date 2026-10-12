# Build Environment

From a clean Linux machine to a built and flashed ESP32 board. This page covers the part the other
pages assume: which tools you need, where they come from, which directory each command runs in,
and the same steps by hand for when you would rather not use the wrapper script.

← back to [Home](Home) · siblings: [User Guide](User-Guide) ·
[Build, Flash & Test](Build-Flash-Test) · [ESPHome Build](ESPHome-Build) ·
[ESP32 Replacement Board](ESP32-Replacement-Build)

---

## What comes from where

None of the build tools live in this repository. The repository holds the firmware source and one
wrapper, `firmware/scripts/dev.py`, which installs and drives ESPHome.

| Tool | Comes from | Version | Needed for |
|---|---|---|---|
| `esphome` | PyPI, installed with `pipx` | `2026.7.4` (the version CI validates against) | building and flashing |
| compilers, `esptool`, ESP-IDF or PlatformIO | downloaded by ESPHome on the first build | chosen by ESPHome | nothing you install yourself |
| `dev.py` | this repository | | optional wrapper |
| `g++` | your distribution | any with C++17 | the host tests |

ESPHome needs no ESP-IDF checkout from you. It downloads its own toolchain into `~/.cache/esphome`
(about 5 GB) on the first build, which takes several minutes.

## Before you start

A Linux x86_64 machine. Package names are for Debian and Ubuntu.

```
sudo apt install git python3 g++
git clone https://github.com/AndrewDemsDS/hisense-w41h1.git
cd hisense-w41h1
```

Know which board you have, because every later command needs it:

| Board | `dev.py --board` | ESPHome `board` | Board file | TX / RX / DE |
|---|---|---|---|---|
| ESP32-C3 SuperMini | `c3` (the default) | `esp32-c3-devkitm-1` | `w41h1-esp32c3.yaml` | 5 / 6 / 10 |
| classic ESP32 (DevKit, D0WDQ6) | `classic` | `esp32dev` | `w41h1.yaml` | 19 / 18 / 4 |

Hardware and wiring: [Hardware & Wiring](Hardware-and-Wiring) and the pin tables in
[ESP32 Replacement Board](ESP32-Replacement-Build).

The serial port is usually `/dev/ttyACM0` for a C3 SuperMini (native USB) and `/dev/ttyUSB0` for a
classic dev board (USB-UART bridge). If opening it fails with a permission error, your user lacks
access to the serial device; on Debian and Ubuntu that access comes from the `dialout` group.

## With dev.py

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

## By hand

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
`esphome run` downloads the toolchain and takes several minutes. Later updates go over Wi-Fi with
the same `esphome run` line and no `--device`. Keep passing the C3 overrides to a C3 node: without
them it receives an image for the wrong chip.

Home Assistant then finds the node over mDNS. Adopt it with the API key from `secrets.yaml`
([ESPHome Build](ESPHome-Build#adopt-it-in-home-assistant)).

## Check the board without an A/C

`firmware/test/virtual_ac.py` is a software model of the indoor unit. It runs on your computer,
attached to a second USB serial adapter, and answers the board the way the A/C mainboard would. It
lets you see the firmware handshake and decode status before any wire touches the appliance. The
normal ESPHome image logs the bus, so there is no separate bench firmware.

One command flashes the board and starts the simulator:

```
python3 firmware/scripts/dev.py bench esphome --board c3 --port /dev/ttyACM0 --sim-port /dev/ttyUSB0
```

`--port` is the board and `--sim-port` is the adapter. The simulator needs pyserial
(`sudo apt install python3-serial`). The wiring for a 3.3 V USB-TTL or a USB-RS485 adapter and the
output that counts as a pass are in [Build, Flash & Test](Build-Flash-Test#bench-stage-no-ac).

## When it fails

| Symptom | Cause | Fix |
|---|---|---|
| `esphome: command not found` after `pipx install` | `~/.local/bin` is not on `PATH` yet | `pipx ensurepath`, then a new terminal |
| `pip install esphome` refuses with "externally-managed" | a recent distro Python does not allow it | install with `pipx` |
| the port cannot be opened | no access to the serial device | add your user to `dialout`, log in again |
| the write does not connect | the board is not in download mode | hold BOOT while plugging in USB |
| a new board never joins Wi-Fi | stale vendor settings in NVS | erase the flash once, then flash again (`firmware/docs/16-esp32-board-notes.md`) |
| `doctor` says ESPHome is at another version | a newer or older ESPHome on `PATH` | `dev.py fetch esphome`, or set `ESPHOME` to the pinned install |

If a `dev.py` step fails, it has already printed the exact command as a `$ ...` line. Run that line
by hand to debug it.

## After the first flash

Do not wire a freshly flashed board straight into the A/C. Follow the three stages in
[ESP32 Replacement Board](ESP32-Replacement-Build#staged-bring-up-never-leave-the-ac-in-an-unknown-state):
bench, then the real bus with only A and B connected while the board is on USB power, then powered
from the connector. `python3 firmware/scripts/dev.py next esphome` prints them with their
warnings. The rule that matters most at stage 2: while the board is on USB, do not also connect
the A/C's ground or 5 V.
