# Stock W41H1 — device-type → model capability map (how features are gated per A/C model)

**Scope:** how the stock dongle decides *which features an attached indoor unit has*. This is the
mechanism our driver needs if it is ever to support an A/C model other than the one on the bench.

**Sources:** static RE of `dumps/w41h1_dump1.bin` (2026-07-16), building on the call graph in
[`10-stock-fw-init-and-comms.md`](10-stock-fw-init-and-comms.md). XIP mapping
`runtime = file_offset + 0x9b6d0000`, independently re-verified before decoding (`0x9b805506` →
`0a 04 00`, `0x9b807823` → `07 01 00`, matching doc 10 §3.3). **[PROVEN]**

Every claim is tagged **[PROVEN]** (read off the disassembly — runtime address cited) or
**[INFERRED]** (reasoning). Treat [INFERRED] as untested: acting on an [INFERRED] claim from
doc 10 §4.5 shipped v10207 and killed the A/C link (post-mortem there).

---

## 1. The one-paragraph version

The A/C tells the module its **device-type** in the DevType (`0x0A`) reply. The module uses that
to pick one of **two capability templates** in flash, copies it to a heap struct, and then lets the
**ProductType (`0x66/40`) reply refine it** by switching individual capabilities on. Every consumer
then skips any capability whose `supported` byte is 0. Static baseline + runtime refinement =
**one mechanism, two stages** — not two competing sources of truth.

```
DevType 0x0A reply ──▶ devtype ──▶ S4: malloc + copy TEMPLATE_{A,B} ──▶ [0x1000b8a8]/[0x1000b8b4]
                                                                              │
ProductType 0x66/40 reply ──▶ flag decision tree (0x9b6f0d84) ──▶ writes `supported` bytes ──┘
                                                          └──▶ strcpy 3-digit profile → 0x100096dc
```

## 2. Device-type table **[PROVEN]**

Exhaustive: all **27** callsites of the devtype getter `0x9b6f307c` + all **10** literal-pool xrefs
of `[0x10009687]`. The only codes the image ever compares are **`1`, `0x15`, `0x36`, `0x37`**
(plus `0xFF`, a sentinel written on a NULL DevType payload @ `0x9b6f2194`).
**There is no lookup table** — dispatch is an `if`/`else-if` chain (`bl 0x9b6f307c; cmp; beq`).

| devtype | struct | malloc | records | profile codes | product |
|---|---|---|---|---|---|
| `0x00`, `0x01` | TEMPLATE_A `0x9b807e20` → `[0x1000b8a8]` | `0x33c` | 69 | `100`–`129`, `199` | air conditioner — **ours** |
| `0x15` | TEMPLATE_B `0x9b808144` → `[0x1000b8b4]` | `0xe4` | 19 | `400`, `499` | **dehumidifier** (`t_pump`, `t_anion`, `f_e_wetsensor`) |
| `0x36` | TEMPLATE_A | `0x33c` | 69 | `200`–`203`, `299` | A/C variant |
| `0x37` | TEMPLATE_A | `0x33c` | 69 | `300`–`311`, `399` | A/C variant (also sets `[0x1002942c]=1` @ `0x9b6f2194`) |
| anything else | **none allocated** | — | — | — | falls to `0x9b6f9638` |

- The **getter** `0x9b6f307c` is `ldrb r0,[0x10009687]; cmp r0,#1; it lo; movlo r0,#1` — it maps
  **0 → 1**, so devtype 0 and 1 are the same model.
- `0x36`/`0x37` share TEMPLATE_A with the plain A/C — they differ by *profile code*, not struct.
- **Do not add driver branches for codes outside `{0,1,0x15,0x36,0x37}`** — nothing else exists.
- **Our unit reports `01 01`** (measured on the wire, doc 10 §4.5) → TEMPLATE_A, profiles `1xx`.

> Corrects three errors in an earlier doc 10 §4.1 (fixed there): `0x9b6f9126` is the `bl` to the
> getter, not the dispatch head; `0x9b6f9130` *is* the TEMPLATE_A branch (shared by all four A/C
> codes, `beq.w` @ `0x9b6f9728`/`0x9b6f9732`), not an `0x36`/`0x37`-only path; and TEMPLATE_A is
> selected by an explicit `==1` test — the real `else` allocates nothing.

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

**`+2` is a hard skip-gate** — `0x9b6f9b50`:
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

`word0`'s `b0`/`b1`/`b3` have **no known consumer** — only `+2` is proven meaningful.

## 4. ProductType refines the struct (they are one mechanism) **[PROVEN]**

`handle_producttype_cmd_result` (`0x9b6f0c4c`) does not just log. Gated on **`devtype <= 1`**
(`ldrb r3,[0x10009687]; cmp r3,#1; bhi.w #0x9b6f16c4` @ `0x9b6f0d84`) it writes `supported` bytes
straight into the S4 struct:

```
ldr  r3,=0x1000b8a8; ldr r8,[r3]     ; the S4-allocated struct
strb sl(=1), [r8,#0x266]  ...        ; ~17 unconditional enables, then a flag-driven tree
```

It also `strcpy`s the 3-digit **profile code** to `0x100096dc` (getter `0x9b6f308c`).
`ac_trans_102_64` set → **`199`** (generic/transparent profile; `beq.w` @ `0x9b6f0ddc` →
`'199'` @ `0x9b6f0de4`).

So: **ProductType is the authoritative refinement layer**, and in the `devtype<=1` path it only
ever *enables*. (Disable writes do exist elsewhere — e.g. `= 0` stores in the `0x37` branch at
`+0x31a` — so "only ever enables" is scoped to this path.)

