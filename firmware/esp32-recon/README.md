# esp32-recon: independent RS-485 A/C bus diagnostic tool

A standalone ESP-IDF app (no Matter, no commissioning) that talks the Hisense A/C
RS-485 bus directly and lets you **verify everything**: wiring/UART/DE, link
bring-up, framing + checksum, every decoded field, command→status round-trips,
ProductType feature flags, and open protocol questions, from a shell you can reach
**remotely over the network** (`nc esp32-recon.local 2323`) or over USB serial.

It reuses the hardware-validated codec **unchanged**: the same
`../src/rs485-driver/hisense_rs485.cpp` (via the `hisense_rs485` component) and the
ESP32 `hisense_hal` shim that the `esp32-matter` firmware uses. So anything it
reports is trustworthy, the on-device `selftest` proves the flashed codec is
byte-identical to the host-validated one.

## Why it exists

Every on-bus check used to require the full ESP32 esp-matter firmware: a ~6-minute
build, a Matter commission, and a Home Assistant round-trip just to look at one
decoded frame. This tool builds in seconds, flashes fast, and gives you a REPL right
on the wire.

## Wiring

From `hisense_hal/include/PinNames.h` (shared with the Matter build):

| ESP32 | Transceiver | |
|-------|-------------|-|
| GPIO19 | DI  | UART1 TX |
| GPIO18 | RO  | UART1 RX |
| GPIO4  | DE + /RE (tied) | half-duplex direction |
| GND    | GND | **common ground with the A/C bus is mandatory** |

Transceiver A/B → the A/C RS-485 bus. 9600 8N1.

## Build & flash (fast: no Matter)

```sh
. ~/esp/esp-idf-v5.5.4/export.sh
cd firmware/esp32-recon
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## First run (USB console)

The UART0 console comes up immediately. Provision WiFi once, then go remote:

```
esp32-recon> selftest            # golden vectors, no hardware — expect PASS
esp32-recon> wifi <ssid> <pass>  # stored in NVS (flash), never in git
esp32-recon> wifi status
```

Then from your laptop (behind the same LAN / WireGuard):

```sh
nc esp32-recon.local 2323
```

## Capture modes

Switch with `mode tap|master` (persisted in NVS; the device reboots into it).

- **tap** (default, safe): drives nothing: DE held low. Passively decodes existing
  bus traffic in **both directions** (point it at the stock dongle ↔ A/C, or the
  kitchen AmebaZ2 ↔ A/C). Ground-truth check of the decode.
- **master**: the ESP32 *is* the module: the driver auto-does link bring-up + ~1 Hz
  polling; `set`/`power`/`producttype`/`raw` are enabled and `set` auto-verifies the
  next status reflects the change.

## Commands

```
mode [tap|master]     show/set capture mode (reboots)
stats                 frame + checksum counters, HAL tx/rx bytes, link health
poll                  show the latest decoded A/C status (all fields)
watch [on|off] [hex]  stream decoded frames as they arrive (hex = include raw bytes)
set <k> <v>           master: mode|temp|fan|swing|eco|turbo|mute|sleep (auto-verifies)
power on|off          master: power the A/C
producttype           master: poll + show A/C feature flags
raw <hex>             master: send a literal frame (protocol probing)
decode <hex>          offline-decode a pasted frame (any mode)
selftest              run codec golden vectors on-target (no hardware)
wifi <ssid> <pass>    store creds in NVS + connect (wifi clear | wifi status)
help                  list commands
```

## Verifying open protocol issues

- **#55 (C/F unit bit):** `mode tap` the stock dongle while toggling the remote's
  °C/°F; `watch hex` and compare the changed byte, or `decode` a captured frame.
- **#49 (envelope `[7]/[8]`): RESOLVED**: not a session token; it's the A/C's device-type/sub-type
  from the `0x0A` reply's inner `[3]/[4]`. Measured 2026-07-16: inner `[3]/[4]`=`01 01`, envelope
  `[9]/[10]`=`00 00` on that frame (stamping the latter killed the link, v10207). `token` on any
  esp32 node re-reports it, useful to see what a **different A/C model** returns.
- **#50 (cold-off power-on):** `mode master`, `power on` on a physically-off unit and
  watch whether status reflects it.

## Notes

- `raw` is send-only in master mode, the A/C's reply isn't surfaced inline (the
  driver consumes replies internally). To see a reply, run a second board in `mode
  tap`, or paste captured bytes into `decode`.
- One remote client at a time. The UART console stays available in parallel.
- WiFi creds live in NVS (flash), provisioned via the `wifi` command, nothing lands
  in a tracked file. `sdkconfig` is gitignored; never put creds in `sdkconfig.defaults`.
- **Never** define `HISENSE_HAL_UART_INTERNAL_LOOPBACK`: it internally loops UART1
  TX→RX and would blind the tool to the real bus.
