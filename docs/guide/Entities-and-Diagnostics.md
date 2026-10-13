# Entities and Diagnostics

Every entity a node exposes, what each means, and how to read the diagnostics that tell you
whether the node and the A/C are talking. For day-to-day use see
[Everyday Control](Everyday-Control). For a fault you can name, start at
[Troubleshooting and FAQ](FAQ-Gotchas).

The list below is what the shipped board files give. A test pins it: the host end-to-end test
compares the node's entities with `firmware/test/host_e2e/entities.golden.json` on every change
to the firmware. Entity ids are shown for the default device name, `Air Conditioner`.

## Entities

### Climate

`climate.air_conditioner`: mode (`off`, `cool`, `heat`, `fan_only`, `dry`, `auto`), target
temperature (16 to 32 °C, step 1), fan (`auto`, `low`, `medium_low`, `medium`, `medium_high`,
`high`), swing (`off`, `vertical`), 13 presets, the room temperature and the current action. The
presets and the limits are in [Everyday Control](Everyday-Control#the-climate-entity).

### Controls

| Name | Entity id | What |
|---|---|---|
| Eco | `switch.air_conditioner_eco` | eco mode |
| Turbo | `switch.air_conditioner_turbo` | turbo mode |
| Quiet | `switch.air_conditioner_quiet` | quiet mode (the unit's mute flag) |
| Panel display | `switch.air_conditioner_panel_display` | the display on the indoor unit |
| Beeper | `switch.air_conditioner_beeper` | whether the unit beeps for this node's commands ([below](#beeper)) |
| Sleep profile | `select.air_conditioner_sleep_profile` | `Off`, `General`, `Old`, `Young`, `Kids` |

### Sensors

| Name | Entity id | Unit | What |
|---|---|---|---|
| Outdoor temperature | `sensor.air_conditioner_outdoor_temperature` | °C | from the outdoor unit |
| Coil temperature | `sensor.air_conditioner_coil_temperature` | °C | the outdoor (condenser) coil |
| Compressor frequency | `sensor.air_conditioner_compressor_frequency` | Hz | how hard the compressor runs |
| Power | `sensor.air_conditioner_power` | W | an estimate. The bus carries a current reading that is a proxy and not amps, and the firmware computes power from it (`firmware/docs/09-energy-monitoring.md`). Unknown when the estimate is above `max_power` (5 kW unless set), which the firmware takes for a bad byte on the bus |
| Voltage | `sensor.air_conditioner_voltage` | V | as the unit reports it |
| Current | `sensor.air_conditioner_current` | A | derived from the power estimate and the voltage. Unknown when the power is |
| Energy today | `sensor.air_conditioner_energy_today` | kWh | the power estimate summed over the day. An unknown power value adds nothing of its own. The day rolls over on Home Assistant's clock |
| Aux heat relay | `binary_sensor.air_conditioner_aux_heat_relay` | | on while the auxiliary heater runs |
| Fault | `binary_sensor.air_conditioner_fault` | | on while the unit reports any fault |

### Diagnostics

| Name | Entity id | What |
|---|---|---|
| AC bus link | `binary_sensor.air_conditioner_ac_bus_link` | on while the unit answers its status poll |
| AC device type | `sensor.air_conditioner_ac_device_type` | the two bytes the unit reports about itself in the handshake, as `HH LL`. Unknown until learned |
| Bus checksum errors | `sensor.air_conditioner_bus_checksum_errors` | [bus counters](#bus-counters) |
| Bus reply timeouts | `sensor.air_conditioner_bus_reply_timeouts` | |
| Bus link losses | `sensor.air_conditioner_bus_link_losses` | |
| Unanswered commands | `sensor.air_conditioner_unanswered_commands` | |
| Command retries | `sensor.air_conditioner_command_retries` | |
| Failed commands | `sensor.air_conditioner_failed_commands` | |
| Capability heat pump, Capability eco, Capability quiet | `binary_sensor.air_conditioner_capability_*` | what the unit says it supports ([below](#capability-and-fault-flags)) |
| Fault indoor temp sensor, Fault indoor to outdoor comms, Fault condensate tray full, Fault outdoor temp sensor | `binary_sensor.air_conditioner_fault_*` | one fault bit each |
| Uptime, WiFi signal, Reset reason | | the node itself. The wording of the reset reason is the chip's own and differs between platforms |
| Start Wi-Fi setup | a button | opens [Wi-Fi setup mode](Wifi-Setup-Mode) |
| Serial port restarts, Flash layout | | the stock module only ([The Stock Module](Stock-Module-Build#after-the-first-boot)) |

The six bus counters and the per-bit `Fault ...` sensors are disabled by default: a node added
to Home Assistant shows them under "disabled entities" on the device page, and you enable the
ones you want there. The summary `Fault` sensor and `AC bus link` are always enabled. Home
Assistant applies this only when it first sees an entity, so on a node it already knows they
stay enabled.

The node's own diagnostics and the button come from `packages/node.yaml`, and the stock module's
two from `packages/amebaz2.yaml`. They are not in the golden file, which covers the A/C's
entities.

### Capability and fault flags

The A/C reports what it supports in its ProductType reply, and its faults as a bitmap. The
component can publish every bit as a binary sensor. The shipped package declares three
capability flags and four fault flags, the ones that have fired on a bench unit. The rest are
options you add to `packages/hisense-ac.yaml` in the same way:

| Group | Options |
|---|---|
| Capability flags (13) | `capability_cool_heat`, `capability_power_save`, `capability_fan_mute` (the three shipped), `capability_ai`, `capability_infinite_fan`, `capability_swing_dir_8`, `capability_swing_follow`, `capability_humidity`, `capability_heat_8c`, `capability_purify`, `capability_q_display`, `capability_enable_8heat`, `capability_trans_102_64` |
| Fault flags (18) | `fault_indoor_temp`, `fault_indoor_comms`, `fault_water_full`, `fault_outdoor_temp` (the four shipped), `fault_indoor_coil_temp`, `fault_indoor_humidity`, `fault_indoor_fan_motor`, `fault_grille`, `fault_indoor_vzero`, `fault_indoor_display`, `fault_indoor_keys`, `fault_indoor_wifi`, `fault_indoor_ele`, `fault_indoor_eeprom`, `fault_outdoor_eeprom`, `fault_outdoor_coil_temp`, `fault_outdoor_gas_temp`, `fault_over_temp` |

A capability flag that is off means "not on this unit", never "not on this model". What each flag
stands for is in `reverse-engineering/docs/11-model-capability-map.md`.

### Behaviour worth knowing

| Situation | What the firmware does |
|---|---|
| A command the unit does not take | sent again ([confirm and retry](#confirm-and-retry)) |
| A command before the first status frame after boot, or while the unit is not answering | refused. The entity keeps its state |
| A mode request to a unit that is off | one frame that carries the mode and power-on |
| A setpoint outside cool and heat | refused by the entity in auto, dry and fan-only (the unit keeps its own) |
| A fan change under turbo, quiet or a sleep profile | refused |
| Special modes changed together | 10 s between special-mode frames, because the unit ignores one that lands too soon after another. Changes that undo each other within that time merge into one frame |
| A status that contradicts a command just sent | ignored for 4 s, so the entity does not flicker back |
| Outdoor and coil temperature after a power cut | for a while after mains power returns the unit reports both as exactly -20 °C, a placeholder. They show as unknown until real readings arrive. A real -20 °C is shown once the unit has reported anything else since the node started. (One observation, one unit, 2026-10-10.) |

## Beeper

The indoor unit beeps when it takes a command. The beep is requested per frame: bit 2 of byte 23
in a command frame, which the vendor module sets by default. The Beeper switch clears that bit on
every command frame the node sends.

- Turning the switch off sends nothing. It takes effect from the next command.
- The setting is stored on the node and survives a reboot. It is on by default.
- The remote control still makes the unit beep. That path does not go through the node.
- The A/C does not report a beeper state, so the switch shows what was last set.

A unit was heard to stay silent with the switch off (maintainer's check, 2026-10-10).

## Bus counters

The counters start at 0 at boot and return to 0 on a restart. On a newly added node they are
disabled in Home Assistant until you enable them on the device page ([Diagnostics](#diagnostics)).

| Counter | Counts |
|---|---|
| Bus checksum errors | frames from the unit that arrived with a bad checksum |
| Bus reply timeouts | frames of any kind the unit did not answer within 500 ms |
| Unanswered commands | the reply timeouts that were commands. One command lost three times counts 3 |
| Bus link losses | times the link was declared lost: five status polls in a row without a valid reply |
| Command retries | commands sent again, for either reason in [confirm and retry](#confirm-and-retry) |
| Failed commands | commands given up on after the last re-send, plus commands dropped at a link loss, and a special-mode write the queue could not take |

How to read them:

- All zero, bus link on: healthy.
- Reply timeouts rising, bus link on: the unit misses some polls. Occasional misses are
  harmless, because the next poll asks again.
- Unanswered commands rising: a send was lost. If `Failed commands` did not rise too, a
  re-send landed and nothing was lost in the end.
- Link losses rising: the unit stopped answering for at least five polls. Check the wiring
  and the DE pin ([the bus link stays off](FAQ-Gotchas#the-ac-bus-link-sensor-stays-off)).
- Checksum errors rising: corrupt bytes on the wire. Usually wiring, a floating DE line or a
  marginal supply.

## Log lines

The component logs under the tag `hisense_ac`. Read the log with `esphome logs <board file>`, or
in the ESPHome dashboard. The deployed level is `INFO`.

| Line | Level | Meaning |
|---|---|---|
| `A/C bus link lost: the unit is not answering` | WARN | five status polls in a row went unanswered |
| `A/C bus link restored` | INFO | the unit answers again |
| `Frame with a bad checksum dropped (N since boot)` | WARN | a damaged reply. Counted in `Bus checksum errors` |
| `Command refused: no status from the unit yet` | WARN | a climate command arrived before the first status frame after boot |
| `Command not sent: no status from the unit yet, so the frame would carry defaults` | WARN | the same for eco and turbo, whose frame is built from the unit's status |
| `Command refused: the unit is not answering` | WARN | a command arrived while the bus link was down |
| `Target temperature refused: the unit only takes one in cool and heat` | WARN | a setpoint in auto, dry or fan-only |
| `Fan change refused: turbo, quiet or sleep is holding the fan speed` | WARN | clear the preset first |
| `Command frame dropped: TX queue full`, `Special-mode queue full, op N refused` | WARN | more commands at once than the node queues. The entity keeps its state |
| `Command frame got no reply from the unit, sending it again` | WARN | the first check of confirm and retry failed |
| `Command not taken by the unit (fields 0x...), sending it again (N of M)` | WARN | the second check failed |
| `Command taken by the unit after N re-send(s)` | INFO | a re-send landed |
| `Command not taken by the unit after N re-send(s) (fields 0x...), showing what it reports` | WARN | given up. Counted in `Failed commands` |
| `N command(s) unconfirmed or unsent when the link dropped, not sent again` | WARN | open commands dropped at a link loss |
| `A/C reports a fault: bitmap 0x...` | WARN | a real fault. `A/C fault cleared` at INFO when it goes |
| `Setup requested from the remote (0x1E reply request bits 0x..)` | INFO | the "77" gesture was seen ([Wi-Fi setup mode](Wifi-Setup-Mode)) |

To see more, raise `logger: level:` in the node's YAML. `DEBUG` adds each status change and every
command sent. `VERY_VERBOSE` adds the raw bytes of every frame in both directions, which is the
level to use when the A/C does not answer. The full table of levels is in
`firmware/esphome/README.md` ("Log levels").

## Confirm and retry

The node checks each command twice.

| Check | When | If it fails |
|---|---|---|
| The unit replied to the frame | within the 500 ms reply window | the frame is sent again in the next bus cycle, ahead of anything queued behind it. Three sends at most, and only while the unit answers its status poll |
| The unit's status shows what was asked | the first status frame about 4 s after the frame left the wire | power, mode, setpoint, fan and swing are sent again, twice at most. Eco, turbo, quiet and sleep are planned again from what the unit reports, once, after the 10 s special-mode pacing |

After the last re-send the node logs a warning, counts the command in `Failed commands`, and lets
the entities show what the unit reports. A newer command replaces the one being checked. A lost
bus link drops every open command.

A mode request to a unit that is off is one frame that carries the mode and power-on together, as
the vendor module sends it. If the unit is still off at the check, the re-send falls back to two
frames, power-on and then the mode.

Limits:

- Some differences are never re-sent, because they are the unit's own behaviour: its setpoint in
  auto, dry and fan-only, the mode, setpoint and fan that turbo forces, the fan under quiet or a
  sleep profile, and anything except power on a unit that reports off.
- A value that is neither the old one nor the requested one is taken as someone using the remote.
  The command is dropped without a failure count.
- Display and beeper are not confirmed. The unit reports neither.
- The acknowledgement byte in the unit's reply is logged and nothing is decided on it. Its layout
  is read from the vendor image and has not been captured on this bus. The status check is what
  tells a command that took from one that did not.
- Commands older than 20 s are not sent again.
- The one-frame power and mode bytes for auto, dry and fan-only have not been captured from the
  vendor module. Only cool and heat appear in its image. The others follow the same rule and are
  marked `VERIFY` in the source. The two-frame fallback covers a unit that does not take them.
- Hardware coverage is thin. Before this existed, runs on two units failed about one check in 52,
  each time with exactly one unanswered command (pull request #179). On the two units that then
  ran the new code, a full actuation run showed no retries (maintainer's report, 2026-10-10), so
  the re-send path has not been seen to fire on a real unit. It is covered by host tests and by
  the host end-to-end test ([Testing and QA](Testing-and-QA)).

The rules are pure functions in `firmware/esphome/components/hisense_ac/hisense_confirm.h`
(`intent_expected_fields()`, `confirm_decision()`), carried out by the command tracker in
`hisense_commands.h`.
