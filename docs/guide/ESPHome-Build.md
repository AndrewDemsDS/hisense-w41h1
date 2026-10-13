# ESPHome Build

The firmware: an ESPHome component, `hisense_ac`, that speaks the A/C's RS-485 bus. The A/C
arrives in Home Assistant as a native `climate` device over the ESPHome API, with no pairing code
and no extra integration. It runs on an [ESP32 replacement board](ESP32-Replacement-Build), on the
stock module itself, and compiles for other ESPHome chips. Source and full rationale:
`firmware/esphome/README.md` and `firmware/docs/15-esphome-path.md`.

← back to [Home](Home) · siblings: [ESP32 Replacement Board](ESP32-Replacement-Build) ·
[Repo Map and Build Pipeline](Repo-Map-and-Build-Pipeline) · [Protocol Overview](Protocol-Overview)

---

## Home Assistant only

ESPHome talks to Home Assistant and to nothing else. Apple Home, Google Home and Alexa see the unit
when Home Assistant re-exposes the climate entity through its own Matter or HomeKit bridge. The
Matter firmwares this project used to ship are retired
([Matter Firmware (Retired)](Matter-Firmware-Retired)).

## Why ESPHome

The RS-485 protocol is the expensive part of this project, and it is finished, hardware-validated
and covered by host tests. Everything above it exists to move decoded values into Home Assistant.
ESPHome does that with a YAML file and one component, every fault bit is its own diagnostic
entity, and updates, logs and a Wi-Fi fallback come with the platform. The choice of board, with
measured figures, is `firmware/docs/13-path-comparison.md` in the repo.

## What you get

