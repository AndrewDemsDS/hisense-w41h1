# 09: Energy monitoring (Watts + kWh), like the cloud app

Goal: expose live **power (W)** and cumulative **energy (kWh)** for the A/C, native to
Home Assistant's Energy Dashboard, matching what the Hisense cloud shows, but computed
locally from the RS-485 telemetry we already decode.

## 0. How the cloud actually does it (and what "accurate" means here)

These inverter splits almost never carry a revenue-grade meter. The cloud number is one of:
- **(a)** a value the indoor MCU reports (it senses DC-bus / compressor current for inverter
  control), or
- **(b)** a **model**: power estimated from compressor frequency + mode.

Our telemetry map (see `INTEGRATION.md §telemetry`, mapped 2026-07-07) suggests this unit
**does sense both voltage and current**, so we can do real `P = V·I` rather than a pure model.
Either way the figure is an **estimate**; one calibration pass against a plug-in meter makes it
trustworthy (§4). The cloud's is an estimate too, ours can be at least as good, and local.

## 1. What we already have (no new decode needed)

From the 3-state correlation (cool 60 Hz → off 0 Hz → heat 50 Hz):

| Byte | Field | Confidence | Use |
|---|---|---|---|
| **@50** | **supply voltage (V)** | High, reads 218 V loaded / 222 V idle (sags under load), matches ~230 V mains | `V` term |
| **@55 (=@145)** | phase/bus current A | Med, zeros when compressor off, tracks load | candidate `I` |
| **@56 (=@144)** | phase/bus current B | Med | candidate `I` / second leg |
| **@60** | active current / power reading | Med | candidate `I` |
| **@42** | compressor actual freq (Hz) | Confirmed | model input / cross-check |
| **@41/@43** | compressor target freq (Hz) | Confirmed | – |
| **@45** | outdoor coil temp (°C) | Confirmed (reverses cool/heat) | diagnostic (COP/efficiency later) |

Unknowns to resolve by calibration: the **scale** of the current byte(s) (raw → amps) and the
**power factor** (inverter input PF is ~0.6–0.98 depending on load).

## 2. Power model: two paths, pick by what calibration shows

**Path A, direct metering (preferred if it validates):**
```
P_watts = (V=@50) × (I=k·@55) × PF
```
We likely have real V and I. Solve `k` (current scale) and `PF` from the calibration points.
Most accurate, responds to real voltage sag, fan load, part-load PF.

**Path B, frequency model (robust fallback):**
```
P_watts = a·comp_freq(@42) + b·fan_level + P_standby
```
Fit `a, b, P_standby` from the same points. Doesn't depend on trusting the current bytes;
this is what most community inverter-A/C energy integrations use.

**Plan:** implement both, calibrate both against the meter, ship whichever tracks better
(likely A, with B as a sanity clamp / fallback when current reads implausibly).

## 3. Matter exposure (all SDK-supported, HA-native)

The SDK ships `ElectricalPowerMeasurement (0x0090)`, `ElectricalEnergyMeasurement (0x0091)`,
and the **Electrical Sensor** device type (`0x0510`). HA's Matter integration turns these into
a **power sensor (W)** and an **energy sensor (kWh)** that feed the Energy Dashboard directly,
no mfg attrs, no HA template math.

Add a **new endpoint (ep8) = Electrical Sensor (0x0510)** carrying:
- `ElectricalPowerMeasurement`:
  - `ActivePower` (0x0008, `power_mw`) ← computed P × 1000
  - `Voltage` (0x0004, `voltage_mv`) ← @50 × 1000  (transparency)
  - `ActiveCurrent` (0x0005, `amperage_ma`) ← calibrated I × 1000
  - `Frequency` (0x000E) ← optional, compressor Hz for visibility
- `ElectricalEnergyMeasurement` (features CUME+IMPE):
  - `CumulativeEnergyImported` (0x0001, mWh) ← running integral of P·dt

(Alternative to a new endpoint: hang both clusters on **ep1**. A new endpoint is the correct
device-type semantics but carries the endpoint-add build risk we already navigated for ep4–7;
if codegen fights it, fall back to ep1.)

**⚠️ Integration reality (found 2026-07-07):** `ElectricalPowerMeasurement` (0x0090) and
`ElectricalEnergyMeasurement` (0x0091) are **Delegate/Instance-based** clusters, NOT plain
ember-RAM. The server reads `Accuracy`, `NumberOfMeasurementTypes`, and the values through a
C++ `Delegate` (cf. `connectedhomeip/.../electrical-power-measurement-server.cpp` +
`examples/energy-management-app/.../ElectricalPowerMeasurementDelegate.{h,cpp}`). So exposure is
**not** the `.zap`-attr + `emberAfWriteAttribute` pattern used for ep4–7, it needs:
1. A minimal `Delegate` (return ActivePower/Voltage/ActiveCurrent from the driver; one-entry
   `Accuracy`; `NumberOfMeasurementTypes=1`), ~120–180 LOC adapted from the reference.
2. Instantiate `ElectricalPowerMeasurement::Instance` (+ `ElectricalEnergyMeasurement::Instance`)
   on the endpoint in the app init (near `matter_drivers` init), fed each status poll.
3. The `.zap` endpoint (Electrical Sensor 0x0510) to attach them.
This is the meatiest step in the whole feature, a focused C++ integration, own build + flash to
validate (HA power sensor cross-checked vs the panel meter). The **computation is already done +
host-tested** (`power_estimate.h`); this is purely the Matter surface.

