# The Stock Module

How to run the ESPHome firmware on the Wi-Fi module that came with the A/C, with nothing added
to the unit. The module's chip is a Realtek RTL8710C, and ESPHome builds for it through its
LibreTiny platform. This is the least proven of the three kinds of hardware: read the state and
the risks before you choose it over an [ESP32 board](ESP32-Replacement-Build).

The design notes, every measurement and the open questions are in
`firmware/docs/15-esphome-path.md` ("The stock module through LibreTiny").

## State today

Measured on one module with the factory flash layout, in an A/C, on 2026-10-10:

- It boots under the factory bootloader and joins Wi-Fi.
- The A/C answers status polls and commands.
- Two updates over the air went through. The node was back within about 10 seconds each time.
- One mains power cycle came back clean.

Not measured:

- The sdk and native flash layouts. They compile and have never run.
- Any run longer than an afternoon.
- An interrupted update, or a power cut during one.
- A first install with the clip or UART download mode.

Two faults showed up:

- **The bus receiver stopped twice** and stayed stopped until a power cycle, while commands still
  reached the unit. The cause is in LibreTiny's receive buffer, which loses count of a byte when
  the receive interrupt lands inside a read. The component now reads with interrupts masked on
  this platform. As a fallback the firmware also reopens the serial port when the bus link drops
  and every 15 seconds while it stays down. Each restart logs
  `A/C bus silent: serial port restarted` and counts in the `Serial port restarts` sensor.
- **A second converted module went silent on the bus within four minutes** and stayed silent
  through restarts and a power-on. Nothing reaches its receive pin, so the cause is outside the
  chip. It is not known yet, and the firmware cannot work around it.

## Risks

- LibreTiny rates this chip family 2 out of 5 for stability.
- Nothing rolls a bad update back. The bootloader boots the valid image with the higher serial
  number and hangs on an image that fails its checks. An image that does not boot needs the clip.
- An image built for the wrong flash layout bricks the unit at its first update (next section).
- A restart or an update can leave the chip dark until power has been off for about 30 seconds
  (LibreTiny issue 396). The build carries the workaround reported there. With it, no hang was
  seen on the tested module.
- Flash `0x1000..0x3FFF` holds the module's own calibration data and must not be overwritten. The
  full-flash image a build produces is blank there, so it cannot be written with the clip as it
  is.
- The way back is a full dump of that unit's flash, taken with the clip before the first flash,
  and written back with the clip ([Recovery and Reflash](Recovery-and-Reflash#stock-module-ch341a-clip)).
  Without the dump there is no complete way back.

How the bootloader picks a slot, and what a rejected image looks like:
`firmware/docs/10-stock-module-boot-and-recovery.md`.

## Find the flash layout first

A module has one of three flash layouts, and the image must be built for the one it has. There
is no default. An image built for another layout writes its first update to the wrong place and
then invalidates the running image.

| Board file | `dev.py --board` | For a module that |
|---|---|---|
| `w41h1-amebaz2-factory.yaml` | `amebaz2-factory` | still has the factory partition table: the vendor firmware, or this project's retired Matter firmware installed over the air |
| `w41h1-amebaz2-sdk.yaml` | `amebaz2-sdk` | was first written with the retired Matter firmware's clip image |
| `w41h1-amebaz2-native.yaml` | `amebaz2-native` | was written from scratch with LibreTiny's own partition table and bootloader |

To tell which one a unit has:

- Its history, as in the table.
- A clip dump: the second firmware image starts at `0x190000` (factory), `0x1B8000` (sdk) or
  `0x1BC000` (native).
- A unit that still runs the retired Matter firmware can be asked over the network
  ([Matter Firmware (Retired)](Matter-Firmware-Retired#find-the-flash-layout)).

Two guards catch a wrong choice. The build fails when the image's slot addresses are not those
of the layout the file names. At run time the image compares its slots with the bootloader's
partition table and publishes the result as the `Flash layout` sensor, for example
`factory ok slot=2 next=0x010000 fw1_sn=... fw2_sn=...`. On `MISMATCH` the image switches its own
update server off. Do not try to update such a unit: it was built for the wrong layout. The check
has run on one module, with the factory layout. A mismatch has not been provoked on hardware, and
ESPHome's safe mode does not run the check.

## Build

Fill in `firmware/esphome/secrets.yaml` first ([Build Environment](ESP32-Build-Environment#secrets)).
Check the Wi-Fi name and password twice: with no rollback, an image that boots and cannot join
your network leaves a unit you can only reach through its setup hotspot.

```
python3 firmware/scripts/dev.py build esphome --board amebaz2-factory
```

or, from `firmware/esphome/`, `esphome compile w41h1-amebaz2-factory.yaml`. The build downloads
the LibreTiny platform and the ARM toolchain the first time (about 800 MB).

`dev.py` has `test` and `build` for the module and refuses the rest: there is no USB port to
flash, watch or bench it over.

## Get the first image on

| The module runs | Route | State |
|---|---|---|
| this project's retired Matter firmware | over the air: that firmware installs the ESPHome image as its own next version ([procedure](Matter-Firmware-Retired#a-stock-module)) | one module runs this way since 2026-10-10. A second one was converted and went silent on the bus afterwards |
| the vendor firmware | the SOIC-8 clip, or UART download mode | not done for this build. Nothing here describes it step by step |

If you write a module with the clip, the tools and their checks are in
[Recovery and Reflash](Recovery-and-Reflash#stock-module-ch341a-clip). The tool that writes one
firmware slot has not run on a module.

## After the first boot

Check, in this order:

1. `Flash layout` reads `factory ok ...` (or `sdk ok ...`, `native ok ...`).
2. `AC bus link` is on, and the climate entity shows the room temperature.
3. `Serial port restarts` is 0 and stays there. If it climbs while the unit answers, the node is
   recovering from something that is not understood yet. It also climbs on a unit that does not
   answer at all, which a restart cannot cure.

If the node does not appear on the network, look for its `<name>-setup` hotspot before reaching
for the clip. That recovery has not been tried on this module.

## Updates

Over the air, from `firmware/esphome/`, with the board file of the unit's layout:

```
esphome run w41h1-amebaz2-factory.yaml --device hisense-ac.local
```

`Flash layout` shows the running slot change after each update. The first ESPHome update of a
module that came from the Matter firmware overwrites the Matter image in the other slot. After
that the clip is the only way back to anything else.

Everything else (adding the node to Home Assistant, the entities, setup mode) is the same as on
an ESP32 board: [ESPHome Firmware](ESPHome-Build). The module has no status LED the firmware can
drive.
