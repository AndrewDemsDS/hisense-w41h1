# NOTICE — third-party components & licensing

This project (the **original code** in `firmware/src/rs485-driver/`, `firmware/test/`,
`firmware/scripts/`, the `.zap`/cluster definitions in `firmware/src/sdk-edits/`, the
`reverse-engineering/tools/`, and all documentation) is licensed **MIT** — see
[`LICENSE`](LICENSE).

It is **not a self-contained firmware build**. It builds *on top of* two large third-party
SDKs that are **not redistributed here** — you obtain them yourself and this repo applies
its changes to them (see [`patches/`](patches/) and
[`firmware/src/sdk-edits/README.md`](firmware/src/sdk-edits/README.md)).

## Third-party dependencies (obtain these yourself)

| Component | License | How this repo modifies it |
|---|---|---|
| **Realtek AmebaZ2 SDK** (`ameba-rtos-z2`) — base RTOS/BSP for the RTL8710C | **Proprietary — © Realtek Semiconductor.** *Not redistributable.* | Base-SDK build-glue edits shipped as [`patches/ameba-rtos-z2.patch`](patches/ameba-rtos-z2.patch). Pinned base commit: `0ee0460bc2cf`. **The patch touches only build-config files (`GCC-RELEASE/Makefile`, `*.json`), executable-bit changes on the prebuilt `gcc_utility` tools, and one Apache-2.0-licensed header — no proprietary Realtek source or binaries are embedded.** |
| **Realtek AmebaZ2 Matter component** (`.../application/matter/`) — Realtek's Matter integration layer | **Proprietary / Apache-2.0 mix — © Realtek.** *Not redistributable.* | Our `room_air_conditioner` example is our own code (dropped into this layer); Realtek framework-file edits (e.g. `matter_events.h`, `platform_opts_matter.h`, the example makefiles) are documented as in-place diffs in `sdk-edits/README.md`. **Realtek proprietary source is never checked in** (e.g. `matter_core.cpp` is used unmodified from the SDK and not vendored here). |
| **connectedhomeip / Matter SDK** (`connectedhomeip`) | **Apache-2.0 — © Project CHIP Authors.** | SDK-file edits shipped as [`patches/connectedhomeip.patch`](patches/connectedhomeip.patch). Pinned base commit: `cc74311cffac`. |

## Matter credentials

The firmware uses **CSA test attestation credentials** — test Vendor ID `0xFFF1`, a Product ID
in the test-DAC range (`0x8001`), and the SDK's test DAC/PAI/CD. These are **development-only**:
the device is **not Matter-certified**, commissions only into controllers that accept test certs
(e.g. `python-matter-server --enable-test-net-dcl`, Home Assistant), shows an "uncertified device"
prompt, and **may not be sold**. Generate your own per-device discriminator/passcode with
`firmware/scripts/gen-creds.sh` (see the README).

## AI assistance

Parts of this project were developed with AI assistance. See the README's *AI assistance*
section; commits carry an `Assisted-by: AI` trailer.
