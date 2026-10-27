# Build Environment

From a clean Linux machine to a built and flashed board, by hand: which tools you need, where
they come from, the secrets file, and the commands. The wrapper script that runs the same steps
for you is described in [Build, Flash and Test](Build-Flash-Test).

## What comes from where

None of the build tools live in this repository. It holds the firmware source and one wrapper
script.

| Tool | Comes from | Version | Needed for |
|---|---|---|---|
| `esphome` | PyPI, installed with `pipx` | `2026.10.0`, the release CI builds with | building and flashing |
| compilers, `esptool`, ESP-IDF or PlatformIO | downloaded by ESPHome on the first build | chosen by ESPHome | nothing you install yourself |
| `g++` | your distribution | any with C++17 | the host tests |
| `firmware/scripts/dev.py` | this repository | | optional wrapper |

ESPHome needs no ESP-IDF checkout from you. On the first build it downloads its own toolchain
into `~/.cache/esphome` (about 5 GB), which takes several minutes.

## Install

A Linux x86_64 machine. Package names are for Debian and Ubuntu.

```
sudo apt install git python3 g++ pipx
git clone https://github.com/AndrewDemsDS/hisense-w41h1.git
cd hisense-w41h1
pipx install esphome==2026.10.0
pipx ensurepath
```

Open a new terminal so `esphome` is on `PATH`. To build a release and not the tip of `main`, see
[Version and releases](ESPHome-Build#version-and-releases).

## Secrets

The build reads four values from `firmware/esphome/secrets.yaml` and stops if one is missing. The
file is gitignored. Start from the example:

```
cd firmware/esphome
cp secrets.yaml.example secrets.yaml
python3 -c "import base64,os; print(base64.b64encode(os.urandom(32)).decode())"
```

| Secret | What |
|---|---|
| `wifi_ssid`, `wifi_password` | the network the node joins |
| `wifi_ap_password` | the password of the node's own setup hotspot, at least 8 characters. Whoever joins that hotspot can move the node to another network, so do not keep the placeholder |
| `hisense_ac__encryption_key` | the API key Home Assistant adopts the node with. Use the key the `python3` line above printed. The one in the example file is public |

The firmware image contains all four. That is why this project publishes no firmware images: you
build your own.

## Build and flash

Know which board you have. Each has a file of its own in `firmware/esphome/`:

| Board | File | TX / RX / DE |
|---|---|---|
| ESP32-C3 SuperMini | `w41h1-esp32c3.yaml` | 5 / 6 / 10 |
| ESP32-C3 SuperMini Plus | `w41h1-esp32c3-plus.yaml` | 5 / 6 / 10 |
| classic ESP32 (DevKit, D0WDQ6) | `w41h1.yaml` | 19 / 18 / 4 |

The full list, with the stock module and the example files, is in
[ESPHome Firmware](ESPHome-Build#board-files).

From `firmware/esphome/`:

```
esphome config w41h1-esp32c3.yaml                       # checks the YAML, no toolchain, seconds
esphome run w41h1-esp32c3.yaml --device /dev/ttyACM0    # build, flash over USB, follow the log
```

The serial port is usually `/dev/ttyACM0` for a C3 SuperMini (native USB) and `/dev/ttyUSB0` for
a classic dev board (a USB to UART bridge).

A brand-new board is erased once before its first flash, because a vendor test image can leave a
Wi-Fi configuration behind. ESPHome brings esptool with it:

```
esptool.py --chip esp32c3 --port /dev/ttyACM0 erase_flash
```

Name the chip (`esp32c3` for a C3, `esp32` for a classic ESP32) so another kind of board on that
port is not erased. Never erase a board that is already running: that throws away its Wi-Fi
settings.

A second node on the same network needs its own name. Set `name` and `friendly_name` in the board
file, or pass them: `esphome -s name bedroom-ac -s friendly_name "Bedroom A/C" run ...`. Keep
passing the same values on every later build of that node.

## After the first flash

Do not wire a freshly flashed board straight into the A/C. Follow the three stages in
[ESP32 Replacement Board](ESP32-Replacement-Build#staged-bring-up). The rule that matters most:
while the board is on USB, connect only A and B, never the A/C's ground or 5 V.

Then add the node to Home Assistant
([ESPHome Firmware](ESPHome-Build#adopt-it-in-home-assistant)). Later updates go over Wi-Fi with
the same `esphome run` line and no serial port ([Updates](ESPHome-Build#updates)).

## When it fails

| Symptom | Cause | Fix |
|---|---|---|
| `esphome: command not found` after `pipx install` | `~/.local/bin` is not on `PATH` yet | `pipx ensurepath`, then a new terminal |
| `pip install esphome` refuses with "externally-managed" | a recent distribution Python does not allow it | install with `pipx` |
| the build stops on a missing secret | `secrets.yaml` lacks one of the four values, often `wifi_ap_password` in a file made before 2026-10-12, when it was added | add it |
| the port cannot be opened | no access to the serial device | add your user to the `dialout` group, log in again |
| the write does not connect | the board is not in download mode | hold BOOT while plugging in USB |
| a new board never joins Wi-Fi | stale vendor settings in NVS | erase the flash once, then flash again (`firmware/docs/16-esp32-board-notes.md`) |
| `dev.py doctor` says ESPHome is at another version | a newer or older ESPHome on `PATH` | `pipx install --force esphome==<the pin doctor names>`, or set `ESPHOME` to the pinned install |

Problems after the board runs are in [Troubleshooting and FAQ](FAQ-Gotchas).