> This corrects doc 10 §5a's *"Whole commit gated on `[0x10009687]==0x15`"* — the `==0x15` test
> only picks the printf **format string**. Fixed in §5a.

## 5. What this means for OUR unit (devtype `0x01`)

**Static baseline: only 9 of 69 capabilities are `supported` before any ProductType refinement** —
`t_work_mode`, `t_power`, `t_temp`, `t_temp_type`, `t_sleep`, `f_temp_in`, `f_e_push`,
`f_e_intemp`, `f_e_incoiltemp`. **[PROVEN]**

Everything else — **fan speed, all swing, humidity, purify, eco, 8heat, electricity** — starts
**off** and is only switched on by the ProductType decision tree.

### 5.1 MEASURED — the A/C *does* answer ProductType **[PROVEN on hardware, 2026-07-16]**

Read live from node 28 (esp32 fw `1.0.7`, `features` command on the `:2323` diag console):

```
0x66/40 ProductType: REPLY PARSED — the A/C DOES answer.
  cool_heat=1  ai=0  infinite_fan=0  power_save/eco=1  fan_mute/quiet=1
  swing_dir_8=0  swing_follow=0  humidity=0  power_display=1  demand_resp=0
  purify=1     <- MISLABELED: [0x0D]&0x80 is really ac_8heat  -> 8heat = 1
  q_display=0  <- MISLABELED: [0x0A]&0x08 is really ac_purify -> purify = 0
```

**This resolves the §5 paradox.** The static baseline has eco/quiet/fan `supported=0`; the
ProductType reply enables them (`power_save=1`, `fan_mute=1`) — and we drive them. The two-stage
mechanism works exactly as the disassembly describes; there is no contradiction and no reason to
doubt the `supported` gate.

> ⚠️ **Scope: these flags are node 28's A/C, not necessarily node 14's.** ProductType is a
> *per-unit* answer. Both are Hisense on the same protocol, but the flags are only proven for the
> unit that returned them. Run `features` against each unit before acting on it for that unit.

**What the flags settle (for node 28's A/C):**

| flag | value | consequence |
|---|---|---|
| `ac_humidity` | **0** | The A/C has **no humidity capability** → the unfed Humidity endpoint in `firmware/docs/01` is **not a firmware gap**; there is nothing to feed it. Drop the endpoint rather than chase it. |
| `ac_swing_direction_8` | **0** | The **8-position louvre** (issue #52 / I19) is **unsupported by this unit** → not worth chasing here. (Basic v/h swing is a different field and works — `poll` shows `hswing=1`.) |
| `ac_power_display` | **1** | **Display/LED IS supported** → the highest-value #52 item is real and worth the capture. |
| `ac_8heat` | **1** | **8 °C frost-guard heat IS supported** (#52 / I19). |
| `ac_purify` | **0** | Purify unsupported → don't expose it. |
| `ac_cool_heat` | **1** | Heat-pump capable, consistent with the heat mode we drive. |

### Recommendations
1. **Gate on the ProductType flags, not on the static `supported` baseline.** §5.1 shows the
   baseline is *meant* to be refined — reading it alone would wrongly conclude eco/quiet/fan are
   unavailable. `hisense_get_features()` already exposes the refined answer; that is the layer to
   gate on. (It remains **[INFERRED]** that an unsupported capability is un-drivable on the wire
   rather than merely unadvertised — but we now have no counter-example.)
2. **Two flags in `hisense_parse_features` are mislabeled** (doc 10 §5a): its `q_display` is
   really `ac_purify`, its `purify` is really `ac_8heat`; true `ac_q_display` is unparsed.
   **Rename, don't rewire** — the byte reads are correct, only the names are wrong.
3. **`f_humidity` (record 44) / `t_temp_type` / `f_temp_in`** give the docs/01 telemetry gaps a
   firmware-grounded gate *once* §6 is settled.
4. **Supporting a dehumidifier (`0x15`)** is a real, bounded piece of work: TEMPLATE_B, 19 records,
   its own verbs (`t_pump`, `t_anion`) and profiles `400`/`499`.

## 6. Unproven — and how to settle each

| # | Open question | How to settle |
|---|---|---|
| ~~1~~ | ~~Does our A/C reply to the `0x66/40` poll?~~ **ANSWERED §5.1: yes.** | ✅ `features` cmd on the esp32 `:2323` console (fw ≥ 1.0.7). Re-run per unit. |
| 1b | Does **node 14**'s A/C return the same flags as node 28's? | The AmebaZ2 has no diag console. Either port `features` to a Matter attribute, or trust per-unit gating only where measured. |
| 2 | Which profile (`100`–`129`/`199`) our unit resolves to. | Log the `0x66/40` payload, or read `0x100096dc` (getter `0x9b6f308c`) on a stock dongle. |
| 3 | `desc` `(b0,b1)`=command vs `(b2,b3)`=status frame. **[INFERRED]** | Dump the mask table in `0x9b6f8adc`'s prologue; check `t_temp` (b2=4 → status byte 6) against a live `0x66` capture. |
| 4 | `word0` `b0`/`b1`/`b3` semantics. | Unknown; no consumer found, but dataflow tracing was not exhaustive. |
| 5 | Whether the 17 unconditional enables are ever written back to `0`. | Looked straight-line in asm; not exhaustively traced. Disable writes exist in the `0x37` branch (`+0x31a`). |
| 6 | Full behaviour of STRUCT_A readers `0x9b6f1e3a`, `0x9b6f8150`. | Not decompiled in full. |
