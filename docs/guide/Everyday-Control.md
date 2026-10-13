# Everyday Control

What you get in Home Assistant once the node is adopted, how each control behaves, and the limits
worth knowing before you build automations on it. The full list of entities, with their ids and
the diagnostics, is the reference page [Entities and Diagnostics](Entities-and-Diagnostics).

## The device page

The A/C is one device, `Air Conditioner` unless you renamed the node. Home Assistant sorts its
entities into groups:

| Group | Entities |
|---|---|
| Controls | the climate entity, and the `Sleep profile` select |
| Sensors | `Outdoor temperature`, `Coil temperature`, `Compressor frequency`, `Power`, `Voltage`, `Current`, `Energy today`, `Aux heat relay`, `Fault` |
| Configuration | the `Eco`, `Turbo`, `Quiet`, `Panel display` and `Beeper` switches |
| Diagnostic | the bus link and counters, capability and fault flags, uptime, Wi-Fi signal, `Start Wi-Fi setup` |

## The climate entity

| Control | Values |
|---|---|
| Mode | `off`, `cool`, `heat`, `fan_only`, `dry`, `auto` |
| Target temperature | 16 to 32 °C in whole degrees |
| Fan | `auto`, `low`, `medium_low`, `medium`, `medium_high`, `high` |
| Swing | `off`, `vertical` |
| Preset | `none`, `eco`, `quiet`, `turbo`, `eco_quiet`, `sleep_general`, `sleep_old`, `sleep_young`, `sleep_kids`, `eco_sleep_general`, `eco_sleep_old`, `eco_sleep_young`, `eco_sleep_kids` |

It also shows the room temperature from the unit's own sensor and what the unit is doing (cooling,
heating, idle and so on).

## Special modes

Eco, turbo, quiet and the sleep profiles can be set in two ways, and both drive the same thing on
the A/C:

- as a preset on the climate entity, which is what a thermostat card and a climate group use,
- with the `Eco`, `Turbo` and `Quiet` switches and the `Sleep profile` select (`Off`, `General`,
  `Old`, `Young`, `Kids`), which are handy as dashboard tiles.

Keep whichever you use. The others can be removed from the YAML
([capability gating](ESPHome-Build#capability-gating)).

## Other controls

- Panel display switches the display on the indoor unit.
- Beeper decides whether the unit beeps when it takes a command from this node. The remote
  still makes it beep. The setting is stored on the node and is on by default
  ([details](Entities-and-Diagnostics#beeper)).
- Energy today is the day's energy, summed from the firmware's power estimate. Add it to the
  Home Assistant Energy dashboard. How the estimate is made and how close it is:
  `firmware/docs/09-energy-monitoring.md`.

## Limits and behaviour to know

- The setpoint only applies in cool and heat. In auto, dry and fan-only the unit keeps its
  own, and the entity refuses the write.
- Turbo, quiet and the sleep profiles hold the fan. While one is active, a fan change is
  refused. Clear the preset first. Under quiet the fan reads `low`: the A/C only reaches quiet
  through its mute flag, so it is a preset and not a fan speed.
- Turbo takes over. The A/C forces cooling at 16 °C on high fan while turbo is on.
- Eco and turbo exclude each other on the A/C, so switching one on can turn the other off.
- Special modes settle slowly. The A/C ignores a special-mode command that lands too soon
  after the previous one, so the node sends them 10 s apart. A combined preset such as
  `eco_sleep_old` takes about 10 s. The entity shows the requested preset meanwhile.
- A command is checked. The node compares each command with what the unit reports about 4 s
  later and sends it again when the unit missed it. A write can therefore take a few seconds to
  settle. If the unit never takes it, the entity goes back to what the unit reports and
  `Failed commands` rises ([confirm and retry](Entities-and-Diagnostics#confirm-and-retry)).
- Commands need a unit that answers. Right after boot, before the first status has arrived,
  and while the unit is not answering, a command is refused and the entity keeps its state.
- A unit that is off ignores everything except power. A fan change made while it is off is
  not applied.
- Horizontal swing is not offered by default. Most of these units have no motor for it
  ([option](ESPHome-Build#capability-gating)).
- Outdoor and coil temperature read "unknown" for a while after a power cut
  ([why](Entities-and-Diagnostics#behaviour-worth-knowing)).

## Dashboard cards

Entity ids follow the device name. With the default name the climate entity is
`climate.air_conditioner`. Check yours on the device page if you renamed the node.

The thermostat with the presets in it:

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

Or the special modes as tiles under the thermostat:

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

Several units can be driven as one thermostat: [Climate Groups](Climate-Groups).
