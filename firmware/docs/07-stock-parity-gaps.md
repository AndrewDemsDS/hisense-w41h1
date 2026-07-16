# 07 · Stock-parity gap matrix (Gitea #19)

Single source of truth for "what does the stock A/C do that we don't (yet) mirror." Consolidates
the previously-scattered lists in `05-ha-control-and-native-ui.md`, `01-expose-all-clusters.md`,
`reverse-engineering/docs/{03,05,10}`, and the recon `HIL-RUNBOOK.md`.

Columns: **DEC** = decoded in the driver · **CTRL** = a command builder exists · **EXP** = exposed
to Matter/HA. Capability source = the `0x66/40` ProductType `HisenseFeatures` flags
(`hisense_rs485.h:315-329`, decoder `.cpp:179-201`); the Ghidra-authoritative bit map is
`reverse-engineering/docs/10 §5a` (`handle_producttype_cmd_result @0x9b6f0c4c`).

## Matrix

| Stock feature | DEC | CTRL | EXP | Notes |
|---|:--:|:--:|:--:|---|
| Power on/off | ✅ | ✅ | ✅ | OnOff ep1 |
| Mode (cool/heat/dry/fan/auto) | ✅ | ✅ | ✅ | Thermostat FeatureMap 35 (Heat+Cool+Auto); dry/fan via HA unlock |
| Setpoint | ✅ | ✅ | ✅ | Occupied Cooling/Heating Setpoint |
| Fan (6 discrete speeds) | ✅ | ✅ | ✅ | FanControl mode + percent |
| Eco / power-save | ✅ | ✅ | ✅ | ep3 OnOff "Eco" + mfg `0xFFF1FC00/0x0000` |
| Turbo / boost | ✅ | ✅ | ✅ | ep5 OnOff "Turbo" + mfg `/0x0002` |
| Mute / quiet | ✅ | ✅ | ✅ | ep4 OnOff "Quiet" + mfg `/0x0002`; `hisense_build_mute_frame` |
| Sleep profile (4) | ✅ | ✅ | ✅ | ep6 ModeSelect; `hisense_build_sleep_frame` |
| Aux/PTC heat relay | ✅ | — | ✅ | ep7 BooleanState (read-only status) |
| Outdoor + coil temp | ✅ | — | ✅ | ep2 / ep8 TemperatureMeasurement |
| Power (V/I/W) | ✅ | — | ✅ | ElectricalPowerMeasurement (see #16, `power_estimate.h`) |
| **Vertical swing on/off** | ✅ | ✅ | ❌ | **CHEAP WIN** — decoded (`vswing_on`), controllable (`HisenseCommand.vswing`), but the esp32 build never creates/writes a FanControl **RockSetting** attr (only shadows it at `app_main.cpp:359`). Pure app-layer to expose. |
| **Display / panel on/off** | cap only | ✅ | ❌ | **CHEAP WIN** — `HisenseCommand.display_on` (@20, 0xC0/0x40) is controllable but wired to no endpoint. Expose as an OnOff switch. (Dimmer *level* = `ac_power_display`, needs new RE.) |
| 8 °C frost-guard heat | ⚠️ cap (mislabeled) | ❌ | ❌ | Capability bit `ac_8heat` (byte26 0x80) IS read — but under the wrong field name `purify` (see bug below). No control frame RE'd. `docs/05:79` marks it likely absent on this unit. |
| Purify / ionizer | ⚠️ cap (mislabeled) | ❌ | ❌ | `ac_purify` (byte23 0x08) read under wrong name `q_display`; live `purify_on` (b36 0x20) "bit always 0 — feature absent on this unit". No builder. |
| AI / smart | ✅ cap | ❌ | ❌ (logged) | `ai` (byte28 0x40). Capability only, no control frame. |
| Demand-response | ✅ cap | ❌ | ❌ | `demand_resp` 2-bit (byte35). Capability only. |
| Infinite / stepless fan | ✅ cap | ⚠️ | ⚠️ | `infinite_fan` (byte25 0x08); we drive 6 discrete speeds only, not stepless. |
| 8-position louvre aim | ✅ cap | ⚠️ | ❌ | `swing_dir_8`/`swing_follow` capability + on/off swing decoded, but no per-position index command. |
| Fresh-air / dew | — | — | — | **No `ac_*` flag exists** — not in the stock feature set; nothing to mirror. |

## Cheap wins (do first — app-layer only, no protocol RE)

1. **Vertical swing → FanControl RockSetting** on ep1: write `RockSetting` from `st->vswing_on`, and
   drive `s_cmd.vswing` on a Rock write. Mirrors the AmebaZ2 `matter_drivers.cpp` (which had it;
   the esp32 port dropped it). Exposes swing to HA.
2. **Display on/off → an OnOff switch** (like the eco/turbo/mute switches), driving
   `HisenseCommand.display_on`.

Everything else (8 °C heat, purify, dimmer level, AI, demand-response, stepless fan, 8-pos louvre) is
**capability-decoded only** — each needs its *control frame* reverse-engineered before it can be
exposed, and several are argued physically-absent on this unit (`docs/05:79-80`). Not cheap; defer /
track under #52.

## Reading capabilities live

The `HisenseFeatures` set is polled (`hisense_build_producttype_request`) and cached
(`hisense_get_features`) but **only logged** (`on_features`, `app_main.cpp:430`) — it is **not** written
to any Matter attribute, so it can't be read via matter-server. The only remote read today is the
telnet diag console `:2323` `decode` path. To make it HA-readable, map `hisense_get_features()` into
extra `0xFFF1FC00` attributes (or a features console command) — see recommendation in #19.

## Data-quality bugs found during this review (→ own issue)

- **`HisenseFeatures.purify` and `.q_display` read the wrong bits.** Per the Ghidra decode
  (`docs/10 §5a`): `purify` actually carries **`ac_8heat`** (byte26 0x80) and `q_display` actually
  carries **`ac_purify`** (byte23 0x08). The real `ac_q_display` is byte39 0x40 — **not decoded**.
  Wrong labels flow into `on_features` logging, `diag_console.cpp:122`, and `matter_drivers.cpp:347`.
- **Not decoded at all:** `ac_q_display` (byte39 0x40), `ac_enable_8heat` (byte39 0x04),
  `ac_trans_102_64` (byte38 0x08).

These are capability-flag *labels* (not used for control today), so low-impact — but wrong. Fix =
rename the two fields + add the three missing, per `docs/10 §5a`.
