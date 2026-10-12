#pragma once
// The special modes (eco, turbo, quiet, the sleep profiles): the preset table, the ordered writes
// that take the unit from one combination to another, what each write changes, and the queue of
// writes still to send. Pure, no ESPHome types.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hisense_mapping.h"
#include "hisense_protocol.h"

namespace esphome::hisense_ac {

// ---- Special modes as presets ------------------------------------------------------------------
// Measured on hardware: eco + each sleep profile coexist; quiet + sleep do not; turbo combines with
// nothing; eco + quiet coexist. The first two rows are ESPHome built-ins (NONE, ECO); no custom
// name may equal a built-in, since set_preset(const char *) converts those.
struct SpecialState {
  bool eco{false};
  bool turbo{false};
  bool mute{false};
  uint8_t sleep{0};  // profile 0 = off, 1 General, 2 Old, 3 Young, 4 Kids (status byte 17 / 2)
};

struct PresetRow {
  const char *name;
  bool eco;
  bool turbo;
  bool mute;
  uint8_t sleep;
};

static constexpr PresetRow PRESETS[] = {
    {"none", false, false, false, 0},          {"eco", true, false, false, 0},
    {"quiet", false, false, true, 0},          {"turbo", false, true, false, 0},
    {"eco_quiet", true, false, true, 0},       {"sleep_general", false, false, false, 1},
    {"sleep_old", false, false, false, 2},     {"sleep_young", false, false, false, 3},
    {"sleep_kids", false, false, false, 4},    {"eco_sleep_general", true, false, false, 1},
    {"eco_sleep_old", true, false, false, 2},  {"eco_sleep_young", true, false, false, 3},
    {"eco_sleep_kids", true, false, false, 4},
};
static constexpr size_t PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);
static constexpr uint8_t PRESET_NONE = 0;
static constexpr uint8_t PRESET_ECO = 1;
static constexpr uint8_t PRESET_QUIET = 2;
static constexpr uint8_t PRESET_TURBO = 3;
static constexpr uint8_t PRESET_FIRST_SLEEP = 4;  // sleep_general is PRESET_FIRST_SLEEP + 1
static constexpr uint8_t PRESET_FIRST_CUSTOM = 2;

// Special modes the unit has, from YAML (supports_eco / _quiet / _turbo / _sleep).
static constexpr uint8_t SUPPORT_ECO = 0x01;
static constexpr uint8_t SUPPORT_QUIET = 0x02;
static constexpr uint8_t SUPPORT_TURBO = 0x04;
static constexpr uint8_t SUPPORT_SLEEP = 0x08;

inline bool preset_available(size_t idx, uint8_t support) {
  if (idx >= PRESET_COUNT)
    return false;
  const PresetRow &r = PRESETS[idx];
  if (r.eco && (support & SUPPORT_ECO) == 0)
    return false;
  if (r.mute && (support & SUPPORT_QUIET) == 0)
    return false;
  if (r.turbo && (support & SUPPORT_TURBO) == 0)
    return false;
  if (r.sleep != 0 && (support & SUPPORT_SLEEP) == 0)
    return false;
  return true;
}

// Name -> row index, or -1. HA passes the advertised string back verbatim.
inline int preset_index(const char *name) {
  if (name == nullptr)
    return -1;
  for (size_t i = 0; i < PRESET_COUNT; i++) {
    if (std::strcmp(PRESETS[i].name, name) == 0)
      return static_cast<int>(i);
  }
  return -1;
}

inline SpecialState special_from_status(bool eco_on, bool turbo_on, bool mute_on, uint8_t sleep_raw) {
  SpecialState s;
  s.eco = eco_on;
  s.turbo = turbo_on;
  s.mute = mute_on;
  s.sleep = static_cast<uint8_t>(sleep_raw / 2);  // status byte 17 carries profile * 2
  return s;
}

