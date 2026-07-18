# Stock W41H1: device-type → model capability map (how features are gated per A/C model)

**Scope:** how the stock dongle decides *which features an attached indoor unit has*. You need this
mechanism to support any A/C model other than the one on the bench.

**Sources:** static RE of `dumps/w41h1_dump1.bin` (2026-07-16), building on the call graph in
[`10-stock-fw-init-and-comms.md`](10-stock-fw-init-and-comms.md). XIP mapping
`runtime = file_offset + 0x9b6d0000`, re-verified before decoding (`0x9b805506` →
`0a 04 00`, `0x9b807823` → `07 01 00`, matching doc 10 §3.3). **[PROVEN]**

Each claim below is tagged **[PROVEN]** (read off the disassembly, runtime address cited) or
**[INFERRED]** (reasoning). Treat [INFERRED] as untested. We acted on one [INFERRED] claim from
doc 10 §4.5, shipped it as v10207, and killed the A/C link (post-mortem there).

---

## 1. The one-paragraph version

The A/C reports its **device-type** in the DevType (`0x0A`) reply. The module picks one of **two
capability templates** in flash, copies it to a heap struct, and lets the **ProductType (`0x66/40`)
reply refine it** by switching individual capabilities on. Consumers skip any capability whose
`supported` byte is 0. Static baseline plus runtime refinement is **one mechanism in two stages**,
so the two never compete.

```
DevType 0x0A reply ──▶ devtype ──▶ S4: malloc + copy TEMPLATE_{A,B} ──▶ [0x1000b8a8]/[0x1000b8b4]
                                                                              │
ProductType 0x66/40 reply ──▶ flag decision tree (0x9b6f0d84) ──▶ writes `supported` bytes ──┘
                                                          └──▶ strcpy 3-digit profile → 0x100096dc
```

## 2. Device-type table **[PROVEN]**

This sweep covered all **27** callsites of the devtype getter `0x9b6f307c` and all **10**
literal-pool xrefs of `[0x10009687]`. The image compares only **`1`, `0x15`, `0x36`, `0x37`**,
plus `0xFF`, a sentinel written on a NULL DevType payload @ `0x9b6f2194`.
**No lookup table exists.** Dispatch is an `if`/`else-if` chain (`bl 0x9b6f307c; cmp; beq`).

| devtype | struct | malloc | records | profile codes | product |
|---|---|---|---|---|---|
| `0x00`, `0x01` | TEMPLATE_A `0x9b807e20` → `[0x1000b8a8]` | `0x33c` | 69 | `100`–`129`, `199` | air conditioner (**ours**) |
| `0x15` | TEMPLATE_B `0x9b808144` → `[0x1000b8b4]` | `0xe4` | 19 | `400`, `499` | **dehumidifier** (`t_pump`, `t_anion`, `f_e_wetsensor`) |
| `0x36` | TEMPLATE_A | `0x33c` | 69 | `200`–`203`, `299` | A/C variant |
| `0x37` | TEMPLATE_A | `0x33c` | 69 | `300`–`311`, `399` | A/C variant (also sets `[0x1002942c]=1` @ `0x9b6f2194`) |
| anything else | **none allocated** | | | | falls to `0x9b6f9638` |

- The **getter** `0x9b6f307c` is `ldrb r0,[0x10009687]; cmp r0,#1; it lo; movlo r0,#1`. It maps
  **0 → 1**, so devtype 0 and 1 name the same model.
- `0x36` and `0x37` share TEMPLATE_A with the plain A/C. Their *profile code* differs, the struct
  does not.
- **Do not add driver branches for codes outside `{0,1,0x15,0x36,0x37}`.** Nothing else exists.
- **Our unit reports `01 01`** (measured on the wire, doc 10 §4.5) → TEMPLATE_A, profiles `1xx`.

> Corrects three errors in an earlier doc 10 §4.1 (fixed there): `0x9b6f9126` is the `bl` to the
> getter, not the dispatch head; `0x9b6f9130` *is* the TEMPLATE_A branch (shared by all four A/C
> codes, `beq.w` @ `0x9b6f9728`/`0x9b6f9732`), not an `0x36`/`0x37`-only path; and TEMPLATE_A is
> selected by an explicit `==1` test. The real `else` allocates nothing.

