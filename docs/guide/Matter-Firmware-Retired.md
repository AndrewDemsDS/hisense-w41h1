# Matter Firmware (Retired)

> **Retired on 2026-10-12.** This page describes the two Matter firmwares this project used to
> ship. They are no longer on `main`, and no fixes will be made to them. The current firmware is
> the [ESPHome firmware](ESPHome-Build). The last tree that has the Matter builds is the signed
> tag `matter-final`.

This is the one page about the Matter firmware: what was retired and why, where it still lives,
and how a unit that runs it moves to ESPHome without being opened.

## What was retired

| Firmware | Hardware | Last version |
|---|---|---|
| AmebaZ2 Matter build (`room_air_conditioner` on Realtek's SDK and connectedhomeip) | the stock AEH-W41H1 module | 1.3.50 |
| ESP32 Matter build (esp-matter) | the ESP32 replacement board | 1.1.19 |

With them went the SDK setup scripts and patches, the ZAP data model, the Matter half of `dev.py`
(build, package, stage, flash, release, revert to the vendor firmware), the release workflows,
the SDK pin automation, and the `hisense-unified-ac` Home Assistant integration as a submodule.
The decision is recorded in issue #182.

## Why

- The builds used Matter test credentials and were never certified, so in practice they were only
  used from Home Assistant, which ESPHome serves directly.
- Every control had to be a standard cluster. Each extra (the beeper, the panel display) became
  another On/Off endpoint, and the diagnostics sat on a manufacturer cluster that needed a second
  integration to show.
- The AmebaZ2 build needed about 15 GB of Realtek and connectedhomeip sources, patches, and edits
  to files that cannot be redistributed.
- Three firmwares on one protocol meant every fix was made and tested three times. Command
  confirm and retry only ever existed in the ESPHome build.

The cost: no direct pairing with Apple Home, Google Home or Alexa. Home Assistant can expose the
climate entity to them through its own Matter or HomeKit bridge.

## Where it lives now

```
git checkout matter-final
```

The tree at that tag builds both firmwares, and its documentation describes them: commissioning,
OTA, the ZAP recipe, the break-glass update path, and the way back to the vendor firmware.
Version numbers such as "AmebaZ2 1.3.50" and paths under `firmware/src/` or
`firmware/esp32-matter/` in `CHANGELOG.md` refer to that tree.

## Until you move a unit

Nothing stops working. The firmware on the unit keeps running.

- The [`hisense-unified-ac`](https://github.com/AndrewDemsDS/hisense-unified-ac) integration has
  its own repository and still works with a node that runs a Matter build.
- Such a unit can join a [climate group](Climate-Groups) through that integration (1.4.0 or
  later), whose climate entity uses the same names as the ESPHome firmware. Add that entity to
  the group, not the raw Matter climate.
- Take the images a unit needs for the way back (its delta base, its clip image) from the tag or
  from your own archive before you start a move.

## Moving a unit to ESPHome

Both procedures below install the ESPHome image through the update path the Matter firmware
already has, with the unit left closed. Each has worked on hardware once, neither has an
automatic rollback, and the way back to Matter over the air has not been tried for either.

What both have in common:

- `firmware/esphome/secrets.yaml` must hold the Wi-Fi credentials of the network the unit is on
  now, and an API key. Check both twice. An ESPHome image that boots and cannot join your Wi-Fi
  leaves a unit you cannot reach over the network. It opens a hotspot named `<name>-setup`, which
  may let you fix the credentials from within radio range. That recovery has not been tried after
  a move.
- Give the node its own hostname if another ESPHome A/C node is already on the network
  (`--name <hostname> --friendly-name "<name>"`).
- Do it while the A/C is idle. The node drops off the bus for the length of the restart.
- Home Assistant sees a different device afterwards. Entity ids change. Add the ESPHome device
  with its API key and remove the old node from the Matter integration, so matter-server stops
  trying to reach it. If the unit was set up in the `hisense-unified-ac` integration, delete that
  entry. Update automations, dashboards and climate groups to the new entity ids.
- Watch the [bus counters](Entities-and-Diagnostics#bus-counters) for a few days.

### An ESP32 board

**State: done once.** One ESP32-C3 node was moved this way on 2026-10-10 and has taken ESPHome
updates over the air since (maintainer's report, not recorded in a pull request). It has not been
tried on the classic ESP32 board.

The esp-matter firmware has a manual update path that bypasses Matter. On a trigger it downloads
one file over plain HTTP from a URL compiled into the image, writes it to the idle OTA slot and
restarts into it. It applies no version rule and expects no Matter OTA wrapper, so it accepts any
valid ESP32 application image for the same chip. An ESPHome build is such an image.

| | After the move |
|---|---|
| Application | ESPHome, in the slot that was idle |
| Bootloader and partition table | the esp-matter build's, untouched: two app slots of `0x1E0000` bytes, the `nvs` and `fctry` data partitions |
| Other app slot | the last Matter image, until the first ESPHome update overwrites it |
| NVS | left in place, including the Matter fabric data, which nothing reads any more |
| Later updates | ESPHome's own, which work on this layout |

Risks particular to this route:

- There is no rollback. The esp-matter build does not enable the bootloader's rollback, so
  the board boots whatever was written last. The way out of a bad image is a USB flash
  ([Recovery and Reflash](Recovery-and-Reflash#esp32-boards)).
- Serve the right file. An ESPHome build produces `firmware.ota.bin` and
  `firmware.factory.bin`. Only `firmware.ota.bin` is an application image. The factory file
  starts with a bootloader and a partition table and must never be written into an app slot.
- Build for the right chip and pins. Wrong pins give a node that joins Wi-Fi and never hears
  the A/C, or one that holds the bus.
- The download is unauthenticated. The board flashes whatever is at that URL. Put the file
  there for the move and remove it afterwards.
- The URL is fixed when the Matter image is built (`HISENSE_OTA_URL` in that tree's
  `firmware/scripts/ota-release.env`). If it is a placeholder, or the board cannot reach it, the
  trigger does nothing useful.
- Power. A flash write is the highest-current thing the board does. The update path lowers
  the Wi-Fi transmit power and paces the download for that reason, and a truncated download is
  aborted, not booted. A brownout in the middle of a write can still corrupt the slot.
- The image must fit the slot, `0x1E0000` bytes. The ESPHome image is about 1 MB.

Procedure:

1. Check that the node answers on Matter, or on the break-glass listener (port 2324, present
   only when the image was built with a token), that you control the HTTP server behind the URL,
   and that you can reach the board with a USB cable if it goes wrong.
2. Build the image:

   ```
   python3 firmware/scripts/dev.py build esphome --board c3 --name <hostname> --friendly-name "<name>"
   ```

   It is `firmware.ota.bin` in the build tree, normally
   `firmware/esphome/.esphome/build/<hostname>/.pioenvs/<hostname>/`.
3. Copy `firmware.ota.bin` to the HTTP server under the file name the URL ends with
   (`esp32-ota.bin` for images built by the Matter tree's `dev.py`). Fetch the URL from another
   machine and compare the checksum with the file you built.
4. Trigger the fetch. Either write `88` to `Identify.IdentifyTime` on endpoint 1 through Matter,
   or send the token to the listener:

   ```
   printf 'TOKEN\r\n' | nc <device-ip> 2324
   ```

   The listener answers `ok`. The board downloads the image and restarts by itself.
5. Within a minute or two the node should answer at `<hostname>.local` and show up in Home
   Assistant as a discovered ESPHome device. `AC bus link` should turn on within a few seconds of
   boot. If the node does not appear, look for the `<hostname>-setup` hotspot before reaching for
   the cable.
6. Remove the image from the HTTP server, or put the Matter image back if other Matter nodes use
   the same URL.
7. Send one ESPHome update over the air. It proves the normal update path on the kept partition
   layout and overwrites the old Matter image in the other slot:

   ```
   cd firmware/esphome
   esphome -s name <hostname> run w41h1-esp32c3.yaml --device <hostname>.local
   ```

Afterwards the board still has the esp-matter bootloader and partition table. A later USB flash
of ESPHome's `firmware.factory.bin` replaces both with ESPHome's defaults, and erasing the flash
first removes the stale Matter data in NVS. That has not been done on a moved node. An image
built from this repository before 2026-10-10 let the Wi-Fi radio sleep, which the esp-matter
firmware never did, and a board with a weak signal became unreachable
([the measurement](ESPHome-Build#wi-fi-power-saving)).

### A stock module

**State: one module runs this way.** A module with the factory flash layout was converted on
2026-10-10, inside an A/C, and has taken two ESPHome updates over the air since. A second
converted module went silent on the bus within four minutes and stayed silent, for a reason
outside the chip that is not known. The sdk layout has not been converted, and nothing has run
for longer than an afternoon. What was measured, and the risks of the stock module under ESPHome
as such, are on [The Stock Module](Stock-Module-Build).

The module's bootloader boots the firmware slot that holds a valid image with the higher serial
number (the `FWHS` serial, not the Matter version). The Matter firmware's update writes whatever
its provider sends into the slot it is not running from. ESPHome's LibreTiny platform builds an
application image in the same format, signed with the same default key. Two things are missing
for the Matter firmware to accept it, and `firmware/scripts/matter-convert.py` adds both:

- The serial. LibreTiny stamps the image with a number taken from the build time (about 55.9
  million on the day of the test). The script re-signs the image with the next serial in the
  Matter numbering (`SERIAL_BASE` plus the version number). Changing the serial invalidates the
  manifest signature and the first sub-image trailer, so all of them are computed again.
- The wrapper. A Matter `.ota` header and a provider manifest, at a software version one
  above what the unit runs.

| | After the conversion |
|---|---|
| Bootloader, partition table, calibration data | untouched |
| The slot the Matter firmware ran from | still the Matter image, until the first ESPHome update overwrites it |
| The other slot | the ESPHome image |
| Matter fabric and commissioning data | still in flash, unused. The node stops answering on Matter |
| Wi-Fi credentials | not carried over. ESPHome uses the ones compiled in from `secrets.yaml` |

Risks particular to this route:

- A wrong flash layout bricks the unit at its first ESPHome update, not at the conversion.
  Find the layout first and do not guess.
- Nothing rolls a bad image back, and a bad signature hangs the bootloader, which does not
  fall back to the other slot. `matter-convert.py convert` verifies every signature before it
  writes a file and refuses an image that does not fit the slot. Do not edit the files it
  produces.
- The way back is the clip: a full dump taken before the module ever ran custom firmware, or
  the Matter clip image ([Recovery and Reflash](Recovery-and-Reflash#stock-module-ch341a-clip)).
- One of two converted units stopped answering on the bus. Be ready to go back with the clip.

#### The tool

`matter-convert.py` has three commands. Its settings come from
`firmware/scripts/ota-release.env` (copy `ota-release.env.example`), or from the file the `ENVF`
environment variable names.

| Command | Does |
|---|---|
| `slots <unit-ip>` | reads both firmware slots' serials from the unit. Read-only |
| `backup <unit-ip> [out.bin]` | saves the unit's inactive slot and prints its address, which tells the flash layout. Read-only |
| `convert --board B [--name N] [--software-version V] [--serial S] [--stage]`, with `B` one of `amebaz2-factory` and `amebaz2-sdk` | re-signs the built ESPHome image and wraps it as a Matter `.ota` with its provider manifest, in `firmware/built-images/`. `--stage` also puts both on the OTA provider host and restarts matter-server |

`slots` and `backup` talk to the Matter firmware's break-glass listener. `convert` needs
connectedhomeip's `ota_image_tool.py`, from any checkout of that project (`OTA_TOOL` in the
settings file). The settings file also holds `VID`, `PID`, `SERIAL_BASE`, the break-glass token
and port, and the provider host for `--stage`.

#### Find the flash layout

```
python3 firmware/scripts/matter-convert.py slots <unit-ip>
python3 firmware/scripts/matter-convert.py backup <unit-ip>
```

`slots` answers `ok: fw1_sn=<n> fw2_sn=<n> cur=<slot>`. Note the running slot and both serials:
the serial of the image you install has to be above both. `backup` prints `addr=`, the address of
the slot the unit is not running from. `0x10000` or `0x190000` is the factory layout, `0xC000` or
`0x1B8000` is the sdk layout. It needs Matter firmware 1.3.9 or later. The longer account is in
`firmware/docs/15-esphome-path.md` ("Which flash layout a unit has").

#### Procedure

1. Build the ESPHome image for the unit's layout. The build fails if the image's slot addresses
   are not those of the layout named.

   ```
   python3 firmware/scripts/dev.py build esphome --board amebaz2-factory --name <hostname> --friendly-name "<name>"
   ```

2. Re-sign and wrap it:

   ```
   python3 firmware/scripts/matter-convert.py convert --board amebaz2-factory --name <hostname> --stage
   ```

   It writes three files to `firmware/built-images/`: `rac-v<version>-esphome.bin` (the re-signed
   image), `.ota` (the same with a Matter header) and `.json` (the provider manifest). By default
   the software version is one above the last Matter release (1.3.50, so 1.3.51), and the serial
   is `SERIAL_BASE` plus that version. Compare the serial it prints with the two that `slots`
   gave. If it is not above both, pass a higher one with `--serial <n>`, and a higher version
   with `--software-version <n>` if the unit runs a Matter build you numbered higher yourself.

   `--stage` copies the `.ota` and the manifest to the provider directory on your Home Assistant
   host and restarts matter-server. It also archives the other manifests for this product there,
   so no other unit is offered a Matter image in the meantime. Without `--stage`, copy the two
   files yourself and restart matter-server.
3. Start the update for this one unit: the firmware update entity the Matter integration shows
   for it, or matter-server's `update_node` for its node id. The node never reports on Matter
   again, so do not wait for a new Matter version to show. Matter updates often fail on the first
   attempt, because the provider loses a discovery race. Try again three to five times, about 15
   seconds apart.
4. The unit restarts by itself when the image is written. Within a minute or two it should answer
   at `<hostname>.local` and appear in Home Assistant as a discovered ESPHome device. Then run
   the checks in [After the first boot](Stock-Module-Build#after-the-first-boot).
5. Take the conversion image off the provider: remove `rac-v<version>-esphome.ota` and its
   manifest from the provider directory and restart matter-server. Every other unit of the same
   product would otherwise be offered the ESPHome image as its next update.
6. Send one ESPHome update over the air ([Updates](Stock-Module-Build#updates)). It proves the
   normal update path on the kept layout. It also overwrites the Matter image in the other slot,
   so after this step the clip is the only way back.

Afterwards the unit's serials are LibreTiny's, in the tens of millions. A Matter image built from
the tag has a serial far below that and would not be chosen by the bootloader.

## What was kept from the Matter work

The knowledge that does not depend on Matter stayed, in documents that describe the hardware and
the protocol.

| Fact | Where |
|---|---|
| The RS-485 protocol, every byte map, the "77" request and display bits | `reverse-engineering/docs/03-rs485-ac-protocol.md`, [Protocol Overview](Protocol-Overview) |
| DE timing and bring-up order on the stock module | `reverse-engineering/docs/10-stock-fw-init-and-comms.md` |
| The stock module's flash layout, slot selection by FWHS serial, image signing, the QE-bit triage | `firmware/docs/10-stock-module-boot-and-recovery.md` |
| Clip flashing and recovery | [Recovery and Reflash](Recovery-and-Reflash) |
| ESP32 board wiring, the GPIO warnings, the stale-NVS trap | [ESP32 Replacement Board](ESP32-Replacement-Build), `firmware/docs/16-esp32-board-notes.md` |
| The old bus driver, hardware-validated on both Matter builds | `firmware/test/reference-driver/`, kept as the reference the ESPHome codec is tested against |
| The power estimate and its calibration | `firmware/docs/09-energy-monitoring.md` |
| What the Matter builds exposed as endpoints and clusters | the guide pages at the tag `matter-final` |