// An exact row wins. Otherwise an arbitration is in flight (the A/C drops one of an illegal pair a
// second or two later), so report the strongest mode really on: turbo, eco, quiet, then sleep. A
// preset the unit does not offer degrades to none.
inline uint8_t preset_detect(const SpecialState &s, uint8_t support) {
  for (size_t i = 0; i < PRESET_COUNT; i++) {
    const PresetRow &r = PRESETS[i];
    if (r.eco == s.eco && r.turbo == s.turbo && r.mute == s.mute && r.sleep == s.sleep)
      return preset_available(i, support) ? static_cast<uint8_t>(i) : PRESET_NONE;
  }
  uint8_t guess = PRESET_NONE;
  if (s.turbo) {
    guess = PRESET_TURBO;
  } else if (s.eco) {
    guess = PRESET_ECO;
  } else if (s.mute) {
    guess = PRESET_QUIET;
  } else if (s.sleep >= 1 && s.sleep <= 4) {
    guess = static_cast<uint8_t>(PRESET_FIRST_SLEEP + s.sleep);
  }
  return preset_available(guess, support) ? guess : PRESET_NONE;
}

// One bus write that changes a special mode. FEATURE carries a Feature (byte 33, combined frame);
// MUTE is 0/1 and SLEEP the profile, each on its own single-field frame.
enum SpecialOpKind : uint8_t {
  SPECIAL_OP_FEATURE = 0,
  SPECIAL_OP_MUTE = 1,
  SPECIAL_OP_SLEEP = 2,
};
struct SpecialOp {
  SpecialOpKind kind;
  uint8_t value;
};
static constexpr size_t PRESET_PLAN_MAX = 5;

// The A/C swallows a special-mode command that arrives too soon after the previous one. Measured:
// 6 s failed repeatedly, 8 s and 12 s engaged. 10 s is 8 with margin.
static constexpr uint32_t SPECIAL_SETTLE_MS = 10000;

// Ordered writes taking the unit from `now` to the special-mode state `want`; only writes that
// change something.
//   1. sleep off first when the target has none (quiet and sleep drop whichever came first).
//   2. clears: byte 33 to neutral (ECO_OFF if eco was on), then mute off.
//   3. sets: byte 33 to ECO or TURBO (one write switches between them), then mute on.
//   4. sleep profile last, and re-sent after any byte 33 write: the combined frame re-asserts the
//      fan while a profile owns it (measured: eco then sleep keeps both, sleep then eco does not).
inline size_t special_plan(const SpecialState &now, const SpecialState &want, SpecialOp out[PRESET_PLAN_MAX]) {
  size_t n = 0;
  bool feature_change = (want.eco != now.eco) || (want.turbo != now.turbo);
  bool feature_set = feature_change && (want.eco || want.turbo);

  if (want.sleep == 0 && now.sleep != 0)
    out[n++] = {SPECIAL_OP_SLEEP, 0};
  if (feature_change && !feature_set)
    out[n++] = {SPECIAL_OP_FEATURE, static_cast<uint8_t>(now.eco ? FEATURE_ECO_OFF : FEATURE_NONE)};
  if (now.mute && !want.mute)
    out[n++] = {SPECIAL_OP_MUTE, 0};
  if (feature_set)
    out[n++] = {SPECIAL_OP_FEATURE, static_cast<uint8_t>(want.eco ? FEATURE_ECO : FEATURE_TURBO)};
  if (want.mute && !now.mute)
    out[n++] = {SPECIAL_OP_MUTE, 1};
  if (want.sleep != 0 && (want.sleep != now.sleep || feature_change))
    out[n++] = {SPECIAL_OP_SLEEP, want.sleep};
  return n;
}

// The same, to preset row `target`.
inline size_t preset_plan(const SpecialState &now, uint8_t target, SpecialOp out[PRESET_PLAN_MAX]) {
  if (target >= PRESET_COUNT)
    return 0;
  const PresetRow &t = PRESETS[target];
  SpecialState want;
  want.eco = t.eco;
  want.turbo = t.turbo;
  want.mute = t.mute;
  want.sleep = t.sleep;
  return special_plan(now, want, out);
}

