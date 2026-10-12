# FAQ & Gotchas

The load-bearing traps, distilled for operator/future-you. Each answer links to the authoritative
doc.

## Which hardware should I use?

**An ESP32 board** (a C3 SuperMini or a classic ESP32 with a 3.3 V transceiver) in place of the
module, unless you have a reason not to: cheap, flashed and recovered over USB. **The stock
module** through LibreTiny keeps the original hardware, with no rollback and the clip as the only
recovery. ([User Guide](User-Guide#pick-the-hardware))

## What happened to the Matter firmware?

It is retired. `main` carries the ESPHome firmware only, and the last tree with the two Matter
builds is the tag `matter-final`. A unit that runs one keeps working.
([Matter Firmware (Retired)](Matter-Firmware-Retired))

## Can Apple Home, Google Home or Alexa see the A/C?

Through Home Assistant: expose the climate entity with HA's own Matter or HomeKit bridge. The node
itself talks to Home Assistant only.

## Can I switch a node from the old Matter firmware to ESPHome without opening the unit?

Yes, on both kinds of hardware, and each has been done once with no automatic rollback. Read the
risks first: [an ESP32 board](Migrating-ESP32-Matter-to-ESPHome),
[a stock module](Converting-a-Stock-Module-to-ESPHome).

## The `AC bus link` sensor stays off.

The node is running but hears nothing from the A/C. In order of likelihood:

1. **A and B swapped.** Harmless; swap them.
2. **TX and RX swapped** between the board and the transceiver (vendor labels on auto-direction
   modules vary). Swap them.
3. **Wrong pins for the board.** A C3 SuperMini flashed without `--board c3` uses the classic ESP32
   pins. Re-flash with the right `--board`.
4. **DE not reaching the transceiver**, or a floating DE on the C3 without its pulldown.

Raise `logger: level:` to `DEBUG` to see every decoded frame, or prove the wiring with no A/C at all
using `dev.py bench esphome`. ([Build, Flash & Test](Build-Flash-Test#bench-stage-no-ac))

## Home Assistant can't find the node, or rejects it.

- **Not discovered:** mDNS does not cross VLANs. Add it by hand (ESPHome integration, host = the
  node's IP, port 6053).
- **"Invalid encryption key" or it keeps asking to re-authenticate:** the key in HA must be the
  `hisense_ac__encryption_key` from the `secrets.yaml` the node was built with. If you regenerated
  it, re-flash, then update the key in HA.
- **It never joined Wi-Fi:** look for the `hisense-ac-setup` hotspot and set Wi-Fi through its
  captive portal.

([ESPHome Build](ESPHome-Build#adopt-it-in-home-assistant))

## How do I update it?

Over the air, with `esphome run` on the node's board file from `firmware/esphome/` (or the same
`-s` board overrides you flashed with). No version bump involved.
([ESPHome Build](ESPHome-Build#updates))

## My ESP32 board won't flash or has stopped booting.

Hold **BOOT** while plugging in USB to force download mode, then flash again. An ESP32 always
recovers over USB. ([Recovery & Reflash](Recovery-and-Reflash#esp32-boards))

## A stock module went quiet after an update. Is it bricked?

Often not. A module that booted an image without working Wi-Fi credentials is alive and
unreachable. Look for its `<name>-setup` access point. What settles it is the flash QE bit, read
with the clip: the bootloader clears it when it rejects an image.
(`firmware/docs/10-stock-module-boot-and-recovery.md`)

## An update to a stock module "finished" but the old firmware is still running.

The module's bootloader picks the slot with the higher `FWHS` serial, not the newest write. An
image whose serial is not above the running one never boots. LibreTiny's serials rise with the
build time, so this only bites an image that was re-signed by hand or built with a clock that went
backwards. (`firmware/docs/10-stock-module-boot-and-recovery.md`)

## What is "77"?

Press the remote's swing button **6 times** and the display shows **"77"**. On the vendor firmware
that resets the module's Wi-Fi and starts pairing. There is no reset frame: the mainboard sets a
request bit in its heartbeat reply, and the display lights when the module reports that it is
provisioning. The ESPHome firmware does not act on the request. To give a node new Wi-Fi, use its
setup hotspot or re-flash. ([Protocol Overview](Protocol-Overview#the-77-request))

## Setpoint changes do nothing sometimes.

Temperature is only honored in **cool/heat**. The firmware refuses the write while the unit runs
in dry, fan-only or auto and logs a warning, so the entity keeps the setpoint the unit reports. In
auto the unit holds its own value. ([Everyday Control](Everyday-Control))

## A command did not take. How do I tell what happened?

Look at the bus counters ([Entities and Diagnostics](Entities-and-Diagnostics#bus-counters)).
`Unanswered commands` rising means the unit did not acknowledge a frame. The node sends such a
command again and counts it in `Command retries`, or in `Failed commands` if it gave up.

## Can I stop the indoor unit beeping on every command?

Yes, with the **Beeper** switch. It only silences commands from the node. The remote still beeps.

## The module keeps browning out / the radio drops.

Power it from the **A/C**, not a bench supply. The 5 V from the unit is what the radio expects.
On an ESP32 on USB, it is usually the ground loop: connect only A and B until the laptop is
unplugged. ([Hardware & Wiring](Hardware-and-Wiring))

## I bricked a module. Now what?

An ESP32 board recovers over USB (see above). A stock module: write its own full dump back with
the **CH341A clip** and the tools in `firmware/flasher/` (not flashrom). Without a dump of that
unit there is no complete way back, which is why you take one first.
([Recovery & Reflash](Recovery-and-Reflash))

## How do I find out which features MY A/C has?

Read the `capability_*` binary sensors. The A/C reports its own capabilities in a ProductType
reply, and the firmware publishes one sensor per flag: 8 °C frost-guard heat, purify, AI mode, an
8-position louvre, humidity, a dimmable display and so on.

A flag that is off means *not on this unit*, never *not on this model*. Two units of the same model
can differ. Trim the YAML to what your unit reports
([ESPHome Build](ESPHome-Build#capability-gating-is-a-yaml-decision)).
