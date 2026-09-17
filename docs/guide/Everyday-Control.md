# Everyday Control

Once it is in Home Assistant, the A/C is a normal local device. This page covers what shows up on
each firmware, the special modes, and the dashboard card.

## ESPHome build

The ESPHome node needs no companion integration: the A/C arrives as **one climate entity** with
everything else as separate entities on the same device.

| Entity | What you use it for |
|---|---|
| climate | power, mode (auto / cool / heat / dry / fan only), setpoint 16 to 32 °C, fan speed (auto / low / medium_low / medium / medium_high / high), swing, and the special modes as presets |
| Eco, Turbo, Quiet, Panel display switches | the special modes, written straight to the A/C |
| Sleep profile select | Off / General / Old / Young / Kids |
| Outdoor temperature, Coil temperature, Compressor frequency | what the unit is doing |
| Power, Voltage, Current, Energy today | add **Energy today** to the HA Energy dashboard |
| AC bus link, Fault, per-fault and capability flags | diagnostics; `AC bus link` off means the node cannot hear the A/C |

Eco and Turbo are exclusive on the A/C itself, so switching one on can turn the other off. The
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

Everything below this point is about the two **Matter** builds.

## What HA exposes natively (Matter)

HA's Matter integration presents the raw W41H1 as **several** entities on one device:

- a **climate** entity (HVAC mode + setpoint),
- a **separate fan** entity (speed + oscillate/swing),
- a redundant device-mandated **Power** switch,
- unnamed **On/Off switches** for the special modes, plus a sleep select.

Usable, but split across tiles. HVAC modes: off / cool / heat / auto / dry / fan-only (the firmware
advertises Heat, Cool and Auto in the Thermostat FeatureMap; dry and fan-only need the companion
integration below). Setpoint is only honored in **cool/heat**. A temp change in dry / fan /
auto / off is a no-op.

## The unified climate integration (recommended for Matter)

`integrations/hisense-unified-ac` is a HACS custom integration that **merges those entities into one
climate entity**, so you get a single Thermostat card with the special modes as presets. Details:
`integrations/hisense-unified-ac/README.md`.

It gives you:
- HVAC: off / cool / heat / auto / dry / fan-only
- Fan: auto / low / medium_low / medium / medium_high / high (1.4.0 and later)
- Swing: off / vertical
- Presets: **none / eco / quiet / turbo / eco_quiet / sleep_\* / eco_sleep_\*** (folded in from the special-mode switches + sleep select)
- Setpoint gated to cool/heat (a temp change elsewhere shows no target and is a no-op)

**Install (HACS):**
1. HACS → Integrations → ⋮ → **Custom repositories** → add the repo, category **Integration**.
2. Install **Hisense W41H1 Unified AC**, restart HA.
3. Settings → Devices & Services → **Add Integration** → *Hisense W41H1 Unified AC* → pick the A/C's
   native Matter **climate** entity (fan / special-mode switches / sleep select auto-detect). Repeat
   per A/C.

Then hide the now-redundant native entities (the Power switch and the raw special-mode switches).

> **Prerequisite:** dry / fan-only / single-setpoint must be unlocked on the native climate. HA
> gates those on a vendor allow-list; the companion
> [`ha-matter-extra-hvac-modes`](https://github.com/AndrewDemsDS/ha-matter-extra-hvac-modes)
> integration (domain `matter_extra_hvac_modes`) lifts that gate for test-vendor `0xFFF1` devices.

The ESPHome climate entity and the unified climate entity expose the same names, so units on either
firmware can be controlled together as one thermostat: see [Climate Groups](Climate-Groups).

## Special modes (Eco / Turbo / Mute-Quiet / Sleep)

On the **raw** Matter device these live in a manufacturer cluster (`0xFFF1FC00`) that
`python-matter-server` can't *write*: its custom-cluster path is read/poll only, so a direct
`matter_write` template switch does **not** work. The unified integration is what makes them
controllable (as presets); on the plain device they surface only as the auto-detected switches/select.
Background and the guard/interlock matrix (Eco⊕Turbo exclusivity, forced fan states, etc.):
`firmware/docs/05-ha-control-and-native-ui.md`.

## Dashboard card

Both READMEs converge on a built-in **vertical-stack** with a **Thermostat** card plus fan/swing
tiles (a HA integration can't ship a native Lovelace card, so you add it yourself). With the unified
entity it collapses to one thermostat card with HVAC modes, fan/swing icons, and a preset dropdown:

```yaml
type: thermostat
entity: climate.your_ac_unified
features:
  - type: climate-hvac-modes
  - type: climate-fan-modes
    style: icons
  - type: climate-swing-modes
    style: icons
  - type: climate-preset-modes
    style: dropdown
```

The full ~200-line native (no-JS) layout with the link/health chip and the guard rationale is in
`firmware/docs/05-ha-control-and-native-ui.md`. Point `entity` at your unit, one card per A/C.

> Outdoor temperature (ep2) and coil temperature (ep8) are safe to add as tiles on the AmebaZ2
> build. On the ESP32 build those two currently read null in matter-server (known issue, see
> [ESP32 Replacement Build](ESP32-Replacement-Build#status--remaining-work)).
