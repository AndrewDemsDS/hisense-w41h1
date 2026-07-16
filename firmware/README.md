# W41H1 Matter Firmware — full-feature + fixed attestation

Custom firmware for the Hisense **AEH-W41H1** A/C Wi-Fi module (Realtek **RTL8710C /
AmebaZ2**) that fixes the two defects in the stock firmware:

1. **Only a bare Thermostat is exposed over Matter** (LocalTemperature + heat/cool
   setpoints + mode; verified from the binary — no FanControl, no swing, no humidity, no
   eco/purify/etc.). Goal: expose the **full** A/C control surface.
2. **Broken attestation certs** (VID/PID don't cross-reference the CD → `err 604`), so
   stock Matter controllers (Home Assistant) refuse to commission it. Goal: **self-
   consistent certs** so any controller accepts it with no bypass.

The module already reads/writes every A/C function over RS-485 (`ac_cool_heat`,
`ac_humidity`, `ac_fan_*`, `ac_swing_*`, `ac_power_save`, `ac_purify`, `ac_8heat`,
`ac_q_display`, …). This project only adds the Matter clusters and wires the existing
RS-485 driver to them.

## Base SDK

The build is driven from the **AmebaZ2 base SDK**, not from `ameba-rtos-matter` alone —
`ameba-rtos-matter` is a *component* that plugs into it. Three repos, laid out under
`sdk` (`→ ~/ameba-dev`) exactly as upstream's `docs/amebaz2_general_build.md` requires:

```text
sdk/                                      (~/ameba-dev)
├── ameba-rtos-z2/                        Ameba-AIoT/ameba-rtos-z2  — base SDK, build runs here
│   ├── component/common/application/matter/   ← Ameba-AIoT/ameba-rtos-matter @ release/v1.4.2
│   └── third_party/connectedhomeip       → symlink to ../../connectedhomeip (made by matter_setup.sh)
└── connectedhomeip/                      project-chip/connectedhomeip @ v1.4.2-branch (recursive)
```

- **`ameba-rtos-z2`** (`main`) — the RTL8710C SDK; `make` targets live in
  `project/realtek_amebaz2_v0_example/GCC-RELEASE`.
- **`ameba-rtos-matter`** (`release/v1.4.2`) — Realtek's Matter porting layer (provides the
  `room_air_conditioner_port` Room A/C app that this project extends).
- **`connectedhomeip`** (`v1.4.2-branch`, matches the stock firmware's Matter version) —
  the Matter SDK: **ZAP** (cluster editor), **chip-cert** (DAC/PAI/CD generator), build env.

`ameba-rtos-z2/matter_setup.sh amebaz2` wires the symlink, selects the Matter version, and
flips `ENABLE_MATTER=1`. Build target for this device: **`make room_air_conditioner_port`** then
**`make is_matter`** (Room A/C, not the bare `thermostat_port`).

## Status

**Matter firmware implemented and shipping** — Phases 1-3 of the original plan below are
done: core control, fan+swing+setpoints, and the manufacturer cluster (eco/turbo/mute/sleep).
**v23 is running on two units**, including a unit converted straight from stock **over the air**
(no CH341A) via the stock Matter OTA Requestor (F2). The remote-triggered **"77" recommission**
flow (F1) is bench-validated: it reopens the commissioning window and swaps fabrics without
wiping the device. Live status, the open-issues tracker, and the fleet-OTA plan for the
remaining units live in the **Gitea issues tracker**.

## Flashing a unit — two ways

A fresh **stock** `AEH-W41H1` can be converted to this firmware **without ever opening it**, or
via SPI clip:

1. **OTA conversion — preferred (no disassembly, no CH341A).** The stock firmware ships a live
   commissionable Matter interface **+ an OTA Requestor cluster (`0x2A`)**, so you commission it
   (`chip-tool --bypass-attestation-verifier 1`, on the same L2/IoT SSID), repackage the `.ota` to
   the stock target vid/pid (`5004/13825`), and hand it our image over Matter BDX — its AmebaZ2
   image processor writes + boots it (same SDK ⇒ same partition layout). Proven on the kitchen unit.
   Full recipe + the four traps (attestation `604`, cross-VLAN mDNS, `0x0501` retry, vid/pid header):
   **[docs/12](docs/12-ota-convert-stock-unit.md)** — automated in `scripts/ota_convert_stock.sh`.
2. **CH341A SPI flash — direct / recovery fallback.** Clip the GD25Q32 and write the app region:
   `python3 flasher/ch341flash.py built-images/flash_rac-integrated-vNN.bin` (region `0x0–0x140000`,
   preserves the commissioning KV at `0x2FF000+` → power-cycle, no re-commission). Whole-chip
   restore: `ch341flash-full.py`; back up first with `ch341dump.py`. See
   **[docs/10](docs/10-firmware-ota-procedure.md)** (clip-image + recovery details). ⚠️ Handle gently — **repeated clip cycles can ESD-kill
   the module's RF** (this is how the living-room unit died); prefer the OTA path when the unit is alive.

Once on our firmware, version-to-version updates are ordinary **Matter OTA**
([docs/10](docs/10-firmware-ota-procedure.md)) — no physical access at all.

## Original plan (historical)

Original phased plan (all six steps done): see git history. Ongoing work — open issues and the
fleet-OTA of the remaining units — lives in the Gitea issues tracker.

## Setup

`./setup.sh [sdk-root]` clones all three repos into the correct layout (default
`~/ameba-dev`, the `sdk` symlink target), wires them with `matter_setup.sh`, and
bootstraps the connectedhomeip build env. See it for the exact toolchain / host packages.
