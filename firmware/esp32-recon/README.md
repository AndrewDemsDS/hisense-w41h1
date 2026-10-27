# esp32-recon: a console on the A/C bus

A small ESP-IDF program for a classic ESP32 that sits on the Hisense RS-485 bus and gives you a
command line on it: listen to a bus someone else drives, decode frames, compare two frames bit by
bit, or be the bus master and send commands. It has no Home Assistant and no pairing. Use it for
byte-level questions (which bit moved when I pressed this button) and for checking wiring.

It is not the firmware. The firmware is under [`firmware/esphome/`](../esphome/README.md). This
tool was kept when the Matter firmwares were retired because it is the one way to run the frozen
reference driver on a wire.

> **The network console has no authentication and can drive the A/C.** Once the board is on
> Wi-Fi, anyone who can reach TCP port 2323 on it gets the same command line as the USB console,
> with no password. In master mode that includes `power`, `set` and `raw`, which sends any bytes
> you type onto the bus of an appliance. Run it on a network you trust, keep it in tap mode
> unless you are driving on purpose, and do not leave it installed. `wifi clear` removes the
> stored credentials.

## What it is built from

It builds the reference driver unchanged: `firmware/test/reference-driver/hisense_rs485.cpp`, the
file the host tests compile, through the `hisense_rs485` component in `components/`, on the ESP32
HAL shim in `components/hisense_hal/`. The `selftest` command runs the codec's golden vectors on
the board. The ESPHome firmware has its own codec, held equal to this one by
`firmware/test/test_esphome_codec_parity.cpp`, so a decode seen here is the decode the firmware
makes. What this tool does not have is anything the firmware learned after the driver was frozen
(confirm and retry, the stricter setup-request rules).

## Wiring

From `components/hisense_hal/include/PinNames.h`, for the classic ESP32:

| ESP32 | Transceiver | |
|---|---|---|
| GPIO19 | DI | UART1 TX |
| GPIO18 | RO | UART1 RX |
| GPIO4 | DE and /RE (tied) | direction |
| GND | GND | see below |

Transceiver A and B go to the A/C's RS-485 pair, 9600 8N1. Use a 3.3 V transceiver. The ground
and power rules are the firmware's: while the board is powered from a computer's USB port,
connect only A and B
([`firmware/docs/16-esp32-board-notes.md`](../docs/16-esp32-board-notes.md#ground-loop)).

The header also has a pin set for the ESP32-C3 (5, 6, 10). CI builds the classic ESP32 target
only, and that is the target this page describes.

## Build and flash

It needs ESP-IDF. CI builds it every night with v5.5.6 in Espressif's container (the
`esp32-recon` job of `.github/workflows/nightly.yaml`), so that is the version known to work.

```sh
. $IDF_PATH/export.sh
cd firmware/esp32-recon
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The build has a few compiler warnings of its own (missing field initializers in
`recon_console.c`). CI reports their count and does not fail on them.

## First run

The USB console comes up at once. Check the codec, then put the board on Wi-Fi if you want the
network console:

```
esp32-recon> selftest            # golden vectors, no hardware. Expect PASS
esp32-recon> wifi <ssid> <pass>  # stored in the board's flash (NVS), never in git
esp32-recon> wifi status
```

Then, from a machine on the same network:

```sh
nc esp32-recon.local 2323
```

One network client at a time. The USB console stays available beside it.

Credentials can also be compiled in for a board with no console at hand: copy
`main/wifi_secrets.h.example` to `main/wifi_secrets.h` (gitignored) and fill it in. `sdkconfig`
is gitignored too. Never put credentials in `sdkconfig.defaults`.

## Two modes

Switch with `mode tap` or `mode master`. The choice is stored in flash and the board restarts
into it.

- **tap** (the default): drives nothing. DE is held low. It decodes the traffic of a bus that
  something else drives (the stock module, or a node running the firmware), in both directions.
- **master**: the board is the module. The driver brings the link up and polls about once a
  second, and `set`, `power`, `producttype` and `raw` are enabled. It must be the only master on
  the bus: take the module or the node off first.

## Commands

```
mode [tap|master]     show or set the mode (restarts)
stats                 frame and checksum counters, HAL byte counters, link health
poll                  the latest decoded status, every field
watch [on|off] [hex]  stream decoded frames as they arrive (hex adds the raw bytes)
snap [status|cmd]     tap: remember the latest raw frame of that kind
diff [status|cmd]     tap: which bytes and bits changed since snap
decode <hex>          decode a pasted frame, in any mode
set <k> <v>           master: mode|temp|fan|swing|eco|turbo|mute|sleep, then checks the next status
power on|off          master: switch the unit
producttype           master: poll and show the unit's capability flags
token                 the device type learned from the unit's 0x0A reply
raw <hex>             master: send a literal frame
selftest              the codec's golden vectors, on the board
wifi <ssid> <pass>    store credentials and connect (wifi clear, wifi status)
help                  list the commands
```

`snap` and `diff` are the working pair for reverse engineering: take a baseline, change one
thing on the remote, and `diff` shows the byte and bit that moved. The procedures for the open
protocol questions are in [`HIL-RUNBOOK.md`](HIL-RUNBOOK.md).

## Notes

- `raw` only sends. The reply is consumed by the driver and not shown. To see one, run a second
  board in tap mode, or paste captured bytes into `decode`.
- `token` prints the unit's device type and sub-type, read from bytes 16 and 17 of its `0x0A`
  reply (the reply's inner `[3]/[4]`). The command's name is from before the bytes were
  understood, when they were taken for a session token
  ([`firmware/docs/16-esp32-board-notes.md`](../docs/16-esp32-board-notes.md#envelope-bytes-78-are-the-acs-device-type)).
- Never define `HISENSE_HAL_UART_INTERNAL_LOOPBACK`. It loops UART1 TX to RX inside the chip and
  blinds the tool to the real bus.
- The stack size of the bus task is set to 4096 for ESP-IDF in the component's `CMakeLists.txt`.
  The reason is in [`firmware/docs/16-esp32-board-notes.md`](../docs/16-esp32-board-notes.md#task-stack-units-differ-by-platform).