Compressor Hz and outdoor-coil temp have **no standard cluster**: keep them as the existing
mfg attrs (local diagnostics) or add a `TemperatureMeasurement` endpoint for the coil if wanted.

## 4. Calibration procedure: **clamp meter**

Reference instrument: a **clamp ammeter** on the A/C live conductor. Two sub-cases:
- **True-power clamp** (reads W / PF / kWh): gives real W directly → best; calibrate P against it.
- **Current-only clamp** (reads A): gives us the true input current → **directly calibrates the
  @55 byte→amps scale**. We then form apparent power `S = V(@50)·I`, and real power
  `P = S·PF`. This unit's inverter almost certainly has **active PFC → assume PF ≈ 0.95**
  (tunable); at deep part-load PF drops, so the freq-model (Path B) is the sanity clamp there.

⚠️ **Safety:** clamping the live conductor means accessing the mains lead into the A/C. Mains
voltage, clamp *around a single insulated conductor* (never bare metal), power the readings from
the A/C's own supply, and if the terminal block isn't safely reachable, clamp at the isolator/plug
lead instead. Don't open anything live you're not comfortable with.

**Steps** (interactive, ~15 min):
1. Restart `decode_ac_frames.py` logging while I mark boundaries.
2. Hold ~4 states, read the clamp at each: **off/standby**, **low-cool (~40 Hz)**,
   **hard-cool/turbo (~69 Hz)**, **heat (~50 Hz)**. Tell me the amps (and W/PF if the clamp shows
   them) at each; I diff the frame to pair your reading with @50/@55/@56/@60/@42.
3. `tools/fit_power.py` (to be written) fits the @55→A scale (and Path-B coefficients) by
   least-squares.
4. Bake coefficients into the firmware `power_estimate.h`. Optional later: expose them as
   writable mfg attrs so you can retune from chip-tool without a reflash.

## 4a. Calibration (physical-meter validated: PD3-63VA panel meter, L3)

Power model, from three **timestamp-synced** points (photo EXIF ↔ logger `[HH:MM:SS]`), A/C on L3, power =
`V · ΔI(L3−baseline) · PF(0.95)`:

| Time | L3 A | ΔI (A/C) | P (W) | @55 | @42 Hz |
|---|---|---|---|---|---|
| 22:07:36 | 1.1 | 0.0 | 0 | 0 | 0 |
| 22:12:32 | 2.3 | 1.2 | 268 | 8 | 31 |
| 22:10:48 | 5.3 | 4.2 | 934 | 15 | 68 |

`@55` is **√power**, not linear amps, a linear fit gives incompatible slopes (33 vs 62 W/count),
but **quadratic-through-origin fits both load points to <1 %**:

```
P_watts ≈ 4.15 · @55²        (k = 4.15 W per count², PF 0.95 baked in)
  @55=8  -> 266 W (meter 268)   @55=15 -> 934 W (meter 934)
```
Derived current for transparency: `I ≈ P/(V·PF)` → @55=15 → 4.47 A (meter 4.2), @55=8 → 1.27 A
(meter 1.2). `@60` gives the same curve (~0.66·@60²) as a cross-check. **Caveat:** two non-zero
points, the quadratic is strongly supported but a 4th steady point would bulletproof it (tap now
removed, so deferred). Voltage term uses @50 (coarse ~220; the meter's live 234–236 V says @50
reads ~6 % low, minor, since current dominates).

## 4b. Compute location: **firmware**

The driver computes P and the running energy integral and publishes them via the standard
`ElectricalPowerMeasurement` / `ElectricalEnergyMeasurement` clusters (§3). HA reads them as-is,
single source of truth, Energy-Dashboard-native. The calc lives in a **pure, host-testable**
`power_estimate.h` (mirrors `matter_aircon_map.h`) so the model is unit-tested without hardware;
only the calibration constants wait on §4.

## 5. Energy accumulation (kWh)

Integrate in firmware on each status poll (~1 Hz): `energy_mWh += P_watts · dt_hours · 1000`,
publish to `CumulativeEnergyImported`. Persist periodically (NVS) so a reboot doesn't zero it,
or accept reset-on-reboot for v1 and let HA's long-term stats hold history. (HA can also derive
kWh from the power sensor via a Riemann-sum helper, so firmware energy is optional but cleaner.)

## 6. Implementation phases

- **P1: Calibrate.** Meter + 4-point capture → `fit_power.py` → coefficients. *(gates accuracy;
  do first)*
- **P2: Firmware compute.** Add `power_estimate.h` (pure, host-testable): `P(v,i,freq,fan)` +
  energy integrator. Unit-test like `matter_aircon_map.h`.
- **P3: Matter wire.** ep8 Electrical Sensor in the `.zap`; glue feeds ActivePower/Voltage/
  Current + CumulativeEnergyImported each downlink. `clean_matter_libs`-class rebuild.
- **P4: HA.** Confirm the power + energy sensors appear; add to Energy Dashboard.
- **P5: Validate.** Compare our W vs the cloud app across states; tune if needed.

## 7. Open questions

- **Reference meter available?** Determines whether we calibrate (accurate) or ship an
  uncalibrated first-guess. This is the gating input.
- **Which current byte** (@55 vs @56 vs @60) is the true input current, and its scale, resolved
  by P1.
- **New endpoint vs ep1** for the electrical clusters: decide at P3 based on codegen behaviour.
- Persist energy across reboot (NVS) now, or defer to HA long-term stats.
