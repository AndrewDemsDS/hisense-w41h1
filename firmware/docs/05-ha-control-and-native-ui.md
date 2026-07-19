# W41H1: HA control layer: safety guards + native (no-JS) dashboard

Design doc for locally driving the de-clouded Hisense AEH-W41H1 over Matter (via
python-matter-server, HA 2026.6) with **built-in Lovelace cards only** and a correct set of
safety guards. Produced by a 4-lens research fan-out (stock cloud plugin `hisense_ac_plugin`,
our RS-485 driver, stock-FW/link RE docs 09/10, HA native-card docs) + adversarial verification.

> **Superseded by a shipped integration.** The native unified dashboard/entity this doc designs
> toward now ships as **`integrations/hisense-unified-ac`**: a HACS custom integration (separate
> repo/submodule). Treat the YAML and tables below as reference *for that integration* rather than
> an open design; the feature/guard matrix (§1), the guard-ownership table (§2), and the dashboard
> YAML (§3) remain the load-bearing spec.

Three design facts this doc is built on, load-bearing for the tables below:
- **The firmware auto-powers-on on mode-select**: `matter_drivers.cpp:177-180`: a non-off
  `SystemMode` write calls `hisense_send_power(true)` **then** `hisense_flush_command()` (mode
  frame), so `climate.set_hvac_mode: cool` already emits power-on-then-mode. HA does *not* own
  auto-power-on. The empirical "wrote SystemMode, unit stayed off" is a **link-readiness** issue (or
  the on-frame being ineffective from cold-off), a HW-verify item (P4), not an HA guard.
- **Native cards call `climate.*`/`fan.*` directly**, so guard *scripts* placed in front of a write
  are unreachable. Guards can only be **(a) firmware-side** (power-on, setpoint-rides-with-mode, bus
  pacing) or **(b) reactive automations** that fire *after* the state change.
- **Specials (Eco/Turbo/Mute/Sleep) are not controllable from HA via the mfg cluster.** matter-server
  `write_attribute` resolves `ALL_ATTRIBUTES[cluster_id]` (the CHIP registry); cluster `0xFFF1FC00`
  is absent → the write returns `null`/KeyError, not a real write. Its custom-cluster mechanism
  (`custom_clusters.py`) is **read/poll only**. The native path is to re-expose these via **standard
  Matter** in firmware (Thermostat **Presets** → HA `preset_mode` for Sleep/Eco; a fan speed for
  Quiet; a boost/preset for Turbo).

There is no atomic SystemMode+setpoint write in matter-server, no decoded power bit to read back, and
`SpeedCurrent`/`PercentCurrent`/`LocalTemperature` are real Matter readbacks that update on their own
(manual resync fights them). The native card uses the entity's 16–32 `AbsMin/Max`, not a 16–30 clamp.

---

## 1. Feature matrix (validated inventory)

Command-side byte encodings are DI-tap-confirmed (`reverse-engineering/docs/10` HIL chronology, Phase D) and survive the `docs/10 §5`
"ESPHome layout isn't in stock FW" correction (that correction bites the *status* decode, not these
command bytes). Values in the cloud column are STRING property writes.

### HVAC modes
| Mode | Cloud `t_work_mode` | Our SystemMode → 0x65 byte18 | Auto-powers-on? | Temp settable | HA control |
|---|---|---|---|---|---|
| Off | `"0"` power (`t_power:"0"`) | Off → literal OFF frame | n/a | – | native `set_hvac_mode: off` |
| Cool | `"2"` | 3 → `0x50` | **yes (firmware)** | yes | native |
| Heat | `"1"` | 4 → `0x30` | **yes (firmware)** | yes | native |
| Dry | `"3"` | 8 → `0x70` | **yes (firmware)** | **no** | native |
| Fan-only | `"0"` | 7 → `0x10` | **yes (firmware)** | **no** | native |
| Auto | absent on window AC (`0,1,2,3,5`) | 1 → `0x90` (driver-reachable) | (would) | – | **needs FeatureMap Auto bit (FW)** |
| E-star/eco | `"5"` (a *mode*, unmapped in HA) | ≈ mfg-Eco special (byte33 `0x30`) | assumed | – | none today; reconcile w/ mfg-Eco (open Q) |

