# Recovery & Reflash

The safety net. How you get back from a bad flash depends on the hardware: an ESP32 board (ESPHome
or Matter) is recovered over its USB port, the stock AmebaZ2 module over a **CH341A SPI programmer +
SOIC-8 clip**.

> **First install?** This page is for **recovery and clean reflash** of a unit you've already
> converted. For a first install see the [User Guide](User-Guide), or
> [Installing the Custom Firmware](Installing-Custom-Firmware) for a stock AmebaZ2 module.

## ESP32 boards (ESPHome and Matter)

An ESP32 cannot be bricked by a bad application image: the ROM bootloader is in silicon, so a USB
write always gets you back. No clip, no dump.

**1. Get the board into download mode.** Plug it in over USB. If no serial port appears, or the
write fails to connect, hold the **BOOT** button while plugging it in (or while pressing RESET),
then release. On the C3 SuperMini that USB port is the only way in, which is why its GPIO18/19 must
never carry the UART ([ESP32 Replacement Build](ESP32-Replacement-Build#the-hard-won-gotchas)).

**2. Rewrite the firmware.**

- **ESPHome:** flash it again from source. `secrets.yaml` is compiled in, so the node comes back on
  your Wi-Fi with the same API key and Home Assistant picks it up without re-adoption.

  ```
  python3 firmware/scripts/dev.py flash esphome --board c3 --port /dev/ttyACM0
  ```

- **ESP32 Matter:** rebuild and flash with `dev.py flash esp32 --board c3 --port P`, or write a
  release image without a toolchain. Download the files for your chip from
  [Releases](https://github.com/AndrewDemsDS/hisense-w41h1/releases) and write them at the offsets
  in that release's `<chip>-flasher-args.json`:

  ```
  # ESP32-C3 SuperMini
  esptool.py --chip esp32c3 -p /dev/ttyACM0 write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
    0x0 esp32c3-bootloader.bin 0x8000 esp32c3-partition-table.bin \
    0xf000 esp32c3-ota-data.bin 0x20000 esp32c3-v<ver>.bin

  # classic ESP32: the bootloader sits at 0x1000, and the flash frequency is 40m
  esptool.py --chip esp32 -p /dev/ttyUSB0 write_flash --flash_mode dio --flash_size 4MB --flash_freq 40m \
    0x1000 esp32-bootloader.bin 0x8000 esp32-partition-table.bin \
    0xf000 esp32-ota-data.bin 0x20000 esp32-v<ver>.bin
  ```

  After a USB flash, the image you wrote is the node's new delta-OTA base. Archive that exact
  `.bin` before the next OTA, or the next update will be refused
  ([OTA Updates](OTA-Updates#esp32-delta-ota)).

**What survives.** A normal flash leaves the NVS partition alone, so a Matter node keeps its
fabric and an ESPHome node keeps any Wi-Fi it learned through the setup hotspot.
`dev.py erase <target> --port P` wipes NVS: that is right for a brand-new board with stale vendor
settings, and it means re-commissioning (Matter) or re-joining Wi-Fi for anything else.

## AmebaZ2 module (CH341A clip)

If an OTA bricks a stock module, or you want a clean-slate restore, reflash the GD25Q32 directly.

> **Safety first.** A bad flash can brick the module. Before writing anything, keep a full stock dump
> (`dumps/w41h1_dump1.bin`) as your recovery net, and never delete it. `dumps/` is local-only /
> gitignored (it holds Wi-Fi creds + the device RSA key + the vendor blob).

### Use the clip tooling, NOT flashrom

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

### Two write modes

| Script | Range | Effect |
|---|---|---|
| `ch341flash.py <image>` | `0x0 – 0x140000` (~1.27 MB app) | **Region** write. **Preserves** the Matter commissioning KV (`0x2FF000+`) → no re-commission, just power-cycle. This is the normal update/repair path. |
| `ch341flash-full.py <image>` | `0x0 – 0x400000` (whole 4 MB) | **Whole-chip** write. Erases factory data incl. the Matter KV → the device comes up **un-commissioned** (recoverable from the stock dump). Use for a clean-slate restore. |

**Why region-only preserves commissioning:** the Matter fabric/commissioning state lives in the
KV/FTL partition above `0x2FF000`. Writing only `0x0–0x140000` never touches it, so the device boots
the new app and rejoins its existing fabric, no re-pairing.

### Images

- **`built-images/flash_rac-integrated-v<N>.bin`**: the custom build to run (produced by
  `dev.py ota amebaz2 package`). Flash with `ch341flash.py`.
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
- Why a new ESP32 board's NVS breaks commissioning: `firmware/esp32-matter/README.md`
- Attestation / custom-cert path (only if you need a real VID/PID; historical/conditional):
  `firmware/docs/02-fix-attestation.md`
