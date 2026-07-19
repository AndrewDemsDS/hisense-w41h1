# Upstream dependencies

This repo is a set of original MIT sources + patches that overlay onto two large third-party
SDKs you fetch yourself. Pinned commits live in [`versions.env`](versions.env) (the single source
of truth, sourced by `firmware/setup.sh`) and are mirrored in [`NOTICE.md`](NOTICE.md).

| Upstream | License | Link | Pin | How it's used |
|---|---|---|---|---|
| **Realtek AmebaZ2 base SDK** (`ameba-rtos-z2`) | Proprietary © Realtek, **not redistributable** | [Ameba-AIoT/ameba-rtos-z2](https://github.com/Ameba-AIoT/ameba-rtos-z2) | `0ee0460bc2cf` | Base RTOS/BSP for the RTL8710C; the build runs inside its tree. Our edits ship as [`patches/ameba-rtos-z2.patch`](patches/ameba-rtos-z2.patch). |
| **Realtek AmebaZ2 Matter component** (`ameba-rtos-matter`) | Proprietary © Realtek, **not redistributable** | [Ameba-AIoT/ameba-rtos-matter](https://github.com/Ameba-AIoT/ameba-rtos-matter) | branch `release/v1.4.2` | Plugs into the z2 component slot. Edits that can't be a git patch are reproduced by [`scripts/apply-matter-edits.sh`](scripts/apply-matter-edits.sh). |
| **connectedhomeip / Matter SDK** (`connectedhomeip`) | Apache-2.0 © Project CHIP Authors | [project-chip/connectedhomeip](https://github.com/project-chip/connectedhomeip) | `cc74311cffac` (`v1.4.2-branch`) | Matter SDK + build env (pigweed/gn/ninja/zap) + the host `chip-tool` / `ota-*-app` used by the OTA sim. Edits ship as [`patches/connectedhomeip.patch`](patches/connectedhomeip.patch). |
| **HA companion integration** (`hisense-unified-ac`) | MIT (this author) | submodule `integrations/hisense-unified-ac` → [AndrewDemsDS/hisense-unified-ac](https://github.com/AndrewDemsDS/hisense-unified-ac) | tracked commit | HACS custom integration: one unified climate entity + native card for the de-clouded A/C. |

## Why the Realtek SDKs aren't vendored

They're proprietary and not redistributable, so this repo cannot commit or submodule their source.
`firmware/setup.sh` fetches them from Realtek's own GitHub and checks out the pinned commit; our
changes to them live only as `patches/` (git-apply-able) + `scripts/apply-matter-edits.sh` (sed, for
the untracked component overlay). connectedhomeip is Apache-2.0 but multi-GB with recursive
submodules, so it too is fetched + pinned rather than vendored.

## Setup order

1. `firmware/setup.sh`: fetch the three SDKs into `~/ameba-dev` (the `sdk` symlink) and check out the pins.
2. `scripts/setup.sh`: apply `patches/` + overlay our sources into the SDK example dir.

See [`README.md`](README.md) and [`firmware/docs/10-firmware-ota-procedure.md`](firmware/docs/10-firmware-ota-procedure.md).
