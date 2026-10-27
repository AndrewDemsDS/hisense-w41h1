# ESPHome Firmware

The firmware is an ESPHome component, `hisense_ac`, that speaks the A/C's RS-485 bus and presents
the unit to Home Assistant as a native climate device. This page covers what you do with it once
the tools are installed: choose the board file, add the node to Home Assistant, update it, pin a
release, and trim the entities to what your unit supports.

Depth, options and design notes: `firmware/esphome/README.md` and
`firmware/docs/15-esphome-path.md`.

## State

The firmware has driven live A/Cs from Home Assistant since 2026-08, with the units powered from
the connector's 5 V rail and closed up. Since 2026-10-10 it is the only firmware on the
maintainer's units. A sniffer pass on the wire, comparing its frames with the vendor module's, is
still open.

ESPHome talks to Home Assistant and to nothing else. Apple Home, Google Home and Alexa see the
unit when Home Assistant exposes the climate entity through its own Matter or HomeKit bridge.

## Board files

There is one file per board in `firmware/esphome/`. Each names the chip, the three bus pins and
what that board has on top (a status LED, the stock module's flash layout). The entities come from
a shared package, so their names and ids are the same on every board.

| File | Board | TX / RX / DE | State |
|---|---|---|---|
| `w41h1-esp32c3.yaml` | ESP32-C3 SuperMini | 5 / 6 / 10 | runs on a live A/C |
| `w41h1.yaml` | classic ESP32 | 19 / 18 / 4 | runs on a live A/C |
| `w41h1-esp32c3-plus.yaml` | ESP32-C3 SuperMini Plus | 5 / 6 / 10 | compiles. The same node as the SuperMini plus the RGB pixel, which has not been checked on a board |
| `w41h1-amebaz2-factory.yaml` | the stock module, factory flash layout | PA14 / PA13 / PA17 | runs on one module in an A/C since 2026-10-10 ([The Stock Module](Stock-Module-Build)) |
| `w41h1-amebaz2-sdk.yaml`, `w41h1-amebaz2-native.yaml` | the stock module, the other two layouts | PA14 / PA13 / PA17 | compile, never run |
| `w41h1-esp8266.yaml` | ESP8266 D1 mini | 15 / 13 / 5 | example, compiles, never run |
| `w41h1-rp2040.yaml` | Raspberry Pi Pico W | 4 / 5 / 6 | example, compiles, never run |

To tell a SuperMini from a SuperMini Plus, look at the LED on GPIO8. The plain SuperMini has a small blue
LED there. The Plus has an addressable RGB pixel (a square white package) and usually an external
antenna connector. They need different files: a pin toggled as for a plain LED means nothing to
the pixel, and a pixel that nothing writes to can sit lit in a random colour.

A second node needs its own name. The default is `hisense-ac`, shown in Home Assistant as
`Air Conditioner`. Set `name` and `friendly_name` in the board file, or pass
`-s name bedroom-ac -s friendly_name "Bedroom A/C"` on every build of that node.

For another chip, copy the nearest board file, change the platform block and the three pins, and
add what that platform needs. `firmware/esphome/README.md` ("Boards") says how the packages fit
together. The transceiver rules are the ones in
[ESP32 Replacement Board](ESP32-Replacement-Build#parts).

Building and the first flash: [Build Environment](ESP32-Build-Environment) by hand, or
[Build, Flash and Test](Build-Flash-Test) with `dev.py`.

## Adopt it in Home Assistant

There is no pairing code and no extra integration.

1. The node joins Wi-Fi with the network in `secrets.yaml`. If it cannot (wrong password,
   another network), it opens a hotspot named `<name>-setup` after about a minute, by default
   `hisense-ac-setup`. Join it with the `wifi_ap_password` from `secrets.yaml`. A captive portal
   asks for your Wi-Fi, and the node moves onto it.
2. Home Assistant discovers it over mDNS. Settings, Devices & services shows a discovered
   ESPHome device. Select Configure and paste the API key, the `hisense_ac__encryption_key` value
   from `secrets.yaml`.
3. Not discovered? mDNS does not cross VLANs. Add it by hand: Add integration, ESPHome, host
   `hisense-ac.local` or the node's IP address, port 6053. The API is plain TCP, so a node on a
   separate IoT network only needs Home Assistant allowed to reach that port.

The device appears as Air Conditioner with a climate entity and the switches, sensors and
diagnostics around it: [Everyday Control](Everyday-Control).

To change the API key later, edit `secrets.yaml`, update the node (over the air is fine), then
enter the new key when Home Assistant reports that the node needs to be authenticated again.

## Updates

Over Wi-Fi, from `firmware/esphome/`, with the board file you first flashed:

```
esphome run w41h1-esp32c3.yaml
```

ESPHome builds the image, offers the node on the network as the upload target (or pass
`--device hisense-ac.local`, or the node's IP address), uploads it and follows the log. Three
things to keep the same as on the first flash:

- The board file. A C3 node given the classic file receives an image for the wrong chip.
- `secrets.yaml`. A different API key locks Home Assistant out until you update the key there.
- The node's `name`, if you set one.

After an update, check that the `compiled on` line in the log shows the new build time. An ESP32
that resets on the first boot of a new image is rolled back to the old one by its bootloader
([symptom and fix](FAQ-Gotchas#an-update-uploads-then-the-node-is-back-on-the-old-firmware)).

The shipped configuration sets no OTA password, so anything on the same network can push an
image. Add `password:` under `ota:` if the node shares a network you do not control.

If a node stops answering after an update, reflash it over USB
([Recovery and Reflash](Recovery-and-Reflash#esp32-boards)). The stock module is updated the same
way with its own board file, and has no USB to fall back on
([The Stock Module](Stock-Module-Build#updates)).

## Version and releases

The firmware version is written once, in `firmware/esphome/packages/version.yaml`, and built into
every node. Home Assistant shows it on the device page as the firmware, for example
`2.0.0 (ESPHome 2026.10.0)`. There is nothing to bump for your own node: the version is that of the
tree you build from. Between two releases the tree keeps the number of the last one.

A release is the git tag `vX.Y.Z` on `main`, with a section in `CHANGELOG.md` and a GitHub release
that carries that section. A release has no firmware image attached. An image holds its builder's
Wi-Fi credentials, API key and hotspot password, so everyone builds their own.

The first release is 2.0.0, the tag `v2.0.0` (2026-10-13). The examples below use it.

To build a release from a clone, check out its tag and build as usual:

```
git checkout v2.0.0
```

A configuration kept outside a clone can take the components from git at a tag:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/AndrewDemsDS/hisense-w41h1
      ref: v2.0.0
      path: firmware/esphome/components
    components: [hisense_ac]      # and w41h1_module on the stock module
```

Your YAML then defines the UART, the hub and the entities itself, and the node has no firmware
version unless your YAML sets `esphome: project:`. To get the whole node of the board files (every
entity, setup mode and the version), take the packages from git as well. That form, with its one
substitution that points the packages at the components of the same tag, is in
`firmware/esphome/README.md` ("Using a release from your own configuration").

## Capability gating

Units differ in what they support, and the A/C reports it. You remove the entities your unit does
not have.

1. Flash with everything declared.
2. Read the `Capability ...` diagnostic sensors
   ([list](Entities-and-Diagnostics#capability-and-fault-flags)). A flag that is off means "not on
   this unit". Two units of the same model can differ.
3. Trim the YAML. For a cooling-only unit set `supports_heat: false` on the climate platform. For
   the presets set `supports_eco`, `supports_quiet`, `supports_turbo` or `supports_sleep` to
   `false`: every preset that needs the missing mode disappears. Delete the switches and the
   select your unit has no use for. `supports_horizontal_swing: true` adds horizontal swing for a
   unit with that louvre motor.

The entities live in `firmware/esphome/packages/hisense-ac.yaml`. Every option of every platform
is in `firmware/esphome/upstream/hisense_ac.mdx`.

## Wi-Fi power saving

The node keeps its radio on (`power_save_mode: none`). ESPHome's default on an ESP32 lets the
radio sleep between beacons. Measured on an ESP32-C3 node with a weak signal (about -77 dBm) on
2026-10-10, that default gave 65 % ping loss, an API connection that dropped about once a minute,
and commands that did not reach the unit. With the radio always on: 100 of 100 pings and a
command taken in 0.5 s.

A node built from this repository since that day has the setting. A node built from your own YAML
needs `power_save_mode: none` under `wifi:`. A board on a supply too weak for an always-on radio
can go back with `-s wifi_power_save light`.
