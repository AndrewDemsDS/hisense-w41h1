/********************************************************************************
 * power_estimate.h
 *
 * PURE active-power / energy estimation for the Hisense A/C, host-unit-testable
 * (plain stdint, no CHIP/SDK types) -- the "energy side" QA surface, mirroring
 * matter_aircon_map.h. matter_drivers.cpp feeds the results into the standard
 * Matter ElectricalPowerMeasurement / ElectricalEnergyMeasurement clusters.
 *
 * CALIBRATION (2026-07-07, PD3-63VA panel meter on L3, 3 timestamp-synced points;
 * power = V * dI(L3-baseline) * PF0.95). See firmware/docs/09-energy-monitoring.md.
 *
 *     @55 (current-proxy status byte)  ->  real power
 *        0  -> 0 W        8 -> 268 W        15 -> 934 W
 *
 * @55 tracks SQRT(power), not linear amps: a linear fit gives incompatible slopes
 * (33 vs 62 W/count) while a quadratic-through-origin fits BOTH load points to <1%:
 *
 *     P[W] = k * @55^2 ,  k = 4.15  (=> mW: 4150 per count^2)
 *
 * @50 is the (coarse ~220 V) supply-voltage byte; it reads ~6% low vs the panel
 * meter but current dominates the product so the error is minor.
 ********************************************************************************/
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HISENSE_POWER_MW_PER_C2  4150ULL /* milliwatts per @55^2 count (k=4.15 W) */
#define HISENSE_PF_PERMIL        950     /* assumed inverter input power factor * 1000 */

/* Sanity ceiling: a glitchy/reserved @55 byte (255 -> ~270 kW) must not poison HA's
 * energy dashboard. Clamp to the cluster's declared ActivePower maxMeasuredValue. */
#define HISENSE_POWER_MW_MAX  50000000LL   /* 50 kW = ElectricalPowerMeasurement declared max */

/* Active power in milliwatts from the current-proxy status byte (@55). */
static inline int64_t hisense_active_power_mw(uint8_t i55)
{
    int64_t p = (int64_t)(HISENSE_POWER_MW_PER_C2 * (uint64_t)i55 * (uint64_t)i55);
    return (p > HISENSE_POWER_MW_MAX) ? HISENSE_POWER_MW_MAX : p;
}

/* Supply voltage in millivolts from the voltage status byte (@50, whole volts). */
static inline int64_t hisense_voltage_mv(uint8_t v50)
{
    return (int64_t)v50 * 1000;
}

/* Derived active current in milliamps: I = P / (V * PF). 0 if voltage unknown.
 * (For transparency only -- @55 is a power proxy, so current is back-computed.) */
static inline int64_t hisense_active_current_ma(uint8_t i55, uint8_t v50)
{
    if (v50 == 0) return 0;
    int64_t p_mw = hisense_active_power_mw(i55);
    return (p_mw * 1000) / ((int64_t)v50 * HISENSE_PF_PERMIL);
}

/* Energy integrator. Accumulate in mW-milliseconds (lossless fixed point) each
 * status poll; convert to mWh on read for CumulativeEnergyImported. Negative /
 * spurious power is clamped to zero (import only). */
static inline void hisense_energy_add(uint64_t *acc_mw_ms, int64_t power_mw, uint32_t dt_ms)
{
    if (power_mw > 0) *acc_mw_ms += (uint64_t)power_mw * (uint64_t)dt_ms;
}

static inline uint64_t hisense_energy_mwh(uint64_t acc_mw_ms)
{
    return acc_mw_ms / 3600000ULL; /* mW*ms -> mWh */
}

#ifdef __cplusplus
}
#endif