| Entity | Covers |
|---|---|
| `climate` | power, mode (auto/cool/heat/dry/fan_only), setpoint 16 to 32, fan (auto, low, medium_low, medium, medium_high, high), swing, current temperature, action, special-mode presets |
| `switch` | Eco, Turbo, Quiet, panel display, beeper |
| `select` | Sleep profile (Off / General / Old / Young / Kids) |
| `sensor` | indoor, outdoor and coil temperature, compressor Hz, power, voltage, current, bus checksum errors, uptime, Wi-Fi signal |
| `binary_sensor` | aux heat relay, bus link, aggregate fault, 18 per-bit faults, 13 capability flags |
| `text_sensor` | A/C device type (the learned link bytes), reset reason |
| `button` | Start Wi-Fi setup ([below](#wi-fi-setup-mode-and-the-status-led)) |

Quiet is a preset, not a fan speed: the A/C only reaches it through its mute flag, and while it is
on the fan reads `low`.

The climate presets are `none`, `eco`, `quiet`, `turbo`, `eco_quiet`, `sleep_general`,
`sleep_old`, `sleep_young`, `sleep_kids` and `eco_sleep_*` for each profile. Every unit exposes
the same names, so a climate group (for example Climate Group Helper) can sync presets across
units. A preset that needs several bus
writes spaces them 10 s apart, because the A/C ignores a special-mode command that lands too soon
after the previous one, so a combined preset such as `eco_sleep_old` takes about 10 s to settle.
While turbo, quiet or a sleep profile is active they own the fan, and a fan change is refused
rather than silently undone a second later. The switches and the select still work and share the
same pacing, so keep them for dashboards or delete them if the presets are all you use.

Plus what ESPHome gives for free: OTA, a captive-portal AP fallback, logs streamed over the API on
the deployed image, and `total_daily_energy` feeding the HA Energy dashboard. The device page in
Home Assistant shows the firmware version ([Updates](#updates)).

## Hardware

An ESP32 board and a **3.3 V** RS-485 transceiver on the A/C's 4-pin connector. Use the pin tables
and the GPIO warnings from [ESP32 Replacement Board](ESP32-Replacement-Build) and
[Hardware & Wiring](Hardware-and-Wiring), including the ground-loop rule for bench work and the warning against a 5 V MAX485
module. The shipped defaults are the validated classic-ESP32 set (TX 19, RX 18, DE 4); an ESP32-C3
SuperMini uses 5 / 6 / 10.

There is one file per board in `firmware/esphome/`, and each holds only the chip, the three pins
and what that board has on top (a status LED, the stock module's flash layout). The node's name
defaults to `hisense-ac`: set `name` and `friendly_name` in the file for a second node. The entities come from a shared package, so they are the same on
every board.

| File | Board | Status |
|---|---|---|
| `w41h1.yaml` | classic ESP32, TX 19 / RX 18 / DE 4 | runs on a live A/C |
| `w41h1-esp32c3.yaml` | ESP32-C3 SuperMini, 5 / 6 / 10 | runs on a live A/C |
| `w41h1-esp32c3-plus.yaml` | ESP32-C3 SuperMini Plus, 5 / 6 / 10 | compiles. The same node as the row above plus the RGB pixel, which has not been checked on a board |
| `w41h1-amebaz2-factory.yaml`, `-sdk.yaml`, `-native.yaml` | the stock W41H1 module itself, one file per flash layout | factory layout runs on one module in an A/C (2026-10-10); sdk and native compile only (see below) |
| `w41h1-esp8266.yaml`, `w41h1-rp2040.yaml` | ESP8266 D1 mini, Raspberry Pi Pico W | examples, compile only, never run |

TX and RX go to the `uart:` block and DE to the `hisense_ac:` component, all three through the
`tx_pin`, `rx_pin` and `de_pin` substitutions. The component drives DE itself with the
hardware-validated timing (5 ms settle, 25 ms drain).

**SuperMini or SuperMini Plus?** Look at the LED on GPIO8. The plain SuperMini has a small blue
LED there. The Plus has an addressable RGB pixel (a WS2812: a square white package), and usually
an external antenna connector as well. The two need different files, because a pin toggled as
for a plain LED means nothing to the pixel, and a pixel that nothing writes to can sit lit in a
random colour.

## Flash it

The guided way, from the repo root (details in [Build, Flash & Test](Build-Flash-Test#esphome)):

```bash
python3 firmware/scripts/dev.py fetch esphome                            # esphome==2026.7.4 + secrets.yaml
python3 firmware/scripts/dev.py erase esphome --port /dev/ttyACM0        # factory-fresh board only
python3 firmware/scripts/dev.py flash esphome --board c3 --port /dev/ttyACM0
```

By hand:

```bash
pipx install esphome==2026.7.4            # the pinned version; plain pip is refused on recent distro Pythons
cd firmware/esphome
cp secrets.yaml.example secrets.yaml      # then fill it in (below)
esphome run w41h1.yaml                    # classic ESP32: build, flash, follow the logs
esphome run w41h1-esp32c3.yaml            # C3 SuperMini
esphome run w41h1-esp32c3-plus.yaml       # C3 SuperMini Plus
esphome logs w41h1.yaml                   # logs only, later
```

`secrets.yaml` is gitignored and holds four values. The build stops if one is missing.

| Secret | What |
|---|---|
| `wifi_ssid`, `wifi_password` | the network the node joins |
| `wifi_ap_password` | the password of the node's own setup hotspot, at least 8 characters. Whoever joins that hotspot can move the node to another network, so do not keep the placeholder |
| `hisense_ac__encryption_key` | the API key Home Assistant adopts the node with. Make a new one, the example file says how |

The older form, `esphome -s board esp32-c3-devkitm-1 -s tx_pin 5 -s rx_pin 6 -s de_pin 10 run
w41h1.yaml`, still works. It builds the node of `w41h1-esp32c3.yaml` without the status LED, and
it is what `dev.py --board c3` ran until 2026-10-12. `dev.py --board c3` now builds
`w41h1-esp32c3.yaml` itself, and `--board c3-plus` builds `w41h1-esp32c3-plus.yaml`.

Home Assistant discovers the node over mDNS and adopts it with the API key from `secrets.yaml`
([below](#adopt-it-in-home-assistant)).

On a factory-fresh board, erase first: `dev.py erase esphome --port <port>`, then flash with
`dev.py flash esphome --board <c3|classic> --port <port>`, so the board overrides are applied.
By hand, that is `esptool.py erase_flash` followed by the matching `esphome run` line above with
`--device <port>`. A stale vendor Wi-Fi config left in NVS is a documented time sink on these
boards. Never erase a board that is already running: that throws away its Wi-Fi settings.

## Adopt it in Home Assistant

No pairing code and no extra integration.

1. **Get it on Wi-Fi.** The node joins the network in `firmware/esphome/secrets.yaml`. If it cannot
   (wrong password, different network), it opens a hotspot named **`hisense-ac-setup`** after about a
   minute. Join it from a phone with the `wifi_ap_password` from `secrets.yaml`; the captive
   portal asks for your Wi-Fi and the node reboots onto it.
2. **Add it in Home Assistant.** HA discovers the node over mDNS: Settings → Devices & Services shows
   a discovered **ESPHome** device. Select **Configure** and paste the API encryption key, the
   `hisense_ac__encryption_key` value from your `secrets.yaml`.
3. **Not discovered?** Add it by hand: Settings → Devices & Services → **Add Integration** →
   **ESPHome**, host `hisense-ac.local` or the node's IP address, port `6053`.

The device appears as **Air Conditioner** with the climate entity and every switch, sensor and fault
flag; see [Everyday Control](Everyday-Control). The API is plain TCP, so an A/C on a separate IoT
VLAN only needs HA allowed to reach port 6053 and the node added by IP.

To change the key later, edit `secrets.yaml`, re-flash (over the air is fine), then update the key in
HA when it reports the node as needing re-authentication.

## Updates

From `firmware/esphome/`, with the same board file or board overrides you first flashed with:

```
esphome run w41h1.yaml
esphome run w41h1-esp32c3.yaml
```

The first line is for the classic ESP32, the second for a C3 SuperMini. ESPHome builds the image,
offers the node over the network as the upload target (pick it, or pass
`--device hisense-ac.local` or the node's IP), uploads it and follows the logs. Build from the same
`secrets.yaml` as the first flash: a different API key locks Home Assistant out until you update
the key there. The reference YAML sets no OTA password, so
anything on the same network can push an image; add `password:` under `ota:` if the node shares a
network you do not control.

The firmware has a version, written once in `firmware/esphome/packages/version.yaml` and built
into every node. Home Assistant shows it on the device page as the firmware, for example
`2.0.0 (ESPHome 2026.7.4)`. There is nothing to bump for an update of your own node: the version
is that of the tree you build from. To move to a release, check out its tag (`git checkout
v2.0.0`) and run the line above. Releases are listed on the repository's releases page with what
changed, and they carry no firmware image: an image holds your Wi-Fi credentials and keys, so you
build your own. A configuration kept outside a clone can pin the components and the packages to
a tag from git (`firmware/esphome/README.md`, "Using a release from your own configuration").

If a node stops answering after an update, reflash it over USB
([Recovery & Reflash](Recovery-and-Reflash#esp32-boards)). The stock module is updated the same
way with its own board file, and has no USB to fall back on
([below](#the-stock-module-without-a-replacement-board)).

## Wi-Fi setup mode and the status LED

To move a node to another network without a cable, press **horizontal airflow six times** on the
remote. The panel shows "77" and the node opens its hotspot, `<name>-setup`. Join it with the
`wifi_ap_password` and enter the new network in the captive portal (if the phone does not open
it, browse to the hotspot's gateway address). The `Start Wi-Fi setup` button in Home Assistant
does the same for a unit whose remote is not at hand.

Setup mode ends in a restart, on whichever comes first: the new network has connected, the
gesture is repeated (at least 10 s after the first), or 10 minutes have passed. The node is off
the network for that time and the A/C keeps running and keeps answering the remote. A gesture
made by mistake costs nothing else: the node comes back on the network it had.

A network that was entered and has not connected when the mode ends is undone: the node goes
back to the one it had. That also holds when the node restarts for another reason in between,
and after a power cut, where the old network is put back at the next boot. A network entered in
the last minute still gets a minute and a half to connect. Put the new network in `secrets.yaml`
as well, because an update built from a changed configuration goes back to the credentials
compiled in.

If the hotspot is already open because the network is out of reach, the gesture only lights
"77": the node keeps trying its network and is not restarted.

What it cannot do: open the hotspot on demand on a node built with `fast_connect`, a hidden
network or an enterprise login (the build warns, the node logs it and stays on its network), and
end by gesture on a unit that asks with the sustained request bit, which no unit here has been
seen to do.

This has not run on hardware yet. The cases, the limits, the check to run on a unit and what
happens on the bus are in `firmware/esphome/README.md` ("Wi-Fi setup mode and the 77 gesture").

A board with an LED on a GPIO can show the node's state. The LED is dark while everything is
fine.

| Board file | LED | Shows |
|---|---|---|
| `w41h1-esp32c3.yaml` | the SuperMini's blue LED on GPIO8 (`packages/status-led.yaml`) | slow blink: no Wi-Fi, no Home Assistant connection, or the A/C not answering. Fast blink: setup mode, or the hotspot open by itself |
| `w41h1-esp32c3-plus.yaml` | the Plus board's RGB pixel on GPIO8 (`packages/status-pixel.yaml`) | blue: setup mode, or the hotspot open by itself. Red: no Wi-Fi for 15 s. Amber: Wi-Fi up, the A/C not answering for 15 s |

The other board files have no status LED. Either package can be added to a board file with the
pin as a substitution. A power LED wired to the supply, and the LEDs on an RS-485 transceiver
board, cannot be switched from firmware.

## Bring it up in stages

Use the three stages in
[ESP32 Replacement Board](ESP32-Replacement-Build#staged-bring-up-never-leave-the-ac-in-an-unknown-state),
with `dev.py test esphome` and `dev.py bench esphome` for stage 1. What passing looks like: in
stage 1 the `AC bus link` sensor turns on and the climate entity populates; in stage 2
the decoded indoor temperature, mode and compressor Hz match the unit.

`firmware/test/hil_esphome_actuation.py` drives a real node over the API and checks two things per
control: that the command lands, and that nothing else moved. It snapshots and restores the unit's
state, so it leaves a live A/C as it found it. See [Testing & QA](Testing-and-QA).

## Capability gating is a YAML decision

Units differ in what they support, and the A/C reports it. You delete the entities your unit does
not have. Flash with everything declared, read the `capability_*` binary sensors your A/C
answers with, then trim the YAML to match. For the presets, set `supports_eco`, `supports_quiet`,
`supports_turbo` or `supports_sleep` to `false` on the climate platform; every preset that needs
the missing mode disappears.

## The stock module, without a replacement board

The `w41h1-amebaz2-*.yaml` files build this same firmware for the chip inside the AEH-W41H1
itself, a Realtek RTL8710C, through ESPHome's LibreTiny platform. The module that came with the
A/C runs ESPHome and nothing is added to the unit. One module with the factory layout has run it
in an A/C since 2026-10-10. It got there over the air from this project's Matter firmware, which
is now retired: [Converting a Stock Module from Matter to ESPHome](Converting-a-Stock-Module-to-ESPHome).
A module that still runs the vendor firmware has no tested route yet (see the risk list below).

There are three files because a module has one of three flash layouts, and the image must be
built for the one it has. An image built for another layout writes its first update to the wrong
place and then invalidates the running image, which leaves a unit only the clip recovers.

| File | For a module that |
|---|---|
| `w41h1-amebaz2-factory.yaml` | still has the factory partition table: the vendor firmware, or the retired Matter firmware installed over the air |
| `w41h1-amebaz2-sdk.yaml` | was first written with the retired Matter firmware's clip image |
| `w41h1-amebaz2-native.yaml` | was written from scratch with LibreTiny's own partition table and bootloader |

If you do not know the unit's history and it still runs the Matter firmware (1.3.9 or later), that
firmware can tell you: `matter-convert.py backup <unit-ip>` prints the address of the inactive slot.
`0x10000` or `0x190000` means factory, `0xC000` or `0x1B8000` means sdk. A clip dump shows it too
(`firmware/docs/15-esphome-path.md`, "Which flash layout a unit has"). Once a unit runs ESPHome, its
`Flash layout` diagnostic sensor reports whether the image matches the module, and an image that
does not match refuses updates.

**What was measured on that module (2026-10-10):** it boots under the factory bootloader and joins
Wi-Fi, the layout check reports a match, the A/C answers status polls and commands, and two
updates with `esphome upload` went through, each back within about 10 seconds. One mains power
cycle came back clean.

**One fault was found and fixed.** Twice the receive side of the bus port stopped and stayed
stopped until a power cycle, while commands still reached the unit. The cause is in LibreTiny's
receive buffer, which loses count of a byte when the receive interrupt lands inside a read. The
component now reads with interrupts masked on this platform. Reopening the port also clears it,
so the board package still restarts the port when the bus link drops and every 15 seconds while
it stays down, as a fallback. Each restart logs a warning and counts in the `Serial port
restarts` diagnostic sensor. On a unit that answers, that sensor stays at 0. If yours climbs
while the unit answers, it is recovering from something that is not understood yet. It also
climbs on a unit that does not answer at all, which a restart cannot cure: one of the two
converted modules ended that way, for a reason outside the chip that is still open.

**Not measured:** the sdk and native layouts (they compile only), any run longer than an
afternoon, an interrupted update, and the return to the Matter firmware. It is the newest and
least proven combination here. Before you put it into an A/C you depend on, the known risks:

- LibreTiny rates this chip family 2 out of 5 for stability.
- A restart or an update can leave the chip dark until power has been off for about 30 seconds
  (LibreTiny issue 396). The file carries the workaround reported there. With it, no hang was
  seen on the tested module.
- Nothing rolls a bad update back, unlike the ESP32. An image that does not boot needs the clip.
- A module that does not run the retired Matter firmware needs the SOIC-8 clip or UART download
  mode for the first flash ([Recovery & Reflash](Recovery-and-Reflash#stock-module-ch341a-clip)).
  That route has not been used for this build.
- Flash `0x1000..0x3FFF` holds the module's own calibration data and must not be overwritten. The
  full-flash image the build produces is blank there, so it cannot be written as it is with the
  clip. The over-the-air conversion does not touch that region.
- Dump the whole chip first if you can. The dump is the only way back.
- How the bootloader picks a slot and what a rejected image looks like:
  `firmware/docs/10-stock-module-boot-and-recovery.md`.

The checks done so far and the full list are in `firmware/docs/15-esphome-path.md`.

## Wi-Fi power saving and the temperature placeholder

**The radio stays on.** The node config turns Wi-Fi power saving off (`power_save_mode: none`).
ESPHome's default on an ESP32 lets the radio sleep between beacons, and on a board with a weak
signal that lost most pings and dropped commands
([the measurement](Migrating-ESP32-Matter-to-ESPHome#afterwards)). A board on a supply too weak
for an always-on radio can go back with `-s wifi_power_save light`.

**Outdoor and coil temperature read "unknown" after a power cut.** For a while after mains power
returns, the unit reports both as exactly -20 C, a placeholder it sends until it has real
readings. The firmware publishes that pair as unknown and starts publishing once real values
arrive. A real -20 C is still shown once the unit has reported anything else since the node
started. This rests on one observation on one unit (2026-10-10).

## One component, its own codec

The component under `firmware/esphome/components/hisense_ac/` carries its own port of the protocol
code and talks to the bus through ESPHome's `uart:` component. It uses nothing from ESP-IDF, which
is why it builds for every board in the table above. The codec is a port of the project's first
bus driver, which ran on real units for months and is kept as a frozen reference under
`firmware/test/reference-driver/`. A host test runs every frame builder and parser of both over
the same inputs and compares them byte for byte, and it runs in CI. A protocol change that the
reference also covers is made in both, so the comparison keeps meaning something.

## Status

Phases 1 to 3 (hub and climate, the full control surface, telemetry and diagnostics) are done and
the firmware has driven a live A/C from Home Assistant since 2026-08, including all four sleep
profiles. Units run it from the connector's 5 V rail, closed up (stage 3 of the bring-up). Since
2026-10-10 it is the only firmware on the maintainer's units. A sniffer pass on the wire is still
open. Progress is tracked in `firmware/docs/15-esphome-path.md` and issue #13.
