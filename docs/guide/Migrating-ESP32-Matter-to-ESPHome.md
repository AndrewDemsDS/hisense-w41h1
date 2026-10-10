# Migrating an ESP32 Node from Matter to ESPHome

How to move an ESP32 board that runs the esp-matter firmware over to the ESPHome firmware without
opening the indoor unit. The board fetches the ESPHome image itself, through the HTTP break-glass
path the Matter firmware already carries.

**Status: done once.** One ESP32-C3 node was moved this way on 2026-10-10 and takes ESPHome
updates over the air since (maintainer's report; not recorded in a pull request). It has not been
tried on the classic ESP32 board, and the way back to Matter over the air has not been tried at
all. Treat it as a procedure that has worked once. Issue #182 tracks turning it into a tested path.

This page is for the ESP32 replacement board only. The stock AmebaZ2 module has its own page:
[Converting a Stock Module from Matter to ESPHome](Converting-a-Stock-Module-to-ESPHome).

## Why it works

The esp-matter firmware has a manual update path that bypasses Matter: on a trigger it downloads
one file over plain HTTP from a URL compiled into the image, writes it to the idle OTA slot and
reboots into it ([OTA Updates](OTA-Updates#break-glass-manual-http-ota-when-matter-ota-will-not-finish)).
It applies no version rule and expects no Matter OTA wrapper, so it accepts any valid ESP32
application image for the same chip. An ESPHome build for ESP-IDF is such an image.

What changes and what does not:

| | After the migration |
|---|---|
| Application | ESPHome, in the slot that was idle |
| Bootloader | the esp-matter build's, untouched |
| Partition table | the esp-matter build's, untouched: two app slots of `0x1E0000` bytes, the `nvs` and `fctry` data partitions |
| Other app slot | still holds the last Matter image until the first ESPHome update overwrites it |
| NVS contents | left in place, including the Matter fabric data, which nothing reads any more |
| Later updates | ESPHome's own OTA, which writes to whichever slot is idle and works on this layout |

## Risks

Read these before starting. Most of them end with a USB cable at the board.

- **There is no rollback.** The esp-matter build does not enable the bootloader's rollback
  feature, so the board boots whatever was written last. An ESPHome image that starts but cannot
  join your Wi-Fi leaves a node you cannot reach over the network. The reference YAML opens a
  fallback access point named `<name>-setup` in that case, which may let you fix the credentials
  from within radio range. That recovery is untested here.
- **Serve the right file.** An ESPHome build produces `firmware.ota.bin` and
  `firmware.factory.bin`. Only `firmware.ota.bin` is an application image. The factory file is a
  whole-flash image that starts with a bootloader and a partition table, and it must never be
  written into an app slot.
- **Build for the right chip and pins.** A C3 image for a C3 board, with the TX, RX and DE pins
  your board is wired for. Wrong pins give a node that joins Wi-Fi and never hears the A/C, or one
  that holds the bus.
- **The break-glass path must work before you depend on it.** The URL is fixed when the Matter
  image is built (`HISENSE_OTA_URL`). If that address is a placeholder, or the board cannot reach
  it from its network, the trigger does nothing useful.
- **The download is unauthenticated.** The board flashes whatever is at that URL. Put the file
  there for the migration and remove it afterwards.
- **Power.** A flash write is the highest-current thing the board does, and a board on the A/C's
  5 V rail has little margin. The break-glass path lowers Wi-Fi transmit power and paces the
  download for that reason, and a truncated download is aborted, not booted. A brownout in the
  middle of a write can still corrupt the slot.
- **The image must fit the slot.** `0x1E0000` bytes (1,966,080). The ESPHome image for this
  component is about 1 MB.
- **Home Assistant sees a different device.** Entity ids, the device entry and anything that
  refers to them (automations, dashboards, climate groups) change. The Matter node has to be
  removed by hand.
- **Going back is not proven.** In principle the same slot layout would take an esp-matter
  application image through ESPHome's OTA, and the `fctry` partition is untouched. Nobody has
  tried it. The known way back is a USB flash
  ([Recovery & Reflash](Recovery-and-Reflash#esp32-boards-esphome-and-matter)).

## Before you start

1. The node answers on Matter, or at least on the break-glass listener (port 2324, only present
   when the image was built with a token).
2. You know the URL the running Matter image fetches from, and you control the HTTP server behind
   it. For images built with `dev.py ota esp32 build` it is `HISENSE_OTA_URL` from
   `firmware/scripts/ota-release.env`.
3. `firmware/esphome/secrets.yaml` holds the Wi-Fi credentials of the network the board is on
   now, and an API key. Check both twice. A typo here is the no-rollback case above.
4. The A/C is idle. The node drops off the bus for the length of the reboot.
5. You can reach the board with a USB cable if it goes wrong.

## Procedure

**1. Build the ESPHome image.** Give the node its own hostname if another ESPHome A/C node is
already on the network.

```
python3 firmware/scripts/dev.py build esphome --board c3 --name <hostname> --friendly-name "<name>"
```

The image is `firmware.ota.bin` in the build tree, normally
`firmware/esphome/.esphome/build/<hostname>/.pioenvs/<hostname>/`.

**2. Serve it at the break-glass URL.** Copy `firmware.ota.bin` to the HTTP server under the file
name the URL ends with (`esp32-ota.bin` for images built by `dev.py`). Fetch the URL from another
machine and compare the size or checksum with the file you built.

**3. Trigger the fetch.** Either write `Identify.IdentifyTime = 88` on endpoint 1 through Matter,
or send the token to the listener:

```
printf 'TOKEN\r\n' | nc <device-ip> 2324
```

The listener answers `ok` and the board starts the download. It reboots by itself when the image
is written.

**4. Confirm the node came up as ESPHome.** Within a minute or two it should answer at
`<hostname>.local` and show up in Home Assistant as a discovered ESPHome device. Check its log:

```
python3 firmware/scripts/dev.py monitor esphome --board c3 --name <hostname> --port <hostname>.local
```

`AC bus link` should turn on within a few seconds of boot. If the node does not appear, look for
the `<hostname>-setup` access point before reaching for the cable.

**5. Clean up Home Assistant.** Add the ESPHome device with its API key. Remove the old node from
the Matter integration so matter-server stops trying to reach it. If the unit was set up in the
`hisense-unified-ac` integration, delete that entry: the ESPHome device carries the same entities
natively. Update automations, dashboards and climate groups to the new entity ids.

**6. Remove the image from the HTTP server,** or put the Matter image back if other Matter nodes
still use the same URL for their own break-glass.

**7. Send one ESPHome update over the air.** This proves the normal update path on the kept
partition layout and overwrites the old Matter image in the other slot:

```
python3 firmware/scripts/dev.py flash esphome --board c3 --name <hostname> --port <hostname>.local
```

or `esphome run` with the board overrides shown in [OTA Updates](OTA-Updates#esphome-updates).

## Afterwards

- The delta-OTA base archived for this node under `firmware/built-images/` is no longer needed
  for it.
- The board still has the esp-matter bootloader and partition table. A later USB flash of
  ESPHome's `firmware.factory.bin` replaces both with ESPHome's defaults. That has not been done
  on a migrated node. Erasing the flash first (`dev.py erase esphome --port P`) removes the stale
  Matter data in NVS along with everything else.
- Watch the bus counters for a few days
  ([Entities, Endpoints and Diagnostics](Entities-and-Diagnostics#bus-counters)).
- **Check the radio is always on.** The esp-matter firmware never let the Wi-Fi radio sleep. An
  ESPHome build does by default on an ESP32 (`power_save_mode: light`), and a board that was fine
  on Matter can become unreachable on the same access point. Measured on a migrated ESP32-C3 node
  with a weak signal (about -77 dBm) on 2026-10-10: 65 % ping loss, the API connection dropping
  about once a minute, and commands that did not reach the unit. With `power_save_mode: none`:
  100 of 100 pings and a command taken in 0.5 s. `packages/node.yaml` sets `none` since that day,
  so an image built from this repo now has it. A node built before then, or from your own YAML,
  needs `power_save_mode: none` under `wifi:` and one more update.
