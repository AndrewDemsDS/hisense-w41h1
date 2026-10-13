# 09: The power estimate and how it was calibrated

The firmware publishes the A/C's power, voltage and current from bytes the unit already sends on
the bus, and Home Assistant's Energy dashboard can use the result. This page says what the
numbers are, how far to trust them, and how the one constant behind them was measured. Read it
before you rely on the kWh figure, or if you want to calibrate against your own meter.

## What is published

| Entity | Source |
|---|---|
| Power (W) | `4.15 * raw^2`, where `raw` is status byte 55 |
| Voltage (V) | status byte 50, in whole volts as the unit reports it |
| Current (A) | derived: `P / (V * 0.95)` |
| Energy today (kWh) | ESPHome's `total_daily_energy` on the Power sensor, in `packages/hisense-ac.yaml` |

The values are published when they change and at least once a minute. Power and current read 0
while the compressor is off. The energy sensor integrates on the node with `method: left` (the
value between two updates is the older one) and resets at midnight. Home Assistant's long-term
statistics hold the history.

The model is `active_power_mw()` and `active_current_ma()` in
`firmware/esphome/components/hisense_ac/hisense_telemetry.h`, pure functions with host tests
(`test_esphome_codec_parity.cpp` holds them equal to the reference in
`firmware/test/reference-driver/power_estimate.h`, and `test_esphome_map_gaps.cpp` covers their
bounds).

## How far to trust it

It is an estimate, not a meter. These inverter splits almost never carry a revenue-grade meter,
and the vendor's cloud figure is an estimate too: either a value the indoor MCU reports (it
senses bus or compressor current for inverter control) or a model from compressor frequency and
mode.

- The constant comes from **two non-zero points** on one unit. The quadratic fits both to under
  1 %, which supports the shape, but a third steady point was never taken.
- The power factor is assumed (0.95, typical for an inverter with active PFC). At deep part load
  the real power factor is lower.
- Byte 50 reads about 6 % below a panel meter (about 220 against 234 to 236 V). The power figure
  does not depend on it. The derived current does.
- There is no plausibility clamp on byte 55. `active_power_mw()` is bounded at 50 kW to keep the
  arithmetic in range, and nothing more.
- No comparison against the vendor app's numbers is on record.

## The calibration

Reference: a PD3-63VA panel meter on the phase the A/C is on (L3). Three points, matched by time
(photo EXIF against the logger's `[HH:MM:SS]`). Power is `V * dI(L3 minus baseline) * PF(0.95)`:

| Time | L3 A | dI (A/C) | P (W) | byte 55 | byte 42 (Hz) |
|---|---|---|---|---|---|
| 22:07:36 | 1.1 | 0.0 | 0 | 0 | 0 |
| 22:12:32 | 2.3 | 1.2 | 268 | 8 | 31 |
| 22:10:48 | 5.3 | 4.2 | 934 | 15 | 68 |

Byte 55 tracks the **square root of power**, not amps. A linear fit gives incompatible slopes
(33 against 62 W per count). A quadratic through the origin fits both load points to under 1 %:

```
P_watts = 4.15 * raw55^2        (PF 0.95 folded in)
  raw 8  -> 266 W (meter 268)     raw 15 -> 934 W (meter 934)
```

The derived current, for comparison: raw 15 gives 4.47 A (meter 4.2), raw 8 gives 1.27 A
(meter 1.2). Byte 60 gives the same curve (about `0.66 * raw60^2`) as a cross-check. The tap that
logged the frames has since been removed, so a fourth point is deferred.

## The bytes behind it

From the three-state correlation in
[`reverse-engineering/docs/03-rs485-ac-protocol.md`](../../reverse-engineering/docs/03-rs485-ac-protocol.md)
(cool at 60 Hz, off at 0 Hz, heat at 50 Hz, mapped 2026-07-07):

| Byte | Field | Confidence | Use |
|---|---|---|---|
| 50 | supply voltage (V) | high: 218 V loaded, 222 V idle (sags under load), consistent with 230 V mains | the Voltage sensor |
| 55 (= 145) | first labelled "phase or bus current A": zero when the compressor is off, tracks load. The calibration showed it follows the square root of power | calibrated above | the Power sensor |
| 56 (= 144) | "phase or bus current B" | medium | not used |
| 60 | "active current or power reading", on the same curve as byte 55 | medium | cross-check only |
| 42 | compressor frequency, actual (Hz) | confirmed | the Compressor frequency sensor |
| 41, 43 | compressor frequency, target (Hz) | confirmed | not used |
| 45 | outdoor (condenser) coil temperature (°C) | confirmed (reverses between cool and heat) | the Coil temperature sensor |

## What was planned and not built

- **A frequency model as a second path** (`P = a * comp_freq + b * fan_level + P_standby`), which
  is what most community inverter A/C integrations use. It would not depend on byte 55 and could
  bound it. It was never implemented, so there is no cross-check at run time.
- **A fitting script** (`tools/fit_power.py`) for a least-squares fit from recorded pairs. It was
  never written. The constant above was fitted by hand.
- **Coefficients that can be changed without a rebuild.** The constant is compiled in
  (`POWER_MW_PER_COUNT2`).
- **A lifetime energy counter on the node.** The reference `power_estimate.h` carries a pure,
  host-tested integrator pair (`hisense_energy_add`, `hisense_energy_mwh`) that nothing calls: a
  lifetime counter would need persistence to survive a restart. `total_daily_energy` and Home
  Assistant's statistics do the job.

On the retired Matter builds (tag `matter-final`) the same three values were attributes of
`ElectricalPowerMeasurement` on endpoint 1, and energy was left to a Riemann-sum helper in Home
Assistant. That cluster is delegate-based in connectedhomeip, which made it the most awkward
endpoint of those builds.

## Calibrating against your own meter

You need a reference that reads the A/C alone: a clamp meter on the A/C's live conductor, or a
panel meter on its circuit with the baseline subtracted. A clamp that reads true power (W) is
best. One that reads only amps gives apparent power, and you have to assume a power factor.

**Safety.** This means working at the mains lead of the A/C. Clamp around a single insulated
conductor, never bare metal. If the terminal block is not safely reachable, clamp at the
isolator or the plug lead. Do not open anything live that you are not qualified to open.

1. Log byte 55 next to the meter. With the node's log at `VERY_VERBOSE` the raw status frames are
   in the log. With a tap on the bus, `reverse-engineering/tools/decode_ac_frames.py --port <tap>`
   decodes them, and `firmware/esp32-recon/` shows them on a console.
2. Hold three or four steady states and note the meter at each: off, a low cool (about 40 Hz), a
   hard cool or turbo (about 69 Hz), heat (about 50 Hz).
3. Fit `k` in `P = k * raw^2` through the origin and compare with 4.15.
4. A different constant goes into `POWER_MW_PER_COUNT2` in `hisense_telemetry.h` and into the
   reference `power_estimate.h`, in one commit, or the parity test fails. Say in the commit what
   it was measured with.