## 3. The capability struct **[PROVEN]**

The templates are **not opaque blobs**. They are arrays of 12-byte records, and the capability
names are **cleartext strings in rodata**:

```c
struct cap_entry {           // 12 bytes
    uint8_t  b0, b1;
    uint8_t  supported;      // +2  <- THE GATE
    uint8_t  b3;
    const char *name;        // +4  -> e.g. "t_work_mode", "f_humidity", "t_pump"
    uint32_t desc;           // +8  {b0,b1,b2,b3} wire-position descriptor
};
```

Template `word0 = 0x00010000` (byte2 = 1) → supported; `0x00000000` → not.

**`+2` is a hard skip-gate.** From `0x9b6f9b50`:
```
ldr.w r3,[fp]        ; fp = &[0x1000b8a8]
muls  r7, r6, #0xc   ; record index * 12
ldrb  r2, [r2, #2]   ; +2 = supported
cmp   r2, #0
beq.w #0x9b6f9d76    ; skip this capability entirely
```

### 3.1 Runtime struct = template + synthesized inserts

The malloc sizes exceed the templates because S4 **inserts records during the copy**. Both
reconstruct to *exactly* the malloc size:

- **STRUCT_A** = `TA[0..14]` + `{"f_electricity"}`@`0x9b6f9252` + `TA[15..66]` +
  `{"f_ecm"}`@`0x9b6f9624` → **69 × 12 = 828 = `0x33c`** ✓
- **STRUCT_B** = `TB[0..7]` + `{"f_power_display"}`@`0x9b6f97e2` + `{"f_power_consumption"}`@`0x9b6f97f6`
  + `TB[8]` + **`TA[45]`** (borrowed cross-template via `sub.w r2,r5,#0x108` @ `0x9b6f9820`)
  + `TB[9..14]` + `{"f_ecm"}`@`0x9b6f98a0` → **19 × 12 = 228 = `0xe4`** ✓

The templates are **contiguous** in flash (`0x9b807e20 + 0x324 == 0x9b808144`); TEMPLATE_A's real
extent is 67 records (`0x324`) and TEMPLATE_B's is 15 (`0xb4`).

Corroborated **three independent ways** **[PROVEN]**: the record-count loop bounds `0x45`(69) /
`0x13`(19) selected by devtype @ `0x9b6f79a4`; the literal `add.w ip, r3, #0x33c` @ `0x9b6f8ae8`;
and every one of ~40 ProductType `strb` offsets landing exactly on `record*12 + 2`.

### 3.2 The `desc` word (+8)

Two consumers read **different halves** of it:

| consumer | reads | meaning |
|---|---|---|
| `0x9b6f79a4` | `idx = b0+2`, `group = b1>>3` (vs `buf[1]`), `bit = (b1&7)-1` | **[PROVEN]** |
| `0x9b6f8adc` | `idx = b2+2` (`ldrb r5,[r3,#0xa]`, `cbz` → skip if 0); `b3 < 0x48` → bitfield via mask table; `b3 >= 0x48` → 16-bit BE read | **[PROVEN]** |

**[INFERRED]** that `(b0,b1)` = position in the **command (`0x65`)** frame and `(b2,b3)` = position
in the **status (`0x66`)** frame. Consistent with spot checks (`f_votage` b3=`0x80` → 16-bit read;
`t_temp` b2=4 → status byte 6) but **not confirmed**. See §6.

`word0`'s `b0`/`b1`/`b3` have **no known consumer**. Only `+2` is proven meaningful.

## 4. ProductType refines the struct (they are one mechanism) **[PROVEN]**

`handle_producttype_cmd_result` (`0x9b6f0c4c`) does more than log. Gated on **`devtype <= 1`**
(`ldrb r3,[0x10009687]; cmp r3,#1; bhi.w #0x9b6f16c4` @ `0x9b6f0d84`) it writes `supported` bytes
straight into the S4 struct:

```
ldr  r3,=0x1000b8a8; ldr r8,[r3]     ; the S4-allocated struct
strb sl(=1), [r8,#0x266]  ...        ; ~17 unconditional enables, then a flag-driven tree
```

