# Hisense AEH-W41H1 — de-cloud with custom Matter firmware

Replace the ConnectLife cloud on a Hisense **`AEH-W41H1`** A/C Wi-Fi module (Realtek
**RTL8710C / AmebaZ2**) with custom **Matter** firmware, for local control from Home
Assistant — **zero cloud**.

> Not affiliated with, endorsed by, or supported by Hisense, Realtek, or the CSA. Uses Matter
> **test** credentials → development/personal use only, **not a certified Matter product**, **not
> for sale**. See [`NOTICE.md`](NOTICE.md). Reverse-engineering of hardware you own, for
> interoperability. Do this at your own risk — a bad flash can brick the module (recoverable with
> the stock dump).

**User guide:** the operator + developer guides (commissioning, HA control, OTA, recovery, build
pipeline) live in [`firmware/docs/`](firmware/docs/) and [`reverse-engineering/docs/`](reverse-engineering/docs/).

## What you get

- **Local Matter control** — the A/C commissions into `python-matter-server` / Home Assistant; no
  ConnectLife, no `hijuconn` cloud.
- **Full control surface** — HVAC mode (incl. Auto), setpoint (16–32 °C), fan (6 speeds), vertical
  swing, and Eco / Quiet / Turbo / Sleep special modes.
- **Energy monitoring** — live power (W) + voltage, derived from the bus current proxy.
- **OTA updates over Wi-Fi** — after the first CH341 flash, everything else is wireless.

## How it works

```
Home Assistant ─┬─ python-matter-server ── Matter/Wi-Fi ──► RTL8710C module
                                                             (custom AmebaZ2 Matter firmware)
                                                                    │ RS-485 (9600 8N1)
                                                                    ▼
                                                             A/C mainboard
```

The module runs the Realtek AmebaZ2 Matter `room_air_conditioner` example with **our RS-485
driver** bridging Matter attributes ↔ the A/C's internal RS-485 bus (protocol
reverse-engineered + sniff-validated — see [`reverse-engineering/docs/03`](reverse-engineering/docs/03-rs485-ac-protocol.md)).

## Hardware

| | |
|---|---|
| SoC | Realtek RTL8710C (AmebaZ2), secure boot **OFF** |
| Flash | GD25Q32 4 MB (JEDEC `c84016`) |
| A/C bus | UART0 **TX=PA_14 RX=PA_13** @ 9600 8N1 (no DE/RE); log console PA_16 |
| Module port | 4-pin: **5 V · GND · RS-485 A · B** (power from the A/C — bench power browns out the radio) |
| First flash | CH341A SPI programmer + SOIC-8 clip on the GD25Q32 (then OTA forever after) |

## Repository layout

| Path | What |
|---|---|
| `firmware/src/rs485-driver/` | the bus driver (`hisense_rs485.{h,cpp}`) + pure `matter_aircon_map.h` + `power_estimate.h` — **our code (MIT)** |
| `firmware/src/sdk-edits/` | the Matter integration: `matter_drivers.cpp` glue, the `.zap`, the `0xFFF1FC00` mfg-cluster def, and `core-patches/` — plus `README.md` documenting every in-place SDK edit |
| `firmware/scripts/` | `ota-release.sh` (build/package/flash/OTA), `gen-creds.sh`, Matter helpers |
| `firmware/flasher/` | pyusb CH341A flasher (per-sector verify + retry — use this, **not flashrom**) |
| `firmware/test/` | no-hardware QA — host codec + Matter-map tests + `virtual_ac.py` simulator |
| `firmware/docs/` | wiring plan, attestation, QA strategy, energy monitoring, and the OTA/build procedure (`10-firmware-ota-procedure.md`) |
| `reverse-engineering/` | protocol/hardware/cloud/OTA RE, `tools/` (sniffer, decoders), `esphome/` (ESP32-replacement config) |
| `patches/` | your delta to the two SDKs (`git apply`-able; base commits in [`NOTICE.md`](NOTICE.md)) |
| `dumps/` | ⚠️ **local-only, gitignored** — raw flash (Wi-Fi creds + device RSA key + vendor blob). Never published. |

## Quickstart