// What the unit should report once `op` lands. Encoding facts only (byte 33 is one enum, so ECO
// clears turbo and TURBO clears eco); the A/C's own cross-mode drops show up in the next status.
inline void special_apply(SpecialState *s, const SpecialOp &op) {
  switch (op.kind) {
    case SPECIAL_OP_FEATURE:
      switch (static_cast<Feature>(op.value)) {
        case FEATURE_ECO:
          s->eco = true;
          s->turbo = false;
          break;
        case FEATURE_TURBO:
          s->turbo = true;
          s->eco = false;
          break;
        case FEATURE_ECO_OFF:
          s->eco = false;
          break;
        default:
          s->turbo = false;  // NONE: turbo off
          break;
      }
      break;
    case SPECIAL_OP_MUTE:
      s->mute = op.value != 0;
      break;
    case SPECIAL_OP_SLEEP:
      s->sleep = op.value;
      break;
    default:
      break;
  }
}

// The fan index a special mode pins the A/C to, or -1 when free. Turbo forces high; quiet and sleep
// force their low profile. A request for another speed would be overwritten by the next status
// frame, so the climate entity refuses it instead.
inline int forced_fan_index(const SpecialState &s) {
  if (s.turbo)
    return FAN_INDEX_HIGH;
  if (s.mute || s.sleep != 0)
    return FAN_INDEX_LOW;
  return -1;
}

// Quiet and sleep report as either of the two bottom ladder steps, so both count as agreeing.
inline bool fan_request_allowed(const SpecialState &s, uint8_t wanted_idx) {
  int pin = forced_fan_index(s);
  if (pin < 0)
    return true;
  if (pin == FAN_INDEX_LOW)
    return wanted_idx == FAN_INDEX_QUIET || wanted_idx == FAN_INDEX_LOW;
  return wanted_idx == static_cast<uint8_t>(pin);
}

// The special-mode state the unit is being taken to: what it should report once every write already
// sent (`projected`) and every write still queued has landed.
inline SpecialState special_wanted(const SpecialState &projected, const SpecialOp *queue, size_t len) {
  SpecialState s = projected;
  for (size_t i = 0; i < len; i++)
    special_apply(&s, queue[i]);
  return s;
}

// A user's special-mode write joining the writes that are queued and not yet sent. A queued write of
// the same kind that the new one makes pointless is dropped: one whose absence leaves the unit in
// the same place once everything has landed. Quiet switched on and off again before the first
// write went out is one frame, not two, and since every write waits SPECIAL_SETTLE_MS for its
// turn, that is 10 s saved each. The user's newest write always stays, so the last thing asked
// for is stated on the wire even when the unit should be there already.
// False when the queue is full after merging; with the three kinds there are, that takes a `cap`
// below four.
inline bool special_queue_push(const SpecialState &projected, SpecialOp *queue, size_t *len, size_t cap,
                               const SpecialOp &op) {
  SpecialState target = special_wanted(projected, queue, *len);
  special_apply(&target, op);
  for (size_t i = *len; i-- > 0;) {
    if (queue[i].kind != op.kind)
      continue;
    SpecialState without = projected;
    for (size_t j = 0; j < *len; j++) {
      if (j != i)
        special_apply(&without, queue[j]);
    }
    special_apply(&without, op);
    if (without.eco != target.eco || without.turbo != target.turbo || without.mute != target.mute ||
        without.sleep != target.sleep)
      continue;
    for (size_t j = i + 1; j < *len; j++)
      queue[j - 1] = queue[j];
    (*len)--;
  }
  if (*len >= cap)
    return false;
  queue[(*len)++] = op;
  return true;
}

}  // namespace esphome::hisense_ac
