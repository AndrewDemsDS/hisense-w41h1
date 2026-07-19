# Recovery & Reflash

The safety net. If an OTA bricks a module, or you want a clean-slate restore, reflash the GD25Q32
directly with a **CH341A SPI programmer + SOIC-8 clip**.

> **First install?** This page is for **recovery / brick-restore and clean reflash** of a unit you've
> already converted. If you're converting a brand-new **stock** module for the first time, start at
> [Installing the Custom Firmware](Installing-Custom-Firmware). It covers both the no-clip Matter-OTA
> path and the CH341A clip path. Come back here when you need to un-brick or restore stock.

> **Safety first.** A bad flash can brick the module. Before writing anything, keep a full stock dump
> (`dumps/w41h1_dump1.bin`) as your recovery net, and never delete it. `dumps/` is local-only /
> gitignored (it holds Wi-Fi creds + the device RSA key + the vendor blob).

## Use the clip tooling, NOT flashrom

Flash with `firmware/flasher/` (pyusb, per-sector verify + retry), **not** `flashrom`. The common
flaky CH341A does **silent partial writes** with flashrom on this chip. Clip the SOIC-8 onto the
GD25Q32 (pin 1 = dot corner). In-circuit reads/writes can fail because the SoC contends the bus.
Lift the flash or hold the SoC in reset if you see `0xFF`/no-device.

![Which chip to clip](images/module-internal-annotated.png)

*RF shield removed; ① 4-pin A/C connector ② RS-485 transceiver ③ SPI flash (clip here) ④ RTL8710C SoC. Photo: FCC ID 2AGCCAEH-W41H1, public record.*

![CH341A clip wiring](images/ch341a-clip.png)

*SOIC-8 clip maps 1:1 to the flash pins; pin 1 = the dot/dimple corner.*

> ⚠️ The common black CH341A drives SPI at ~5 V even in "3.3 V" mode, a hazard for 3.3 V flash. Use
> a 3.3 V-modded board or a level adapter.

## Two write modes

| Script | Range | Effect |
|---|---|---|
| `ch341flash.py <image>` | `0x0 – 0x140000` (~1.27 MB app) | **Region** write. **Preserves** the Matter commissioning KV (`0x2FF000+`) → no re-commission, just power-cycle. This is the normal update/repair path. |
| `ch341flash-full.py <image>` | `0x0 – 0x400000` (whole 4 MB) | **Whole-chip** write. Erases factory data incl. the Matter KV → the device comes up **un-commissioned** (recoverable from the stock dump). Use for a clean-slate restore. |

**Why region-only preserves commissioning:** the Matter fabric/commissioning state lives in the
KV/FTL partition above `0x2FF000`. Writing only `0x0–0x140000` never touches it, so the device boots
the new app and rejoins its existing fabric, no re-pairing.

## Images

- **`built-images/flash_rac-integrated-v<N>.bin`**: the custom build to run (produced by
  `ota-release.sh package`). Flash with `ch341flash.py`.
- **`built-images/flash_rac-stock-v1.bin`**: the **stock recovery image** (4 MB = stock
  `room_air_conditioner` `flash_is.bin` + 0xFF pad; built-in test DAC/PAI/CD, pairing code
  `34970112332`). Its `0x0` system block is byte-identical to the stock dump, so a whole-chip
  `ch341flash-full.py` write is safe. Keep it as the fallback recovery image alongside
  `dumps/w41h1_dump1.bin`.

Verify a fresh flash by commissioning into HA Matter (an "uncertified device" warning is expected
with the test certs; see [Commissioning & HA Setup](Commissioning-and-HA-Setup)).

## References

- Flasher scripts: `firmware/flasher/`
- Image/packaging + the stock-recovery note: `firmware/docs/10-firmware-ota-procedure.md` (§5)
- Hardware/flash access + CH341A dumping: `reverse-engineering/docs/01-hardware.md`
- Attestation / custom-cert path (only if you need a real VID/PID; historical/conditional):
  `firmware/docs/02-fix-attestation.md`
