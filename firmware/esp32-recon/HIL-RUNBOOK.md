# esp32-recon HIL runbook — settling the open bus/protocol issues

Turnkey procedures for the hardware-in-the-loop issues, using `esp32-recon`. Each is
"wire it, flash it, run these commands, record the result on the issue." No procedure
needs the Matter stack.

## Setup (once)

```sh
. ~/esp/esp-idf-v5.5.4/export.sh
cd firmware/esp32-recon && idf.py set-target esp32 && idf.py flash monitor
esp32-recon> selftest            # golden vectors — expect PASS before trusting anything
esp32-recon> wifi <ssid> <pass>  # then reach it remotely: nc esp32-recon.local 2323
```

**Two rigs, depending on the issue:**
- **Tap rig** (RE / byte-hunting): leave the **stock dongle** (or kitchen AmebaZ2) driving
  the bus; wire the ESP32 RX (GPIO18) + GND onto the same A/B pair; `mode tap`. You watch
  real traffic + toggle features on the A/C's remote/app. DE stays low — the ESP32 is invisible.
- **Master rig** (control): the ESP32 is the **only** module on the bus (no stock dongle);
  `mode master`. The ESP32 drives link + poll and you issue `set`/`power`.

The `snap`/`diff` delta finder is the core RE tool: capture a baseline frame, toggle one
thing on the remote, `diff` shows exactly which byte/bit moved. Reference field offsets are
in `firmware/src/rs485-driver/hisense_rs485.h` (status flags at `[35]`/`[36]`, mode `[18]`,
setpoint `[19]`, etc.).

---

## #55 (I22) · C/F unit bit — firmware always assumes Celsius

**Goal:** find the status/command bit that encodes °C vs °F, so the firmware can read and
track it instead of hardcoding Celsius.

**Tap rig:**
```
mode tap → reboots
watch off                 # keep the console quiet; we use snap/diff
snap status               # baseline at current unit
# --- on the A/C remote/app: switch the temperature UNIT (°C -> °F) ---
diff status               # -> the byte(s)/bit(s) that carry the unit flag
snap cmd                  # baseline the dongle's command frame
# --- toggle the unit again on the remote ---
diff cmd                  # if the dongle echoes the unit in its command frame
```
**Record:** the changed offset + bit mask (e.g. `off 19 bit0x…`). Cross-check against the
setpoint at `[19]` (a °F setpoint reads in the 61–90 range vs 16–32 for °C — the driver's
`HisenseState.fahrenheit`/setpoint decode should branch on this bit).
**Pass:** a single, repeatable bit flips with the unit and nothing else does.

---

## #52 (I19) · Display/LED backlight · 8 °C frost-guard heat · AI/smart · 8-pos louvre

**Goal:** locate the command bytes + status bits for each missing feature, and confirm which
the unit even supports.

**First, capability:**
```
mode master → reboots
producttype               # ac_power_display, ac_ai, ac_swing_direction_8, cool_heat, ...
```
Only chase features `producttype` reports as present.

**Then, per feature (tap rig, one at a time):**
```
snap status
# --- toggle ONE feature on the remote/app (e.g. panel LED off) ---
diff status               # status bit for that feature
snap cmd
# --- toggle it again ---
diff cmd                  # the command byte/value the dongle sends to set it
```
Repeat for: display/LED dimmer, 8 °C frost-guard heat (a HEAT sub-state — also watch
`[18]` mode nibble + setpoint `[19]`), AI/smart mode, and the 8-position louvre aim
(watch the swing bytes vs the 2-bit swing field the driver knows).
**Record:** a small table {feature → command offset/value, status offset/bit} per feature on
the issue. **Pass:** each feature's command reproduces the remote's effect and its status bit
tracks it.

---

## #50 (I17) · Cold-off power-on may not power a physically-off unit

**Goal:** confirm whether a power-on command actually starts a unit that is physically OFF
(vs only working from standby).

**Master rig**, unit physically OFF:
```
mode master → reboots
watch on
power on
# observe: does a status frame arrive with power=1, and does the compressor/fan actually start?
```
Also try the reverse and the "cold" path:
```
power off   (wait)   power on     # repeat a few times, note reliability
```
**Record:** whether `power on` from a truly-off unit yields `power=1` + real airflow, and how
reliably (N/N). If it needs a wake/link sequence first, capture the frames (`watch hex`) that
precede a successful start. **Pass:** deterministic power-on from the off state.

---

## #49 (I16) · Envelope `[7]/[8]` — RESOLVED ON HARDWARE 2026-07-16. No tap rig needed.

**Settled.** The "session token" premise was **wrong**. Measured on node 28 (fw 1.0.6) via the
instrumented `token` console command, both sources read from the same `0x0A` DevType reply:

```
  device-type  inner [3]/[4] : 01 01  [learned from A/C]   <- USED
  session tok  envel [9]/[10]: 00 00  [captured]           <- v10207 stamped this, link died
```

So `[7]/[8]` is the A/C's **device-type/sub-type** (static per model), read from the DevType
reply's inner `[3]/[4]`; the envelope `[9]/[10]` on that frame is `00 00`. `[learned from A/C]`
also proves the driver's learning path fires rather than riding its `01 01` default. Full
narrative + the v10207 post-mortem: RE `docs/10` §4.5.

**Re-run only if** supporting a **different A/C model** (to see what device-type it reports):
```
nc <node-ip> 2323   ->   token
```
No bus wiring required — any esp32 node running the driver reports it. A tap capture
(`mode tap` + `watch hex` through a dongle power-cycle) is now purely optional corroboration.

---

## #15 · Outdoor coil temp + compressor Hz sensors  &  #16 · Power calibration

**Goal:** validate the telemetry decode (coil `[45]`, compressor `[42]`, current `[55]`,
voltage `[50]`) against reality.

**Master rig** (or tap), running A/C under load:
```
mode master → reboots
watch on
# run the A/C: cool hard, then off, then heat — watch coil/comp/I/V evolve
```
- **Coil** (`coil_temp_c`): should reverse between cool/heat (per the driver note); confirm it
  tracks the condenser, unlike outdoor `[44]`.
- **Compressor Hz** (`compressor_freq`): 0 stopped, ramps under load.
- **Power (#16):** read `I`/`V` (`current_raw`/`voltage_raw`) at several known loads and compare
  to a **panel meter**; the driver models `P[W] = 4.15 * raw^2`. Record (raw, measuredW) pairs to
  re-fit the constant if it's off. **Pass:** decode monotonic + within tolerance of the meter.

---

## Recording results

Post each result as a comment on the matching issue (offsets/bits, N/N reliability,
frame hex evidence), and close the issue when its "Done:" bar is met. The tool's `decode`/`diff`
output pastes cleanly into an issue comment.
