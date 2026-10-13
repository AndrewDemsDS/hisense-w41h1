# 13: Choosing the hardware: an ESP32 board or the stock module

For someone who has not decided what to put in the unit. There is one firmware, the ESPHome
`hisense_ac` component, and it runs on an ESP32 board that replaces the Wi-Fi module or on the
module itself. This page compares the two.

Short answer: use an ESP32 board unless you have a reason to keep the original module.

> **Retired: the Matter columns of this comparison.** This page used to compare three firmwares.
> The two Matter builds are not in the tree any more. The last tree with them is the tag
> `matter-final`, and the version of this page at that tag has their columns and figures.
> Current: one ESPHome firmware on two kinds of hardware, below. See
> [`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md).

## At a glance

| | **ESP32 replacement board** | **Stock W41H1 module** (LibreTiny) |
|---|---|---|
| **Cost** | ~€5: an ESP32 board and a 3.3 V RS-485 transceiver | €0 if the module works, plus ~€5 for a CH341A clip you need for recovery |
| **Sourcing** | available everywhere | the W41H1 is fragile (ESD) and hard to source in the EU |
| **First flash** | USB, `esphome run` | over the air from the retired Matter firmware (done on two modules). By clip or UART download mode: not done yet |
| **Recovery** | USB, always: the ROM bootloader is in silicon | the SOIC-8 clip and a dump of that unit |
| **A bad update** | rolled back by the bootloader | nothing rolls it back. A rejected image hangs the module |
| **Toolchain** | `pipx install esphome`, which fetches its own ESP-IDF, about 5 GB | the same command. LibreTiny fetches what it needs, about 800 MB |
| **Flash budget** | 4 MB, the image takes 51 to 55 % of the slot | 4 MB, the image takes 35 to 40 % of a slot of 1472 or 1712 KiB ([figures and their date](../esphome/README.md#boards)) |
| **Updates** | full image over ESPHome's own OTA | the same, into the slot the bootloader will prefer next |
| **Diagnostics** | logs over the API plus diagnostic entities | the same, plus `Flash layout` and `Serial port restarts` |
| **Status LED** | optional, on a board that has one | none: no LED is documented on the module |
| **Maturity** | on live A/Cs since 2026-08, powered from the connector | first run in an A/C on 2026-10-10, factory flash layout only |

Other chips ESPHome supports (ESP8266, RP2040) have example board files that compile. Nobody has
run one, so they are not in the table.

## The dimensions, in detail

### Cost and sourcing

The ESP32 route costs about €5 in parts: an ESP32 dev board and a **3.3 V** RS-485 transceiver
(MAX3485 / SP3485 / SN65HVD75). Do not use a 5 V MAX485 module: its RO pin pushes 5 V into the
ESP32 RX and kills it. Power and bus both come from the A/C's 4-pin module connector (5 V confirmed
on the wire).

The stock module costs nothing in parts **if** it is alive, but budget for a CH341A programmer and
SOIC-8 clip regardless: that is how a module is recovered.

The real difference is **sourcing**. W41H1 modules die easily from handling and ESD, and are
increasingly hard to buy in the EU. An ESP32 is a commodity.

### Getting the firmware on, and getting it off again

An ESP32 is written over USB and can always be written again. That is the whole story.

The stock module has no USB. The modules that have run ESPHome got it over the air from this
project's retired Matter firmware, which installed the image as its own next version
([`docs/guide/Converting-a-Stock-Module-to-ESPHome.md`](../../docs/guide/Converting-a-Stock-Module-to-ESPHome.md)).
For a module on the vendor firmware the routes are the clip or UART download mode, and neither
has been used for this build. The clip tool that writes one firmware slot exists and has not
written a module yet ([`firmware/flasher/README.md`](../flasher/README.md)). The module's
bootloader boots the valid slot with the higher serial and hangs on an image that fails its
checks, with no fallback ([`10-stock-module-boot-and-recovery.md`](10-stock-module-boot-and-recovery.md)).
An image built for the wrong flash layout bricks the unit at its first update
([`15-esphome-path.md`](15-esphome-path.md#which-flash-layout-a-unit-has)).

### Toolchain

`pipx install esphome`, then `esphome run`. ESPHome downloads the compiler and SDK for the board it
builds. Nothing proprietary has to be obtained by hand, and nothing is vendored in this repository.
The component uses ESPHome's `uart:` component and no chip SDK, which is why one source tree
builds for both.

### Stability on the stock module

LibreTiny rates the chip family 2 out of 5 for stability. On the tested module the bus receiver
stopped twice, from a lost count in LibreTiny's receive buffer that the component now prevents, and
a second converted unit went silent on the bus for a reason that is still open. A restart or an
update can leave the chip dark until power has been off for about 30 seconds (LibreTiny issue 396).
The `w41h1_module` component carries the workaround. The full list of open risks is in
[`15-esphome-path.md`](15-esphome-path.md#open-risks).

### Energy

Not a differentiator: neither board's own consumption has been measured. The A/C's power draw is
estimated the same way on both ([`09-energy-monitoring.md`](09-energy-monitoring.md)).

## Recommendation

- **Starting fresh, or the module is dead:** an ESP32-C3 SuperMini and a 3.3 V transceiver.
- **A working module that already runs this project's retired Matter firmware:** convert it over
  the air if you accept the risks, and keep the clip and a dump at hand. One of the two converted
  modules stopped answering on its bus for a reason that is still open.
- **A working module on the vendor firmware:** leave it, or replace it with an ESP32 board. There
  is no tested route onto it yet.

## What this comparison does not settle

- **Long-term reliability is not compared.** The fleet is small and young. The known hardware
  failure has been a dead W41H1 radio, which is a sample of one and not evidence of a rate.
- **The stock module has the least field time,** and the sdk and native flash layouts have not
  run at all.
- **Numbers are from this project's units and builds.** Image sizes will move with ESPHome
  versions and enabled features.
