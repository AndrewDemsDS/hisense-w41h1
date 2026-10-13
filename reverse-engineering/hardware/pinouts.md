# Pinouts

The three pinouts needed to read the module's flash, tap its bus, or wire a replacement board.
The chip pinouts are the standard ones of the parts. The connector's pin order was measured on a
W41H1 on the bench. The parts themselves are described in
[`../docs/01-hardware.md`](../docs/01-hardware.md).

## GD25Q32(B) SPI NOR flash (SOIC-8)

```
 1 CS   ┌───┐  8 VCC (3.3 V)
 2 DO   │   │  7 HOLD
 3 WP   │   │  6 CLK
 4 GND  └───┘  5 DI
```

Pin 1 is the corner with the dot. For a read with a CH341A, the clip goes on in this order, at
3.3 V.

## UM3352E RS-485 transceiver (SOIC-8, MAX485 compatible)

```
 1 RO   ┌───┐  8 VCC (5 V from the A/C)
 2 /RE  │   │  7 B   (bus)
 3 DE   │   │  6 A   (bus)
 4 DI   └───┘  5 GND
```

- Pins 6 and 7 (A and B) go to the A/C mainboard's RS-485 bus.
- Pin 1 (RO) carries the bytes the A/C sends, as logic-level UART. A point to tap.
- Pin 4 (DI) carries the bytes the module sends. The other point to tap.
- Pin 3 (DE) is driven by the SoC's `PA_17`: high while the module sends.
- The logic level is probably 5 V, because VCC comes from the A/C. It was not measured. Bring it
  down to 3.3 V before an adapter that takes 3.3 V only.

## The A/C's 4-pin port

It carries 5 V, ground, and RS-485 A and B. A replacement board connects here.

The pin order measured on a W41H1. The board's silkscreen numbers the pins 4, 3, 2, 1 from left
to right:

```
 pin:   4      3       2       1
 wire:  blue   yellow  white   red
 sig:   GND    A / B   A / B   5 V
```

Pins 2 and 3 are the differential pair. Which of them is A was not determined: if the bus does
not link, swap them. Check the wire colours on your own unit, because they can differ between
revisions. As a cross-check against the transceiver, A and B go to its pins 6 and 7, ground to
pin 5 and 5 V to pin 8.

Wiring a replacement board step by step is in
[`docs/guide/Hardware-and-Wiring.md`](../../docs/guide/Hardware-and-Wiring.md).
