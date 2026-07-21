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

**Shipped:** Path A only (`power_estimate.h`, §4a). Path B was never implemented; there is no
frequency-based sanity clamp on the current reading.

## 3. Matter exposure: shipped

`ElectricalPowerMeasurement (0x0090)` ships on **ep1** (both AmebaZ2 and ESP32). The "hang it on
ep1" fallback considered below is what got built, because a new device-type endpoint carries the
same endpoint-add build risk already navigated for ep4-7. Feature set is `kAlternatingCurrent`
only (single-phase); optional attributes are `Ranges`, `Voltage`, `ActiveCurrent`.
`ActivePower`/`Voltage`/`ActiveCurrent` are fed from `power_estimate.h` each status poll, in the
cluster's native mW/mV/mA units, and read 0 while the unit is off (not used as a liveness signal).
kWh comes from HA Riemann-summing the power sensor (§5), the acceptable alternative this doc
already flagged. `ElectricalEnergyMeasurement` (0x0091, `CumulativeEnergyImported`) was never
added.

**Delegate/Instance mechanics (found 2026-07-07, still true):** `ElectricalPowerMeasurement` is a
**Delegate/Instance-based** cluster, not plain ember-RAM: the server reads `Accuracy`,
`NumberOfMeasurementTypes`, and the values through a C++ `Delegate`, so exposure is not the
`.zap`-attr + `emberAfWriteAttribute` pattern used for ep3-7. The shipped delegate lives at
`firmware/src/sdk-edits/ElectricalPowerMeasurementDelegate.{h,cpp}` (adapted from CHIP's
energy-management-app reference) and is shared as-is by the ESP32 build
(`#include <ElectricalPowerMeasurementDelegate.h>` in `app_main.cpp`).

Outdoor and coil temp now have a standard cluster too: `TemperatureMeasurement` on ep2 (outdoor)
and ep8 (coil), see `docs/01`/`docs/07`. Compressor Hz remains without a standard cluster and stays
gapped (the mfg-cluster attr for it is unreachable from HA, see `docs/01`).

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

The driver computes P and publishes it via the standard `ElectricalPowerMeasurement` cluster (§3).
HA reads it as-is, Energy-Dashboard-native. The calc lives in a **pure, host-testable**
`power_estimate.h` (mirrors `matter_aircon_map.h`) so the model is unit-tested without hardware.

## 5. Energy accumulation (kWh): HA Riemann sum, not firmware

kWh is derived in HA from the power sensor via a Riemann-sum helper; HA's long-term stats hold the
history. No firmware integrator is wired up. `power_estimate.h` carries a pure, host-tested
energy-integrator pair (`hisense_energy_add`/`hisense_energy_mwh`, `test/test_matter_map.cpp:
166-167`), but nothing calls it: there's no `ElectricalEnergyMeasurement` cluster to publish
`CumulativeEnergyImported` to (§3), and a live version would still need NVS persistence to survive
a reboot.

## 6. Implementation phases

- **P1: Calibrate.** Done, §4a: meter + 4-point capture, quadratic fit `P ≈ 4.15·@55²`.
- **P2: Firmware compute.** Done: `power_estimate.h`, pure and host-tested like
  `matter_aircon_map.h`.
- **P3: Matter wire.** Done, on ep1 rather than a new endpoint (§3): `ElectricalPowerMeasurement`
  only, fed each downlink. No `ElectricalEnergyMeasurement` (§5).
- **P4: HA.** Power sensor confirmed live; Energy Dashboard wiring is a per-installation HA step,
  not tracked here.
- **P5: Validate.** Open: no recorded comparison against the cloud app's numbers.
