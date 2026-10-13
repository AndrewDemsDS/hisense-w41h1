# Climate Groups

How to drive several A/Cs as one thermostat. Every unit exposes the same modes, fan names and
presets, so a Home Assistant climate group can set them together: one setpoint, one mode, one fan
speed and one preset for the whole house.

The group only keeps the units' settings in step. Each A/C still regulates on its own indoor
sensor.

## What you need

| Piece | Version | Why |
|---|---|---|
| Home Assistant | 2026.3 or later | required by Climate Group Helper |
| [Climate Group Helper](https://github.com/bjrnptrsn/climate_group_helper) | 1.10.0 or later | the group itself, from the HACS default list |
| This firmware | 2.0.0 (current `main`) | special modes as climate presets, and the fan names below |

A unit that still runs the retired Matter firmware can join the same group
([how](Matter-Firmware-Retired#until-you-move-a-unit)).

## The shared vocabulary

A group offers only what all its members agree on, so matching names are what make the whole set
available. Every unit exposes these:

| Attribute | Values |
|---|---|
| HVAC modes | `off`, `cool`, `heat` (heat-pump units), `auto`, `dry`, `fan_only` |
| Fan modes | `auto`, `low`, `medium_low`, `medium`, `medium_high`, `high` |
| Presets | `none`, `eco`, `quiet`, `turbo`, `eco_quiet`, `sleep_general`, `sleep_old`, `sleep_young`, `sleep_kids`, `eco_sleep_general`, `eco_sleep_old`, `eco_sleep_young`, `eco_sleep_kids` |
| Swing | `off`, `vertical` |
| Setpoint | 16 to 32 °C, step 1 |

A unit that lacks a capability (a cooling-only unit, no quiet flag) offers fewer values, and the
group narrows to match.

## Set it up

1. HACS → search **Climate Group Helper** → download → restart Home Assistant.
2. Settings → Devices & services → Helpers → **Create helper** → **Climate Group**.
3. Name it (for example `House AC`) and add each node's `climate` entity.
4. Open the new helper's options, turn on **advanced mode**, save, and open the options again to see
   the sync and timing sections. Apply the settings below.

## Recommended settings

These are the values the group was validated with.

| Option | Value | Why |
|---|---|---|
| Feature strategy | `intersection` | offer only what every member can do |
| Target temperature average / rounding | `median` / `integer` | the units take whole degrees; a 21.5 target would read as a permanent disagreement |
| Current temperature average | `mean` | or add a room sensor as the group temperature |
| Sync mode | `mirror` | a change on any unit (remote, app, dashboard) spreads to the others |
| Sync attributes | `hvac_mode`, `temperature`, `fan_mode` | add `preset_mode` once you have watched presets settle in your house |
| Debounce delay | 1.0 s | rapid `+`/`-` taps become one command |
| Retry attempts / delay | 2 / 1.0 s | all members are local |
| Force retry | off | local members report their state reliably |
| Member command delay | 0 s | |
| UI grace period | 5 s | the firmware holds a commanded value for 4 s before reading back |

## How it behaves

- Mirror means the whole house. Switching one unit off with its remote switches every member
  off. Use the helper's member isolation rules if a room should opt out.
- Presets settle slowly. The A/C ignores a special-mode command that lands within about 8 s of
  the previous one, so the firmware spaces them 10 s apart and a combined preset such as
  `eco_sleep_old` takes about 10 s. The entity keeps showing the requested preset until it has
  landed, so a mirroring group does not copy a half-applied state.
- Some modes own the fan. While turbo, quiet or a sleep profile is active, a different fan speed
  is refused: the A/C would overwrite it a second later. Clear the preset first.
- Turbo moves the setpoint. The A/C forces cooling and 16 °C when turbo engages, and a mirroring
  group spreads that setpoint too.
- Fan writes while off do nothing. Units ignore fan changes while powered off, so a fan change
  made on a group that is off is not applied until it is on.

## Wall thermostat

A group is a single climate entity, so anything that controls one climate entity controls the
house: the HA thermostat card, voice assistants, or a wall panel running ESPHome that targets the
group.

What a single unit exposes and how its presets behave: [Everyday Control](Everyday-Control).
Removing a capability a unit lacks: [ESPHome Firmware](ESPHome-Build#capability-gating).
