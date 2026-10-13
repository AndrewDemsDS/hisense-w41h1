# Wi-Fi Setup Mode and the Status LED

How to move a node to another Wi-Fi network without a cable, what the status LED tells you, and
what ESPHome's safe mode does when a firmware keeps crashing. Getting a board back with a cable
or a clip is on [Recovery and Reflash](Recovery-and-Reflash).

> **Setup mode has run once on a unit, in part.** On 2026-10-13, on an ESP32-C3 SuperMini Plus,
> the remote's gesture opened the hotspot, the node left the home network, and after the
> 10 minute timeout it restarted onto the network it had. Not seen on a unit: the portal, a
> network entered in it, a second gesture, the button, the status LED or pixel, and anything on
> the stock module or another chip. The check to run on a unit, the full table of cases and what
> happens on the bus are in `firmware/esphome/README.md` ("Wi-Fi setup mode and the 77 gesture").

## The hotspot

Every node has a setup hotspot named `<name>-setup`, by default `hisense-ac-setup`. Its password
is the `wifi_ap_password` from `secrets.yaml`. On the hotspot, a captive portal takes a new
network name and password. If the phone does not open the portal by itself, browse to the
hotspot's gateway address (the node's log line "hotspot open" prints it).

The hotspot opens in two ways:

- **By itself**, when the configured network has been out of reach for about a minute. This is
  ESPHome's own fallback. The node keeps trying its network meanwhile.
- **On demand**, through setup mode, while the network is still there.

## Setup mode

Start it in one of two ways:

- On the remote, press **horizontal airflow six times**. This is the gesture that makes the
  vendor module reset its Wi-Fi.
- In Home Assistant, press the `Start Wi-Fi setup` button on the device page. Use this for a unit
  whose remote is not at hand.

The panel shows "77", the node leaves its network and opens the hotspot. Join it and enter the new
network in the portal.

Setup mode ends in a restart, on whichever comes first:

- the network entered in the portal has connected,
- the gesture is repeated, at least 10 s after the first,
- 10 minutes have passed (`wifi_setup_timeout`, a substitution, 30 s to 24 h). A network entered
  shortly before that still gets 90 s to connect.

The node is off the network for that time. The A/C keeps running and keeps answering the remote.
A gesture made by mistake costs nothing else: the node comes back on the network it had.

What happens to the Wi-Fi settings:

| Case | Result |
|---|---|
| Nothing entered in the portal | nothing changes |
| A network entered, and it connects | it is kept |
| A network entered that has not connected when the mode ends | the network the node had is written back |
| The node restarts for another reason, or loses power, in between | the same, on the way down or at the next boot |

Put the new network in `secrets.yaml` as well. A network saved through the portal is stored under
a key that ESPHome derives from the build's configuration, so an update built from a changed
configuration goes back to the Wi-Fi compiled in.

If the hotspot is already open because the network is out of reach, the gesture or the button
only lights "77". The node keeps trying its network and is not restarted.

### What it cannot do

- Open the hotspot on demand on a node built with `fast_connect: true`, a network marked
  `hidden: true`, or an enterprise login. `esphome config` warns, and on the node the gesture and
  the button log why and change nothing. The shipped configuration sets none of these.
- Open the hotspot twice in one boot. If the fallback already opened and closed earlier in the
  same boot, the node logs that, restarts after 30 s, and the next gesture works.
- Keep a static IP or several configured networks through a failed attempt. Only one name and
  password are written back.
- End by gesture on a unit that asks with the sustained request bit (`0x08`). Such a unit leaves
  setup mode by the timeout or by a network connecting. The bench unit sends the other bit
  (`0x20`), and no unit here has been seen to send `0x08`.
- Notice a gesture made while an update is uploading.

### What you see in the log

Under the tag `wifi_setup_mode`, at the deployed log level:

| Line | Meaning |
|---|---|
| `Wi-Fi setup mode: leaving the network and opening the hotspot` | the mode started |
| `Wi-Fi setup mode: hotspot open, ...` | join it now. The line ends with the portal's address |
| `Wi-Fi setup mode: the hotspot is already open` | the fallback was open. Only "77" changed |
| `Wi-Fi setup mode: a network was entered, waiting for it to connect` | the portal took a network |
| `Wi-Fi setup mode: connected, restarting` | the new network works |
| `Wi-Fi setup mode: the network entered did not connect, restoring the previous one, ...` | the mode ended without a connection |
| `Wi-Fi setup mode: the hotspot did not open. Restarting, ask again after that` | see the second limit above |

A node in setup mode is off the network, so only a serial console shows these lines as they
happen.

How the request travels on the bus: [Protocol Overview](Protocol-Overview#the-77-request).

## Status LED

A board with an LED on a GPIO can show the node's state. The LED is dark while everything is
fine. Two optional packages exist, one per kind of LED, and only the two ESP32-C3 board files
include one.

| Board file | LED | Shows |
|---|---|---|
| `w41h1-esp32c3.yaml` | the SuperMini's blue LED on GPIO8 (`packages/status-led.yaml`) | slow blink: no Wi-Fi, no Home Assistant connection, or the A/C not answering. Fast blink: setup mode, the hotspot open by itself, or safe mode |
| `w41h1-esp32c3-plus.yaml` | the Plus board's RGB pixel on GPIO8 (`packages/status-pixel.yaml`) | blue: setup mode, or the hotspot open by itself. Red: no Wi-Fi for 15 s. Amber: Wi-Fi up, the A/C not answering for 15 s. A missing Home Assistant connection is not shown |

The pixel package has not been checked on a board: its colour order, that it is dark after boot,
and how visible its low brightness is through the unit's housing.

Either package can be added to another board file with the pin as a substitution
(`status_led_pin`, `status_pixel_pin`). The pixel package is for ESP32 boards only. Firmware
cannot switch a power LED that is wired to the supply (the red one on some SuperMini boards), nor
the LEDs on an RS-485 transceiver board. The stock module has no LED the firmware drives.

## Safe mode

ESPHome's safe mode is active on every board file. Ten boots in a row that do not stay up for a
minute put the node into it. In safe mode only the logger, Wi-Fi with its hotspot and portal, the
update server and a plain status LED run. The A/C bus and every entity stay off, so a firmware
that crashes in the A/C code can still be replaced over the air. After five minutes without an
update the node restarts and tries the normal firmware again.

On an ESP32 the bootloader also rolls a new image back by itself when it never reaches that first
good minute. The stock module has no such rollback: safe mode only helps when the image gets as
far as starting ESPHome, and its flash layout check does not run in safe mode.
