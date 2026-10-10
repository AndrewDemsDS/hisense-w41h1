# Entities, Endpoints and Diagnostics

What each of the three firmwares exposes, side by side, and how to read the bus diagnostics that
tell you whether the node and the A/C are talking. For day-to-day use see
[Everyday Control](Everyday-Control). This page is the reference behind it.

Versions this page describes: ESPHome component on `main` as of 2026-10-10, AmebaZ2 1.3.50,
ESP32 Matter 1.1.19, and the companion integration `hisense-unified-ac` 1.6.0.

## Parity table

"Matter" means both Matter builds. They have the same endpoints: the AmebaZ2 build takes them
from the `.zap`, the ESP32 build creates them in code, and a host test
(`firmware/test/test_zap_edit.py`) checks the two against the shared map.

| Function | ESPHome entity | Matter endpoint and cluster | Unified integration 1.6.0 |
|---|---|---|---|
| Power, mode, setpoint | `climate` | ep1 OnOff + Thermostat | climate |
| Fan (auto and five speeds), vertical swing | `climate` fan and swing modes | ep1 FanControl (`FanMode`, `PercentSetting`, `RockSetting`) | climate fan and swing modes |
| Indoor temperature | `climate` current temperature | ep1 Thermostat `LocalTemperature` | climate current temperature |
| Outdoor temperature | `sensor` Outdoor temperature | ep2 TemperatureMeasurement | Outdoor temperature |
| Eco | `switch` Eco, preset `eco` | ep3 OnOff, label `Eco` | Eco switch, preset |
| Quiet | `switch` Quiet, preset `quiet` | ep4 OnOff, label `Quiet` | Quiet switch, preset |
| Turbo | `switch` Turbo, preset `turbo` | ep5 OnOff, label `Turbo` | Turbo switch, preset |
| Sleep profile | `select` Sleep profile, `sleep_*` presets | ep6 ModeSelect (Off, General, Old, Young, Kids) | Sleep profile select, presets |
| Aux heat relay | `binary_sensor` Aux heat relay | ep7 contact sensor (BooleanState), label `Aux Heat` | Aux heat relay |
| Coil temperature | `sensor` Coil temperature | ep8 TemperatureMeasurement | Coil temperature |
| Panel display | `switch` Panel display | ep9 OnOff, label `Display` | Panel display switch |
| Any fault | `binary_sensor` Fault | ep10 contact sensor (BooleanState), label `Fault` | Fault |
| Beeper | `switch` Beeper | ep11 OnOff, label `Beeper` | Beeper switch |
| Power, voltage, current | `sensor` Power, Voltage, Current | ep1 ElectricalPowerMeasurement | Power, Voltage, Current |
| Energy today | `total_daily_energy` (stock ESPHome platform) | none | none (use a Riemann sum helper on Power) |
| Compressor frequency | `sensor` Compressor frequency | ep1 mfg `0x0010` | Compressor frequency |
| Capability flags | 13 `binary_sensor` `capability_*` | ep1 mfg `0x0012` `Features1` | one binary sensor per flag |
| Per-fault detail | 18 `binary_sensor` `fault_*` | ep1 mfg `0x0013` `Faults1` | one binary sensor per fault |
| Bus checksum errors | `sensor` `checksum_errors` | ep1 mfg `0x0014` `ChecksumErrors` | Bus checksum errors |
| Bus reply timeouts | `sensor` `reply_timeouts` | ep1 mfg `0x0015` `ReplyTimeouts` | Bus reply timeouts |
| Unanswered commands | `sensor` `unanswered_commands` | ep1 mfg `0x0016` `UnansweredCommands` | Unanswered commands |
| Bus link losses | `sensor` `link_losses` | ep1 mfg `0x0017` `LinkLosses` | Bus link losses |
| A/C device type | `text_sensor` `link_token` | ep1 mfg `0x0018` `LinkToken` | AC device type |
| Bus link | `binary_sensor` `bus_link` | ep1 mfg `0x0019` `BusLink` | AC bus link |
| Command retries, failed commands | `sensor` `command_retries`, `failed_commands` | none: the Matter builds do not retry | none |

"mfg" is the manufacturer cluster `0xFFF1FC00` on endpoint 1. Home Assistant's Matter integration
does not render it, so on a Matter node those rows only appear through the unified integration,
which reads them from matter-server by raw attribute path. The mfg attributes `0x0000` to `0x0003`
(Eco, Turbo, Mute, SleepProfile) mirror the ep3 to ep6 controls.

### Where the firmwares differ

