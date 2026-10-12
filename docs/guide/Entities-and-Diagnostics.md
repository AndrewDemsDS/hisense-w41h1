# Entities and Diagnostics

Every entity the firmware exposes, and how to read the bus diagnostics that tell you whether the
node and the A/C are talking. For day-to-day use see [Everyday Control](Everyday-Control). This
page is the reference behind it.

This page describes the ESPHome component on `main`.

## Entities

| Function | Entity |
|---|---|
| Power, mode, setpoint | `climate` |
| Fan (auto and five speeds), vertical swing | `climate` fan and swing modes |
| Indoor temperature | `climate` current temperature |
| Outdoor temperature | `sensor` Outdoor temperature |
| Eco | `switch` Eco, preset `eco` |
| Quiet | `switch` Quiet, preset `quiet` |
| Turbo | `switch` Turbo, preset `turbo` |
| Sleep profile | `select` Sleep profile, `sleep_*` presets |
| Aux heat relay | `binary_sensor` Aux heat relay |
| Coil temperature | `sensor` Coil temperature |
| Panel display | `switch` Panel display |
| Any fault | `binary_sensor` Fault |
| Beeper | `switch` Beeper |
| Power, voltage, current | `sensor` Power, Voltage, Current |
| Energy today | `total_daily_energy` (stock ESPHome platform) |
| Compressor frequency | `sensor` Compressor frequency |
| Capability flags | 13 `binary_sensor` `capability_*` |
| Per-fault detail | 18 `binary_sensor` `fault_*` |
| Bus checksum errors | `sensor` `checksum_errors` |
| Bus reply timeouts | `sensor` `reply_timeouts` |
| Unanswered commands | `sensor` `unanswered_commands` |
| Bus link losses | `sensor` `link_losses` |
| Command retries, failed commands | `sensor` `command_retries`, `failed_commands` |
| A/C device type | `text_sensor` `link_token` |
| Bus link | `binary_sensor` `bus_link` |
| Uptime, WiFi signal, Reset reason | on every board. The text of the reset reason is the chip's own and differs between platforms |
| Serial port restarts, Flash layout | the stock module only. The first counts how often the firmware had to reopen the bus port after the link dropped, and should stay at 0 |

### Behaviour worth knowing

| Situation | What the firmware does |
|---|---|
| A command the unit does not take | sent again, see [below](#confirm-and-retry) |
| Mode request to a unit that is off | one frame carrying mode and power-on |
| Setpoint outside cool and heat | refused by the entity in auto, dry and fan-only (the unit keeps its own) |
| Special modes changed together | 10 s between special-mode frames, because the unit ignores one that lands too soon after another |
| Horizontal swing | `supports_horizontal_swing` option, off by default |
| Outdoor and coil temperature after a power cut | for a while after mains power returns the unit reports both as exactly -20 C, a placeholder. They show as unknown until real readings arrive. (One observation, one unit, 2026-10-10.) |

The retired Matter builds exposed the same functions as endpoints and a manufacturer cluster. That
table is in this page's history at the `matter-final` tag
([Matter Firmware (Retired)](Matter-Firmware-Retired)).

## Beeper

The indoor unit beeps when it takes a command. The beep is requested per frame: bit 2 of frame
byte 23 in a class `0x65` command, which the stock module calls `t_beep` and sets by default. The
Beeper switch clears the bit on every command frame the node sends.

- Turning the switch off sends nothing. It takes effect from the next command.
- The setting is stored on the node and survives a reboot. It is on by default, so behaviour does
  not change until you switch it off.
- The remote control still makes the unit beep. That path does not go through the node.
- The A/C does not report a beeper state, so the switch shows what was last set.

Hardware status: a unit was heard to stay silent with the switch off (maintainer's check,
2026-10-10).

## Bus counters

Six values describe the health of the RS-485 link. The counters start at 0 at boot and return to 0
on a reboot.

| Value | Counts |
|---|---|
| Checksum errors | frames from the unit that arrived with a bad checksum |
| Reply timeouts | frames of any kind the unit did not answer within 500 ms |
| Unanswered commands | the reply timeouts that were commands (a user write the unit never acknowledged) |
| Link losses | times the link was declared lost: five status polls in a row without a valid reply |
| A/C device type | the two bytes the unit reports about itself in the handshake, `HH LL`. Unknown until learned |
| Bus link | on while the unit answers its status poll |

Two more count the retries:

| Value | Counts |
|---|---|
| Command retries | commands sent again, for either reason described below |
| Failed commands | commands given up on after the last re-send, plus commands dropped at a link loss |

`unanswered_commands` counts every unanswered send, so one command lost three times counts 3.

### Reading them

They are ordinary sensors on the device page, in the Diagnostic group. Declare the ones you want in
the YAML. The shipped `w41h1.yaml` declares all of them.

### What the numbers mean

- **All zero, bus link on:** healthy.
- **Reply timeouts rising, bus link still on:** the unit misses some polls. Occasional misses are
  harmless because the next poll asks again.
- **Unanswered commands rising:** a send was lost. Check whether `failed_commands` also rose. If
  it did not, a re-send landed and nothing was lost in the end.
- **Link losses rising:** the unit stopped answering for at least five polls. Check the wiring and
  the DE pin first ([FAQ & Gotchas](FAQ-Gotchas)). Commands sent during the outage are lost.
- **Checksum errors rising:** corrupt bytes on the wire, usually wiring, a floating DE line or a
  marginal supply.

## Confirm and retry

Before 2026-10-10 a command frame was sent once and the result never checked. Hardware runs on two
units failed about one check in 52, each time with `unanswered_commands` at exactly 1 (pull request
#179). The component now checks each command twice.

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
