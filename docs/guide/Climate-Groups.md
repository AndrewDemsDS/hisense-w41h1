# Climate Groups

Several A/Cs, one thermostat. With every unit exposing the same climate vocabulary, a Home Assistant
climate group can drive them together: one setpoint, one mode, one fan speed, one preset for the
whole house. This works across firmwares, so an ESPHome node and a Matter node can sit in the same
group.

The group is a control layer, not a new control loop. Each A/C still regulates on its own indoor
sensor; the group keeps their settings in step.

## What you need

| Piece | Version | Why |
|---|---|---|
| Home Assistant | 2026.3 or later | required by Climate Group Helper |
| [Climate Group Helper](https://github.com/bjrnptrsn/climate_group_helper) | 1.10.0 or later | the group itself, from the HACS default list |
| ESPHome build (this repo) | current `main` | special modes as climate presets, fan names below |
| Matter builds + [hisense-unified-ac](https://github.com/AndrewDemsDS/hisense-unified-ac) | 1.4.0 or later | six-step fan names on the Matter path |
| AmebaZ2 firmware | 1.3.38 or later | in-between fan speeds stick (earlier images bump them one step) |

ESP32 Matter builds need no firmware change for grouping.

## The shared vocabulary

Every unit, whatever its firmware, exposes these names. A group offers only what all its members
agree on, so matching names is what makes the whole set available.

| Attribute | Values |
|---|---|
| HVAC modes | `off`, `cool`, `heat` (heat-pump units), `heat_cool`, `dry`, `fan_only` |
| Fan modes | `auto`, `low`, `medium_low`, `medium`, `medium_high`, `high` |
| Presets | `none`, `eco`, `quiet`, `turbo`, `eco_quiet`, `sleep_general`, `sleep_old`, `sleep_young`, `sleep_kids`, `eco_sleep_general`, `eco_sleep_old`, `eco_sleep_young`, `eco_sleep_kids` |
| Swing | `off`, `vertical` |
| Setpoint | 16 to 32 °C, step 1 |

A unit that lacks a capability (a cooling-only unit, no quiet flag) offers fewer values, and the
group narrows to match.

## Pick the right member entities

| Firmware | Add this entity | Not this |
|---|---|---|
| ESPHome | the node's `climate` entity | |
| ESP32 Matter, AmebaZ2 Matter | the **hisense-unified-ac** climate entity | the raw Matter climate: it has no fan, swing or presets, and reports a 7 to 35 °C range |

## Set it up

1. HACS → search **Climate Group Helper** → download → restart Home Assistant.
2. Settings → Devices & services → Helpers → **Create helper** → **Climate Group**.
3. Name it (for example `House AC`) and add the member entities from the table above.
4. Open the new helper's options, turn on **advanced mode**, save, and open the options again to see
   the sync and timing sections. Apply the settings below.

## Recommended settings

These are the values the group was validated with on a mixed ESPHome + Matter house.

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
| UI grace period | 5 s | the ESPHome build holds a commanded value for 4 s before reading back |

## How it behaves

- **Mirror means the whole house.** Switching one unit off with its remote switches every member
  off. That is the point of a group; use member isolation rules if a room should opt out.
- **Presets settle slowly.** The A/C ignores a special-mode command that lands within about 8 s of
  the previous one, so the ESPHome build spaces them 10 s apart and a combined preset such as
  `eco_sleep_old` takes about 10 s. The entity keeps showing the requested preset until it has
  landed, so a mirroring group does not copy a half-applied state.
- **Some modes own the fan.** While turbo, quiet or a sleep profile is active, a different fan speed
  is refused: the A/C would overwrite it a second later. Clear the preset first.
- **Turbo moves the setpoint.** The A/C forces cooling and 16 °C when turbo engages, and a mirroring
  group spreads that setpoint too.
- **Fan writes while off do nothing.** Units ignore fan changes while powered off, so a fan change
  made on a group that is off is not applied until it is on.

## Wall thermostat

A group is a single climate entity, so anything that controls one climate entity controls the
house: the HA thermostat card, voice assistants, or a wall panel running ESPHome that targets the
group.

See also: [Everyday Control](Everyday-Control) for what a single unit exposes, and
[ESPHome Build](ESPHome-Build) for the preset details and YAML capability gating.
