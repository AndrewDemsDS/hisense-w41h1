# Recovery & Reflash

The safety net. How you get back from a bad flash depends on the hardware: an ESP32 board is
recovered over its USB port, the stock module over a **CH341A SPI programmer + SOIC-8 clip**.

> **First install?** This page is for recovery and clean reflash. For a first install see the
> [User Guide](User-Guide).

## ESP32 boards

An ESP32 cannot be bricked by a bad application image: the ROM bootloader is in silicon, so a USB
write always gets you back. No clip, no dump.

**1. Get the board into download mode.** Plug it in over USB. If no serial port appears, or the
write fails to connect, hold the **BOOT** button while plugging it in (or while pressing RESET),
then release. On the C3 SuperMini that USB port is the only way in, which is why its GPIO18/19 must
never carry the UART ([ESP32 Replacement Board](ESP32-Replacement-Build#the-hard-won-gotchas)).

**2. Rewrite the firmware.** Flash it again from source. `secrets.yaml` is compiled in, so the node
comes back on your Wi-Fi with the same API key and Home Assistant picks it up without re-adoption.

```
python3 firmware/scripts/dev.py flash esphome --board c3 --port /dev/ttyACM0
```

**What survives.** A normal flash leaves the NVS partition alone, so the node keeps any Wi-Fi it
learned through the setup hotspot. `dev.py erase esphome --port P` wipes NVS: that is right for a
brand-new board with stale vendor settings, and it means re-joining Wi-Fi for anything else.

A board that still runs the retired ESP32 Matter firmware is rewritten the same way, with the
release files from the `matter-final` tag's documentation, or moved to ESPHome
([how](Migrating-ESP32-Matter-to-ESPHome)).

## Stock module (CH341A clip)

If an update leaves a stock module dark, or you want to put back what was on it, write the GD25Q32
flash chip directly. Read `firmware/docs/10-stock-module-boot-and-recovery.md` first: a quiet
module is often not a dead one, and one read of the flash status register tells the two apart.

> **Safety first.** Before writing anything, take a full dump of the chip and keep it
> (`dumps/w41h1_dump1.bin`). It holds this unit's calibration data, keys and vendor firmware, and
> it is the only complete way back. `dumps/` is local-only and gitignored (it holds Wi-Fi
> credentials, the device RSA key and the vendor blob).

### Use the clip tooling, NOT flashrom

Flash with `firmware/flasher/` (pyusb, per-sector verify and retry), **not** `flashrom`. The common
flaky CH341A does **silent partial writes** with flashrom on this chip. Clip the SOIC-8 onto the
GD25Q32 (pin 1 = dot corner). In-circuit reads and writes can fail because the SoC contends the
bus. Lift the flash or hold the SoC in reset if you see `0xFF` or no device.

![Which chip to clip](images/module-internal-annotated.png)

*RF shield removed; ① 4-pin A/C connector ② RS-485 transceiver ③ SPI flash (clip here) ④ RTL8710C SoC. Photo: FCC ID 2AGCCAEH-W41H1, public record.*

![CH341A clip wiring](images/ch341a-clip.png)

*SOIC-8 clip maps 1:1 to the flash pins; pin 1 = the dot/dimple corner.*

> ⚠️ The common black CH341A drives SPI at ~5 V even in "3.3 V" mode, a hazard for 3.3 V flash. Use
> a 3.3 V-modded board or a level adapter. Keep the A/C **unplugged** while clipped. The module is
> fragile: clip cycles and handling have killed modules by ESD.

### The tools

| Script | Does |
|---|---|
| `ch341dump.py` | read the whole 4 MB chip to a file. Do it before every write |
| `ch341_readverify.py` | read-only: spot-check that the chip matches an image at key offsets, and report QE |
| `ch341_sr.py` | read the status registers. The **QE bit** (SR2 bit 1) is cleared when the bootloader rejected an image, set after a healthy boot |
| `ch341_setqe.py` | set the QE bit again |
| `ch341flash-full.py <image>` | write the whole chip, `0x0` to `0x400000`, and set QE at the end. For putting a full dump back |
| `ch341flash.py <image>` | write `0x0` to `0x140000` only and leave the rest of the chip alone |

`firmware/flasher/requirements.txt` pins `pyusb`. A stock Linux install needs no udev rule for the
programmer.

### Getting a module back

The way back that has worked on hardware: write the unit's **own** full dump with
`ch341flash-full.py`, then power-cycle. The unit comes up as it was when the dump was taken. If
the dump was taken after custom firmware had run, replace the first firmware slot with the
original vendor slot bytes and erase the second, as described in
`firmware/docs/10-stock-module-boot-and-recovery.md`, section 5.

Two rules from that page that are easy to break here:

- A full-flash image produced by an ESPHome build is blank at `0x1000..0x3FFF`, where the module
  keeps its calibration data. Do not write it over a module as it is.
- Do not copy another unit's dump onto a module to save time. Per-unit data lives outside the
  firmware slots.

Writing the ESPHome firmware to a module with the clip as a first install has not been done. The
module that runs ESPHome got it over the air
([ESPHome Build](ESPHome-Build#the-stock-module-without-a-replacement-board)).

## References

- Flasher scripts: `firmware/flasher/`
- How the module boots, image signing, the QE-bit triage: `firmware/docs/10-stock-module-boot-and-recovery.md`
- Hardware, flash access and dumping: `reverse-engineering/docs/01-hardware.md`
- Why a new ESP32 board must be erased once: `firmware/docs/16-esp32-board-notes.md`