- **Outdoor and coil temperature after a power cut.** For a while after mains power returns the
  unit reports both as exactly -20 C, a placeholder. The ESPHome build shows them as unknown until
  real readings arrive. The two Matter builds publish the placeholder, so they show -20 C in that
  window. (One observation, one unit, 2026-10-10.)
- **Serial port restarts** exists only on the stock module under ESPHome. It counts how often the
  firmware had to reopen the bus port after the link dropped, and should stay at 0.

| | ESPHome | Matter (AmebaZ2 and ESP32) |
|---|---|---|
| A command the unit does not take | sent again, see [below](#confirm-and-retry-esphome-only) | sent once; the miss is counted in `UnansweredCommands` |
| Mode request to a unit that is off | one frame carrying mode and power-on | a power-on frame, then a mode frame |
| Setpoint outside cool and heat | refused by the entity in auto, dry and fan-only (the unit keeps its own) | a no-op in dry and fan-only: the setpoint is stripped from the frame |
| Special-mode pacing | 10 s between special-mode frames | none: two switches toggled together can lose the second |
| Energy counter | yes | no |
| Horizontal swing | `supports_horizontal_swing` option, off by default | not exposed |

The Matter column lists known gaps, recorded in pull request #173 and issue #168. Issue #182
proposes retiring the Matter builds instead of closing them. See
[Home](Home#state-of-the-project).

## Version requirements on the Matter builds

| Feature | AmebaZ2 | ESP32 Matter |
|---|---|---|
| Beeper endpoint (ep11) | 1.3.49 | 1.1.19 |
| Bus counters, `LinkToken`, `BusLink` (`0x0014` to `0x0019`) | 1.3.49 | 1.1.19 |
| Setpoints over the full 16 to 32 °C range | 1.3.49 | 1.1.19 |
| Sleep profile list populated on ep6 | 1.3.50 (empty in some earlier builds) | all |
| Glue fixes for SystemMode Auto ending as Cool (#170, #173) | 1.3.48 | 1.1.18 |

After an update that adds an endpoint, Home Assistant needs a node re-interview before the new
entity appears ([Commissioning & HA Setup](Commissioning-and-HA-Setup#re-interview-after-a-structure-changing-ota)).

Two notes on the setpoint range. Before these versions the Thermostat server applied its default
dead band between the heating and cooling setpoints, so AmebaZ2 refused cooling setpoints below
18.5 °C and heating setpoints above 29.5 °C, and ESP32 refused below 18 °C and above 30 °C. The
fix sets `MinSetpointDeadBand` to 0. On ESP32 that attribute is stored in flash, so a node on which
a controller once wrote another value keeps that value until 0 is written again.

On AmebaZ2 the sleep profile names live in a Realtek SDK file this repo cannot vendor. A restored
SDK brought back the stock table, and builds made from it shipped an empty `SupportedModes` list:
Home Assistant showed the sleep select as unavailable while `CurrentMode` still worked. From 1.3.50
`dev.py ota amebaz2 build` writes the list on every build (`firmware/src/sdk-edits/README.md`).

## Beeper

The indoor unit beeps when it takes a command. The beep is requested per frame: bit 2 of frame
byte 23 in a class `0x65` command, which the stock module calls `t_beep` and sets by default. All
three firmwares now have a Beeper switch that clears the bit on every command frame the node sends.

- Turning the switch off sends nothing. It takes effect from the next command.
- The setting is stored on the node and survives a reboot. It is on by default, so behaviour does
  not change until you switch it off.
- The remote control still makes the unit beep. That path does not go through the node.
- The A/C does not report a beeper state, so the switch shows what was last set.

Hardware status: on an ESPHome node, a unit was heard to stay silent with the switch off
(maintainer's check, 2026-10-10). The Matter builds clear the same bit through the same routine,
and a host test compares the two byte for byte, but nobody has listened to a unit driven by a
Matter node with the switch off.

## Bus counters

Six values describe the health of the RS-485 link. The counters start at 0 at boot and return to 0
on a reboot.

| Value | Counts |
|---|---|
| Checksum errors | frames from the unit that arrived with a bad checksum |
| Reply timeouts | frames of any kind the unit did not answer within 500 ms |
| Unanswered commands | the reply timeouts that were commands (a user write the unit never acknowledged) |
| Link losses | times the link was declared lost: five status polls in a row without a valid reply |
| A/C device type | the two bytes the unit reports about itself in the handshake, `HH LL`. Unknown (0 on Matter) until learned |
| Bus link | on while the unit answers its status poll |

ESPHome adds two more, which exist because only that firmware retries:

| Value | Counts |
|---|---|
| Command retries | commands sent again, for either reason described below |
| Failed commands | commands given up on after the last re-send, plus commands dropped at a link loss |

One difference in meaning: on ESPHome `unanswered_commands` counts every unanswered send, so one
command lost three times counts 3. On the Matter builds nothing is sent again, so each lost command
counts once.

### Reading them

**ESPHome.** They are ordinary sensors on the device page, in the Diagnostic group. Declare the
ones you want in the YAML. The shipped `w41h1.yaml` declares all of them.

**Matter, with the unified integration.** Version 1.6.0 creates a sensor for each counter and for
the device type when the node reports the attribute, and uses `BusLink` for its `AC bus link`
sensor. The integration reads them from matter-server, so its entry needs the Matter server URL
and node id (both are filled in by the config flow).

**Matter, by hand.** matter-server stores the attributes under the numeric path
`<endpoint>/<cluster id>/<attribute id>`, all decimal. The cluster id `0xFFF1FC00` is `4294048768`,
so the six values are `1/4294048768/20` to `1/4294048768/25`. Read one with matter-server's
`read_attribute` WebSocket command, or look in the node's attribute dump.

### What the numbers mean

- **All zero, bus link on:** healthy.
- **Reply timeouts rising, bus link still on:** the unit misses some polls. Occasional misses are
  harmless because the next poll asks again.
- **Unanswered commands rising:** a write was lost. On ESPHome, check whether `failed_commands`
  also rose. If it did not, a re-send landed and nothing was lost in the end. On Matter the command
  is gone and the entity returns to the unit's real state at the next status.
- **Link losses rising:** the unit stopped answering for at least five polls. Check the wiring and
  the DE pin first ([FAQ & Gotchas](FAQ-Gotchas)). Commands sent during the outage are lost.
- **Checksum errors rising:** corrupt bytes on the wire, usually wiring, a floating DE line or a
  marginal supply.

## Confirm and retry (ESPHome only)

Before 2026-10-10 every firmware sent a command frame once and never checked the result. Hardware
runs on two ESPHome units failed about one check in 52, each time with `unanswered_commands` at
exactly 1 (pull request #179). The ESPHome component now checks each command twice.

| Check | When | If it fails |
|---|---|---|
| The unit replied to the frame | within the 500 ms reply window | the frame is sent again in the next bus cycle, ahead of anything queued behind it. Three sends at most, and only while the unit answers its status poll |
| The unit's status shows what was asked | first status frame about 4 s after the frame left the wire | power, mode, setpoint, fan and swing are sent again, twice at most. Eco, turbo, quiet and sleep are planned again from what the unit reports, once, after the 10 s special-mode pacing |

After the last re-send the component logs a warning, increments `failed_commands`, and lets the
entities show what the unit reports. A newer command replaces the one being checked. A lost bus
link drops every open command.

A mode request to a unit that is off is one frame that carries the mode and power-on together, as
the stock module sends it. If the unit is still off at the check, the re-send falls back to the
older pair of frames, power-on and then the mode.

### Limits

- The Matter builds do not have it. They send once. A design for the shared driver is written up in
  pull request #173 and is not implemented.
- Some differences are never re-sent, because they are the unit's own behaviour: its setpoint
  in auto, dry and fan-only, the mode, setpoint and fan that turbo forces, the fan under quiet or a
  sleep profile, and anything except power on a unit that reports off.
- A value that is neither the old one nor the requested one is taken as someone using the
  remote. The command is dropped without a failure count.
- Display and beeper are not confirmed. The unit does not report either.
- The acknowledgement byte is not acted on. The stock reply layout has not been captured on
  this bus, so the component treats any reply as "answered" and relies on the status check.
- Commands older than 20 s are not sent again.
- The one-frame power and mode bytes for auto, dry and fan-only have not been captured. Only cool
  (`0x5C`) and heat (`0x3C`) appear in the stock image. The others follow the same field rule and
  are marked `VERIFY` in the source. The fallback covers a unit that does not take them.
- Hardware coverage is thin. The rules are host-tested (`firmware/test/test_esphome_confirm.cpp`,
  which repeats the hub's glue and can drift from it). On the two units that run this code, a full
  actuation run after the update showed no retries (maintainer's report, 2026-10-10), so the
  re-send path itself has not yet been seen to fire on a real unit.

Source for the rules: `intent_expected_fields()` and `confirm_decision()` in
`firmware/esphome/components/hisense_ac/hisense_map.h`.
