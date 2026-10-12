# Everyday Control

Once it is in Home Assistant, the A/C is a normal local device. This page covers what shows up,
the special modes, and the dashboard card.

## What you get

The node needs no companion integration: the A/C arrives as **one climate entity** with everything
else as separate entities on the same device.

| Entity | What you use it for |
|---|---|
| climate | power, mode (auto / cool / heat / dry / fan only), setpoint 16 to 32 °C, fan speed (auto / low / medium_low / medium / medium_high / high), swing, and the special modes as presets |
| Eco, Turbo, Quiet, Panel display switches | the special modes, written straight to the A/C |
| Beeper switch | off stops the unit beeping for commands from this node (the remote still beeps). Stored on the node, on by default |
| Sleep profile select | Off / General / Old / Young / Kids |
| Outdoor temperature, Coil temperature, Compressor frequency | what the unit is doing |
| Power, Voltage, Current, Energy today | add **Energy today** to the HA Energy dashboard |
| AC bus link, Fault, per-fault and capability flags | diagnostics; `AC bus link` off means the node cannot hear the A/C |
| Bus checksum errors, Bus reply timeouts, Unanswered commands, Command retries, Failed commands, Bus link losses, AC device type | bus health, counted since boot ([Entities and Diagnostics](Entities-and-Diagnostics#bus-counters)) |

A command the unit misses is sent again by the node, so a write can take a few seconds longer to
settle than it used to. If the unit never takes it, the entity goes back to what the unit reports
and `Failed commands` rises
([how it works and its limits](Entities-and-Diagnostics#confirm-and-retry)).

The setpoint is only honored in cool and heat. In auto, dry and fan-only the unit keeps its own,
and the entity refuses the write.

Eco and Turbo are exclusive on the A/C itself, so switching one on can turn the other off. While
turbo, quiet or a sleep profile is active they own the fan, and a fan change is refused. The
capability flags tell you which features your unit has; delete the entities it lacks from
the YAML ([ESPHome Build](ESPHome-Build#capability-gating-is-a-yaml-decision)).

A dashboard card for it, with the special modes as tiles beside the thermostat:

```yaml
type: vertical-stack
cards:
  - type: thermostat
    entity: climate.air_conditioner
    features:
      - type: climate-hvac-modes
      - type: climate-fan-modes
        style: icons
      - type: climate-swing-modes
        style: icons
  - type: grid
    columns: 4
    square: false
    cards:
      - type: tile
        entity: switch.air_conditioner_eco
      - type: tile
        entity: switch.air_conditioner_turbo
      - type: tile
        entity: switch.air_conditioner_quiet
      - type: tile
        entity: select.air_conditioner_sleep_profile
```

Entity IDs follow the device name (`friendly_name` in `w41h1.yaml`); check yours under the device
page if you renamed it.

With the presets in the thermostat card itself, the tiles are optional:

```yaml
type: thermostat
entity: climate.air_conditioner
features:
  - type: climate-hvac-modes
  - type: climate-fan-modes
    style: icons
  - type: climate-swing-modes
    style: icons
  - type: climate-preset-modes
    style: dropdown
```

Several units can be driven as one thermostat: [Climate Groups](Climate-Groups). Every entity and
the diagnostics are listed in [Entities and Diagnostics](Entities-and-Diagnostics).
