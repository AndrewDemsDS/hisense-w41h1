# firmware/flasher: clip tools for the stock module's flash

Read and write the GD25Q32 SPI flash of a stock AEH-W41H1 module with a CH341A programmer and a
SOIC-8 clip. These tools are the way back when a module does not boot, and the only way to take
a full backup of one. This page is the reference: each tool, its arguments, what it checks before
it writes, and what has and has not been proven on a module.

Before you clip anything, read the procedure, the wiring and the warnings in
[`docs/guide/Recovery-and-Reflash.md`](../../docs/guide/Recovery-and-Reflash.md) (3.3 V, the A/C
unplugged, never flashrom: it writes this chip partly and reports success on this programmer).
What a rejected image looks like, and why a quiet module is often not a dead one, is in
[`firmware/docs/10-stock-module-boot-and-recovery.md`](../docs/10-stock-module-boot-and-recovery.md).

## Install

```bash
python -m venv ~/.flasher-venv
~/.flasher-venv/bin/pip install -r firmware/flasher/requirements.txt    # pyusb, pinned
```

A stock Arch install needs no udev rule for the programmer. The tools find the CH341A by its USB
id (`1a86:5512`) and stop with `CH341A not found - replug` when it is not there.

## The tools

| Tool | Writes | Does |
|---|---|---|
| `ch341_sr.py` | nothing | reads the JEDEC id and both status registers, and says whether the QE bit is set |
| `ch341dump.py <out.bin>` | a local file | reads the whole 4 MB chip twice and writes the file only when both reads agree and look like a dump |
| `ch341_readverify.py <dump.bin>` | nothing | spot-checks that the chip holds a full dump, and reports QE |
| `ch341_setqe.py` | **the status register** | sets the QE bit, without which the module does not boot |
| `ch341flash-full.py <dump.bin> --backup <dump.bin> [--confirm chip-0x400000]` | **the whole chip** | replaces every byte, per-unit data included, then sets QE. For putting a unit's own full dump back |
| `ch341flash.py <image.bin> --layout factory\|sdk\|native --slot 1\|2 --backup <dump.bin> [--confirm TOKEN]` | **one firmware slot** | writes the image and erased flash to the slot's end, then sets QE. Everything outside the slot is left alone |

`ch341common.py` holds the transport, the checks and the sector writer that all of them share.

## What has run on a module

> **The slot writer has not run on a module.** `ch341flash.py` has written the host test's
> simulated chip only. The details are in the table.

| Tool | Status |
|---|---|
| `ch341_sr.py`, `ch341dump.py` | used on modules: the QE read and the dumps that `firmware/docs/10-stock-module-boot-and-recovery.md` rests on |
| `ch341_setqe.py` | no run on a module is recorded. The writers set QE themselves through the same call |
| the sector writer (`flash_region()`: erase, program, verify and retry per sector) | used on modules. It is what brought units back |
| `ch341flash-full.py` | a unit's own dump was written back with it and booted. That run was before the checks listed below were added (2026-10-12). With the checks it has run against the host test's simulated chip only |
| `ch341flash.py`, the slot writer | **never run on a module in this form.** Until 2026-10-12 this script wrote `0x0..0x140000`, the range of the retired Matter clip image, which under the current layouts starts at the partition table and ends inside a firmware slot. What has not been seen on a module is a boot after a write of one slot alone |
| `ch341_readverify.py` with the even grid | compared against real dumps as files. Not recorded as run against a chip since the change |

The host test (`firmware/test/test_flasher.py`, part of `run_tests.sh`) runs every check and the
writer against a chip that is a bytearray. It cannot see what a real GD25Q32 does.

## What stands between a command line and an erase

Both writers refuse, with "Nothing was written", unless all of this holds. The first three are
checked before the programmer is opened.