It also `strcpy`s the 3-digit **profile code** to `0x100096dc` (getter `0x9b6f308c`).
`ac_trans_102_64` set → **`199`** (generic/transparent profile; `beq.w` @ `0x9b6f0ddc` →
`'199'` @ `0x9b6f0de4`).

**ProductType is the authoritative refinement layer.** Within the `devtype<=1` path it only
*enables*. Disable writes exist elsewhere, such as the `= 0` stores in the `0x37` branch at
`+0x31a`, so scope "only enables" to this path.

> This corrects doc 10 §5a's *"Whole commit gated on `[0x10009687]==0x15`"*. That `==0x15` test
> picks the printf **format string** and nothing more. Fixed in §5a.

## 5. What this means for our unit (devtype `0x01`)

**Static baseline: 9 of 69 capabilities carry `supported=1` before any ProductType refinement.**
They are `t_work_mode`, `t_power`, `t_temp`, `t_temp_type`, `t_sleep`, `f_temp_in`, `f_e_push`,
`f_e_intemp`, `f_e_incoiltemp`. **[PROVEN]**

The rest, including fan speed, every swing field, humidity, purify, eco, 8heat and electricity,
start **off**. The ProductType decision tree switches them on.

### 5.1 MEASURED: the A/C does answer ProductType **[PROVEN on hardware, 2026-07-16]**

Read live from node 28 (esp32 fw `1.0.7`, `features` command on the `:2323` diag console):

```
0x66/40 ProductType: REPLY PARSED — the A/C DOES answer.
  cool_heat=1  ai=0  infinite_fan=0  power_save/eco=1  fan_mute/quiet=1
  swing_dir_8=0  swing_follow=0  humidity=0  power_display=1  demand_resp=0
  purify=1     <- MISLABELED: [0x0D]&0x80 is really ac_8heat  -> 8heat = 1
  q_display=0  <- MISLABELED: [0x0A]&0x08 is really ac_purify -> purify = 0
```

