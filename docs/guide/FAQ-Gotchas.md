# Troubleshooting and FAQ

Faults and common questions about a node, organised by what you see. Each entry says what to
check, in order, and links to the page that has the detail. Errors from the build tools themselves (a missing command, a port that will not
open) are in [Build Environment](ESP32-Build-Environment#when-it-fails).

Two tools answer most questions:

- The diagnostics on the device page: `AC bus link` and the counters
  ([what they mean](Entities-and-Diagnostics#bus-counters)). On a newly added node the counters
  are disabled entities: enable them on the device page first.
- The node's log: `esphome logs <board file>` from `firmware/esphome/`, or the ESPHome
  dashboard ([what the lines mean](Entities-and-Diagnostics#log-lines)).

## The node and the network

### Home Assistant does not find the node

- mDNS does not cross VLANs. Add the node by hand: the ESPHome integration, host = the node's IP
  address, port 6053.
- The node may not be on Wi-Fi at all. Look for its `<name>-setup` hotspot (by default
  `hisense-ac-setup`) and give it the network through the captive portal
  ([the hotspot](Wifi-Setup-Mode#the-hotspot)).

### Home Assistant rejects the key, or keeps asking to authenticate again

The key in Home Assistant must be the `hisense_ac__encryption_key` from the `secrets.yaml` the
node was built with. If you made a new key, update the node first, then enter the key in Home
Assistant.

### The node drops off Wi-Fi or commands arrive late

- Check `WiFi signal`. With a weak signal, a node that lets its radio sleep loses most of its
  traffic. Nodes built from this repository keep the radio on. A node built from your own YAML
  needs `power_save_mode: none` ([the measurement](ESPHome-Build#wi-fi-power-saving)).
- Check `Reset reason` and `Uptime`. A brownout reset points at the supply (next entry).

### An update uploads, then the node is back on the old firmware

On an ESP32, the log of the old firmware shows `OTA rollback detected` and
`Last reset was due to brownout`. The board's 3.3 V supply dips when Wi-Fi starts at full power,
the new image resets on its first boot, and the bootloader rolls back. Check the supply and its
wiring first. If the board has to run as it is, cap the transmit power in that node's YAML
(`firmware/esphome/README.md`, "Troubleshooting", has the block). After any update, confirm that
the `compiled on` line in the log shows the new build time.

On a stock module the cause is different: its bootloader boots the image with the higher serial
number, which is not always the newest write. LibreTiny's serials rise with the build time, so
this only bites an image that was re-signed by hand or built with a clock that went backwards
(`firmware/docs/10-stock-module-boot-and-recovery.md`).

### The node has to move to another Wi-Fi network

Use [Wi-Fi setup mode](Wifi-Setup-Mode#setup-mode): the remote's gesture or the
`Start Wi-Fi setup` button. On a unit, the gesture has opened the hotspot and the timeout has
put the node back on its network (once, on an ESP32-C3), and no network has been entered in the
portal yet. The way that is known to work is to put the new network in `secrets.yaml` and update
the node while it is still on the old one.

### The LED on the board blinks, or the pixel is lit

It is the status LED: [what each pattern means](Wifi-Setup-Mode#status-led).

## The A/C does not answer

### The AC bus link sensor stays off

The node runs and hears nothing from the A/C. The log says
`A/C bus link lost: the unit is not answering`. In order of likelihood:

1. A and B swapped. Harmless. Swap them.
2. TX and RX swapped between the board and the transceiver. Vendor labels on auto-direction
   modules vary.
3. The wrong board file. A C3 SuperMini built from the classic file uses the classic pins,
   and the other way round.
4. DE not reaching the transceiver, or a floating DE on a C3 without its pulldown
   ([pins to avoid](ESP32-Replacement-Build#pins-and-mistakes-to-avoid)).
5. A 5 V transceiver module. Its RO pin drives 5 V into the ESP32 and can have killed the RX
   pin.

To see whether the A/C sends anything at all, set `logger: level: VERY_VERBOSE` in the node's
YAML: it logs the raw bytes of every frame in both directions. To prove the board and its wiring
with no A/C, run the [bench stage](Build-Flash-Test#bench-stage-no-ac).

### The link drops now and then

Read the [bus counters](Entities-and-Diagnostics#bus-counters). Rising checksum errors mean
corrupt bytes on the wire: wiring, a floating DE line, or a marginal supply. Rising link losses
with clean checksums mean the unit stopped answering for five polls in a row.

### On a stock module, Serial port restarts climbs

The firmware reopens its bus port when the link drops. If the number climbs while the unit
answers, the node is recovering from something that is not understood yet. If the unit does not
answer at all, a restart cannot cure it: one converted module ended that way
([state of the stock module](Stock-Module-Build#state-today)).

### The module or the board keeps browning out

Power it from the A/C, not from a bench supply. On an ESP32 on USB it is usually the ground loop:
connect only A and B until the laptop is unplugged
([staged bring-up](ESP32-Replacement-Build#staged-bring-up)).

## Commands

### A command did nothing

Look at the log. The node says why it refused or lost a command.

| Log line | Cause |
|---|---|
| `Command refused: no status from the unit yet`, or `Command not sent: no status from the unit yet ...` | the node has just booted and has not heard a status frame. Try again a few seconds later |
| `Command refused: the unit is not answering` | the bus link is down ([above](#the-ac-bus-link-sensor-stays-off)) |
| `Target temperature refused: the unit only takes one in cool and heat` | the unit keeps its own setpoint in auto, dry and fan-only |
| `Fan change refused: turbo, quiet or sleep is holding the fan speed` | clear the preset first |
| `Command frame dropped: TX queue full` | too many commands at once. Send them again more slowly |
| `Command not taken by the unit after N re-send(s) ...` | the unit answered and did not do it. `Failed commands` went up |

No line at all means the node sent the command and saw the unit take it. If the value changed
back afterwards, look at what else controls the unit: the remote, a climate group that mirrors
another unit, or the unit's own rules (turbo forces cooling at 16 °C).

### A change takes several seconds to show

That is expected. The node waits about 4 s before it believes a status that contradicts a command
it just sent, and it sends special-mode commands 10 s apart
([limits](Everyday-Control#limits-and-behaviour-to-know)).

### Unanswered commands or Command retries rise

The unit missed a frame and the node sent it again. Nothing was lost unless `Failed commands`
rose as well ([confirm and retry](Entities-and-Diagnostics#confirm-and-retry)).

### A fan change made while the unit is off is ignored

The unit ignores everything except power while it is off.

## Sensors

### Outdoor and coil temperature read "unknown"

For a while after mains power returns the unit sends a placeholder and the firmware does not
publish it ([details](Entities-and-Diagnostics#behaviour-worth-knowing)).

### Power does not match my meter

`Power` is an estimate computed from a value the bus carries, not a measurement
(`firmware/docs/09-energy-monitoring.md` has the calibration).

### Power and Current show unknown

The estimate came out above `max_power`, 5 kW unless the node's `hisense_ac:` block sets another
value. The firmware takes that for a bad byte on the bus and publishes no number. If the unit
does draw more than 5 kW, raise the limit:

```yaml
hisense_ac:
  max_power: 8kW
```

## Flashing and recovery

### An ESP32 board will not flash, or has stopped booting

Hold BOOT while plugging in USB to force download mode, then flash again. An ESP32 always
recovers over USB ([how](Recovery-and-Reflash#esp32-boards)).

### A stock module went quiet after an update

It is often not bricked. A module that booted an image without working Wi-Fi credentials is alive
and unreachable: look for its `<name>-setup` hotspot. What settles the question is the flash QE
bit, read with the clip. The bootloader clears it when it rejects an image
(`firmware/docs/10-stock-module-boot-and-recovery.md`).

### Flash layout says MISMATCH

The image was built for another flash layout than the module has, and it has switched its own
update server off. Do not try to update the unit
([find the layout](Stock-Module-Build#find-the-flash-layout-first)).

### I bricked a module

Write its own full dump back with the CH341A clip and the tools in `firmware/flasher/`, not with
flashrom. Without a dump of that unit there is no complete way back, which is why you take one
first ([how](Recovery-and-Reflash#stock-module-ch341a-clip)).

## Questions

### Which hardware should I use?

An ESP32 board in place of the module, unless you have a reason to keep the original
([the comparison](User-Guide#choose-the-hardware)).

### Can Apple Home, Google Home or Alexa see the A/C?

Through Home Assistant: expose the climate entity with its Matter or HomeKit bridge. The node
itself talks to Home Assistant only.

### How do I update a node?

Over the air, with `esphome run` on the node's board file ([Updates](ESPHome-Build#updates)).

### How do I find out which features my A/C has?

Read the `Capability ...` diagnostic sensors, and trim the YAML to what the unit reports
([capability gating](ESPHome-Build#capability-gating)).

### Can I stop the indoor unit beeping on every command?

Yes, with the `Beeper` switch. It silences commands from the node. The remote still beeps.

### What is "77"?

Pressing horizontal airflow six times on the remote makes the panel show "77". On the vendor
firmware that resets the module's Wi-Fi and starts pairing. This firmware takes it as "open Wi-Fi
setup mode" ([Wi-Fi setup mode](Wifi-Setup-Mode#setup-mode),
[how it works on the bus](Protocol-Overview#the-77-request)).

### What happened to the Matter firmware?

It is retired: [Matter Firmware (Retired)](Matter-Firmware-Retired).
