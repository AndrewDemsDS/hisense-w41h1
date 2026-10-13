# ESPHome firmware

Local control of a Hisense A/C over the reverse-engineered RS-485 bus, exposed to Home Assistant
through ESPHome's native API. It runs on an ESP32 board that takes the Wi-Fi module's place, and
on the stock module itself. **Home Assistant only.** The Matter firmwares this project used to
ship are retired (tag `matter-final`).

One component and one set of entities for every board. The ESP32 and ESP32-C3 builds run on live
units, and so does the stock module's factory-layout build, on one module ([Boards](#boards)).
The firmware has a version, and a release is a git tag ([Version and releases](#version-and-releases)).

The user guide covers everything a builder needs, so it is not repeated here:

- installing ESPHome on a clean machine and the first flash, step by step:
  [`docs/guide/ESP32-Build-Environment.md`](../../docs/guide/ESP32-Build-Environment.md)
- entities, hardware, flashing (both boards), staged bring-up, capability gating and status:
  [`docs/guide/ESPHome-Build.md`](../../docs/guide/ESPHome-Build.md)
- wiring and the GPIO / ground-loop warnings:
  [`docs/guide/ESP32-Replacement-Build.md`](../../docs/guide/ESP32-Replacement-Build.md)
- the bench against `virtual_ac.py`, and what is hardware-verified:
  [`docs/guide/Build-Flash-Test.md`](../../docs/guide/Build-Flash-Test.md)
- design rationale and the phase log: [`../docs/15-esphome-path.md`](../docs/15-esphome-path.md)

## Quick use

```bash
cp secrets.yaml.example secrets.yaml      # Wi-Fi, hotspot password, API key; the real file is gitignored
esphome run w41h1.yaml                    # classic ESP32: build, flash, follow logs
esphome run w41h1-esp32c3.yaml            # ESP32-C3 SuperMini
esphome run w41h1-esp32c3-plus.yaml       # ESP32-C3 SuperMini Plus (RGB pixel on GPIO8)
```

Or `firmware/scripts/dev.py flash esphome --board c3 --port <port>` from the repo root, which
builds `w41h1-esp32c3.yaml` as it is. `--board c3-plus` builds `w41h1-esp32c3-plus.yaml` and
`--board classic` builds `w41h1.yaml`.

## Boards

| File | Board | TX / RX / DE | Status | Image (ESPHome 2026.7.4) |
|---|---|---|---|---|
| `w41h1.yaml` | classic ESP32 (`esp32dev`), board and pins overridable with `-s` | 19 / 18 / 4 | runs on a live A/C | 929 KB, 51 % of the slot |
| `w41h1-esp32c3.yaml` | ESP32-C3 SuperMini | 5 / 6 / 10 | runs on a live A/C | 987 KB, 54 % |
| `w41h1-esp32c3-plus.yaml` | ESP32-C3 SuperMini Plus (RGB pixel on GPIO8, see [Status LED](#status-led)) | 5 / 6 / 10 | compiles; same node as the row above plus the pixel, which has not been checked on a board | 1018 KB, 55 % |
| `w41h1-amebaz2-factory.yaml`, `-sdk.yaml`, `-native.yaml` | the stock W41H1 module itself (RTL8710C through LibreTiny), one file per flash layout | PA14 / PA13 / PA17 | **factory layout runs on one module in an A/C (2026-10-10)**; sdk and native compile, never run | 610 KB, 40 % of the factory layout's slot, 35 % of the other two |
| `w41h1-esp8266.yaml` | ESP8266 D1 mini | GPIO15 / GPIO13 / GPIO5 | example, compiles, never run | 513 KB, 49 % |
| `w41h1-rp2040.yaml` | Raspberry Pi Pico W | GPIO4 / GPIO5 / GPIO6 | example, compiles, never run | 596 KB, 57 % |

Every board file says three things: which chip, which pins, and which extras (a status LED, the
stock module's flash layout). Everything else comes from packages that all of them include, so the
entity names and object ids are the same on every chip:

- `packages/version.yaml`: the firmware version, and nothing else. `node.yaml` includes it
  ([Version and releases](#version-and-releases)).
- `packages/esp32.yaml`: the `esp32:` platform block on ESP-IDF, for the board the file names in
  its `board` substitution. The three ESP32 files include it.
- `packages/node.yaml`: the node's name (`name` and `friendly_name`, by default `hisense-ac` and
  `Air Conditioner`: set both in the board file for a second node), logger, API, Wi-Fi, the setup hotspot and
  [setup mode](#wi-fi-setup-mode-and-the-77-gesture), and three diagnostics every
  board has (`Uptime`, `WiFi signal`, `Reset reason`). Wi-Fi power saving is
  off (`power_save_mode: none`): on an ESP32-C3 with a weak signal ESPHome's default, `light`,
  lost 65 % of pings and commands did not arrive. A board on a weak supply can go back with the
  `wifi_power_save` substitution (`-s wifi_power_save light`).
- `packages/hisense-ac.yaml`: the UART, the `hisense_ac` hub and every entity.
- `packages/ota.yaml`: ESPHome's update server. The AmebaZ2 files take theirs from
  `packages/amebaz2.yaml` instead, with an id, so `w41h1_module` can switch it off. That package
  also holds the chip, the board definition and the module's three pins, which are fixed, so an
  AmebaZ2 board file names only its flash layout.
- `packages/status-led.yaml` and `packages/status-pixel.yaml`: optional, for a board with a plain
  LED or an addressable RGB pixel on a GPIO ([Status LED](#status-led)). Only the two ESP32-C3
  files include one.

To add a board, copy the nearest file, change the platform (the `board` substitution for an
ESP32, the platform block otherwise) and the three pins, and add what
that platform needs on top (the ESP8266 file moves the logger off the bus UART, the AmebaZ2
files add a package of their own). The files sit next to `components/` and not in a directory of their own
because ESPHome resolves the local component path and `secrets.yaml` against the directory of the
file it is given.

Board files build into `.esphome/build/<name>/`. Two files with the same `name` share that
directory, so give `-s name <something>` when switching between boards on one machine.

### The stock module (AmebaZ2)

The `w41h1-amebaz2-*.yaml` files build ESPHome for the Realtek RTL8710C inside the AEH-W41H1
itself, through ESPHome's LibreTiny platform, so the unit needs no replacement board. **The
factory layout has run on one module in an A/C since 2026-10-10:** boot, Wi-Fi, the bus, and two
updates over the air. The sdk and native layouts compile and have not run on hardware. What was
measured and what is open is in
[`../docs/15-esphome-path.md`](../docs/15-esphome-path.md#the-stock-module-through-libretiny).
A unit that still runs this project's retired Matter firmware is converted over the air:
[`docs/guide/Converting-a-Stock-Module-to-ESPHome.md`](../../docs/guide/Converting-a-Stock-Module-to-ESPHome.md).

There are three files because a module can have one of three flash layouts, and the image has to
be built for the one it has. An image built for another layout bricks the unit at its first
update. There is no default.

| File | For a module that |
|---|---|
| `w41h1-amebaz2-factory.yaml` | went from the vendor firmware to this project's firmware over the air (factory partition table) |
| `w41h1-amebaz2-sdk.yaml` | was first written with this repo's clip image (Realtek SDK partition table) |
| `w41h1-amebaz2-native.yaml` | was written from scratch with LibreTiny's own partition table and bootloader |

[Which flash layout a unit has](../docs/15-esphome-path.md#which-flash-layout-a-unit-has) says how
to find out.

A board file only names its layout. What the module needs in code is the `w41h1_module`
component (`components/w41h1_module/`), which `packages/amebaz2.yaml` configures:

```yaml
w41h1_module:
  layout: factory          # required, no default: factory, sdk or native
  hisense_ac_id: ac_bus
  serial_port_restarts:
    name: Serial port restarts
  flash_layout:
    name: Flash layout
```

The layouts are the `layout-*.json` files in the component's directory. Its codegen hands the
one named to PlatformIO as the board override and adds the reboot workaround's linker flags. Its
`w41h1_slots.h` fails the build if the image does not have the addresses of the layout it names,
and at run time compares them with the bootloader's partition table: the result is the
`Flash layout` diagnostic sensor, and on a mismatch the image switches its own update server
off. The component validates only on `rtl87xx`. On any other chip `esphome config` rejects it
with a message that says so. It uses nothing outside its own directory, so it also works from a
git source (`external_components` with `path: firmware/esphome/components` and both
`hisense_ac` and `w41h1_module` listed).

One fault showed up on hardware and is fixed. The receive side of the bus port stopped twice and
stayed stopped until a power cycle, while commands still reached the unit. LibreTiny's receive
buffer loses count of a byte when its interrupt lands inside a read, so `hisense_ac` reads with
interrupts masked on this platform. Reopening the port also clears it, so `w41h1_module` still
restarts the port when the hub reports the link lost and every 15 s
while it stays down, as a fallback. It logs a warning each time and counts the restarts in a
`Serial port restarts` diagnostic sensor. On a unit that answers, that sensor stays at 0. A
second converted module went silent on the bus for a reason outside the chip that is still open
(`firmware/docs/15-esphome-path.md`, "The silent unit").

The other open risks, in short:

- LibreTiny rates this chip family 2 out of 5 for stability.
- A restart or an OTA can leave the chip dark until a 30 second power cut (LibreTiny issue 396).
  `w41h1_module` carries the workaround from that issue. With it, two updates and a mains power
  cycle came back clean on the tested module.
- Nothing rolls a bad update back. An image that does not boot needs the clip.
- A first flash with the clip or UART download mode must leave flash `0x1000..0x3FFF` (the
  module's calibration data) as it was. The over-the-air conversion does not touch it.
- No long run yet, and the way back to the retired Matter firmware has not been tried.

`dev.py test esphome --board amebaz2-factory` and `dev.py build esphome --board amebaz2-factory`
(or `amebaz2-sdk`, `amebaz2-native`) validate and compile one. `matter-convert.py convert --board
amebaz2-factory` re-signs the built image and wraps it as a Matter `.ota` for the conversion.
`dev.py` refuses a bare `--board amebaz2`, and refuses to flash any of them over a serial port.

### Build cache

`dev.py` sets `PLATFORMIO_BUILD_CACHE_DIR` (default `~/.cache/w41h1-dev/pio-build-cache`) for
every ESPHome build, and PlatformIO then keeps each compiled object by content. Export the same
variable to get it when calling `esphome` directly. It pays off on the boards PlatformIO
compiles, which here are the stock module, the ESP8266 and the RP2040. Measured on the stock
module's image: 29 s cold, 18 s into a new build directory (a second node name, a new checkout),
6 s after a clean with nothing changed, for 77 MB of cache per flash layout. The ESP32 boards gain
nothing from it: ESPHome builds those with ESP-IDF's own tools, which already use ccache when it
is installed.

An object served from the cache prints no compiler warnings, so a warning shows once, on the
build that compiled the file. When nothing at all changed, the cache hands back the linked
firmware whole, and LibreTiny's slot images (`image_firmware_is.*.bin`), which it
writes while linking, are not produced. `firmware.uf2`, the file an update uses, is. `dev.py build`
notices and builds once more without the cache. PlatformIO never prunes the directory. Delete it
when it has grown.

## Version and releases

The firmware version is written in one file, `packages/version.yaml`, as ESPHome's `project:`
block. It is semver and starts at 2.0.0 (the retired Matter firmwares were 1.x). `node.yaml`
includes that file, so a node built from the packages carries the version without anything in
its own YAML:

- Home Assistant shows it as the device's firmware, for example `2.0.0 (ESPHome 2026.7.4)`.
- The ESPHome dashboard shows it beside the node, and the node announces it over mDNS.
- It is in the image as `ESPHOME_PROJECT_VERSION`.

The version is not part of any entity's name, object id or unique id, so a new version does not
move an entity in Home Assistant. With a project set, Home Assistant takes the device's
manufacturer and model from the project name (`AndrewDemsDS` and `hisense-w41h1`). Between two
releases the tree keeps the number of the last one. The `compiled on` line in the log tells two
builds of one version apart.

A release is the git tag `vX.Y.Z` on `main`, with a section in [`CHANGELOG.md`](../../CHANGELOG.md)
and a GitHub release that carries that section. Releases have no firmware images attached: an
image holds its builder's Wi-Fi credentials, API key and hotspot password, so everyone builds
their own. How a release is cut: [`CONTRIBUTING.md`](../../CONTRIBUTING.md#releasing).

### Using a release from your own configuration

A clone checked out at the tag is the simplest way: `git clone --branch v2.0.0`, then the
[Quick use](#quick-use) lines. A configuration kept somewhere else can take the release from git
instead, in two ways.

**The components only.** Your YAML defines the UART, the hub and the entities itself (every
option is in [`upstream/hisense_ac.mdx`](upstream/hisense_ac.mdx)):

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/AndrewDemsDS/hisense-w41h1
      ref: v2.0.0
      path: firmware/esphome/components
    components: [hisense_ac]      # and w41h1_module on the stock module
```

A node built this way has no firmware version unless its YAML sets `esphome: project:` itself.

**The packages too**, which gives the node of the board files: every entity, setup mode and the
version. The packages are ESPHome remote packages, and one substitution points them at the
components of the same tag:

```yaml
substitutions:
  name: bedroom-ac
  friendly_name: Bedroom A/C
  tx_pin: "5"
  rx_pin: "6"
  de_pin: "10"
  w41h1_components:
    type: git
    url: https://github.com/AndrewDemsDS/hisense-w41h1
    ref: v2.0.0
    path: firmware/esphome/components

packages:
  w41h1:
    url: https://github.com/AndrewDemsDS/hisense-w41h1
    ref: v2.0.0
    files:
      - firmware/esphome/packages/node.yaml
      - firmware/esphome/packages/ota.yaml
      - firmware/esphome/packages/hisense-ac.yaml

esp32:
  board: esp32-c3-devkitm-1
  framework:
    type: esp-idf
```

The tag is written twice and both have to be the same one. The four secrets `node.yaml` reads
(`wifi_ssid`, `wifi_password`, `wifi_ap_password`, `hisense_ac__encryption_key`) come from the
`secrets.yaml` beside your own file. For the status LED add `packages/status-led.yaml` or
`packages/status-pixel.yaml` to `files:` and its substitutions to yours. For the stock module
take `packages/amebaz2.yaml` in place of `ota.yaml`, set `layout`, and use the pins of the
`w41h1-amebaz2-*.yaml` files.

Without the `w41h1_components` substitution the packages look for a `components/` directory
beside the file given to `esphome`, which is how the board files in this repository find theirs.

## Recovery and indication

### Wi-Fi setup mode and the "77" gesture

On the remote, press horizontal airflow six times. The panel shows "77" and the node opens its
hotspot, `<name>-setup`, with the password from `wifi_ap_password` in `secrets.yaml`. Join it,
open `http://192.168.4.1` and enter the new network. The same thing is a button in Home
Assistant, `Start Wi-Fi setup`.

Setup mode ends in a restart, on whichever comes first:

- the network entered in the portal has connected,
- the gesture is repeated (at least 10 s after the first),
- `wifi_setup_timeout` has passed (10 minutes, a substitution in `packages/node.yaml`, between
  30 s and 24 h). A network entered shortly before that still gets 90 s to connect.

A gesture made by mistake costs that much: the node is off the network until one of those, then
back on the network it had. The A/C keeps running and keeps answering the remote throughout.

What happens to the Wi-Fi credentials, case by case:

| Case | Result |
|---|---|
| Nothing entered in the portal | Nothing changes. The node restarts onto the network it had |
| A network entered, and it connects | It is kept. The node restarts onto it |
| A network entered, it has not connected when the mode ends (timeout or second gesture) | The network the node had is written back, then the restart |
| The node restarts for another reason in between (Home Assistant's API restarts a node nothing has connected to for 15 minutes, a restart button, an update) | The same: written back on the way down |
| Power is cut in between | The network the node had is written back at the next boot |
| Power is cut in the instant between the new network connecting and the restart | The new network is taken for one that never connected and is undone. Enter it again |

"The network the node had" is the one it was last connected to in this boot, or the one loaded
from flash at boot. A node that has not been connected since boot and has nothing saved goes
back to the Wi-Fi in `secrets.yaml`.

While setup mode is on, the SSID and password to go back to are kept in the node's flash in a
preference of the `wifi_setup_mode` component, beside the copy the image or ESPHome's own saved
settings already hold. The preference is overwritten with zeroes when the mode ends, and at boot
when one is found left over. The SSID and password are never logged.

When the hotspot is already open because the network is out of reach (ESPHome opens it by
itself after a minute), the gesture or the button only lights "77". The node keeps trying its
network and is not restarted: "77" goes off when a network connects, on a second gesture, or
after `wifi_setup_timeout`. A network entered in the portal that has not connected by then is
replaced by the one the node had, as above.

What to know before relying on it:

- **It has not run on hardware.** The decisions are host-tested
  (`firmware/test/test_esphome_setup_mode.cpp` for the mode,
  `firmware/test/test_esphome_setup_request.cpp` for the gesture on the bus) and the images
  compile. Whether the hotspot opens on each chip, and what a given unit sends for the gesture,
  has to be seen on a unit. The check to run is below.
- The hotspot and the home network cannot be up together: ESPHome closes its hotspot as soon as
  the station connects. So the node leaves the network for the duration.
- ESPHome opens its hotspot once per boot. If the fallback already opened and closed earlier in
  the same boot (the router was away for a minute), setup mode cannot open it again: the node
  logs that, restarts after 30 s, and the next gesture works.
- A network saved through the portal is stored under a key that ESPHome derives from the build's
  configuration. An update built from a changed configuration goes back to the Wi-Fi in
  `secrets.yaml`. Put the new network there as well.
- Only the SSID and password are put back after a failed attempt, as a saved network, which
  takes the place of the build's own until the next update. A node configured with a static IP
  or several networks loses those until then. With several networks and the hotspot already
  open, nothing is written back at all (the node cannot tell a portal entry from its own move to
  the next network).
- **With `fast_connect: true`, a network marked `hidden: true`, an enterprise login, or no
  network in the build at all, the hotspot cannot be opened on demand.** The first two reconnect
  before the hotspot is up, an enterprise login cannot be written back, and without a network
  the hotspot is open anyway. `esphome config` says so with a warning, and on the node the
  gesture and the button log why and change nothing. The fallback hotspot still opens when the
  network is gone, and the gesture lights "77" on it. The shipped configuration sets none of
  these.
- **A unit that asks with the sustained bit (`0x08`) cannot end setup mode with the gesture.**
  While "77" is on, the node sends that same bit, so the unit's own is not read. Such a unit
  leaves by the timeout or by a network connecting. The bench unit sends `0x20` and is not
  affected. No unit here has been seen to send `0x08`.
- The wait for an update is not covered: an upload blocks the node's loop, so a gesture made
  during one is lost.

The check on a unit, with the remote and a phone. The log at `INFO` shows each step under the tag
`wifi_setup_mode`. A node with a serial console shows them while it is off the network.

1. Gesture. "77" appears, the status LED blinks fast (the pixel turns blue), the node drops out
   of Home Assistant, and the hotspot `<name>-setup` is listed within a few seconds. Log:
   "leaving the network and opening the hotspot", then "hotspot open".
2. Gesture again after 10 s. "77" goes off and the node is back in Home Assistant within a
   minute, on the same network.
3. Gesture, join the hotspot, enter a network that does not exist. Log: "a network was entered".
   Wait for the timeout (set `wifi_setup_timeout: 2min` for the test). The node restarts and
   comes back on the network it had. Log before the restart: "the network entered did not
   connect, restoring the previous one".
4. The same, but cut the power one minute after entering the network. At the next boot the log
   says "The node went down in setup mode with a network that had not connected: restoring the
   previous one" and the node connects to the network it had.
5. Gesture, enter the real network. The node restarts onto it. Log: "connected, restarting".
6. Press `Start Wi-Fi setup` in Home Assistant, then end it with the gesture.
7. Switch the router off for two minutes with the node running. The fallback hotspot opens by
   itself. Gesture: "77" lights and the node does not restart. Switch the router on: "77" goes
   off when the node reconnects, without a restart.
8. On the stock module, steps 1 and 2 also show whether the restart out of setup mode comes back
   (LibreTiny issue 396).

On the bus, the gesture is a bit in byte 17 of the unit's reply to the `0x1E` heartbeat. The bench
unit sets `0x20` for one reply per gesture, and that is taken at once. The vendor firmware also
tests `0x08`, which no unit here has been seen to send: it has to hold for three replies, is
ignored while the node itself shows "77" (the node sends the same bit to light it), and is
ignored when it is already set at boot. A reply with a bad checksum never counts. With the log at
`DEBUG`, every change of those bits is logged, taken or not.

The mode is the `wifi_setup_mode` component, which `packages/node.yaml` wires to the hub's
`on_setup_request` and to the button. In a configuration of your own:

```yaml
wifi_setup_mode:
  id: wifi_setup
  timeout: 10min               # 30 s to 24 h
  # hisense_ac_id: ac_bus      # found by itself when there is one hub

hisense_ac:
  on_setup_request:
    - wifi_setup_mode.toggle:  # in when off, out when on. Also: wifi_setup_mode.start, .stop

binary_sensor:
  - platform: template
    name: Wi-Fi setup
    condition:
      wifi_setup_mode.active:  # true while "77" is on the panel
```

It needs `wifi:` with `ap:`, `captive_portal:` and the hub. For other uses of the gesture, the
hub has the pieces on their own:

```yaml
hisense_ac:
  id: ac_bus
  on_setup_request:            # x = the request bits, 0x20 or 0x08
    - hisense_ac.set_setup_indicator: true     # "77" on the panel
    - delay: 30s
    - hisense_ac.set_setup_indicator: false
```

### Status LED

Off while the node is healthy. Two optional packages, one per kind of LED. A board file that
includes neither has no status LED.

| Package | For | Shows |
|---|---|---|
| `packages/status-led.yaml` | a plain LED on a GPIO (`status_led_pin`, `status_led_inverted`) | slow blink: no Wi-Fi, no Home Assistant connection, or the A/C bus link down. Fast blink: setup (below), or ESPHome's safe mode |
| `packages/status-pixel.yaml` | one WS2812 RGB pixel, ESP32 only (`status_pixel_pin`) | blue: setup (below). Red: no Wi-Fi for 15 s. Amber: Wi-Fi up, the A/C bus link down for 15 s. Dim, and not an entity in Home Assistant |

"Setup" is one state for both packages: setup mode from the request until the restart, whether
the hotspot has opened yet or failed to open, and the fallback hotspot open by itself. The rule
is the `wifi_setup_mode` component's `status_display()`, and both packages ask it. Through the
restart itself the plain LED is dark (the pin is not driven), and the pixel keeps its blue for
about a second until the new boot writes it black.

The two ESP32-C3 SuperMini boards differ exactly here, and both have the LED on GPIO8:

- **SuperMini** (`w41h1-esp32c3.yaml`): a small blue LED, lit when the pin is low.
- **SuperMini Plus** (`w41h1-esp32c3-plus.yaml`): an RGB pixel, a square white package, on a
  board that usually also has an external antenna connector. A pixel that nothing writes to can
  sit lit in a random colour. With the package it is written to black at boot.

GPIO8 is a strapping pin on the C3. Using it for the board's own LED is safe: the chip reads it
only at reset, before the firmware drives it. Wire nothing else to it.

Firmware cannot switch a power LED that is wired to the supply (the red one on some SuperMini
boards), nor the LEDs on an RS-485 transceiver board, which follow the bus lines. Those need tape
or a soldering iron. No LED is documented on the stock module
(`reverse-engineering/docs/01-hardware.md`), so its board files take neither package.

Not checked on hardware yet: the pixel's colour order (`GRB`, the WS2812B's, a substitution if
a board differs), that it is dark after boot, and how visible 8 % brightness is through the
unit's housing.

### Safe mode

ESPHome's safe mode is active on every board file without any configuration: the `ota:` platform
loads it. Ten boots in a row that do not stay up for a minute put the node into it. In safe mode
only the logger, Wi-Fi with its hotspot and portal, the update server and a plain status LED
run. The API, the A/C bus and every entity stay off, so a firmware that crashes in the A/C code
can still be replaced over the air. After five minutes without an update the node restarts and
tries the normal firmware again. The boot counter is kept in flash.

On ESP32 the bootloader also rolls a new image back by itself when it never reaches that first
good minute. On the stock module (LibreTiny) there is no such rollback: safe mode only helps when
the image gets as far as starting ESPHome, and an image that does not boot needs the clip. The
stock module's flash layout check does not run in
safe mode ([`../docs/15-esphome-path.md`](../docs/15-esphome-path.md#which-flash-layout-a-unit-has)).

## Layout

| Path | What |
|---|---|
| `w41h1.yaml` | the classic ESP32 board file and the original entry point; board and pins are substitutions |
| `w41h1-*.yaml` | one file per other board (see [Boards](#boards)) |
| `packages/` | what the board files include: `node.yaml`, `version.yaml` (through `node.yaml`), `hisense-ac.yaml`, `ota.yaml`, `esp32.yaml` for the ESP32 boards, the optional `status-led.yaml` and `status-pixel.yaml`, and `amebaz2.yaml` for the stock module (configuration only) |
| `components/hisense_ac/` | the custom component: hub, climate, switches, select, sensors |
| `components/w41h1_module/` | the stock module's own component, `rtl87xx` only: its flash layouts (`layout-*.json`), the layout guard (`w41h1_slots.h`), the reboot workaround, and the serial port recovery (`w41h1_module.cpp`, with its timing rule in `w41h1_uart_policy.h`) |
| `tests/components/hisense_ac/` | every option of every platform, at the path and in the shape ESPHome's own `tests/components/<name>/` uses, so it moves upstream unchanged |
| `tests/components/w41h1_module/` | every option of `w41h1_module`, in the same shape, for the one platform it validates on |
| `tests/test_build_components/` | stand-ins for ESPHome's shared UART test packages, so the relative include in the test resolves here too |
| `tests/build.*.yaml` | repo-only harness so `esphome config` and `esphome compile` can run those tests (CI runs `config`). `build.rtl87xx-ard.yaml` builds the `w41h1_module` test and, with it, every option of `hisense_ac` on AmebaZ2, for which `hisense_ac` has no upstream-shaped test. `build.hub-only.esp32-c3-idf.yaml` is the hub with no entity platform, which CI also compiles |
| `secrets.yaml.example` | template for the gitignored `secrets.yaml` |

Every file of the `hisense_ac` component passes ESPHome's own gates on the release this repo
builds with: `script/ci-custom.py`, their `.clang-format` and `.clang-tidy`, ruff, and pylint
(`firmware/scripts/esphome-upstream-check.sh`, a CI job on every change to the firmware). The
nightly workflow runs the same gates against their `dev` branch for information. `w41h1_module` is held to the same clang-format and custom
source rules by `cpp-lint.sh`. It is not put through ESPHome's Python gates: it is specific to
this module and not meant for upstream.

[`upstream/hisense_ac.mdx`](upstream/hisense_ac.mdx) is the draft of the component's page for
ESPHome's documentation repository (`esphome-docs`, `src/content/docs/components/`), which an
upstream pull request has to be paired with. Keep it in step with the option schemas.

### Log levels

The component logs under the tag `hisense_ac`. Set the level with ESPHome's `logger:` component.

| Level | What it shows |
|---|---|
| `WARN` | the bus link dropping, a command that could not be sent, got no reply, was sent again or was given up on, a status frame with a bad checksum, a fault reported by the unit |
| `INFO` | the bus link coming back, a fault clearing, a command the unit took on a re-send, a setup request from the remote, the panel's "77" going on or off |
| `DEBUG` | each status change, every command sent, the unit's capability bitmap, every change in the request bits of the unit's `0x1E` reply |
| `VERBOSE` | every decoded status frame, every poll that got no reply |
| `VERY_VERBOSE` | the raw bytes of every frame sent and received |

Use `VERY_VERBOSE` when the link is down. It shows whether the A/C sends anything at all, and you
do not need to add a `debug:` block to the UART.

### Confirm and retry

A command used to be one frame, sent once. The component now checks each one twice and sends it
again when the unit did not take it.

| Check | When | On failure |
|---|---|---|
| The unit replied to the frame | within the 500 ms reply window | the frame goes out again in the next bus cycle, ahead of anything queued after it, three sends at most, and only while the unit answers its status poll |
| The unit's status shows what was asked | first status frame 4 s after the frame left the wire | power, mode, setpoint, fan and swing: the frame is sent again, twice at most. Eco, turbo, quiet and sleep: the plan to the wanted state is made again from what the unit reports, once, and waits out the 10 s special-mode pacing |

After the last re-send the component logs a warning, counts the command in `failed_commands` and
lets the entities show what the unit reports. A newer command replaces the one still being checked,
so an older request is never sent again over it. A lost bus link drops every open command.

Some differences are the unit's own behaviour and are not treated as a lost command: it keeps its
own setpoint in auto, dry and fan-only, turbo forces cool at 16 C on high fan, quiet and the sleep
profiles hold the fan, and a unit that is off ignores everything except power. A value that is
neither the old one nor the requested one means someone used the remote, and the command is
dropped. The rules are `intent_expected_fields()` and `confirm_decision()` in `hisense_map.h`,
tested in `firmware/test/test_esphome_confirm.cpp`.

A mode request to a unit that is off is one frame that carries both the mode and power-on, as the
stock module sends it (frame byte 18: `0x5C` cool, `0x3C` heat). If the unit is still off four
seconds later, the re-send is the older pair of frames, power-on and then the mode.

The `unanswered_commands`, `command_retries` and `failed_commands` sensors count all of this since
boot.

### The codec port (`hisense_protocol.*`, `hisense_map.h`)

The component carries its own protocol code: frame builders and parsers, the enum and
fan-ladder mapping, presets, the power estimate, and the remote's "77" request with the
heartbeat flag that shows "77" on the panel. It leaves out the rest of the stock module's cloud
pairing (smart-config itself and the binding token). The code is an ESPHome-style port of the
project's first bus driver, which ran in the retired Matter firmwares and is kept, frozen, as a
test reference in `firmware/test/reference-driver/`.

`firmware/test/test_esphome_codec_parity.cpp` runs every builder and parser of both over the same
inputs (the full command cartesian product, exhaustive small domains, and seeded random frames)
and compares them byte for byte and field by field. It runs in `run_tests.sh`, so the lint gate
and CI fail on any divergence. The reference is the hardware-validated side of that comparison.
A protocol change that the reference also covers is made in both, so the test keeps its meaning.

The port is the only transport: `packages/hisense-ac.yaml` has a `uart:` block and the bus is
scheduled in `loop()` (`hisense_bus.*`). It passed the hardware-in-loop actuation test on a live A/C (48/49 on
2026-09-25; the one miss, heat_cool readback, is an intermittent A/C-side flake). The earlier
fallback that ran the first driver's own FreeRTOS bus task was removed: it pulled in files from
outside the component, which upstream ESPHome cannot accept.

### Portability

The component uses nothing outside ESPHome's own API: `uart::UARTDevice` for the bytes, a
`GPIOPin` for DE, `millis()` for time, ESPHome's logging macros and ESPHome's preferences for the
beeper switch. It calls neither ESP-IDF nor FreeRTOS, and its format strings do not assume an
integer width. It compiles without a warning of its own for ESP32, ESP32-C3, ESP8266, RP2040 and
AmebaZ2.

Two things differ between platforms and are covered by host tests in
`firmware/test/test_esphome_bus.cpp`.

The first is a UART write that blocks. ESP-IDF queues the bytes and returns. LibreTiny waits on
the transmit FIFO, so a 50 byte command frame holds the loop for up to about 52 ms, the frame's
time on the wire. The DE release is counted from before the write, so it falls 25 ms after the
last byte either way.

The second is a coarse loop. The scheduler stays correct when it is polled only every 16 ms. DE
is then released up to one loop period late. The component asks ESPHome for a tight loop while DE
is high to keep that delay short. How short it is on a platform other than ESP32 has to be
measured there.

Every deadline is an elapsed-time or signed-difference comparison on a 32 bit `millis()`, tested
through the 49.7 day wrap.

## Troubleshooting

### An update uploads, then the node is back on the old firmware

The log of the old firmware shows `OTA rollback detected` and `Last reset was due to brownout`.
The board's 3.3 V supply dips when Wi-Fi starts at full power, so the new image resets on its
first boot and the bootloader rolls back. The node can look healthy all the same: ESP-IDF lowers
the transmit power on the boot that follows a brownout, so the old image comes up fine.

Check the supply and its wiring first. If the board has to run as it is, cap the transmit power
from boot in that node's YAML:

```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_ESP_PHY_MAX_WIFI_TX_POWER: "10"

wifi:
  output_power: 8.5dB
```

After any update, confirm that the `compiled on` line in the log shows the new build time.

