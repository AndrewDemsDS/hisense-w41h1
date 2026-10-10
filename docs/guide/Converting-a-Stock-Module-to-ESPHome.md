# Converting a Stock Module from Matter to ESPHome

How to move a stock AEH-W41H1 module that runs this project's Matter firmware over to the ESPHome
firmware, over the air, with the module left in the A/C. The Matter firmware installs the ESPHome
image itself, as if it were its own next version.

**Status: done once.** One module with the factory flash layout was converted this way on
2026-10-10, inside an A/C, and has taken two ESPHome updates over the air since. The sdk layout has
not been converted, the way back to Matter has not been tried, and nothing has run for longer than
an afternoon. Treat it as a procedure that has worked once.

For an ESP32 replacement board, see
[Migrating an ESP32 Node from Matter to ESPHome](Migrating-ESP32-Matter-to-ESPHome).

## Why it works

The module's bootloader boots the firmware slot that holds a valid image with the higher serial
number (the `FWHS` serial, not the Matter version). The Matter firmware's update writes whatever
the provider sends into the slot it is not running from. It does not look at what the image does.

ESPHome's LibreTiny platform builds an application image in the same format, signed with the same
default key. Two things are missing for the Matter firmware to accept it:

- **The serial.** LibreTiny stamps the image with a number taken from the build time (about 55.9
  million on the day of the test). That would boot, since it is far above anything the Matter
  build uses, but it puts the unit out of step with the Matter numbering. The image is re-signed
  with the next serial in the Matter scheme instead (`SERIAL_BASE` plus the version number, the
  rule `dev.py ota amebaz2 build` follows). Changing the serial invalidates the manifest
  signature and the first sub-image trailer, so all of them are recomputed.
- **The wrapper.** A Matter `.ota` header and a provider manifest, at a software version one
  above what the unit runs.

`dev.py convert` does both and checks the result the way the bootloader will.

| | After the conversion |
|---|---|
| Bootloader, partition table, calibration data | untouched |
| The slot the Matter firmware was running from | still holds the Matter image, until the first ESPHome update overwrites it |
| The other slot | the ESPHome image |
| Matter fabric, commissioning data | still in flash, unused. The node stops answering on Matter |
| Wi-Fi credentials | not carried over. ESPHome uses the ones compiled in from `secrets.yaml` |

## What was measured, and what was not

On one module, 2026-10-10:

- The image booted under the factory bootloader and joined Wi-Fi. Its `Flash layout` sensor
  reported the layout as matching, running from slot 2 with the next update going to `0x010000`,
  and the Matter image still in slot 1.
- Two ESPHome updates with `esphome upload` succeeded. The running slot went 2, 1, 2 and the node
  was back within about 10 seconds each time.
- No reboot hang was seen, with the workaround flags in the image. One mains power cycle also came
  back clean.
