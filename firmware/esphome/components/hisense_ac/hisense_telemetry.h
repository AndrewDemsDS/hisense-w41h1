#pragma once
// Telemetry: the power estimate, when a sensor value is worth publishing, and the outdoor
// temperature placeholder. Pure, no ESPHome types.

#include <cstddef>
#include <cstdint>

namespace esphome::hisense_ac {

// ---- Power estimate ----------------------------------------------------------------------------
// Calibrated against a panel meter: status byte 55 tracks sqrt(power), P[W] = 4.15 * raw^2. Byte 50
// is supply voltage in whole volts, about 6% low.
static constexpr uint64_t POWER_MW_PER_COUNT2 = 4150;  // milliwatts per byte-55 count squared
static constexpr int64_t POWER_PF_PERMIL = 950;        // assumed inverter power factor * 1000
static constexpr int64_t POWER_MW_MAX = 50000000;      // 50 kW; a glitched byte must not reach HA

inline int64_t active_power_mw(uint8_t raw55) {
  auto p = static_cast<int64_t>(POWER_MW_PER_COUNT2 * raw55 * raw55);
  return p > POWER_MW_MAX ? POWER_MW_MAX : p;
}

inline int64_t voltage_mv(uint8_t raw50) { return static_cast<int64_t>(raw50) * 1000; }

// I = P / (V * PF), back-computed since byte 55 is a power proxy. 0 when the voltage is unknown.
inline int64_t active_current_ma(uint8_t raw55, uint8_t raw50) {
  if (raw50 == 0)
    return 0;
  return (active_power_mw(raw55) * 1000) / (static_cast<int64_t>(raw50) * POWER_PF_PERMIL);
}

// Energy accumulates losslessly in mW*ms per status poll; import only, so non-positive power adds 0.
inline void energy_add(uint64_t *acc_mw_ms, int64_t power_mw, uint32_t dt_ms) {
  if (power_mw > 0)
    *acc_mw_ms += static_cast<uint64_t>(power_mw) * dt_ms;
}

inline uint64_t energy_mwh(uint64_t acc_mw_ms) { return acc_mw_ms / 3600000ULL; }

// ---- Telemetry pacing ---------------------------------------------------------------------------
// A status frame arrives about once a second and rarely differs from the previous one. A sensor
// value goes out when it is the first, when it changed, or when the periodic refresh is due. The
// refresh keeps integrating sensors (total_daily_energy) advancing through a steady load.
static constexpr uint32_t TELEMETRY_REFRESH_MS = 60000;

inline bool telemetry_publish_due(bool has_state, float last, float value, bool refresh_due) {
  if (!has_state || refresh_due)
    return true;
  // "Unknown" (NaN) staying unknown is not a change. Without this a sensor held at NaN would go
  // out with every status frame, about once a second, since NaN never compares equal to itself.
  if (last != last && value != value)
    return false;
  // Negated equality, not !=, so a NaN on one side counts as a change.
  return !(last == value);
}

// ---- Outdoor temperatures: the "not measured yet" placeholder -----------------------------------
// Right after mains power comes back, the unit fills the outdoor temperature (byte 44) and the
// coil temperature (byte 45) with 0xEC, which reads as -20 C, until it has real readings. Published
// as they are, both sensors show a confident -20 C for a while after every power cut.
//
// VERIFY: one observation, on one unit, 2026-10-10. After a mains power cycle both bytes were
// 0xEC, and later the same afternoon the same sensors read 35 and 26 C. How long the placeholder
// lasts, whether both bytes leave it in the same frame, and whether it can come back while the
// unit stays powered were not measured. The RE docs show no handling of this value in the stock
// firmware (reverse-engineering/docs/03-rs485-ac-protocol.md lists both bytes as direct Celsius).
//
// The rule is as narrow as that observation allows, because -20 C is also a temperature:
//   1. BOTH bytes are exactly 0xEC in the same frame. A lone -20 is published. With the
//      compressor running the coil sits well away from the outdoor air (33 against 25 in docs/03),
//      so the two only agree on a unit at rest.
//   2. No real pair has been seen since the link came up. Once either byte has held anything
//      else, the readings are trusted until the link drops, so a site that cools to -20 C is
//      shown as -20 C.
// What this still hides: a unit at rest in exactly -20 C weather, from the moment the node (or
// the link) starts until either reading moves by one degree.
static constexpr uint8_t OUTDOOR_TEMP_PLACEHOLDER_RAW = 0xEC;  // -20 as int8, see above

struct OutdoorTempGate {
  bool seen_real{false};
};

/// The bus link dropped: the unit may have lost power, so the next pair is judged afresh.
inline void outdoor_temp_gate_reset(OutdoorTempGate *gate) { gate->seen_real = false; }

/// True when this frame's outdoor and coil temperatures are readings, false when they are the
/// placeholder and should be published as unknown. Call once per status frame.
inline bool outdoor_temps_measured(OutdoorTempGate *gate, int8_t outdoor_c, int8_t coil_c) {
  const bool placeholder_pair = static_cast<uint8_t>(outdoor_c) == OUTDOOR_TEMP_PLACEHOLDER_RAW &&
                                static_cast<uint8_t>(coil_c) == OUTDOOR_TEMP_PLACEHOLDER_RAW;
  if (!placeholder_pair)
    gate->seen_real = true;
  return gate->seen_real;
}

}  // namespace esphome::hisense_ac