### 1. Prerequisites
- Linux with the `arm-none-eabi` toolchain and Python 3.
- The **Realtek AmebaZ2 SDK** + its **Matter component**, and **connectedhomeip**, at the commits
  pinned in [`NOTICE.md`](NOTICE.md). **These are not included** (Realtek's is proprietary).
- A **CH341A** programmer + SOIC-8 clip (first flash only).
- `python-matter-server` run with `--enable-test-net-dcl` (test attestation) + Home Assistant.

### 2. Set up the build tree (two steps, in order)
```bash
firmware/setup.sh           # 1) fetch the 3 SDKs into ~/ameba-dev + check out the pinned commits
scripts/setup.sh            # 2) apply patches/ + the Matter-overlay edits, copy our source in
```
Pins live in [`versions.env`](versions.env); full provenance + licensing in [`UPSTREAM.md`](UPSTREAM.md).
Clone with `--recurse-submodules` to also get the HA companion integration under `integrations/`.

### 3. (optional) Your own commissioning credentials
```bash
firmware/scripts/gen-creds.sh   # unique discriminator + passcode (don't ship the shared test code)
```

### 4. Build
```bash
firmware/scripts/ota-release.sh build     # → firmware_is.bin (+ clip image + .ota)
```

### 5. First flash (CH341, once)
Download a prebuilt image from the [Releases](https://github.com/AndrewDemsDS/hisense-w41h1/releases)
(each `amebaz2-vX.Y.Z` / `esp32-vX.Y.Z` tag attaches the built binaries + `SHA256SUMS`), or build your
own from step 4. Then:
```bash
python3 firmware/flasher/ch341flash.py firmware/built-images/flash_rac-integrated-vN.bin
```
Writing only `0x0–0x140000` preserves the Matter commissioning KV → no re-commission on updates.

### 6. Commission
Open the pairing window (remote **Horizon Airflow × 6 → display "77"**), then commission into
`python-matter-server` (your code from step 3, or the SDK test code `34970112332`). Add the Matter
integration in HA and control the A/C.

### 7. Updates — OTA, no clip
```bash
firmware/scripts/ota-release.sh release --bump --flash
```

> **⚠️ No remote way back to stock (yet).** Once a module has been OTA-flashed to this custom
> firmware, there is currently **no way to revert it to the stock ConnectLife firmware over the air**.
> The only supported recovery path is a whole-chip CH341A write of the stock recovery image
> (`built-images/flash_rac-stock-v1.bin`, see [`firmware/docs/10`](firmware/docs/10-firmware-ota-procedure.md)),
> which means you **must already have a dump of the stock firmware** (and physical access with a
> SOIC-8 clip). If you did not capture a stock dump before flashing, there is no clean rollback.
> Making stock revert (and remote dump capture) possible is tracked in the
> [issues](https://github.com/AndrewDemsDS/hisense-w41h1/issues).

> **⚠️ AmebaZ2 OTA serial gotcha (this cost a whole debugging session):** the bootloader A/B-selects
> the fw1/fw2 slot by the image **`FWHS.header.serial`** (`amebaz2_firmware_is.json`), **not** the
> Matter software version. Every OTA build must **bump the serial** or the device applies the update
> then silently reverts on reboot. `ota-release.sh` does this for you (`serial = base + version`).

## Releases & CI

A host-only lint gate (codec + Matter-map + virtual-AC + `.zap` contiguity + version) runs on every
push/PR. Pushing a signed tag builds and publishes a GitHub Release with the firmware attached:
`amebaz2-vX.Y.Z` (version from `firmware/src/version.txt`) and `esp32-vX.Y.Z` (from
`firmware/esp32-matter/CMakeLists.txt` `PROJECT_VER`). Both release builds run on a self-hosted
`sdk-builder` runner that holds the Realtek SDK + ESP-IDF/esp-matter; see the wiki's build-pipeline
page for the runner setup.

## Attestation & credentials

CSA **test** creds (VID `0xFFF1`/PID `0x8001`) — dev-only, uncertified. Details: [`NOTICE.md`](NOTICE.md#matter-credentials).

## De-clouding your whole home

Once local control works, block the module's WAN egress (deny `*.hijuconn.com` + the OTA host) — see
[`reverse-engineering/docs/04`](reverse-engineering/docs/04-cloud-and-firewall.md). Research on
flashing the **other** units over the air (via the stock firmware's dormant Matter stack, no CH341) is
tracked in the [issues](https://github.com/AndrewDemsDS/hisense-w41h1/issues) (Fleet-OTA).

## AI assistance

I built this with AI assistance across the code, reverse-engineering, and docs. Commits carry an
`Assisted-by: AI` trailer.

## License

Original code and docs: **MIT** ([`LICENSE`](LICENSE)). Third-party components (Realtek SDKs —
proprietary, not vendored; connectedhomeip — Apache-2.0) and the credential caveat: [`NOTICE.md`](NOTICE.md).