- The A/C bus works: status frames decode and commands are answered.
- **The bus receiver stopped twice** and stayed stopped until a power cycle, while commands still
  reached the unit. The cause is not known. The board package now restarts the serial port when
  the link drops, which was shown to bring it back (see
  [ESPHome Build](ESPHome-Build#the-stock-module-without-a-replacement-board)).

Not measured: the sdk layout, a run longer than an afternoon, a failed or interrupted update, a
power cut during an update, and the return to the Matter firmware.

## Risks

- **A wrong layout bricks the unit at its first ESPHome update,** not at the conversion. Find the
  layout first (below) and do not guess.
- **Nothing rolls a bad image back.** The bootloader boots the higher serial as long as the image
  verifies. An ESPHome image that boots but cannot join your Wi-Fi leaves a unit you cannot reach
  over the network. It opens a fallback access point named `<name>-setup`, which may let you fix
  the credentials from within radio range. That recovery has not been tried on this module.
- **A bad signature hangs the bootloader.** It does not fall back to the other slot. `dev.py
  convert` verifies every signature before it writes a file, and refuses an image that does not
  fit the slot. Do not edit the files it produces.
- **The way back is the clip.** A full dump taken before the module ever ran custom firmware, or
  the Matter clip image, written with the SOIC-8 clip
  ([Recovery & Reflash](Recovery-and-Reflash#amebaz2-module-ch341a-clip)). Returning over the air
  has not been tried.
- **The receiver fault above has no known cause.** The recovery restores the link in under a
  second, but a unit that needs it often is not healthy. Watch the `Serial port restarts` sensor.
- **Home Assistant sees a different device.** Entity ids change, and the Matter node has to be
  removed by hand.

## Before you start

1. The unit runs this project's Matter firmware and answers on the break-glass listener. Read the
   slots. The command only reads:

   ```
   python3 firmware/scripts/dev.py ota amebaz2 revert --slots <unit-ip>
   ```

   The answer is `ok: fw1_sn=<n> fw2_sn=<n> cur=<slot>`. Note the running slot and both serials.
   The serial of the image you install has to be above both.
2. Find the flash layout. `revert --backup <unit-ip>` prints `addr=`, the address of the slot the
   unit is not running from: `0x10000` or `0x190000` is the factory layout, `0xC000` or `0x1B8000`
   is the sdk layout. It also reads only. The longer account is in
   `firmware/docs/15-esphome-path.md`, "Which flash layout a unit has".
3. `firmware/esphome/secrets.yaml` holds the Wi-Fi credentials of the network the unit is on now,
   and an API key. Check both twice. A typo here is the no-rollback case above.
4. `firmware/scripts/ota-release.env` is filled in as for a Matter release (`OTA_TOOL`, `VID`,
   `PID`, `SERIAL_BASE`, and the provider host if you want `--stage`).
5. The A/C is idle, and you can get at the module with the clip if it goes wrong.

## Procedure

**1. Build the ESPHome image for the unit's layout.** Give the node its own hostname if another
ESPHome A/C node is already on the network.

```
python3 firmware/scripts/dev.py build esphome --board amebaz2-factory --name <hostname> --friendly-name "<name>"
```

The build fails if the image's slot addresses are not those of the layout named.

**2. Re-sign and wrap it.**

```
python3 firmware/scripts/dev.py convert esphome --board amebaz2-factory --name <hostname> --stage
```

This reads the slot image from the build tree, re-signs it and writes three files to
`firmware/built-images/`: `rac-v<version>-esphome.bin` (the re-signed image), `.ota` (the same
with a Matter header) and `.json` (the provider manifest). By default the software version is one
above the newest Matter version this checkout knows, and the serial is `SERIAL_BASE` plus that
version. Compare the serial it prints with the two from step 1 of the checks. If it is not above
both, pass a higher one with `--serial <n>`, and a higher version with `--software-version <n>`
if the unit already runs a newer Matter build than the checkout.

`--stage` copies the `.ota` and the manifest to the provider directory on your Home Assistant host
and restarts matter-server, the same step a Matter release uses. It also archives the other
manifests for this product there, so no other unit is offered a Matter image in the meantime.
Without `--stage`, copy the two files yourself and restart matter-server.

**3. Install it.** Start the update for this one unit: the firmware update entity the Matter
integration shows for it in Home Assistant, or matter-server's `update_node` for its node id.
`dev.py ota amebaz2 flash` is the wrong tool here. It waits for the node to report the new Matter
version, and this node never reports on Matter again. Matter updates often fail on the first
attempt ([OTA Updates](OTA-Updates#ota-is-flaky-by-design-retry)); try again.

**4. Confirm the node came up as ESPHome.** It reboots by itself when the image is written. Within
a minute or two it should answer at `<hostname>.local` and appear in Home Assistant as a
discovered ESPHome device. Then check, in this order:

- `Flash layout` reads `factory ok ...` (or `sdk ok ...`). On `MISMATCH` the image has switched
  its own update server off. Do not try to update the unit. It was built for the wrong layout.
- `AC bus link` is on, and the climate entity shows the room temperature.
- `Serial port restarts` is 0 or stays where it is.

If the node does not appear, look for the `<hostname>-setup` access point before reaching for the
clip.

**5. Take the conversion image off the provider.** Remove `rac-v<version>-esphome.ota` and its
manifest from the provider directory (or stage the current Matter release again) and restart
matter-server. Every other unit of the same product would otherwise be offered the ESPHome image
as its next update.

**6. Clean up Home Assistant.** Add the ESPHome device with its API key and remove the old node
from the Matter integration. If the unit was set up in the `hisense-unified-ac` integration, delete
that entry. Update automations, dashboards and climate groups to the new entity ids.

**7. Send one ESPHome update over the air.**

```
cd firmware/esphome
esphome -s name <hostname> upload w41h1-amebaz2-factory.yaml --device <hostname>.local
```

This proves the normal update path on the kept layout. It also overwrites the Matter image in the
other slot, so after this step the clip is the only way back. `Flash layout` should show the
running slot change.

## Afterwards

- The unit's serials are now LibreTiny's, in the tens of millions. A Matter image built by this
  repo has a serial far below that and would not be chosen by the bootloader.
- Outdoor and coil temperature read "unknown" for a while after mains power returns. The unit
  sends a placeholder until it has real readings, and the ESPHome build no longer publishes it
  ([Entities, Endpoints and Diagnostics](Entities-and-Diagnostics)).
- Watch the bus counters and `Serial port restarts` for a few days.