**Re-measured 2026-07-18 on fw `1.0.10`**, after the mislabel fix + the extended `[0x19]`/`[0x1A]`
tier landed (issue #83). Same node, same A/C:

```
  cool_heat=1 ai=0 infinite_fan=0 power_save/eco=1 fan_mute/quiet=1
  swing_dir_8=0 swing_follow=0 humidity=0 power_display=1 demand_resp=0
  purify=0 ([0x0A]&0x08)   8heat=1 ([0x0D]&0x80, 8C frost-guard)
  q_display=1 ([0x1A]&0x40)  enable_8heat=0 ([0x1A]&0x04)  trans_102_64=0 ([0x19]&0x08)
  raw 0x66/40 reply length = 45B
```

Three things this settles, all **[PROVEN on hardware]**:

1. **The reply is 45 bytes, so the extended tier is reachable on this unit** (it needs > 39). This
   was previously unknown and was the main risk that the three new flags would be undecidable.
2. **`ac_q_display` = 1: a capability this project could not see before.** The old code's
   `q_display` field actually held `ac_purify` (0), so the true Q-display capability read as absent
   for as long as it has been parsed. It is present on this unit.
3. **`ac_trans_102_64` = 0, so this unit does NOT resolve to the generic profile `199`** (docs/10
   §5a: that bit set → `'199'`). Narrows §6 Q2 to the `100`–`129` range.

Every base-tier flag is byte-identical to the 2026-07-16 capture above, which is the regression
signal that the extended tier was added without disturbing the existing decode.

This resolves the §5 paradox. The static baseline leaves eco, quiet and fan at `supported=0`; the
ProductType reply enables them (`power_save=1`, `fan_mute=1`), and we drive them. The two-stage
mechanism behaves as the disassembly describes, so trust the `supported` gate.

> ⚠️ **A `0` means "not on this unit", never "not on this model range".**
>
> ProductType answers **per unit**. These flags come from node 28's A/C. Node 14's unit may differ,
> and someone else's Hisense will differ more. A capability reading `0` here can read `1` on another
> unit: `ac_ai`, `ac_swing_direction_8`, `ac_humidity` and `ac_purify` all exist as stock verbs, so
> some Hisense unit ships them.
>
> **Design rule: keep the code path, gate it at runtime.** Never delete a capability because this
> bench unit lacks it. Read `hisense_get_features()` and expose per unit. Deleting the path breaks
> compatibility for every owner whose unit has the feature, and this repo is meant to serve them.

**What the flags settle (node 28's A/C only):**

| flag | value | what to do |
|---|---|---|
| `ac_power_display` | **1** | **Display/LED supported.** The highest-value #52 item is real. Worth the capture. |
| `ac_8heat` | **1** | **8 °C frost-guard heat supported** (#52 / I19). Worth the capture. |
| `ac_cool_heat` | **1** | Heat-pump capable, consistent with the heat mode we drive. |
| `ac_ai` | **0** | Absent here. **Keep support on the roadmap**, gate on the flag. Other units ship AI/smart mode. |
| `ac_swing_direction_8` | **0** | 8-position louvre absent here. **Keep it gated, don't delete it.** Deprioritise the capture until someone has a unit reporting `1`. (Basic v/h swing is a different field and works: `poll` shows `hswing=1`.) |
| `ac_humidity` | **0** | No humidity capability here, so the unfed Humidity endpoint in `firmware/docs/01` is **not a firmware gap on this unit**. Gate the endpoint on the flag rather than delete it: a unit reporting `1` should get it. |
| `ac_purify` | **0** | Absent here. Gate, don't remove. |

### Recommendations
1. **Gate at runtime on the ProductType flags, never on the static `supported` baseline.** §5.1
   shows the baseline exists to be refined; read it alone and you would conclude eco, quiet and fan
   are unavailable while we drive all three. `hisense_get_features()` already exposes the refined
   answer, so gate there. It stays **[INFERRED]** that an unsupported capability is un-drivable on
   the wire rather than unadvertised, though no counter-example has turned up.
2. **Runtime gating beats compile-time assumptions, because our bench units are not the fleet.**
   Every capability the stock firmware names is one some Hisense unit ships. Build the path, gate
   the exposure, let the A/C decide. A feature this unit reports `0` for is a **deprioritised
   capture target**, not a dead one.
3. **`hisense_parse_features` mislabeled two flags** (doc 10 §5a). Its `q_display` read
   `ac_purify`, its `purify` read `ac_8heat`, and true `ac_q_display` went unparsed (added
   2026-07-18 with `ac_enable_8heat` + `ac_trans_102_64`, gated on `ext_valid`). Renamed
   2026-07-16: same byte reads, correct names.
4. **`f_humidity` (record 44), `t_temp_type` and `f_temp_in`** give the docs/01 telemetry gaps a
   firmware-grounded gate.
5. **Supporting a dehumidifier (`0x15`)** is bounded work: TEMPLATE_B, 19 records, its own verbs
   (`t_pump`, `t_anion`) and profiles `400`/`499`.

## 6. Unproven, and how to settle each

| # | Open question | How to settle |
|---|---|---|
| ~~1~~ | ~~Does our A/C reply to the `0x66/40` poll?~~ **ANSWERED §5.1: yes.** | ✅ `features` cmd on the esp32 `:2323` console (fw ≥ 1.0.7). Re-run per unit. |
| 1b | Does **node 14**'s A/C return the same flags as node 28's? | The AmebaZ2 has no diag console. Either port `features` to a Matter attribute, or trust per-unit gating only where measured. |
| 2 | Which profile (`100`–`129`/`199`) our unit resolves to. **Narrowed 2026-07-18: NOT `199`** (node 28's `ac_trans_102_64` reads 0 (§5.1), and that bit set is what selects `'199'`). So it is in `100`–`129`. | Log the `0x66/40` payload, or read `0x100096dc` (getter `0x9b6f308c`) on a stock dongle. |
| 3 | `desc` `(b0,b1)`=command vs `(b2,b3)`=status frame. **[INFERRED]** | Dump the mask table in `0x9b6f8adc`'s prologue; check `t_temp` (b2=4 → status byte 6) against a live `0x66` capture. |
| 4 | `word0` `b0`/`b1`/`b3` semantics. | Unknown; no consumer found, but dataflow tracing was not exhaustive. |
| 5 | Whether the 17 unconditional enables are ever written back to `0`. | Looked straight-line in asm; not exhaustively traced. Disable writes exist in the `0x37` branch (`+0x31a`). |
| 6 | Full behaviour of STRUCT_A readers `0x9b6f1e3a`, `0x9b6f8150`. | Not decompiled in full. |
