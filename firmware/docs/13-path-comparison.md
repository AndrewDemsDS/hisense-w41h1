# 13 · Choosing a path: ESP32 (esp-matter), ESP32 (ESPHome), or AmebaZ2 (stock module)

There are three ways to run this project, and all three end at the same place: a local device on
your Wi-Fi that speaks the A/C's RS-485 bus, with no cloud. They differ in what hardware goes in the
module bay, which toolchain you build against, and how Home Assistant sees the result.

**Short answer.** If you do not already have a working `AEH-W41H1`, put an **ESP32** in the bay,
then pick its firmware: **esp-matter** if you want Matter (and with it Apple Home, Google Home or
Alexa), **ESPHome** if Home Assistant is the only controller you care about and you want the
smallest possible toolchain. If you have a working module and would rather not add hardware, take
the **AmebaZ2** path. None of them is a dead end: the RS-485 driver is shared source across all
three, so switching later costs a rebuild, not a rewrite.

Every number below was measured on this project's own hardware and builds (2026-07-18, AmebaZ2
`1.2.9` / ESP32 `1.0.10`), not taken from datasheets. Both Matter paths have shipped several
releases since (AmebaZ2 `1.3.22`, ESP32 `1.1.6` as of 2026-07-22); treat the figures below as a
snapshot from that build, not a live measurement. The ESPHome figures are from the 2026-08 bring-up
against ESPHome 2026.7.4.

## At a glance

| | **ESP32** (esp-matter) | **ESP32** (ESPHome) | **AmebaZ2** (stock W41H1) |
|---|---|---|---|
| **Cost** | ~€5 BOM (ESP32 board + 3.3 V RS-485 transceiver) | Same ~€5 BOM, identical wiring | €0 if the module works, plus ~€5 for a CH341A clip you will need anyway |
| **Sourcing** | Available everywhere | Available everywhere | W41H1 is fragile (ESD) and hard to source in the EU |
| **First flash** | USB, no disassembly of anything | USB, `esphome run` | CH341A SPI clip on the GD25Q32 |
| **Reproducibility** | Not byte-reproducible | Not byte-reproducible, and nothing depends on it (no delta base to archive) | Byte-reproducible since 1.3.5 |
| **MCU / toolchain** | ESP-IDF 5.5.4, open source, version-pinned in `dependencies.lock`, ~8.3 GB | `pip install esphome`; it fetches its own ESP-IDF, no esp-matter, no `sdk/` | Realtek AmebaZ2 SDK, proprietary, lives outside the repo, ~32 GB, not pinned |
| **Transport** | Matter over Wi-Fi (2.4 GHz) | ESPHome native API, Home Assistant only | Matter over Wi-Fi (2.4 GHz) |
| **OTA** | Delta, mandatory (a full image is rejected). 873 KB this release | Full image over ESPHome's own OTA, `esphome run` | Full image, 1.2 MB `.ota` |
| **Flash budget** | 4 MB, app 1.66 MB in a 1.88 MB slot (~84 % used) | 4 MB, app 834 KB (45.5 % of the slot), 47.6 KB RAM | 4 MB, `firmware_is.bin` 1.23 MB |
| **Remote diagnostics** | `:2323` console (`features`, `poll`, `decode`, `selftest`) | `logger:` over the API plus diagnostic entities, always on, no second flavour | `:2323` console (`features`, `poll`, `version`), debug flavour |
| **Energy** | Not a differentiator, see below | Not a differentiator, see below | Not a differentiator, see below |
| **Build time** | ~7 min clean | not measured | ~110 s clean (parallel `-j`) |
| **Maturity** | Deployed, several releases | Newest of the three, on a live A/C since 2026-08, connector-power stage still open | Deployed, several releases |

## The dimensions, in detail

### Cost

Both ESP32 firmwares run on the same board and the same wiring, so cost and sourcing do not
separate them; only the toolchain and the transport do.

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

**AmebaZ2 is byte-reproducible as of 1.3.5. ESP32 is not.** Both claims were measured by rebuilding
a commit and comparing hashes against the image built from it earlier.

The AmebaZ2 result used to be the surprising one. Rebuilding 1.2.9 gave `6763c8c5…` against a
deployed `184da838…`, and `-ffile-prefix-map` (which strips build paths) did not help, because paths
were never the problem. The clock was, in three independent places:

1. `__DATE__` / `__TIME__` in the SDK's `app_start.c` boot banner. Fixed by exporting
   `SOURCE_DATE_EPOCH`, pinned to the HEAD commit's own timestamp, which GCC honours for both
   macros.