1. **The file is what the tool writes.**
   - Whole chip: exactly 4 MB, it looks like a dump (not mostly `0xFF`, not an echo of the chip
     id), at least one firmware image in it verifies, and `0x1000..0x1FFF` is not erased. That
     last check refuses a full-flash image produced by a build, which lacks the module's system
     data.
   - Slot: not a 4 MB file, an application image that verifies the way the bootloader checks it
     (the manifest signature, every sub-image trailer and the byte-sum), and no longer than the
     slot. Both an image as a build writes it (LibreTiny's `image_firmware_is.0x010000.bin`) and
     a raw slot (as the retired Matter conversion's `backup` saved it) are accepted.
2. **`--backup` names a usable full dump.** It has to be the size of the chip, look like a dump
   and hold a firmware image that verifies. It is the only complete way back. When the chip is
   erased or cannot be read and the file to write is this unit's own dump, name that file for
   both.
3. **The chip on the clip is a GD25Q32** (JEDEC id `c84016`). Any other answer stops the run:
   check the clip and its orientation.
4. **Slot writer only: the layout matches the chip.** The tool reads every slot address of every
   layout. An image at an address that is not a slot of the layout you named means the wrong
   `--layout` (or a leftover of an earlier layout), and the run stops. A chip with no image at
   any slot address is refused too: the layout cannot be checked, and that case is for
   `ch341flash-full.py`.
5. **A typed confirmation** that names the target and the size: `chip-0x400000`, or
   `<layout>-ota<slot>-0x<slot length>`, for example `factory-ota2-0x170000`. Before the prompt
   the tool prints what it is about to replace, the images and serials involved, and the backup.
   `--confirm <the same token>` is for a script, and a different token stops the run.

What none of them can check:

- **Which unit is on the clip.** The low sectors are the same on every unit dumped so far (two
  modules), so nothing tells two modules apart. Whether a dump belongs to this unit is for you to
  know. Another unit's dump leaves a module with the wrong per-unit data.
- **Which layout an image was built for.** That cannot be read off the image. The slot writer
  checks the chip's layout, not the image's. Build with the `--board amebaz2-<layout>` that
  matches.
- **Which slot will boot.** The bootloader boots the valid slot with the higher serial. The slot
  writer prints both serials and warns when the other slot's is not lower, and then writes
  anyway: such an image is written and not booted.

## An interrupted or failed write

A run that is interrupted, or stops on a sector that will not verify after four attempts, leaves
the chip half written with the QE bit clear. The module does not boot in that state. Run the
same command again: a sector that already holds its bytes is not erased, so the run resumes
where it stopped. If every sector is written and only the QE bit failed to set, run
`ch341_setqe.py`.

A read that fails in bulk after the id probe passed (a marginal clip) gives all `0xFF` or the id
echoed back. `ch341dump.py` refuses to write such a file, and refuses when its two reads differ.
Re-seat the clip, check pin 1, and make sure nothing else powers the module.

## The check on a module after a change to these tools

From the docstring of `ch341common.py`. Do it on a spare module, never on a unit in service.
Steps 4 and 5 are the ones that have not been done.

1. `ch341dump.py a.bin`, then `ch341dump.py b.bin`, then `cmp a.bin b.bin`: identical.
2. `ch341flash-full.py a.bin --backup a.bin`, and type anything but the token: it stops, and
   `ch341_readverify.py a.bin` still passes with the QE bit as it was.
3. The same with the token: the chip already holds `a.bin`, so all 1024 sectors pass with no
   erase, QE is set and the module boots.
4. `ch341flash.py <a slot image> --layout <the unit's> --slot 2 --backup a.bin`, then
   `ch341dump.py c.bin`: `c.bin` equals `a.bin` everywhere outside the slot, and the module boots
   the slot with the higher serial.
5. `ch341flash-full.py a.bin --backup c.bin` puts the module back, and it boots as in step 3.
6. Once, pull the USB plug during step 5 and run the command again: it resumes and finishes.

## `ch341_readverify.py` is a spot-check

It compares 64 bytes at the start of every 64 KB block and the chip's last 256 bytes with the
file. A pass says the write did not go badly wrong. It is not a full compare: the writers verify
every sector they write. The points are an even grid on purpose
([the dead end it replaced](../docs/10-stock-module-boot-and-recovery.md#6-dead-ends-so-nobody-repeats-them)).

## Dumps are secrets

A dump holds the Wi-Fi credentials the module was given, the device's keys and the vendor's
firmware. Keep it out of git (`dumps/` and `*.bin` are gitignored) and do not share it.
