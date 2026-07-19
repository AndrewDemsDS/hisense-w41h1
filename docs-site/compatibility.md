---
title: Will this work on my A/C?
nav_order: 5
description: >-
  Which Hisense air conditioners and Wi-Fi modules this de-cloud firmware works on: confirmed
  hardware, the AEH-W41H1 and AEH-W4A1 modules, and how to check your own unit before buying
  anything.
---

# Will this work on my A/C?

Short version: **if your A/C uses a ConnectLife Wi-Fi module, the odds are good, and you can find
out for certain in about a minute without opening anything.**

Two things have to be true. The bus protocol has to match, and the module has to be one you can
either reflash or replace.

## Confirmed working

| | |
|---|---|
| **Wi-Fi module** | `AEH-W41H1` (Realtek RTL8710C / AmebaZ2), secure boot off |
| **App it came with** | ConnectLife |
| **Bus** | RS-485, 9600 8N1, on the 4-pin module connector (5 V · GND · A · B) |
| **Verified on** | two units, running continuously |

Both the protocol and the flashing route are proven on this hardware. Everything on this site was
written from those units.

## Very likely, not personally verified

**Other Hisense units using the same module family.** The `AEH-W4A1` appears throughout the
reference material this project built on and speaks the same bus. The protocol work should carry
over; the flashing details may differ.

**Other brands on the same bus.** This A/C bus is not Hisense-only. The community
[`esphome_airconintl`](https://github.com/pslawinski/esphome_airconintl) project drives the identical
protocol on AirconIntl hardware, which is why the codec here could be cross-checked against its
sample frames before it ever touched an A/C. Hisense manufactures for several brands, so a
rebadged unit may well be the same machine underneath.

Being honest about the boundary: "should work" is not "does work". If you try it on something not
listed above, [open an issue](https://github.com/AndrewDemsDS/hisense-w41h1/issues) with what you
find, working or not.

## How to check your own unit

**1. Does it use ConnectLife?** If your A/C pairs with the ConnectLife app, it is in the right
family. Some regions ship the same hardware under a different app name.

**2. Look at the module bay.** The Wi-Fi module is a small plastic dongle in a slot on the indoor
unit, usually behind the front panel and reachable without tools. The part number is printed on it.

**3. Count the pins.** Four pins (5 V, GND, and an RS-485 A/B pair) is the signature this project
targets.

## If your module is not supported, or is dead

You do not need the original module. The [ESP32 route](guide/ESP32-Replacement-Build.html) puts an
ESP32 plus an RS-485 transceiver in the module bay instead, speaking the same bus bytes. It costs
about €5, needs no CH341A clip, and is the recommended path if you do not already have a working
`AEH-W41H1` — those modules are fragile and increasingly hard to buy.

The [path comparison](firmware/13-path-comparison.html) covers the tradeoffs with figures measured
on real hardware.

## What you get either way

Local Matter control in Home Assistant with no cloud: mode, setpoint, six fan speeds, swing, and
the Eco / Quiet / Turbo / Sleep special modes, plus temperature and energy telemetry. See
[everyday control](guide/Everyday-Control.html).