2. The SDK's `build_info` make target, which regenerates `.ver` on every build by shelling out to
   `date` (and to `id -u -n`, which also leaked the builder's username into a public image).
   Patched in place to derive both from `SOURCE_DATE_EPOCH` and a constant.
3. Realtek's `elf2bin.linux`, which seeds `srand(time(NULL))` and derives part of the image header
   from it. Packaging one unchanged `.axf` twice produced two different images. It is a closed
   prebuilt binary with no config knob for this (`cipherkey`, `cipheriv` and `privkey_hash` were
   each tried and none is the source), so the release script pins `time()` under it with a small
   `LD_PRELOAD` shim.

Each of those is a handful of bytes, but the image header carries hashes over the payload, so a
5-byte timestamp smeared into roughly 574 differing bytes and made a rebuild look like a completely
different build. All three fixes live in `firmware/scripts/ota-release.sh`; two full clean builds at
different wall-clock times now produce identical bytes, and the tag-time CI rebuild matches the
image that was flashed.

On ESP32 non-reproducibility is documented upstream and still applies, and there the consequence is
operational rather than theoretical: **you cannot regenerate a deployed ESP32 image from git.**
Archive the exact binary you shipped. Delta OTA embeds the base image's hash and the device verifies
it against its running partition, so a patch built against a rebuilt base is rejected. This project
lost a base binary once and stranded a node on USB-only flashing.

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

ESPHome is the smallest of the three by a wide margin: `pip install esphome`, then `esphome run`.
It pulls its own ESP-IDF and needs no esp-matter checkout, no `sdk/` symlink and no release script.
The driver and the ESP-IDF HAL are reused unchanged, registered as local IDF components from the
custom component's `__init__.py`, so there is still exactly one copy of the protocol code in the
repo. See [`15-esphome-path.md`](15-esphome-path.md).

Practically: a newcomer can stand up the ESP32 toolchain unattended, and the ESPHome one in a
minute. The AmebaZ2 toolchain needs the SDK obtained separately and placed correctly first.

### Transport

**This is where ESPHome parts company with the other two.** The esp-matter and AmebaZ2 builds are
both **Matter over Wi-Fi on 2.4 GHz**, so that dimension does not differentiate *them*. Neither is a
Thread device, neither needs a Thread border router, and both commission with the same pairing flow
through `python-matter-server`.

The ESPHome build speaks the **ESPHome native API to Home Assistant and nothing else**. No
commissioning, no fabric, no `python-matter-server`, and no companion HACS integration for the
diagnostics (every fault bit and capability flag is its own entity instead of a bitmap). The price
is every controller that is not Home Assistant: Apple Home, Google Home and Alexa all need Matter,
so they need one of the other two builds. That trade is the whole decision.

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

All three targets are 4 MB with A/B OTA slots. The esp-matter build is the tightest in practice:
the app is 1.66 MB inside a 1.88 MB slot, about 84 % full, leaving roughly 300 KB. Dropping the
Matter stack is most of what the ESPHome build's 834 KB (45.5 % of the same slot) buys, which is
also why it needs none of the delta-OTA machinery. That is comfortable now
but it is the budget that a debug console, verbose logging and future features all draw from.

### Diagnostics

The ESP32 build has an embedded `:2323` console with `features`, `poll`, `decode` and `selftest`.
This is not a nicety. It is how per-unit A/C capabilities get measured at all: reading the
`0x66/40` ProductType reply on a live unit is what confirmed the extended capability flags and the
reply length on real hardware.

The AmebaZ2 gained a console too (2026-07-18), so this is no longer a differentiator. It is a
smaller instrument: `features`, `poll` and `version`, without `decode` or `selftest`, because it is
a line-oriented REPL rather than a port of the ESP32's `esp_console` machinery, which depends on
per-task stdout redirection that does not exist on this target.

The ESPHome build answers this differently: there is no console and no debug flavour, because
`logger:` streams over the API on the deployed image and the capability flags, fault bits, bus link
and checksum counter are all diagnostic entities in Home Assistant. Nothing needs to be reflashed to
ask a live unit what it supports.

Both Matter consoles are **debug-flavour only**. They have no authentication and can drive the A/C bus, so
release images on both paths exclude them. If you need to ask a deployed unit what it supports, flash
the debug image, ask, and flash back.

## Recommendation

**Take the ESP32 esp-matter path if** you do not have a working W41H1 and you want Matter: a
controller other than Home Assistant, a toolchain you can rebuild from scratch, remote diagnostics,
or capability work that needs `decode` and `selftest` on a live unit.

**Take the ESP32 ESPHome path if** Home Assistant is your only controller and you would rather not
own a Matter build at all. It is the shortest route from an empty board to a working climate card:
one `pip install`, one YAML, `esphome run`, and the A/C appears over the native API with the same
control surface and per-bit diagnostics. It is also the newest of the three, so prefer it if you are
comfortable being an early user.

**Take the AmebaZ2 path if** you have a healthy module, you want nothing extra in the module bay,
and you are content to build against a large proprietary SDK you must obtain yourself.

**For most people arriving at this project now, an ESP32 is the better default.** Not because the
firmware is better (all three run the same driver and expose the same control surface), but because
every non-firmware factor favours it: parts you can buy, a toolchain you can pin, and flashing over
USB instead of a chip clip. Which of its two firmwares you flash comes down to one question: does
anything other than Home Assistant need to see this A/C?

The AmebaZ2 path keeps its own strong justification: it is the only one that needs **no added
hardware at all**, and it proves the module can be fully de-clouded in place, which is the thing
this project set out to demonstrate.

## What this comparison does not settle

- **Energy consumption is unmeasured** on both paths (see above).
- **Long-term reliability is not compared.** Both fleets are small and young. The known failure has
  been a dead W41H1 radio, which is a sample of one and not evidence of a rate.
- **The ESPHome path has the least field time.** It has driven a live A/C since 2026-08 and found
  two protocol bugs the bench had not, but stage 3 of the bring-up (powered from the A/C connector,
  closed up) is still open, so it has not been left running unattended for as long as the other two.
- **Numbers are from this project's units and builds.** Image sizes and build times will move with
  SDK versions and enabled features.