Setpoint rides **inside** the mode 0x65 at byte19 (`temp*2+1`); a standalone
`OccupiedCooling/HeatingSetpoint` write returns IM `0x86`: the firmware already flushes setpoint in
its own 0x65, so `climate.set_temperature` works when it lands with/after a mode. Temp is only
honored in Cool/Heat (cloud strips it in dry/fan; our range is 16–32 on the wire, model spec 16–30).

### Fan speeds (on the SEPARATE `fan.test_product_3` entity)
`SpeedSetting 1..6` → byte16 `0x03/0x0B/0x0D/0x0F/0x11/0x13` (quiet…high; `0x03` quiet is VERIFY).
Cloud exposes only 3 (`t_fan_speed 5/7/9` = low/med/high). **Conflict to resolve on HW:** the live
glue `INTEGRATION.md:127,237` maps `PercentSetting` through `mapPercentToMode()` into ~4 buckets
(off/low/med/high), *not* the 6-step `percent_to_hisense_fan()`: so the fan-speed slider may yield
4 buckets, not 6. Fan never auto-powers-on; fan is not settable in Dry; `0%` sends **AUTO fan**, not off.

### Special functions

Byte-level mfg-cluster encodings (attr IDs ↔ RS-485 command bytes) and the CompressorHz/OutdoorTemp
telemetry-gap status are owned by [docs/01](01-expose-all-clusters.md#manufacturer-cluster-phase-3--chosen-approach),
not repeated here; this table only adds the HA-facing columns (cloud mapping, interlocks, reachability).

| Special | Cloud | Our path (mfg cluster) | Interlocks (per driver) | HA today |
|---|---|---|---|---|
| Turbo | `t_super` | mfg `0x0001` | **XOR Eco** (shared byte33); forces fan-high + 16 °C | **not reachable** (mfg write broken) |
| Mute/Quiet | `t_fan_mute` | mfg `0x0002` | forces fan quiet; independent of byte33 (so it *fights* Turbo's fan-high) | **not reachable** |
| Eco/E-star | `t_work_mode:"5"` | mfg `0x0000` | XOR Turbo; turn-off may not clear on HW | **not reachable** |
| Sleep | none on window AC | mfg `0x0003` | forces fan-low | **not reachable** |
| Swing V/H | none | RockSetting bits (fan cluster, not mfg) | H-swing command **unvalidated** | native `fan.oscillate` = one collapsed toggle |

Cloud-only, absent on our hardware (drop): `ac_purify`/health (status bit always 0), `ac_8heat`,
`ac_ai`, `ac_dr`, `ac_humidity`, 8-position/follow-me swing, self-clean.

---

## 2. Guards: where each actually lives (corrected)

| # | Guard | Owner | Status |
|---|---|---|---|
| G1 | Mode-select powers the unit on | **Firmware** (`matter_drivers.cpp:177-180`) | already done; **verify on HW (P4)**: the empirical cold-off failure is the open risk |
| G2 | Setpoint rides with the mode frame (no bare 0x86 write) | **Firmware** (flushes setpoint in its own 0x65) | done; HA should still prefer `set_temperature` with `hvac_mode` so a mode is present |
| G3 | Half-duplex pacing / ~1 Hz transaction | **Firmware bus task** (send→listen ≤500 ms) | done; HA adds a light slider **debounce** (≥1.5 s) only as courtesy, cloud uses 3 s cmd-cache + 10 s switch-debounce, so lean generous |
| G4 | Link-ready gate (don't command before DevType/0x07 handshake) | **HA reactive** | **template `binary_sensor.ac_link` = `LocalTemperature` is live**; surface it; optionally block special/automation writes on it. (This is a *link* signal, not a *power* signal, there is no power readback.) |
| G5 | Reconnect resync after A/C-initiated reset/OTA/reconfig | **HA automation** | on `climate` `unavailable→available`, re-send last mode/setpoint from `input_*` mirrors; do **not** fight a reconfig |
| G6 | Special-function interlocks (Eco⊕Turbo, mode-restrict, forced fan) | **Firmware** (once specials move to standard Matter) | deferred with the specials; today unreachable so moot |
| – | Temp/fan validity per mode (no temp in dry/fan; no fan in dry) | **Firmware** should reject; HA card can't clamp | verify HA card behavior on HW; if the card leaves temp dialable in dry, add a reactive automation that reverts it |

Key point: **no imperative guard scripts sit in front of the native cards** (they can't). The firmware
is the enforcement layer; HA contributes G4 (link awareness) and G5 (reconnect resync) as reactive
automations, plus optional slider debounce.

---

## 3. Native (no-JS) dashboard

Two entities stitched with built-in cards (the climate entity has **no** `fan_mode`/`swing_mode`
attrs, so `climate-fan-modes`/`climate-swing-modes` features render empty, fan+swing come from the
fan tile). Feature strings are **hyphenated** (current docs).

```yaml
type: vertical-stack
cards:
  - type: heading
    heading: Living Room A/C

  - type: thermostat
    entity: climate.test_product_3          # rename to climate.living_room_ac
    features:
      - type: target-temperature
      - type: climate-hvac-modes
        hvac_modes: [off, cool, heat, dry, fan_only]   # +auto once the FW FeatureMap bit lands

  - type: tile
    entity: fan.test_product_3
    name: Fan
    features_position: inline
    features:
      - type: fan-speed        # slider (may resolve to ~4 buckets via mapPercentToMode — verify)
      - type: fan-oscillate    # swing (V+H collapsed into one toggle)

  - type: tile                 # explicit power, since there's no decoded power readback on the dial
    entity: switch.test_product_power_3
    name: Power

  # Link/health chip so the user knows when the bus is actually up:
  - type: tile
    entity: binary_sensor.ac_link
    name: A/C link
```

**Specials are intentionally absent** until the firmware exposes them via standard Matter, a
`matter_write` template switch for the mfg cluster does not work (see §0.3). When Sleep/Eco become
Thermostat **Presets**, add a `climate-preset-modes` feature to the thermostat card (native); Quiet is
already a fan speed; Turbo becomes a preset or a boost.

Do **not** place ep2 Humidity / ep3 Temperature tiles (report NULL).

---

## 4. Phased plan

| Phase | Deliverable | HW-on needed? |
|---|---|---|
| **P0** | Rename device → "Living Room AC" (HA device registry; NodeLabel write doesn't persist). Remove the JS `climate-cluster-card` dashboard + resource (pivot to native). | no |
| **P1** | Native `vertical-stack` dashboard (§3): thermostat + fan tile + power tile. `binary_sensor.ac_link` template. | no (renders + fires) |
| **P2** | Reactive automations: G5 reconnect-resync (last mode/setpoint mirrors via `input_*`); optional G4 link-gate + notification; optional fan-slider debounce. | authoring no; validating yes |
| **P3 (firmware/OTA)** | (a) Thermostat **Auto** FeatureMap `3 → 35` (`0x23` = kHeating\|kCooling\|**kAutoMode**); HA lists us in `SINGLE_SETPOINT_DEVICES` (via `matter_ac_unlock`) so Auto renders as a single-setpoint mode, consider the `MinSetpointDeadBand` (thermostat `0x19`) companion if CHIP/device complains → `auto` renders. (b) Re-expose specials via **standard Matter**: Sleep/Eco as Thermostat **Presets** (kPresets + populate `Presets`/`PresetTypes`), Turbo as a preset/boost, retire the mfg-cluster control surface for HA. (c) Wire explicit eco-off (`HISENSE_FEATURE_ECO_OFF`, byte33 `0x10`, via `hisense_apply_eco()`)/turbo-off/display-off frames. Rebuild + serial-bump + OTA (proven pipeline). | reflash + verify |
| **P4 (HIL)** | With the A/C **on + linked**: confirm mode-select really powers on from cold-off (the empirical failure), setpoint (no 0x86), fan slider bucket count (4 vs 6), swing, and (once P3 lands) presets. Confirm the `LocalTemperature`-live link proxy tracks the real handshake. | **yes** |

---

## 5. Open questions to settle on hardware

1. **Cold-off power-on:** does an explicit on-frame actually power a physically-off unit? (empirical
   failure; firmware already sends it, so this is an on-frame/link question, not an HA one).
2. **Fan slider granularity:** 6-step (`percent_to_hisense_fan`) vs ~4-bucket (`mapPercentToMode`),
   which does the live glue use? Reconcile the two source mappings.
3. **Temp visibility in dry/fan:** does HA's Matter climate card auto-hide `set_temperature` when the
   entity is in dry/fan, or stay dialable? (Matter advertises setpoints unconditionally.)
4. **OnOff vs SystemMode display:** with `OnOff=0` while `SystemMode=Cool`, does the climate entity
   show OFF or "cool"? (HA shows off when the OnOff switch is off, confirm.)
5. **Presets feasibility:** can the AmebaZ2 Thermostat cluster advertise kPresets + serve `Presets`
   for Sleep/Eco cleanly? (the clean native path for specials).
6. **Do specials auto-clear** on mode-change/power-off, and are Turbo/Mute truly exclusive (they use
   different frames, so their forced fan states fight)? Needed before presets encode the interlocks.
7. **Sleep enum cardinality + wire type** (on/off vs 5-value) and **eco-off/turbo-off** actually
   clearing on HW (only the `0x04` baseline is wired today).

---

*Grounding: workflow `hisense-ac-control-research` (4-lens fan-out + adversarial verify + completeness
critic), reconciled with live matter-server/HA checks. Command byte offsets are DI-tap-validated per
`reverse-engineering/docs/03` (protocol) + `docs/10` HIL chronology. This doc supersedes the JS-card direction.*

---

## Dashboard card + automation examples (#22)

Copy-paste starting points, wired to a **renamed node's** entity IDs (e.g. a device you renamed to
`living_room_ac`). A second, un-renamed node keeps the un-suffixed auto IDs (`climate.test_product`,
`switch.test_product_switch_3`, …). Matter auto-names are ugly, rename in *Settings → Devices →
Entities* and the IDs stabilize.
Vertical swing is exposed as **fan oscillation** on the `fan.*` entity (FanControl Rocking → `fan.oscillate`);
the panel **Display** is a plain switch (write-only, the A/C reports no display state back).

### Lovelace view

```yaml
type: vertical-stack
cards:
  - type: thermostat
    entity: climate.test_product_2          # mode / setpoint / fan
    name: Living Room A/C
  - type: entities
    title: Modes & fan
    entities:
      - entity: fan.test_product_2          # fan speed + Swing (oscillate)
        name: Fan / Swing
      - entity: select.test_product_sleep_2
        name: Sleep profile
      - entity: switch.test_product_switch_3_2
        name: Eco
      - entity: switch.test_product_switch_4_2
        name: Quiet
      - entity: switch.test_product_switch_5_2
        name: Turbo
      - entity: switch.living_room_living_room_ac_switch_display
        name: Panel display
  - type: glance
    title: Telemetry
    columns: 3
    entities:
      - { entity: sensor.test_product_power_climate,   name: Power }
      - { entity: sensor.test_product_voltage_2,       name: Voltage }
      - { entity: sensor.test_product_active_current_2, name: Current }
      - { entity: sensor.living_room_living_room_ac_temperature_outdoor, name: Outdoor }
      - { entity: sensor.living_room_living_room_ac_temperature_coil,    name: Coil }
      - { entity: binary_sensor.test_product_door,     name: Aux heat }
  - type: gauge
    entity: sensor.test_product_power_climate
    name: A/C power
    unit: W
    min: 0
    max: 1500
    severity: { green: 0, yellow: 800, red: 1200 }
```

### Automations

```yaml
# 1) Auto-Eco when the A/C draws a lot (curb peak power)
- alias: "A/C: Eco when power high"
  trigger:
    - platform: numeric_state
      entity_id: sensor.test_product_power_climate
      above: 900
      for: "00:02:00"
  condition:
    - condition: state
      entity_id: switch.test_product_switch_3_2
      state: "off"
  action:
    - service: switch.turn_on
      target: { entity_id: switch.test_product_switch_3_2 }

# 2) Sleep profile + dim the panel at bedtime (only if the A/C is running)
- alias: "A/C: Sleep at bedtime"
  trigger:
    - platform: time
      at: "23:00:00"
  condition:
    - condition: not
      conditions:
        - condition: state
          entity_id: climate.test_product_2
          state: "off"
  action:
    - service: select.select_option
      target: { entity_id: select.test_product_sleep_2 }
      data: { option: "General" }
    - service: switch.turn_off
      target: { entity_id: switch.living_room_living_room_ac_switch_display }

# 3) Swing on while cooling (even airflow)
- alias: "A/C: Swing while cooling"
  trigger:
    - platform: state
      entity_id: climate.test_product_2
      to: "cool"
  action:
    - service: fan.oscillate
      target: { entity_id: fan.test_product_2 }
      data: { oscillating: true }

# 4) Panel display back on in the morning
- alias: "A/C: Display on in the morning"
  trigger:
    - platform: time
      at: "07:00:00"
  action:
    - service: switch.turn_on
      target: { entity_id: switch.living_room_living_room_ac_switch_display }
```
