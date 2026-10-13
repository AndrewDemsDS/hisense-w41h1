# 16: ESP32 replacement board: notes behind the wiring rules

For someone who wants to know why the build guide says what it says, or who is about to change a
pin. The wiring, the parts list and the pins to avoid are in the guide:
[`docs/guide/ESP32-Replacement-Build.md`](../../docs/guide/ESP32-Replacement-Build.md) and
[`docs/guide/Hardware-and-Wiring.md`](../../docs/guide/Hardware-and-Wiring.md). This page keeps
the reasons and the measurements behind those rules.

Most of them were found on the ESP32 Matter firmware, which is retired (tag `matter-final`,
[`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md)). They are
facts about the board and the bus, so they hold for the ESPHome firmware on the same hardware.

## Pins

| Signal | ESP32-C3 SuperMini | classic ESP32-D0WDQ6 | transceiver |
|---|---|---|---|
| TX | GPIO5 | GPIO19 | DI |
| RX | GPIO6 | GPIO18 | RO |
| DE | GPIO10 | GPIO4 | DE+RE tied |

- **C3 DE moved from GPIO7 to GPIO10** on 2026-08-10 after a wiring fault shorted the DE/EN line
  to 3V3 on the first board. The C3 pin set avoids the strapping pins (2/8/9), USB (18/19) and
  UART0 (20/21). GPIO5/6 double as the JTAG pins MTDI/MTCK, which is harmless: debugging goes
  through the C3's built-in USB-Serial/JTAG, so external JTAG is never muxed in.
- **GPIO16/17 on a WROVER or D0WDQ6 module are bonded to the PSRAM die** and dead as I/O even with
  SPIRAM disabled. An internal loopback test passes on them, because it bypasses the pads.
- **Firmware and wiring must agree on DE.** If they disagree the transceiver never gets
  transmit-enable: the node sends, nothing reaches the bus, and nothing comes back.

## DE timing

The transceiver's DE line has to be high before the first bit and stay high until the last stop
bit has left the wire. The values that work on this bus, validated on hardware: 5 ms of settle
before the first byte, and after the last byte the wire time of the frame plus 25 ms before DE
drops. The ESPHome component does this itself when `de_pin` is set
(`firmware/esphome/components/hisense_ac/`).

The trap behind the second number: on ESP-IDF, writing to the UART only queues bytes into the
FIFO. Dropping DE when the write call returns cuts the frame short. The retired firmware waited
for `uart_wait_tx_done()`. The ESPHome component waits out the frame's wire time, counted from
before the write, so the release is right on a platform whose write blocks too
([`01-component-architecture.md`](01-component-architecture.md#portability)).

The alternative, letting the UART peripheral drive DE (`flow_control_pin` on ESPHome's `uart:`),
has not been validated against this A/C.

The stock module's own DE behaviour (PA_17, the toggle sequence, why it must be set up from a
task) is in
[`reverse-engineering/docs/10-stock-fw-init-and-comms.md`](../../reverse-engineering/docs/10-stock-fw-init-and-comms.md).

## Why a brand-new board must be erased first

A normal flash writes the bootloader, the partition table and the application. It **leaves the NVS
partition untouched**, including whatever the vendor's factory test firmware left there. A Wi-Fi
station config in that NVS is picked up by the new firmware as if it were its own.

On the Matter firmware this made every commissioning attempt fail within about 150 ms, with no
radio activity, as `CHIP Error 0x000000AC`: the Wi-Fi driver adopted the stale network and then
refused any other SSID. It read like a credentials problem when the radio was never involved.
Observed on a new ESP32-C3 SuperMini, 2026-08-07.

The rule for any firmware: erase a new board once before the first flash
(`dev.py erase --port P`, or `esptool.py erase_flash`). Never erase a board that is already
running, since that throws away its settings. `dev.py erase` asks for a typed confirmation and
tells esptool which chip to expect
([`firmware/scripts/README.md`](../scripts/README.md)).

## Wi-Fi power saving

`packages/node.yaml` sets `power_save_mode: none`. ESPHome's default on ESP32 is `light`, where
the radio sleeps between beacons. On an ESP32-C3 node with a weak signal (about -77 dBm) that
default gave 65 % ping loss, an API connection that dropped about once a minute, and commands
that did not reach the unit. With `none`: 100 of 100 pings and a command taken in 0.5 s
(2026-10-10). The retired ESP32 Matter build forced the radio on for the same reason. ESP8266
and LibreTiny already default to `none`.

The cost is current: the radio no longer sleeps between beacons. The setting is a substitution,
`wifi_power_save`, for a board whose supply cannot carry that.

## A supply that dips when Wi-Fi starts

On a SuperMini whose 3.3 V rail dips when Wi-Fi starts at full power, a new image resets on its
first boot and the bootloader rolls the update back. The symptom, and the cap on the transmit
power for a board that has to run as it is, are in
[`firmware/esphome/README.md`](../esphome/README.md#an-update-uploads-then-the-node-is-back-on-the-old-firmware).

## Ground loop

While the board is powered from a laptop's USB port, connect only A and B to the A/C. The A/C's
ground is mains-earthed, and so is the laptop's. Joining them through the board browns it out:
RTCWDT resets and flash-read errors. Ground and 5 V from the A/C connector go on when the laptop
is unplugged.

## Envelope bytes `[7]/[8]` are the A/C's device type

They are the unit's device type and sub type, static per model, and not a session token (measured
2026-07-16). The node reads them from the DevType (`0x0A`) reply's inner `[3]/[4]` and stamps them
on later frames. The default before they are learned is `01 01`. Do **not** stamp that reply's
envelope `[9]/[10]`: it is `00 00` on the `0x0A` frame, and a firmware that did so made the A/C
reject every frame until it was replaced. See
[`reverse-engineering/docs/10-stock-fw-init-and-comms.md`](../../reverse-engineering/docs/10-stock-fw-init-and-comms.md),
section 4.5. The ESPHome build shows the learned bytes as the `AC device type` text sensor.

The whole reply was captured on 2026-10-12 from a deployed ESP32-C3 node, which settles its
length and checksum:

```
F4 F5 01 40 0D 01 00 FE 01 00 00 01 00 0A 04 01 01 01 01 60 F4 FB
```

22 bytes: the LEN byte (`0D`, 13) plus 9, as for every frame. The device type `01 01` is at
bytes 16 and 17, which are inner `[3]/[4]` (the inner payload starts at byte 13). Bytes 9 and 10
are `00 00`. Bytes 18 and 19 (`01 60`) are the 16-bit sum of bytes 2 to 17, the same checksum
rule as the other frames.

## Task stack units differ by platform

Recorded for anyone who builds the reference driver (`firmware/test/reference-driver/`) on a new
platform, as `firmware/esp32-recon/` does: the bus task's stack depth of `1024` is words on
AmebaZ2 (4 KB) and bytes on ESP-IDF (1 KB). On ESP-IDF the status callback's `printf` overflowed
it into a crash loop. The recon tool's `hisense_rs485` component sets
`HISENSE_BUS_TASK_STACK=4096` for that reason.
