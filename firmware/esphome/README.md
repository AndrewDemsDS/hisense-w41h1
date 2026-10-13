# ESPHome firmware: component and board file reference

Local control of a Hisense A/C over its RS-485 module port, exposed to Home Assistant through
ESPHome's native API. This page is the reference for what is in this directory: the three
components and every option they take, the entities, the board files and packages, releases,
Wi-Fi setup mode, the status LED and troubleshooting.

It works with Home Assistant only. The Matter firmwares this project used to ship were retired
on 2026-10-12 (tag `matter-final`,
[`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md)).

If you want a working unit and not a reference, start in the guide:

- the tools and the four secrets, from a clean machine: [`docs/guide/ESP32-Build-Environment.md`](../../docs/guide/ESP32-Build-Environment.md)
- parts, wiring and the staged bring-up of an ESP32 board: [`docs/guide/ESP32-Replacement-Build.md`](../../docs/guide/ESP32-Replacement-Build.md)
- the stock module, without a replacement board: [`docs/guide/Stock-Module-Build.md`](../../docs/guide/Stock-Module-Build.md)
- board files, adoption in Home Assistant, updates: [`docs/guide/ESPHome-Build.md`](../../docs/guide/ESPHome-Build.md)
- what each entity means: [`docs/guide/Entities-and-Diagnostics.md`](../../docs/guide/Entities-and-Diagnostics.md)
- setup mode and the status LED in short: [`docs/guide/Wifi-Setup-Mode.md`](../../docs/guide/Wifi-Setup-Mode.md)

For the code: [`firmware/docs/01-component-architecture.md`](../docs/01-component-architecture.md).
For the stock module's measurements, flash layouts and open risks:
[`firmware/docs/15-esphome-path.md`](../docs/15-esphome-path.md).

## Quick start

```bash
cd firmware/esphome
cp secrets.yaml.example secrets.yaml      # Wi-Fi, hotspot password, API key. The real file is gitignored
esphome run w41h1-esp32c3.yaml            # ESP32-C3 SuperMini: build, flash, follow the log
esphome run w41h1-esp32c3-plus.yaml       # ESP32-C3 SuperMini Plus (RGB pixel on GPIO8)
esphome run w41h1.yaml                    # classic ESP32
```

Or, from the repository root, `python3 firmware/scripts/dev.py flash --board c3 --port <port>`
(`c3-plus`, `classic`), which builds the same file ([`firmware/scripts/README.md`](../scripts/README.md)).

## The three components

| Component | Use it when | Status on hardware |
|---|---|---|
| [`hisense_ac`](#hisense_ac) | always. It is the bus, the hub and every entity platform | runs on live units on ESP32, ESP32-C3 and the stock module |
| [`w41h1_module`](#w41h1_module) | the node is the stock AEH-W41H1 module itself (RTL8710C through LibreTiny). It carries the flash layout, the reboot workaround and the serial port restart | the factory layout runs on one module since 2026-10-10. The sdk and native layouts have not run |
| [`wifi_setup_mode`](#wifi_setup_mode) | you want the node's hotspot and captive portal on demand, from the remote or a button, while the home network is still there | **not run on hardware** |

The board files in this directory use all of them through the packages, so most nodes never name
a component directly. A configuration of your own can take only `hisense_ac`.

## Boards

| File | Board | TX / RX / DE | Status | Image |
|---|---|---|---|---|
| `w41h1.yaml` | classic ESP32 (`esp32dev`). Board and pins can be overridden with `-s` | 19 / 18 / 4 | runs on a live A/C | 929 KB, 51 % of the slot |
| `w41h1-esp32c3.yaml` | ESP32-C3 SuperMini | 5 / 6 / 10 | runs on a live A/C | 987 KB, 54 % |
| `w41h1-esp32c3-plus.yaml` | ESP32-C3 SuperMini Plus (RGB pixel on GPIO8) | 5 / 6 / 10 | compiles. The same node as the row above plus the pixel, which has not been checked on a board | 1018 KB, 55 % |
| `w41h1-amebaz2-factory.yaml`, `-sdk.yaml`, `-native.yaml` | the stock W41H1 module (RTL8710C through LibreTiny), one file per flash layout | PA14 / PA13 / PA17 | factory runs on one module in an A/C (2026-10-10). sdk and native compile and have never run | 610 KB: 40 % of the factory layout's slot, 35 % of the other two |
| `w41h1-esp8266.yaml` | ESP8266 D1 mini | GPIO15 / GPIO13 / GPIO5 | example, compiles, never run | 513 KB, 49 % |
| `w41h1-rp2040.yaml` | Raspberry Pi Pico W | GPIO4 / GPIO5 / GPIO6 | example, compiles, never run | 596 KB, 57 % |

The image sizes were taken on 2026-10-12 with ESPHome 2026.7.4, before the component's refactor
of 2026-10-13. They have not been measured again and will move with ESPHome releases.

A board file says three things: which chip, which pins, and which extras (a status LED, the
stock module's flash layout). Everything else comes from the packages, so the entity names and
object ids are the same on every chip and a unit can move between boards without Home Assistant
noticing. `firmware/test/test_esphome_config_matrix.py` checks that.

### Packages

| Package | Holds | Substitutions |
|---|---|---|
| `packages/version.yaml` | the firmware version, as ESPHome's `project:` block. Included by `node.yaml` | none |
| `packages/node.yaml` | the node: name, logger, API, Wi-Fi, the setup hotspot, the `wifi_setup_mode` component with its script and button, and three diagnostics (`Uptime`, `WiFi signal`, `Reset reason`) | `name` (`hisense-ac`), `friendly_name` (`Air Conditioner`), `wifi_power_save` (`none`), `wifi_setup_timeout` (`10min`) |
| `packages/hisense-ac.yaml` | the UART, the `hisense_ac` hub and every entity | needs `tx_pin`, `rx_pin`, `de_pin`. May override `w41h1_components` |
| `packages/esp32.yaml` | the `esp32:` block on ESP-IDF | needs `board` |
| `packages/ota.yaml` | ESPHome's update server | none |
| `packages/amebaz2.yaml` | for the stock module, in place of `ota.yaml`: the `rtl87xx:` block, the module's three pins, the update server with an id, and `w41h1_module` | needs `layout` |
| `packages/status-led.yaml` | optional: a plain LED on a GPIO | needs `status_led_pin`. `status_led_inverted` (`false`) |
| `packages/status-pixel.yaml` | optional: one WS2812 pixel, ESP32 only | needs `status_pixel_pin`. `status_pixel_order` (`GRB`), `status_pixel_strapping_ok` (`false`) |

`node.yaml` reads four secrets: `wifi_ssid`, `wifi_password`, `wifi_ap_password` and
`hisense_ac__encryption_key`. It expects `hisense-ac.yaml` beside it, because setup mode reaches
the panel through the hub.

Wi-Fi power saving is off (`power_save_mode: none`). The measurement behind that is in
[`firmware/docs/16-esp32-board-notes.md`](../docs/16-esp32-board-notes.md#wi-fi-power-saving). A
board on a supply too weak for it can go back with `-s wifi_power_save light`.

### Adding a board, and a second node

To add a board, copy the nearest file and change the platform (the `board` substitution for an
ESP32, the platform block otherwise) and the three pins. Add what the platform needs on top: the
ESP8266 file moves the logger off the bus UART, the stock module's files take `amebaz2.yaml`.
The board files sit beside `components/` and not in a directory of their own because ESPHome
resolves the local component path and `secrets.yaml` against the directory of the file it is
given.

Two nodes on one network need two names. Set `name` and `friendly_name` in the board file, or
pass `-s name bedroom-ac -s friendly_name "Bedroom A/C"`. Board files build into
`.esphome/build/<name>/`, so two files with the same `name` share a build directory. Give a
second name when you switch between boards on one machine.

### The stock module (AmebaZ2)

The `w41h1-amebaz2-*.yaml` files build ESPHome for the Realtek RTL8710C inside the AEH-W41H1, so
the unit needs no replacement board. There are three files because a module has one of three
flash layouts and the image has to be built for the one it has. **An image built for another
layout bricks the unit at its first update.** There is no default.

| File | For a module that |
|---|---|
| `w41h1-amebaz2-factory.yaml` | went from the vendor firmware to this project's firmware over the air (factory partition table) |
| `w41h1-amebaz2-sdk.yaml` | was first written with this project's retired Matter clip image (Realtek SDK partition table) |
| `w41h1-amebaz2-native.yaml` | was written from scratch with LibreTiny's own partition table and bootloader |

How to tell which one a unit has, what was measured on a module and what is still open:
[`firmware/docs/15-esphome-path.md`](../docs/15-esphome-path.md#which-flash-layout-a-unit-has).
Read that page before you flash a module. A unit that still runs the retired Matter firmware is
converted over the air:
[`docs/guide/Matter-Firmware-Retired.md`](../../docs/guide/Matter-Firmware-Retired.md#a-stock-module).

`dev.py test --board amebaz2-factory` and `dev.py build --board amebaz2-factory` (or
`amebaz2-sdk`, `amebaz2-native`) validate and compile one. `dev.py` refuses a bare
`--board amebaz2` and refuses to flash any of them over a serial port.

### Build cache

`dev.py` sets `PLATFORMIO_BUILD_CACHE_DIR` (default `~/.cache/w41h1-dev/pio-build-cache`) for
every build, and PlatformIO then keeps each compiled object by content. Export the same variable
to get it when you call `esphome` directly. It pays off on the boards PlatformIO compiles, which
here are the stock module, the ESP8266 and the RP2040. Measured on the stock module's image: 29 s
cold, 18 s into a new build directory (a second node name, a new checkout), 6 s after a clean
with nothing changed, for 77 MB of cache per flash layout. The ESP32 boards gain nothing:
ESPHome builds those with ESP-IDF's own tools, which use ccache when it is installed.

An object served from the cache prints no compiler warnings, so a warning shows once, on the
build that compiled the file. When nothing at all changed, the cache hands back the linked
firmware whole, and LibreTiny's slot images (`image_firmware_is.*.bin`), which it writes while
linking, are not produced. `firmware.uf2`, the file an update uses, is. `dev.py build` notices
and builds once more without the cache. PlatformIO never prunes the directory. Delete it when it
has grown.

## `hisense_ac`

The hub is one block and each entity type is a platform of the same name. It needs a
[`uart:`](https://esphome.io/components/uart/) at 9600 baud.

```yaml
uart:
  id: uart_bus
  tx_pin: GPIO5
  rx_pin: GPIO6
  baud_rate: 9600

hisense_ac:
  id: ac_bus
  uart_id: uart_bus
  de_pin: GPIO10

climate:
  - platform: hisense_ac
    name: None
    hisense_ac_id: ac_bus
```

### Hub options

| Option | Type | Default | Notes |
|---|---|---|---|
| `id` | ID | generated | |
| `uart_id` | ID | the only `uart:` | the bus the transceiver is on |
| `de_pin` | output pin | none | the transceiver's DE (and RE) line. The hub raises it 5 ms before a frame and lowers it 25 ms after the last byte |
| `on_setup_request` | automation | none | [the trigger](#the-trigger-and-the-action) |

Validation:

- `hisense_ac:` can be listed once. A board wired to two indoor units is not supported, and a
  second entry is refused with a message that says so.
- The UART has to be 9600 baud, 8 data bits, no parity, 1 stop bit, with both `tx_pin` and
  `rx_pin`. Anything else is refused.
- `de_pin` has to be an output. `mode: INPUT` (or a mode with no `output`) is refused: the hub
  would write to an input and never transmit.
- DE has one driver. `de_pin` together with the UART's `flow_control_pin` is refused. With
  neither, validation passes with a warning: that is right only for a transceiver that switches
  direction by itself, and with any other the node never transmits and the bus link stays down.
  `de_pin` has the timing that was validated on hardware. `flow_control_pin` has not run on a
  unit.

### `climate`

| Option | Type | Default | Notes |
|---|---|---|---|
| `hisense_ac_id` | ID | the only hub | |
| `supports_heat` | boolean | `true` | `false` on a cooling-only unit |
| `supports_horizontal_swing` | boolean | `false` | `true` if the unit has a left/right louvre motor |
| `supports_eco` | boolean | `true` | `false` removes every preset that needs eco |
| `supports_quiet` | boolean | `true` | the same for quiet |
| `supports_turbo` | boolean | `true` | the same for turbo |
| `supports_sleep` | boolean | `true` | the same for the sleep presets |
| `min_temperature` | temperature | `16 °C` | lowest setpoint shown |
| `max_temperature` | temperature | `32 °C` | highest setpoint shown |

Plus every option of ESPHome's [climate](https://esphome.io/components/climate/) schema.

Validation:

- One climate entity per hub. The hub hands each status frame to one climate entity, so a second
  one would never update, and it is refused.
- `min_temperature` may not be above `max_temperature`.
- The range has to lie within what the unit takes: 16 to 32 °C, or 61 to 90 °F (90 °F is
  32.2 °C, which is accepted). A wider range is refused, because it would offer values in Home
  Assistant that go out as the nearest one the unit takes.

The entity offers the modes off, auto, cool, heat (unless `supports_heat: false`), dry and fan
only. Fan modes are `auto`, `low`, `medium_low`, `medium`, `medium_high` and `high`. Swing is
off or vertical, and horizontal and both with `supports_horizontal_swing`. Presets are `none`,
`eco`, `quiet`, `turbo`, `eco_quiet`, `sleep_general`, `sleep_old`, `sleep_young`, `sleep_kids`
and each sleep profile with eco (`eco_sleep_general` and so on). Eco and turbo cannot be on
together. Special-mode commands are sent about 10 s apart, so a preset that changes several
modes can take half a minute to settle, and the entity shows the requested preset meanwhile.

Capability gating is a YAML decision. Leave out what the unit lacks. The `capability_*` binary
sensors report what the unit answers to the ProductType poll.

### `sensor`

All optional. Each takes every option of ESPHome's sensor schema.

| Option | Unit | What |
|---|---|---|
| `indoor_temperature` | °C | room temperature at the indoor unit |
| `outdoor_temperature` | °C | outdoor temperature. Unknown while the unit sends its placeholder after a power cut |
| `coil_temperature` | °C | outdoor (condenser) coil. Unknown under the same condition |
| `compressor_frequency` | Hz | |
| `power` | W | estimated from the current value the unit reports. Not metered |
| `voltage` | V | supply voltage as the unit reports it |
| `current` | A | derived from the power estimate |
| `checksum_errors` | count | frames received with a bad checksum since boot |
| `reply_timeouts` | count | reply windows that closed empty, any frame |
| `unanswered_commands` | count | the command frames among those. Each send counts |
| `command_retries` | count | commands sent again, for a missing reply or a status that did not show the change |
| `failed_commands` | count | commands the unit never took, after the re-sends |
| `link_losses` | count | times the bus link was declared lost |

Values are published when they change and at least once a minute. The six counters are
diagnostic entities, count since boot and have state class `total_increasing`. For energy, add
ESPHome's `total_daily_energy` on the power sensor with `method: left`, as
`packages/hisense-ac.yaml` does.

### `binary_sensor`

All optional. Each takes every option of ESPHome's binary sensor schema.

| Option | Device class | What |
|---|---|---|
| `bus_link` | connectivity, diagnostic | on while the unit answers its status poll |
| `problem` | problem | on while the unit reports any fault |
| `aux_heat` | heat | on while the auxiliary heater relay is closed |
| `fault_*` | problem, diagnostic | one per fault bit, list below |
| `capability_*` | diagnostic | one per capability bit, list below |

The 18 fault options: `fault_indoor_temp`, `fault_indoor_coil_temp`, `fault_indoor_humidity`,
`fault_water_full`, `fault_indoor_fan_motor`, `fault_grille`, `fault_indoor_vzero`,
`fault_indoor_comms`, `fault_indoor_display`, `fault_indoor_keys`, `fault_indoor_wifi`,
`fault_indoor_ele`, `fault_indoor_eeprom`, `fault_outdoor_eeprom`, `fault_outdoor_coil_temp`,
`fault_outdoor_gas_temp`, `fault_outdoor_temp`, `fault_over_temp`.

The 13 capability options: `capability_cool_heat`, `capability_ai`, `capability_infinite_fan`,
`capability_power_save`, `capability_fan_mute`, `capability_swing_dir_8`,
`capability_swing_follow`, `capability_humidity`, `capability_heat_8c`, `capability_purify`,
`capability_q_display`, `capability_enable_8heat`, `capability_trans_102_64`. The unit's two-bit
fields (`power_display`, `demand_resp`) are not booleans and have no entity.

### `switch`

All optional. Each takes every option of ESPHome's switch schema, and has entity category
`config`.

| Option | What |
|---|---|
| `eco` | eco mode. Turns turbo off: the two share a command byte |
| `turbo` | turbo mode |
| `quiet` | quiet mode |
| `display` | the indoor unit's panel display. The unit does not report it, so the switch shows what was last commanded |
| `beeper` | whether the unit beeps when it takes a command from this node. Stored on the node, on by default (`restore_mode: RESTORE_DEFAULT_ON`). Turning it off sends nothing and applies from the next command. The remote still beeps |

Eco and turbo are refused before the first status frame has arrived.

### `select`

| Option | What |
|---|---|
| `sleep_profile` | `Off`, `General`, `Old`, `Young` or `Kids`. Takes every option of ESPHome's select schema |

### `text_sensor`

| Option | What |
|---|---|
| `link_token` | the two-byte device type the unit reports in the handshake, as `HH LL`. Diagnostic. The name is historical: it is a per-model identifier and not a session token |

### The trigger and the action

`on_setup_request` runs when the user presses horizontal airflow six times on the remote, the
gesture that starts pairing on the stock module. It fires once per gesture. `x` (`uint8_t`) holds
the request bits of the unit's `0x1E` reply: `0x20`, `0x08` or both. A request is ignored for
10 s after the previous one. A unit that raises `0x08` has to hold it for three replies in a
row, and that bit is ignored while the panel indicator is on.

`hisense_ac.set_setup_indicator` shows "77" on the unit's panel or takes it off. Nothing else
changes: the unit keeps running and keeps taking commands.

| Option | Type | Notes |
|---|---|---|
| `state` | boolean, templatable | required. `true` shows "77", `false` clears it |
| `id` | ID | the hub, when there is more than one |

```yaml
hisense_ac:
  id: ac_bus
  on_setup_request:
    - hisense_ac.set_setup_indicator: true     # short form
    - delay: 30s
    - hisense_ac.set_setup_indicator:
        id: ac_bus
        state: false
```

In a lambda: `id(ac_bus).set_setup_indicator(true)` and `id(ac_bus).setup_indicator()`.

## `w41h1_module`

Only for the stock module. `packages/amebaz2.yaml` configures it:

```yaml
w41h1_module:
  layout: factory          # required, no default: factory, sdk or native
  hisense_ac_id: ac_bus
  uart_id: uart_bus
  ota_id: ota_server
  serial_port_restarts:
    name: Serial port restarts
  flash_layout:
    name: Flash layout
```

| Option | Type | Default | Notes |
|---|---|---|---|
| `layout` | `factory`, `sdk` or `native` | required | the flash layout the module already has |
| `hisense_ac_id` | ID | the only hub | whose link state is watched |
| `uart_id` | ID | the only `uart:` | the port that is closed and reopened when the link is lost |
| `ota_id` | ID | the only `ota: platform: esphome` | the update server that is switched off on a layout mismatch |
| `serial_port_restarts` | sensor | none | how often the port restart has fired since boot. 0 on a healthy bus. It also counts while the unit does not answer at all, which no restart cures |
| `flash_layout` | text sensor | none | the running slot, the address the bootloader gives for the next update, and whether that agrees with the image. `MISMATCH` means: do not update this unit |
| `update_interval` | time | `60s` | how often the two entities are published |

Validation: only on `rtl87xx` with a board of LibreTiny's RTL8720C family. On any other chip
`esphome config` refuses it with a message that says so. It can be listed once. It needs
`hisense_ac`, `uart` and `ota` in the configuration.

What it does with those options: [`firmware/docs/15-esphome-path.md`](../docs/15-esphome-path.md#what-the-component-does).

## `wifi_setup_mode`

```yaml
wifi_setup_mode:
  id: wifi_setup
  timeout: 10min
  # hisense_ac_id: ac_bus      # found by itself when there is one hub

hisense_ac:
  on_setup_request:
    - wifi_setup_mode.toggle: wifi_setup

binary_sensor:
  - platform: template
    name: Wi-Fi setup
    condition:
      wifi_setup_mode.active: wifi_setup
```

| Option | Type | Default | Notes |
|---|---|---|---|
| `id` | ID | generated | |
| `hisense_ac_id` | ID | the only hub | the hub whose panel shows "77" |
| `timeout` | time | `10min` | how long the hotspot stays open before the node restarts onto the network it had. 30 s to 24 h |

| Action or condition | Does |
|---|---|
| `wifi_setup_mode.toggle` | starts setup mode, and ends it when it is on. For the gesture |
| `wifi_setup_mode.start` | starts setup mode, and does nothing when it is on. For a button |
| `wifi_setup_mode.stop` | ends it |
| `wifi_setup_mode.active` (condition) | true while "77" is on the panel |

Validation: it needs `wifi:`, `captive_portal:` and `hisense_ac:`. A `wifi:` block without `ap:`
is refused. Four Wi-Fi configurations validate with a warning, because the hotspot cannot be
opened on demand with them: `fast_connect`, a network marked `hidden: true`, an enterprise
login, or no network at all. See [the limits](#limits).

## Entities of the shipped node

What `packages/node.yaml` and `packages/hisense-ac.yaml` declare. What each one means, and how to
read the counters, is in
[`docs/guide/Entities-and-Diagnostics.md`](../../docs/guide/Entities-and-Diagnostics.md).

| Type | Names |
|---|---|
| climate | the device itself (`name: None`) |
| switch | Eco, Turbo, Quiet, Panel display, Beeper |
| select | Sleep profile |
| sensor | Outdoor temperature, Coil temperature, Compressor frequency, Power, Voltage, Current, Energy today |
| sensor, diagnostic | Bus checksum errors, Bus reply timeouts, Unanswered commands, Command retries, Failed commands, Bus link losses, Uptime, WiFi signal |
| binary sensor | Aux heat relay, Fault |
| binary sensor, diagnostic | AC bus link, Fault indoor temp sensor, Fault indoor to outdoor comms, Fault condensate tray full, Fault outdoor temp sensor, Capability heat pump, Capability eco, Capability quiet |
| text sensor, diagnostic | AC device type, Reset reason |
| button, diagnostic | Start Wi-Fi setup |
| stock module only | Serial port restarts, Flash layout |

The package declares four of the 18 fault bits (the ones that have fired on a bench unit) and
three of the 13 capability bits. Add the others the same way. The indoor temperature is the
climate entity's current temperature, so the package declares no separate sensor for it.

## Version and releases

The firmware version is written in one file, `packages/version.yaml`, as ESPHome's `project:`
block. It is semver and is 2.0.0 now (the retired Matter firmwares were 1.x). `node.yaml`
includes that file, so a node built from the packages carries the version:

- Home Assistant shows it as the device's firmware, for example `2.0.0 (ESPHome 2026.7.4)`.
- The ESPHome dashboard shows it beside the node, and the node announces it over mDNS.
- It is in the image as `ESPHOME_PROJECT_VERSION`.

The version is not part of any entity's name, object id or unique id, so a new version does not
move an entity. With a project set, Home Assistant takes the device's manufacturer and model
from the project name (`AndrewDemsDS` and `hisense-w41h1`). Between two releases the tree keeps
the number of the last one. The `compiled on` line in the log tells two builds of one version
apart.

A release is the git tag `vX.Y.Z` on `main`, with a section in
[`CHANGELOG.md`](../../CHANGELOG.md) and a GitHub release that carries that section. A release
has no firmware images attached: an image holds its builder's Wi-Fi credentials, API key and
hotspot password, so everyone builds their own. How a release is cut is in
[`CONTRIBUTING.md`](../../CONTRIBUTING.md#releasing).

**No release has been made yet.** As of 2026-10-13 the tag `v2.0.0` does not exist: 2.0.0 is the
number the tree carries, and its changes are under `## Unreleased` in the changelog. The release
workflow has run once, on 2026-10-12, as a dry run from a branch (every compile passed, nothing
was tagged or published). The examples below use `v2.0.0` as the tag they will have. Until it
exists, put a branch or a commit in `ref:`.

### Using a release from your own configuration

A clone checked out at the tag is the simplest way: `git clone --branch v2.0.0`, then the
[Quick start](#quick-start) lines. A node file kept somewhere else (in the ESPHome Device
Builder, for example) can take the release from git instead, in two ways.

**The components only.** Your YAML defines the UART, the hub and the entities itself, with the
options above:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/AndrewDemsDS/hisense-w41h1
      ref: v2.0.0
      path: firmware/esphome/components
    components: [hisense_ac]      # add w41h1_module on the stock module, wifi_setup_mode if used
```

A node built this way has no firmware version unless its YAML sets `esphome: project:` itself.

**The packages too.** This gives the node of the board files: every entity, setup mode and the
version. It is how a node file outside this repository is put together. The packages are ESPHome
remote packages, and one substitution points them at the components of the same tag:

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
come from the `secrets.yaml` beside your own file. For the status LED add
`packages/status-led.yaml` or `packages/status-pixel.yaml` to `files:` and its substitutions to
yours. For the stock module take `packages/amebaz2.yaml` in place of `ota.yaml`, set `layout`,
and leave the pins out (the package sets them).

Without the `w41h1_components` substitution the packages look for a `components/` directory
beside the file given to `esphome`, which is how the board files in this repository find theirs.

## Wi-Fi setup mode and the "77" gesture

> **Setup mode has not run on a unit.** The decisions are host-tested
> (`firmware/test/test_esphome_setup_mode.cpp` for the mode,
> `firmware/test/test_esphome_setup_request.cpp` and `test_esphome_setup_edges.cpp` for the
> gesture on the bus) and the images compile. Whether the hotspot opens on each chip, and what a
> given unit sends for the gesture, has to be seen on a unit. The
> [check list](#the-check-on-a-unit) is below.

On the remote, press horizontal airflow six times. The panel shows "77" and the node opens its
hotspot, `<name>-setup`, with the password from `wifi_ap_password` in `secrets.yaml`. Join it,
open `http://192.168.4.1` and enter the new network. The same thing is a button in Home
Assistant, `Start Wi-Fi setup`.

Setup mode ends in a restart, on whichever comes first:

- the network entered in the portal has connected,
- the gesture is repeated (at least 10 s after the first),
- the timeout has passed (10 minutes, the `wifi_setup_timeout` substitution). A network entered
  shortly before that still gets 90 s to connect.

A gesture made by mistake costs that much: the node is off the network until one of those, then
back on the network it had. The A/C keeps running and keeps answering the remote throughout.

### What happens to the Wi-Fi credentials

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

When the hotspot is already open because the network is out of reach (ESPHome opens it by itself
after a minute), the gesture or the button only lights "77". The node keeps trying its network
and is not restarted: "77" goes off when a network connects, on a second gesture, or after the
timeout. A network entered in the portal that has not connected by then is replaced by the one
the node had, as above.

### Limits

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
- With `fast_connect: true`, a network marked `hidden: true`, an enterprise login, or no network
  in the build at all, the hotspot cannot be opened on demand. The first two reconnect before
  the hotspot is up, an enterprise login cannot be written back, and without a network the
  hotspot is open anyway. `esphome config` says so with a warning, and on the node the gesture
  and the button log why and change nothing. The fallback hotspot still opens when the network
  is gone, and the gesture lights "77" on it. The shipped configuration sets none of these.
- A unit that asks with the sustained bit (`0x08`) cannot end setup mode with the gesture. While
  "77" is on, the node sends that same bit, so the unit's own is not read. Such a unit leaves by
  the timeout or by a network connecting. The bench unit sends `0x20` and is not affected. No
  unit here has been seen to send `0x08`.
- An upload blocks the node's loop, so a gesture made during an update is lost.

### The check on a unit

With the remote and a phone. The log at `INFO` shows each step under the tag `wifi_setup_mode`.
A node with a serial console shows them while it is off the network.

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

With the log at `DEBUG`, every change of the request bits in the unit's `0x1E` reply is logged,
taken or not. Why the mode is built the way it is:
[`firmware/docs/01-component-architecture.md`](../docs/01-component-architecture.md#wifi_setup_mode).

## Status LED

Off while the node is healthy. Two optional packages, one per kind of LED. A board file that
includes neither has no status LED.

| Package | For | Shows |
|---|---|---|
| `packages/status-led.yaml` | a plain LED on a GPIO | slow blink (once in 1.5 s): no Wi-Fi, no Home Assistant connection, or the A/C bus link down. Fast blink (four a second): setup, or ESPHome's safe mode |
| `packages/status-pixel.yaml` | one WS2812 RGB pixel, ESP32 only | blue: setup. Red: no Wi-Fi for 15 s. Amber: Wi-Fi up, the A/C bus link down for 15 s. Blue wins over red, red over amber. Dim (8 % brightness), and not an entity in Home Assistant. A missing Home Assistant connection is not shown |

"Setup" is one state for both packages: setup mode from the request until the restart, whether
the hotspot has opened yet or failed to open, and the fallback hotspot open by itself. The rule
is the `wifi_setup_mode` component's `status_display()`, and both packages ask it. Through the
restart the plain LED is dark (the pin is not driven), and the pixel keeps its blue for about a
second until the new boot writes it black.

The two ESP32-C3 SuperMini boards differ exactly here, and both have the LED on GPIO8:

- **SuperMini** (`w41h1-esp32c3.yaml`): a small blue LED, lit when the pin is low.
- **SuperMini Plus** (`w41h1-esp32c3-plus.yaml`): an RGB pixel, a square white package, on a
  board that usually also has an external antenna connector. A pixel that nothing writes to can
  sit lit in a random colour. With the package it is written to black at boot.

GPIO8 is a strapping pin on the C3. Using it for the board's own LED is safe: the chip reads it
only at reset, before the firmware drives it. Wire nothing else to it. Never put both packages
on one pin.

Firmware cannot switch a power LED that is wired to the supply (the red one on some SuperMini
boards), nor the LEDs on an RS-485 transceiver board, which follow the bus lines. Those need
tape or a soldering iron. No LED is documented on the stock module
(`reverse-engineering/docs/01-hardware.md`), so its board files take neither package.

Not checked on hardware: the pixel's colour order (`GRB`, the WS2812B's, a substitution if a
board differs), that it is dark after boot, and how visible 8 % brightness is through the unit's
housing.

## Safe mode

ESPHome's safe mode is active on every board file without any configuration: the `ota:` platform
loads it. Ten boots in a row that do not stay up for a minute put the node into it. In safe mode
only the logger, Wi-Fi with its hotspot and portal, the update server and a plain status LED
run. The API, the A/C bus and every entity stay off, so a firmware that crashes in the A/C code
can still be replaced over the air. After five minutes without an update the node restarts and
tries the normal firmware again. The boot counter is kept in flash.

On ESP32 the bootloader also rolls a new image back by itself when it never reaches that first
good minute. On the stock module there is no such rollback: safe mode only helps when the image
gets as far as starting ESPHome, and an image that does not boot needs the clip. The stock
module's flash layout check does not run in safe mode, so a unit in safe mode still accepts an
update.

## Log levels

The hub logs under the tag `hisense_ac`. Set the level with ESPHome's `logger:` component. The
packages ship `INFO`.

| Level | What it shows |
|---|---|
| `WARN` | the bus link dropping, a command that could not be sent, got no reply, was sent again or was given up on, a frame with a bad checksum, a fault reported by the unit |
| `INFO` | the bus link coming back, a fault clearing, a command the unit took on a re-send, a setup request from the remote, the panel's "77" going on or off |
| `DEBUG` | each status change, every command sent and confirmed, the verdict on a command's reply, the unit's capability bitmap, every change in the request bits of the unit's `0x1E` reply |
| `VERBOSE` | every decoded status frame, every poll that got no reply |
| `VERY_VERBOSE` | the raw bytes of every frame sent and received |

Use `VERY_VERBOSE` when the link is down. It shows whether the A/C sends anything at all, and
you do not need a `debug:` block on the UART.

## Troubleshooting

### The bus link never comes up

`AC bus link` stays off and `Bus reply timeouts` climbs. Set the log to `VERY_VERBOSE`. If only
`TX` lines appear, nothing comes back from the unit. Check that the firmware's `de_pin` is the
pin the transceiver's DE and RE are wired to and that the transceiver is a 3.3 V part, then try
A and B the other way round
([`docs/guide/Hardware-and-Wiring.md`](../../docs/guide/Hardware-and-Wiring.md)). The reasons
behind the pin rules are in
[`firmware/docs/16-esp32-board-notes.md`](../docs/16-esp32-board-notes.md). If `esphome config`
warned that nothing drives DE, that is the cause.

### `esphome config` refuses the configuration

The messages name the option. The ones specific to these components are listed under
"Validation" in each section above. `tests/invalid/` holds one file per refusal, with the
message it is rejected with.

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

### A new ESP32 board does not join Wi-Fi

Erase a brand-new board once before the first flash (`dev.py erase --port <port>`). The reason is
in [`firmware/docs/16-esp32-board-notes.md`](../docs/16-esp32-board-notes.md#why-a-brand-new-board-must-be-erased-first).

### The stock module: `Flash layout` says `MISMATCH`, or `Serial port restarts` climbs

Both are explained in [`firmware/docs/15-esphome-path.md`](../docs/15-esphome-path.md). Do not
update a unit that reports `MISMATCH`.

## What is in this directory

| Path | What |
|---|---|
| `w41h1.yaml`, `w41h1-*.yaml` | the board files ([Boards](#boards)) |
| `packages/` | what the board files include ([Packages](#packages)) |
| `components/hisense_ac/` | the bus component: codec, scheduler, command tracker, hub, entities |
| `components/w41h1_module/` | the stock module's component, `rtl87xx` only |
| `components/wifi_setup_mode/` | Wi-Fi setup mode |
| `tests/components/<name>/` | every option of every platform of each component, at the path and in the shape ESPHome's own `tests/components/<name>/` uses, so the `hisense_ac` tests move upstream unchanged |
| `tests/test_build_components/` | stand-ins for ESPHome's shared UART test packages, so the relative include in those tests resolves here too |
| `tests/build.*.yaml` | harness files, so `esphome config` and `esphome compile` can run the component tests in this repository. `build.rtl87xx-ard.yaml` builds the `w41h1_module` test and with it every option of `hisense_ac` on the stock module's chip. `build.hub-only.esp32-c3-idf.yaml` is the hub with no entity platform. `build.setup-mode.*.yaml` build the `wifi_setup_mode` test per platform |
| `tests/invalid/` | one configuration per refusal, each with the message it has to be rejected with (`firmware/test/test_esphome_config_matrix.py` runs them) |
| `upstream/hisense_ac.mdx` | the draft of the component's page for ESPHome's documentation repository, which an upstream pull request has to be paired with. It follows esphome.io's format rules. Keep it in step with the option schemas |
| `secrets.yaml.example` | template for the gitignored `secrets.yaml` |

`hisense_ac` passes ESPHome's own gates on the release this repository builds with
(`firmware/scripts/esphome-upstream-check.sh`, a CI job on every change to the firmware). Nothing
has been submitted to ESPHome.
