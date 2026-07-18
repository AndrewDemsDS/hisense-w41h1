# 13 · Choosing a path: ESP32 (esp-matter) vs AmebaZ2 (stock module)

There are two ways to run this project, and both end at the same place: a Matter device on your
Wi-Fi that speaks the A/C's RS-485 bus, with no cloud. They differ in what hardware you put in the
module bay and which SDK you build against.

**Short answer.** If you do not already have a working `AEH-W41H1`, take the **ESP32** path. If you
have a working module and would rather not add hardware, take the **AmebaZ2** path. Neither is a
dead end: the RS-485 driver is shared source, so switching later costs a rebuild, not a rewrite.

Every number below was measured on this project's own hardware and builds (2026-07-18, AmebaZ2
`1.2.9` / ESP32 `1.0.10`), not taken from datasheets.

## At a glance

| | **ESP32** (esp-matter) | **AmebaZ2** (stock W41H1) |
|---|---|---|
| **Cost** | ~€5 BOM (ESP32 board + 3.3 V RS-485 transceiver) | €0 if the module works, plus ~€5 for a CH341A clip you will need anyway |
| **Sourcing** | Available everywhere | W41H1 is fragile (ESD) and hard to source in the EU |
| **First flash** | USB, no disassembly of anything | CH341A SPI clip on the GD25Q32, or OTA conversion from stock (`docs/12`) |
| **Reproducibility** | Not byte-reproducible | Not byte-reproducible |
| **MCU / toolchain** | ESP-IDF 5.5.4, open source, version-pinned in `dependencies.lock`, ~8.3 GB | Realtek AmebaZ2 SDK, proprietary, lives outside the repo, ~32 GB, not pinned |
| **Transport** | Matter over Wi-Fi (2.4 GHz) | Matter over Wi-Fi (2.4 GHz) |
| **OTA** | Delta, mandatory (a full image is rejected). 873 KB this release | Full image, 1.2 MB `.ota` |
| **Flash budget** | 4 MB, app 1.66 MB in a 1.88 MB slot (~84 % used) | 4 MB, `firmware_is.bin` 1.23 MB |
| **Remote diagnostics** | `:2323` console (`features`, `poll`, `decode`, `selftest`) | None yet (tracked separately) |
| **Energy** | Not a differentiator, see below | Not a differentiator, see below |
| **Build time** | ~7 min clean | ~110 s clean (parallel `-j`) |

## The dimensions, in detail

### Cost

The ESP32 path costs about €5 in parts: an ESP32 dev board and a **3.3 V** RS-485 transceiver
(MAX3485 / SP3485 / SN65HVD75). Do not use a 5 V MAX485 module: its RO pin pushes 5 V into the
ESP32 RX and kills it. Power and bus both come from the A/C's 4-pin module connector (5 V confirmed
on the wire).

The AmebaZ2 path costs nothing in parts **if** your module is alive, but you should budget for a
CH341A programmer and SOIC-8 clip regardless: that is how you recover a bricked module, and without
it a bad flash means the module bay is empty until you buy an ESP32 anyway.

The real cost difference is not money, it is **sourcing**. W41H1 modules die easily from handling
and ESD, and are increasingly hard to buy in the EU. An ESP32 is a commodity.

### Reproducibility

**Neither path is byte-reproducible.** This was measured directly, by rebuilding the exact commit
and version that was already deployed and comparing hashes:

| | rebuild | deployed |
|---|---|---|
| ESP32 1.0.10 | `c73d1de8…` | `3d003d66…` |
| AmebaZ2 1.2.9 | `6763c8c5…` | `184da838…` |

ESP-IDF's non-reproducibility is documented upstream. The AmebaZ2 result is the surprising one: the
build differs across runs of the same commit even though `-ffile-prefix-map` strips build paths from
the binary.

The consequence is identical on both paths and it is operational, not theoretical: **you cannot
regenerate a deployed image from git.** Archive the exact binary you shipped. On ESP32 this is
load-bearing, because delta OTA embeds the base image's hash and the device verifies it against its
running partition, so a patch built against a rebuilt base is rejected. This project lost a base
binary once and stranded a node on USB-only flashing.

Toolchain pinning differs sharply, and this is where ESP32 wins clearly. `dependencies.lock` records
the exact IDF version, and the release script now refuses to build when the sourced IDF disagrees
with it. The AmebaZ2 SDK has **no lock file and no version pinning**: it is a ~32 GB tree living
outside the repo at a fixed path, and whatever is in it is what you get.

