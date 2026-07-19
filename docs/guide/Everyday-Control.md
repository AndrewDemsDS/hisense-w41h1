# Everyday Control

Once commissioned, the A/C is a normal local Home Assistant device. This page covers what shows up,
how to collapse it into one clean entity, the special modes, and the dashboard card.

## What HA exposes natively

HA's Matter integration presents the raw W41H1 as **several** entities on one device:

- a **climate** entity (HVAC mode + setpoint),
- a **separate fan** entity (speed + oscillate/swing),
- a redundant device-mandated **Power** switch,
- unnamed **On/Off switches** for the special modes, plus a sleep select.

Usable, but split across tiles. HVAC modes: off / cool / heat / dry / fan-only (Auto needs a
firmware FeatureMap bit). Setpoint is only honored in **cool/heat**. A temp change in dry / fan /
auto / off is a no-op.

## The unified climate integration (recommended)

`integrations/hisense-unified-ac` is a HACS custom integration that **merges those entities into one
climate entity**, so you get a single Thermostat card with the special modes as presets. Details:
`integrations/hisense-unified-ac/README.md`.

It gives you:
- HVAC: off / cool / heat / auto / dry / fan-only
- Fan: auto / low / medium / high
- Swing: off / vertical
- Presets: **eco / quiet / turbo / sleep** (folded in from the special-mode switches + sleep select)
- Setpoint gated to cool/heat (a temp change elsewhere shows no target and is a no-op)

**Install (HACS):**
1. HACS → Integrations → ⋮ → **Custom repositories** → add the repo, category **Integration**.
2. Install **Hisense W41H1 Unified AC**, restart HA.
3. Settings → Devices & Services → **Add Integration** → *Hisense W41H1 Unified AC* → pick the A/C's
   native Matter **climate** entity (fan / special-mode switches / sleep select auto-detect). Repeat
   per A/C.

Then hide the now-redundant native entities (the Power switch and the raw special-mode switches).

> **Prerequisite:** dry / fan-only / single-setpoint must be unlocked on the native climate. HA
> gates those on a vendor allow-list; the companion `matter_ac_unlock` component adds the W41H1's
> test IDs `0xFFF1/0x8001`.

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

> Don't place the ep2 Humidity / ep3 Temperature tiles; they report NULL (known gap).
