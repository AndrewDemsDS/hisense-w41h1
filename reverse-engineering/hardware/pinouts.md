# Pinouts

## GD25Q32(B) SPI NOR flash (SOIC-8)

```
 1 CS   ┌───┐  8 VCC (3.3 V)
 2 DO   │   │  7 HOLD
 3 WP   │   │  6 CLK
 4 GND  └───┘  5 DI
```
Pin 1 = dot/dimple corner. For a CH341A read, wire to the programmer's 25-series
socket/clip in this order (VCC=3.3 V ideally).

## UM3352E RS-485 transceiver (SOIC-8, MAX485-compatible)

```
 1 RO   ┌───┐  8 VCC (5 V from A/C)
 2 /RE  │   │  7 B   (bus)
 3 DE   │   │  6 A   (bus)
 4 DI   └───┘  5 GND
```
- **A (6) / B (7)** → A/C mainboard RS-485 bus.
- **RO (1)** = received bytes, logic-level UART (A/C → dongle). Sniff point.
- **DI (4)** = transmitted bytes, logic-level UART (dongle → A/C). Sniff point.
- Logic likely 5 V (VCC from the A/C) → level-shift to 3.3 V before a 3.3 V-only adapter.

## A/C 4-pin port

Carries **5 V, GND, RS-485 A, RS-485 B**. This is where an ESP32 replacement connects.

Bench-confirmed pin order (W41H1; the board silkscreen numbers the pins 4-3-2-1, left to right):

```
 pin:   4      3       2       1
 wire:  blue   yellow  white   red
 sig:   GND    A / B   A / B   5 V
```

Pins 2 and 3 are the RS-485 differential pair. A and B are interchangeable at wiring time, so swap
them if the bus does not link. Confirm the wire colors on your own unit; they can vary by revision.
(Cross-check against the transceiver: A/B go to pins 6/7, GND to pin 5, 5 V to pin 8.)
