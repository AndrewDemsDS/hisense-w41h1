# ESPHome variant

Local, Matter-free control of a Hisense A/C over the reverse-engineered RS-485 bus, exposed to
Home Assistant through ESPHome's native API. Same ESP32 hardware and wiring as the esp-matter
build, none of the Matter stack. **Home Assistant only.**

One component and one set of entities for every board. The ESP32 and ESP32-C3 builds run on live
units. A build for the stock module's own MCU compiles and has not been on hardware yet
([Boards](#boards)).

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
```

Or `firmware/scripts/dev.py flash esphome --board c3 --port <port>` from the repo root. That
passes the C3 board and pins to `w41h1.yaml` as `-s` overrides, which still works and produces the
same node as `w41h1-esp32c3.yaml`.

## Boards

| File | Board | TX / RX / DE | Status | Image (ESPHome 2026.7.4) |
|---|---|---|---|---|
| `w41h1.yaml` | classic ESP32 (`esp32dev`), board and pins overridable with `-s` | 19 / 18 / 4 | runs on a live A/C | 923 KB, 50 % of the slot |
| `w41h1-esp32c3.yaml` | ESP32-C3 SuperMini | 5 / 6 / 10 | runs on a live A/C | 986 KB, 54 % |
| `w41h1-esp32c3-plus.yaml` | ESP32-C3 SuperMini Plus (RGB pixel on GPIO8, see [Status LED](#status-led)) | 5 / 6 / 10 | compiles; same node as the row above plus the pixel, which has not been checked on a board | 1017 KB, 55 % |
| `w41h1-amebaz2-factory.yaml`, `-sdk.yaml`, `-native.yaml` | the stock W41H1 module itself (RTL8710C through LibreTiny), one file per flash layout | PA14 / PA13 / PA17 | **factory layout runs on one module in an A/C (2026-10-10)**; sdk and native compile, never run | 605 KB, 40 % of the factory layout's slot, 35 % of the other two |
| `w41h1-esp8266.yaml` | ESP8266 D1 mini | GPIO15 / GPIO13 / GPIO5 | example, compiles, never run | 506 KB, 48 % |
| `w41h1-rp2040.yaml` | Raspberry Pi Pico W | GPIO4 / GPIO5 / GPIO6 | example, compiles, never run | 590 KB, 56 % |

Every board file is three things: the node's name, the pins, and the platform block. Everything
else comes from two packages that all of them include, so the entity names and object ids are the
same on every chip:

- `packages/node.yaml`: logger, API, Wi-Fi, the setup hotspot and
  [setup mode](#wi-fi-setup-mode-and-the-77-gesture), and three diagnostics every
  board has (`Uptime`, `WiFi signal`, `Reset reason`). Wi-Fi power saving is
  off (`power_save_mode: none`): on an ESP32-C3 with a weak signal ESPHome's default, `light`,
  lost 65 % of pings and commands did not arrive. A board on a weak supply can go back with the
  `wifi_power_save` substitution (`-s wifi_power_save light`).
- `packages/hisense-ac.yaml`: the UART, the `hisense_ac` hub and every entity.
- `packages/ota.yaml`: ESPHome's update server. The AmebaZ2 files take theirs from
  `packages/amebaz2.yaml` instead, with an id, so `w41h1_module` can switch it off.
- `packages/status-led.yaml` and `packages/status-pixel.yaml`: optional, for a board with a plain
  LED or an addressable RGB pixel on a GPIO ([Status LED](#status-led)). Only the two ESP32-C3
  files include one.

To add a board, copy the nearest file, change the platform block and the three pins, and add what
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
A unit that runs this project's Matter firmware is converted over the air:
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
- No long run yet, and the way back to the Matter firmware has not been tried.

`dev.py test esphome --board amebaz2-factory` and `dev.py build esphome --board amebaz2-factory`
(or `amebaz2-sdk`, `amebaz2-native`) validate and compile one. `dev.py convert esphome --board
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

## Recovery and indication

### Wi-Fi setup mode and the "77" gesture

On the remote, press horizontal airflow six times. The panel shows "77" and the node opens its
hotspot, `<name>-setup`, with the password from `wifi_ap_password` in `secrets.yaml`. Join it,
open `http://192.168.4.1` and enter the new network. The same thing is a button in Home
Assistant, `Start Wi-Fi setup`.

Setup mode ends in a restart, on whichever comes first:

- the network entered in the portal has connected,
- the gesture is repeated (at least 10 s after the first),
- `wifi_setup_timeout` has passed (10 minutes, a substitution in `packages/node.yaml`).

A gesture made by mistake costs that much: the node is off the network until one of those, then
back on the network it had. The A/C keeps running and keeps answering the remote throughout.
Nothing is written to flash on the way in, so a power cut in setup mode also comes back on the old
network. If a network was entered and did not connect, the credentials the node had before are
written back before the restart.

What to know before relying on it:

- **It has not run on hardware.** The codec and the scheduler are host-tested
  (`firmware/test/test_esphome_setup_request.cpp`) and the images compile. Whether the hotspot
  opens on each chip, and what a given unit sends for the gesture, has to be seen on a unit.
- The hotspot and the home network cannot be up together: ESPHome closes its hotspot as soon as
  the station connects. So the node leaves the network for the duration.
- ESPHome opens its hotspot once per boot. If the fallback already opened and closed earlier in
  the same boot (the router was away for a minute), setup mode cannot open it again: the node
  logs that, restarts after 30 s, and the next gesture works.
- A network saved through the portal is stored under a key that ESPHome derives from the build's
  configuration. An update built from a changed configuration goes back to the Wi-Fi in
  `secrets.yaml`. Put the new network there as well.
- Only the SSID and password are put back after a failed attempt. A node configured with a
  static IP, an enterprise login or several networks needs its own handling.
- With `fast_connect: true` the node reconnects before the hotspot is up and setup mode ends at
  once. The shipped configuration does not set it.
- Home Assistant's API has its own 15 minute restart when nothing is connected to it. Keep
  `wifi_setup_timeout` below that.

On the bus, the gesture is a bit in byte 17 of the unit's reply to the `0x1E` heartbeat. The bench
unit sets `0x20` for one reply per gesture, and that is taken at once. The vendor firmware also
tests `0x08`, which no unit here has been seen to send: it has to hold for three replies, is
ignored while the node itself shows "77" (the node sends the same bit to light it), and is
ignored when it is already set at boot. A reply with a bad checksum never counts. With the log at
`DEBUG`, every change of those bits is logged, taken or not.

For other uses, the hub has the pieces on their own:

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
| `packages/status-led.yaml` | a plain LED on a GPIO (`status_led_pin`, `status_led_inverted`) | slow blink: no Wi-Fi, no Home Assistant connection, or the A/C bus link down. Fast blink: the setup hotspot is open, or ESPHome's safe mode |
| `packages/status-pixel.yaml` | one WS2812 RGB pixel, ESP32 only (`status_pixel_pin`) | blue: the setup hotspot is open. Red: no Wi-Fi for 15 s. Amber: Wi-Fi up, the A/C bus link down for 15 s. Dim, and not an entity in Home Assistant |

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
| `packages/` | what the board files include: `node.yaml`, `hisense-ac.yaml`, `ota.yaml`, the optional `status-led.yaml` and `status-pixel.yaml`, and `amebaz2.yaml` for the stock module (configuration only) |
| `components/hisense_ac/` | the custom component: hub, climate, switches, select, sensors |
| `components/w41h1_module/` | the stock module's own component, `rtl87xx` only: its flash layouts (`layout-*.json`), the layout guard (`w41h1_slots.h`), the reboot workaround, and the serial port recovery (`w41h1_module.cpp`, with its timing rule in `w41h1_uart_policy.h`) |
| `tests/components/hisense_ac/` | every option of every platform, at the path and in the shape ESPHome's own `tests/components/<name>/` uses, so it moves upstream unchanged |
| `tests/components/w41h1_module/` | every option of `w41h1_module`, in the same shape, for the one platform it validates on |
| `tests/test_build_components/` | stand-ins for ESPHome's shared UART test packages, so the relative include in the test resolves here too |
| `tests/build.*.yaml` | repo-only harness so `esphome config` and `esphome compile` can run those tests (CI runs `config`). `build.rtl87xx-ard.yaml` builds the `w41h1_module` test and, with it, every option of `hisense_ac` on AmebaZ2, for which `hisense_ac` has no upstream-shaped test |
| `secrets.yaml.example` | template for the gitignored `secrets.yaml` |

Every file in the component passes ESPHome's own gates as of their `dev` branch in September 2026:
`script/ci-custom.py`, their `.clang-format` and `.clang-tidy`, ruff format, and pylint.

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

The shared driver under `firmware/src/rs485-driver/` cannot pass those gates and is not meant to:
it also has to build on AmebaZ2. Upstream ESPHome needs a component that carries its own protocol
code, so the component holds an ESPHome-style port of the codec: frame builders and parsers,
the enum and fan-ladder mapping, presets, the power estimate, and the remote's "77" request with
the heartbeat flag that shows "77" on the panel. It leaves out the rest of the stock module's
cloud pairing (smart-config itself and the binding token).

A second copy is only safe because it cannot drift. `firmware/test/test_esphome_codec_parity.cpp`
runs every builder and parser of both over the same inputs (the full command cartesian product,
exhaustive small domains, and seeded random frames) and compares them byte for byte and field by
field. It runs in `run_tests.sh`, so the lint gate and CI fail on any divergence. A protocol fix
lands in the shared driver first, then gets ported here until the parity test passes again.

The port is the only transport: `packages/hisense-ac.yaml` has a `uart:` block and the bus is
scheduled in `loop()` (`hisense_bus.*`). It passed the hardware-in-loop actuation test on a live A/C (48/49 on
2026-09-25; the one miss, heat_cool readback, is an intermittent A/C-side flake). The earlier
fallback that ran the shared driver's own FreeRTOS bus task was removed: it pulled in files from
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