### MCU and toolchain

The ESP32 builds against ESP-IDF plus esp-matter. Both are open source, installable from scratch,
and pinned. The whole environment is ~8.3 GB.

The AmebaZ2 builds against Realtek's proprietary SDK, which is not redistributable and therefore not
vendored here. It cannot be moved (the build breaks), it is ~32 GB, and the Matter integration edits
live **inside the SDK tree**, mirrored back into this repo rather than the other way around. That
indirection is the single biggest ergonomic difference between the two paths, and it is why the
AmebaZ2 build has more ways to silently produce a wrong image.

Practically: a newcomer can stand up the ESP32 toolchain unattended. The AmebaZ2 toolchain needs the
SDK obtained separately and placed correctly first.

### Transport

Both paths are **Matter over Wi-Fi on 2.4 GHz**, so this dimension does not differentiate them.
Neither is a Thread device, neither needs a Thread border router, and both commission with the same
pairing flow through `python-matter-server`.

Two operational notes. The ESP32 node here is reachable over IPv6 in practice, so tooling that
assumes IPv4 may need adjusting. And the two paths commission with **different product IDs**
(esp-matter's test PID versus the AmebaZ2 PID), which matters because an OTA manifest is only served
to a device whose PID matches.

Where they genuinely diverge is **OTA mechanics**. ESP32 ships delta patches and this is mandatory:
a delta-enabled device rejects a full image outright, transferring it and then never applying it.
Patches are far smaller (873 KB this release, and as small as 45 KB when the binary barely moves),
which matters because OTA fails on weak signal, not on fast links. AmebaZ2 ships full images (1.2 MB)
and selects its A/B boot slot by a serial field in the image header rather than by the Matter
software version, which is a trap that has cost this project a full session.

### Energy

**This is not a differentiator, and this project has not measured it.** Both boards are mains
powered from the A/C's 5 V rail rather than from a battery, and neither firmware enables any power
management: no light sleep, no deep sleep, no Wi-Fi power save. Both radios stay associated
continuously because a Matter-over-Wi-Fi device has to remain reachable.

If you need this quantified, measure at the 5 V rail. Do not infer it from SoC datasheets: idle
current on an always-associated Wi-Fi part is dominated by radio behaviour and AP beacon interval,
not by the core. Any claim here without a meter would be invented, so there is none.

### Flash headroom

Both targets are 4 MB with A/B OTA slots. The ESP32 is the tighter of the two in practice: the app
is 1.66 MB inside a 1.88 MB slot, about 84 % full, leaving roughly 300 KB. That is comfortable now
but it is the budget that a debug console, verbose logging and future features all draw from.

### Diagnostics

The ESP32 build has an embedded `:2323` console with `features`, `poll`, `decode` and `selftest`.
This is not a nicety. It is how per-unit A/C capabilities get measured at all: reading the
`0x66/40` ProductType reply on a live unit is what confirmed the extended capability flags and the
reply length on real hardware.

The AmebaZ2 has **no remote diagnostic surface**. Its only output is the Matter log over UART, which
needs physical access to the module. A capability question about an AmebaZ2 unit currently cannot be
answered without opening the A/C. This gap is tracked as its own issue.

## Recommendation

**Take the ESP32 path if** you do not have a working W41H1, you want a toolchain you can rebuild
from scratch, you want remote diagnostics, or you expect to work on capabilities and need to
measure what your specific A/C reports.

**Take the AmebaZ2 path if** you have a healthy module, you want nothing extra in the module bay,
and you are content to build against a large proprietary SDK you must obtain yourself.

**For most people arriving at this project now, ESP32 is the better default.** Not because the
firmware is better (both run the same driver and expose the same Matter endpoints), but because
every non-firmware factor favours it: parts you can buy, a toolchain you can pin, flashing over USB
instead of a chip clip, and a console to see what the A/C is telling you.

The AmebaZ2 path keeps its own strong justification: it is the only one that needs **no added
hardware at all**, and it proves the module can be fully de-clouded in place, which is the thing
this project set out to demonstrate.

## What this comparison does not settle

- **Energy consumption is unmeasured** on both paths (see above).
- **Long-term reliability is not compared.** Both fleets are small and young. The known failure has
  been a dead W41H1 radio, which is a sample of one and not evidence of a rate.
- **Numbers are from this project's units and builds.** Image sizes and build times will move with
  SDK versions and enabled features.
