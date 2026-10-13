# Build, Flash and Test

`firmware/scripts/dev.py` is one entry point for the steps around the firmware: check the tools,
run the tests, build, flash and bench it. It needs only `python3`, prints every command before it
runs it, and asks before anything slow or destructive. This page is its reference, and the place
where the bench test against the simulator is described. To run the same steps by hand, see
[Build Environment](ESP32-Build-Environment).

## The guided flow

From the repository root:

```
python3 firmware/scripts/dev.py walk esphome --board c3 --port /dev/ttyACM0
```

`walk` asks before each of five steps, and N skips one: check the tools (and offer to install
ESPHome), host tests and the config check, build, flash and logs (with the erase for a brand-new
board offered first), then the staged bring-up with its warnings.

## Commands

| Command | Does |
|---|---|
| `walk` | the guided flow |
| `doctor` | read-only: `git`, `python3` and `g++` present, ESPHome at the pinned version, `secrets.yaml` present. Exits non-zero when something is missing |
| `fetch` | `pipx install esphome==2026.9.1` (asks first), and creates `secrets.yaml` from the example when there is none |
| `test` | `firmware/test/run_tests.sh`, then `esphome config` on the board file |
| `build` | `esphome compile` |
| `erase --port P` | erases the whole flash of an ESP32. For a brand-new board only. You type `ERASE` to confirm |
| `flash --port P` | `esphome run`: build, write over USB, follow the log |
| `monitor --port P` | the log only |
| `bench --port P --sim-port S` | flash, then run the node against the simulator on a second USB adapter ([below](#bench-stage-no-ac)) |
| `next` | prints the staged bring-up and its safety warnings |

The word `esphome` after the command is optional (`dev.py build esphome` and `dev.py build` are
the same). It dates from when the script built other firmwares, and the script's own output still
uses it.

`python3 firmware/scripts/dev.py --help` prints this list. If a step fails, the `$ ...` line the
script printed before it is the command to debug by hand.

## Options

Every command takes the same options.

| Option | Meaning |
|---|---|
| `--board B` | which board file to build (table below). Default `c3` |
| `--port P` | the board's serial device, for example `/dev/ttyACM0` |
| `--sim-port S` | the USB adapter the simulator talks through, for `bench` |
| `--name N` | the node's hostname. Default `hisense-ac`. Give every node after the first its own |
| `--friendly-name T` | the node's name in Home Assistant. Default `Air Conditioner` |

| `--board` | Board file | Hardware | Commands |
|---|---|---|---|
| `c3` | `w41h1-esp32c3.yaml` | ESP32-C3 SuperMini | all |
| `c3-plus` | `w41h1-esp32c3-plus.yaml` | ESP32-C3 SuperMini Plus (RGB pixel on GPIO8) | all |
| `classic` | `w41h1.yaml` | classic ESP32 (D0WDQ6) | all |
| `amebaz2-factory`, `amebaz2-sdk`, `amebaz2-native` | `w41h1-amebaz2-<layout>.yaml` | the stock module, by the flash layout the unit has ([The Stock Module](Stock-Module-Build)) | `doctor`, `fetch`, `test`, `build` |

`dev.py` builds the board file as it is, so the image is the one `esphome compile <that file>`
gives. It refuses a bare `--board amebaz2`, and it refuses to flash, watch or bench a stock
module: the module has no USB port. The ESP8266 and RP2040 example files have no `--board` value.
Build those with `esphome` directly.

Two environment variables:

- `ESPHOME` is the command to run, by default `esphome`. It may carry arguments, as in
  `ESPHOME="uvx esphome"`.
- `PLATFORMIO_BUILD_CACHE_DIR` is where compiled objects are kept between builds, by default
  `~/.cache/w41h1-dev/pio-build-cache`. An empty string switches the cache off. It speeds up the
  boards PlatformIO compiles (the stock module, ESP8266, RP2040) and does nothing for the ESP32
  boards. The numbers and two caveats are in `firmware/esphome/README.md` ("Build cache").

## Step by step

```
python3 firmware/scripts/dev.py doctor esphome
python3 firmware/scripts/dev.py fetch esphome
python3 firmware/scripts/dev.py test esphome --board c3
python3 firmware/scripts/dev.py build esphome --board c3
python3 firmware/scripts/dev.py erase esphome --board c3 --port /dev/ttyACM0    # brand-new board only
python3 firmware/scripts/dev.py flash esphome --board c3 --port /dev/ttyACM0
```

After `fetch`, fill in `firmware/esphome/secrets.yaml`
([the four secrets](ESP32-Build-Environment#secrets)). `fetch` only names Wi-Fi and the API key
in its closing message. The hotspot password is needed too.

## Bench stage: no A/C

`firmware/test/virtual_ac.py` is a software model of the indoor unit. It runs on your computer,
attached to a second USB serial adapter, and answers the board the way the A/C mainboard would.
You see the handshake and decoded status before any wire touches the appliance. The board runs
the normal firmware: there is no separate bench image.

Two ways to wire it, with no mains anywhere:

| Adapter | Connect |
|---|---|
| USB to TTL, 3.3 V | board TX to adapter RX, board RX to adapter TX, GND to GND. No transceiver. DE is driven and unused |
| USB to RS-485 | the board's transceiver A to A, B to B, GND to GND. This exercises the DE timing too |

The board's pins are TX 5, RX 6, DE 10 on a C3 and TX 19, RX 18, DE 4 on a classic ESP32.

```
python3 firmware/scripts/dev.py bench esphome --board c3 --port /dev/ttyACM0 --sim-port /dev/ttyUSB0
```

`--port` is the board and `--sim-port` the adapter. The simulator needs pyserial
(`sudo apt install python3-serial`). The script prints the wiring, asks whether it is done,
flashes the board, starts the simulator and follows the node's log. The simulator answers the
handshake first:

```
# virtual A/C up. initial: {'power': True, 'mode': 'cool', 'setpoint': 24, ...}
  [0x0A] handshake poll -> echoed slave reply
```

The pass is the `AC bus link` sensor turning on and the climate entity filling in. A link that
stays off while the node sends is a wiring fault: TX and RX swapped, no common ground, or, through
a transceiver, DE not reaching the chip.

To look at the bus without Home Assistant in the way, `firmware/esp32-recon/` is a small ESP-IDF
tool for the same board: a console on the wire that decodes every frame, with a tap mode that only
listens.

After the bench come the two stages on the real bus:
[staged bring-up](ESP32-Replacement-Build#staged-bring-up).

## What has been run

"Simulator" means `virtual_ac.py`, not an A/C.

| Step | ESP32 boards | Stock module |
|---|---|---|
| Host tests and `esphome config` | CI, every push | CI, every push |
| A real compile | CI, when the firmware changes | CI, all three layouts, when the firmware changes |
| A fresh clone on clean Ubuntu 24.04: `doctor`, `fetch`, `test`, `build` | both boards, 2026-09-15 | not run |
| On a live A/C, powered from the connector | yes, units run this way | the factory layout, one module, since 2026-10-10 |
| `dev.py flash` and `dev.py bench` on a board | not recorded as run | does not apply |

The test layers themselves are in [Testing and QA](Testing-and-QA).
