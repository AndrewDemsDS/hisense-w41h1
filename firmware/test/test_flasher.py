#!/usr/bin/env python3
"""firmware/flasher: what the clip tools check before they erase, and what a write does when it
is interrupted, against a chip that is a bytearray.

A wrong write here kills a module, so every refusal is checked for the one thing that matters:
that nothing was erased. No USB device is opened: pyusb and the programmer are stand-ins, so the
commands sent to a real GD25Q32 are NOT covered (the transport is the hardware-validated one).

When this checkout has real dumps (dumps/*.bin, gitignored, or the directory W41H1_DUMPS names)
they are run through the same checks, so a rule that would refuse a real unit's dump fails here.
"""

import contextlib
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import types

import edge_harness
from edge_harness import ROOT, check, load_script, patched
import amebaz2_image as az
import dev
from image_fixtures import fake_image

FLASHER = ROOT / "firmware" / "flasher"
sys.path.insert(0, str(FLASHER))

# Stand-ins for pyusb, so the tools import without it and can never reach a device.
usb = types.ModuleType("usb")
usb.core = types.ModuleType("usb.core")
usb.util = types.ModuleType("usb.util")
usb.core.USBError = type("USBError", (Exception,), {})
usb.core.find = lambda **kw: None
usb.util.release_interface = lambda device, n: None
sys.modules.update({"usb": usb, "usb.core": usb.core, "usb.util": usb.util})
import ch341common as ch  # noqa: E402

full = load_script("ch341flash_full", FLASHER / "ch341flash-full.py")
slot = load_script("ch341flash_slot", FLASHER / "ch341flash.py")
dump = load_script("ch341dump", FLASHER / "ch341dump.py")
readverify = load_script("ch341_readverify", FLASHER / "ch341_readverify.py")
setqe = load_script("ch341_setqe", FLASHER / "ch341_setqe.py")

SEC = ch.SEC
tmp_holder = tempfile.TemporaryDirectory(prefix="flasher-")
TMP = pathlib.Path(tmp_holder.name)


class FakeChip:
    """A GD25Q32 as the tools see it: read, sector erase, page program (bits only clear), QE."""
    jedec_id = ch.GD25Q32_JEDEC
    content = b"\x5a" * 0x8000  # what is on the chip before
    dead = ()                   # sector addresses where programming does nothing
    busy = ()                   # sector addresses whose erase never finishes
    usb_errors = 0              # this many reads fail with a USB error first
    interrupt_at = None         # Ctrl-C when this sector is erased
    qe_sets = True
    flaky_from = None           # reads at or above this address differ from call to call
    last = None

    def __init__(self):
        self.mem = bytearray(self.content)
        self.erased, self.qe, self.d, self.reads, self.status_writes = [], 0, None, 0, 0
        type(self).last = FakeChip.last = self

    gone = False                # the programmer was unplugged: it cannot be opened again

    def _open(self):
        if self.gone:
            raise SystemExit("CH341A not found - replug")

    def close(self):
        pass

    def jedec(self):
        return bytes.fromhex(self.jedec_id)

    def read(self, addr, n):
        self.reads += 1
        if self.usb_errors:
            self.usb_errors -= 1
            raise usb.core.USBError("stall")
        if self.flaky_from is not None and addr >= self.flaky_from:
            return bytes([self.reads % 251]) * n
        return bytes(self.mem[addr:addr + n])

    def sector_erase(self, addr):
        if addr == self.interrupt_at:
            raise KeyboardInterrupt
        if addr in self.busy:
            raise ch.FlashTimeout("sector erase")
        self.erased.append(addr)
        self.mem[addr:addr + SEC] = b"\xff" * SEC

    def page_prog(self, addr, data):
        if addr & ~(SEC - 1) not in self.dead:
            self.mem[addr:addr + len(data)] = bytes(a & b for a, b in zip(self.mem[addr:addr + len(data)], data))

    def rdsr2(self):
        return self.qe << 1

    def set_qe(self):
        self.status_writes += 1
        self.qe = 1 if self.qe_sets else 0
        return self.qe


def chip(**attrs):
    """A fresh fake chip class, installed as the programmer every tool opens."""
    cls = type("Chip", (FakeChip,), attrs)
    cls.last = None
    ch.CH341 = cls
    return cls


def run(fn, *args, typed=None):
    """Call a tool. Returns (how it ended, what it printed): None for a normal return, else the
    SystemExit message or code. `typed` is what the operator types, None for no terminal."""
    def no_terminal(prompt):
        if typed is None:
            raise EOFError
        return typed
    out = io.StringIO()
    how = None
    with patched(ch, input=no_terminal), contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        try:
            code = fn(*args)
            how = code if code else None
        except SystemExit as e:
            how = e.code
    return how, out.getvalue()


def write(name, data):
    path = TMP / name
    path.write_bytes(data)
    return str(path)


# ================================================================================================
print("== flash_region: the sector writer ==")

image = bytes((i * 7 + 3) & 0xFF for i in range(3 * SEC))
REST = b"\x5a" * (0x8000 - 3 * SEC)


def region(data, start, **attrs):
    cls = chip(**attrs)
    c = cls()
    how, out = run(ch.flash_region, c, data, start)
    return how, out, c


how, out, c = region(image, 0)
check(how is None and c.mem == image + REST and c.qe == 1,
      "a region write puts the image there, leaves the rest of the chip alone and sets QE at the end")
how, out, c = region(image, SEC)
check(how is None and c.mem[SEC:4 * SEC] == image and c.mem[:SEC] == b"\x5a" * SEC, "the image goes where it is told to")
how, out, c = region(b"\x5a" * (3 * SEC), 0)
check(how is None and not c.erased, "a sector that already holds the image is not erased again")
how, out, c = region(image[:SEC + 10], 0)
check(how is None and c.mem[SEC + 10:2 * SEC] == b"\xff" * (SEC - 10) and c.mem[2 * SEC:] == b"\x5a" * (0x8000 - 2 * SEC),
      "the last sector is padded with 0xFF, and nothing beyond it is touched")
for data, start, why in ((image, 0x100, "a start that is not on a sector"), (image, ch.CHIP_SIZE - SEC, "a range that runs past the chip"),
                         (b"", 0, "no data")):
    how, out, c = region(data, start)
    check(how and "refusing to write" in str(how) and not c.erased, f"refused before anything is erased: {why}")

how, out, c = region(image, 0, dead=(SEC,))
check("SECTOR 0x1000 failed after retries" in str(how) and c.erased.count(SEC) == 4, "a sector that will not verify is tried four times, then the run stops")
check(c.mem[:SEC] == image[:SEC] and c.mem[2 * SEC:3 * SEC] == b"\x5a" * SEC and c.qe == 0 and "does not boot until this same command" in str(how),
      "it stops with the chip half written and QE not set, and says the module does not boot until the write is finished")

# The chip reports write-in-progress for longer than an operation may take. The old writer ignored
# that and went on to program a sector that was still erasing.
how, out, c = region(image, 0, busy=(SEC,))
check("SECTOR 0x1000 failed" in str(how) and "chip still busy after a sector erase" in out and c.mem[SEC:2 * SEC] == b"\x5a" * SEC and c.qe == 0,
      "a chip that stays busy after an erase is not programmed: the sector is retried, then the run stops")
how, out, c = region(image, 0, usb_errors=2)
check(how is None and c.mem[:3 * SEC] == image, "a USB error is retried and the write goes through")
how, out, c = region(image, 0, usb_errors=99)
check("SECTOR 0x0 failed" in str(how) and not c.erased, "a programmer that keeps failing stops the run")
how, out, c = region(image, 0, usb_errors=99, gone=True)
check("SECTOR 0x0 failed after retries" in str(how) and "does not boot until this same command" in str(how),
      "a programmer that was unplugged ends in the half-written message, not in 'not found'")
how, out, c = region(image, 0, qe_sets=False)
check(how and "QE bit did not set" in str(how) and c.mem[:3 * SEC] == image, "a QE bit that will not set is an error, not a line of output")

# Ctrl-C in the middle, then the same command again.
cls = chip(interrupt_at=2 * SEC)
c = cls()
how, out = run(ch.flash_region, c, image, 0)
check("INTERRUPTED, 2 of 3 sectors written" in str(how) and c.qe == 0 and c.mem[:2 * SEC] == image[:2 * SEC],
      "Ctrl-C stops with a message that says how far it got, QE not set")
c.interrupt_at = None
c.erased.clear()
how, out = run(ch.flash_region, c, image, 0)
check(how is None and c.erased == [2 * SEC] and c.mem[:3 * SEC] == image and c.qe == 1,
      "the same command again writes only the sector that was left, then sets QE")

# ================================================================================================
print("== what a file is: a whole-chip image, a slot image, a backup ==")


def synth_dump(slot1=None, slot2=None, system=True, layout="factory"):
    """A whole-chip image the way a W41H1 has it: partition table, system data, bootloader, two
    firmware slots. Sizes are the real ones, the content is made up."""
    blob = bytearray(b"\xff" * ch.CHIP_SIZE)
    blob[0:0x200] = bytes((i * 13 + 1) & 0xFF for i in range(0x200))
    if system:
        blob[0x1000:0x1008] = b"\x99\x96\x81\x88\x01\x02\x03\x04"
    blob[0x4000:0xC000] = bytes((i * 31 + 7) & 0xFF for i in range(0x8000))
    for n, img in ((1, slot1), (2, slot2)):
        if img:
            addr, _ = ch.layout_slot(layout, n)
            blob[addr:addr + len(img)] = img
    return bytes(blob)


VENDOR = fake_image(100, body=0x56000)           # about 1 MiB, like the vendor's
ESPHOME = fake_image(55933965, body=0x30000)     # LibreTiny's serial is a build time
DUMP = synth_dump(VENDOR, VENDOR)

check(ch.chip_image_problems(DUMP) == [], "a dump with two verifying images and the system data may be written")
check([(off, s, g) for off, s, g in ch.images_in(DUMP)] == [(0x10000, 100, True), (0x190000, 100, True)],
      "both images are found at their slot addresses, with their serials")
for blob, needle, why in (
        (b"not a flash image", "it is 17 bytes, the chip is 4194304", "a 17-byte file"),
        (VENDOR, "bytes, the chip is 4194304", "a slot image"),
        (DUMP + b"\x00", "it is 4194305 bytes", "a file one byte longer than the chip"),
        (DUMP[:-1], "it is 4194303 bytes", "a file one byte shorter than the chip"),
        (b"\xff" * ch.CHIP_SIZE, "does not look like a dump", "an erased chip's worth of 0xFF"),
        (bytes(ch.CHIP_SIZE), "does not look like a dump", "4 MB of zeros"),
        ((bytes.fromhex(ch.GD25Q32_JEDEC) + b"\x40") * (ch.CHIP_SIZE // 4), "does not look like a dump", "the chip id echoed back for 4 MB"),
        (bytes((i * 7) & 0xFF for i in range(0x1000)) * 0x400, "no firmware slot in it holds an image", "4 MB of something else (another device's flash)"),
        (synth_dump(VENDOR[:0x200] + b"\x00" + VENDOR[0x201:], None), "0x10000: stale", "a dump whose only image has a flipped byte"),
        (synth_dump(VENDOR, VENDOR, system=False), "lacks the module's system data", "a full-flash image from a build (erased at 0x1000)"),
):
    problems = ch.chip_image_problems(blob)
    check(problems and needle in "; ".join(problems), f"a whole-chip write refuses {why}")
check(ch.chip_image_problems(synth_dump(VENDOR[:0x200] + b"\x00" + VENDOR[0x201:], VENDOR)) == [],
      "one stale slot beside a good one is allowed: that is what a unit looks like after a bad update, and it boots the good one")
check(ch.chip_image_problems(synth_dump(VENDOR, None)) == [], "one image and an erased second slot is the recovery image the docs describe")

for label, blob in (("as a build writes it", ESPHOME), ("as a raw slot, with erased flash after it", VENDOR + b"\xff" * 0x3000)):
    data, serial = ch.slot_data(blob)
    check(az.verify(data[:-4], data[-4:])[0] and serial == az.read_serial(blob), f"a slot image {label} is taken, and what is written verifies")
check(ch.slot_data(VENDOR + b"\xff" * 0x3000)[0] == VENDOR, "the erased tail of a raw slot is not part of the image")
for label, blob in (("a 17-byte file", b"not a flash image"), ("an empty file", b""), ("an image cut short", ESPHOME[:-1]),
                    ("an image with one flipped byte", ESPHOME[:0x300] + bytes([ESPHOME[0x300] ^ 1]) + ESPHOME[0x301:]),
                    ("an image with a stale byte-sum", ESPHOME[:-1] + bytes([ESPHOME[-1] ^ 1])),
                    ("an image whose serial was patched without re-signing", ESPHOME[:az.SERIAL_OFF] + b"\x01" + ESPHOME[az.SERIAL_OFF + 1:])):
    try:
        ch.slot_data(blob)
        check(False, f"a slot write refuses {label}")
    except ValueError as e:
        check(str(e) != "", f"a slot write refuses {label}")

for board, (addr, length) in dev.ESPHOME_AMEBAZ2_SLOTS.items():
    check(ch.layout_slot(board[8:], 1) == (addr, length), f"slot 1 of the {board[8:]} layout is where dev.py has it")
check(ch.layout_slot("factory", 2) == (0x190000, 0x170000), "slot 2 of the factory layout is 0x190000")
check(ch.MAX_IMAGE == max(length for _, length in dev.ESPHOME_AMEBAZ2_SLOTS.values()), "MAX_IMAGE is the longest slot of any layout")

real = pathlib.Path(os.environ.get("W41H1_DUMPS") or ROOT / "dumps")
dumps = sorted(real.glob("*.bin")) if real.is_dir() else []
for path in dumps:
    blob = path.read_bytes()
    check(ch.chip_image_problems(blob) == [], f"real dump {path.name}: the whole-chip checks pass")
    images = ch.images_in(blob)
    layouts = [name for name in ch.LAYOUTS if {off for off, _, _ in images} <= {ch.layout_slot(name, n)[0] for n in (1, 2)}]
    check(len(layouts) == 1, f"real dump {path.name}: its images sit at the slot addresses of exactly one layout ({layouts})")
    for off, _, good in images:
        if good:
            carved = az.split_payload(blob[off:off + ch.MAX_IMAGE + SEC], min_len=0x140, max_len=ch.MAX_IMAGE)
            if carved and az.bytesum(carved[0]) == carved[1]:
                check(ch.slot_data(carved[0] + carved[1] + b"\xff" * SEC)[0] == carved[0] + carved[1],
                      f"real dump {path.name}: the slot at {off:#x} is accepted as a slot image")
print(f"  --   {len(dumps)} real dump(s) checked" if dumps else "  --   SKIP: no real dumps on this host")

# ================================================================================================
print("== ch341flash-full.py: the whole chip ==")

BEFORE = synth_dump(VENDOR, ESPHOME)             # what is on the chip
dump_path = write("own-dump.bin", DUMP)
backup_path = write("backup.bin", BEFORE)
TOKEN = "chip-0x400000"


def whole(*argv, typed=None, **attrs):
    cls = chip(content=BEFORE, **attrs)
    how, out = run(full.main, list(argv), typed=typed)
    return how, out, cls.last


how, out, c = whole(dump_path, "--backup", backup_path, typed=TOKEN)
check(how is None and c.mem == DUMP and c.qe == 1, "the unit's dump, a backup and the typed confirmation: the chip holds the dump, QE is set")
check("about to replace ALL of the chip" in out and "4194304 bytes" in out and "Nothing here can check that" in out,
      "before it asks, it names the target, the size and what it cannot check")
how, out, c = whole(dump_path, "--backup", backup_path, "--confirm", TOKEN)
check(how is None and c.mem == DUMP, "--confirm with the same token does it without a terminal")
for typed in (None, "", "y", "yes", "chip", "CHIP-0X400000", "chip-0x400000 now"):
    how, out, c = whole(dump_path, "--backup", backup_path, typed=typed)
    check(how == "not confirmed. Nothing was written" and c.mem == BEFORE and not c.erased and c.qe == 0,
          f"{'no terminal' if typed is None else repr(typed) + ' typed'}: nothing is erased")
how, out, c = whole(dump_path, "--backup", backup_path, "--confirm", "yes")
check("--confirm has to be chip-0x400000" in str(how) and not c.erased, "--confirm with another word is refused: a script cannot confirm a write it did not name")
how, out, c = whole(dump_path, "--backup", backup_path, "--confirm", TOKEN, jedec_id="c22016")
check("wrong chip id" in str(how) and not c.erased and c.status_writes == 0, "another chip on the clip stops it before anything is erased")

for label, data, needle in (("a 17-byte file", b"not a flash image", "it is 17 bytes"), ("a slot image", ESPHOME, "the chip is 4194304"),
                            ("4 MB that is no dump", bytes(ch.CHIP_SIZE), "does not look like a dump"),
                            ("a full-flash image from a build", synth_dump(ESPHOME, None, system=False), "system data")):
    how, out, c = whole(write("wrong.bin", data), "--backup", backup_path, "--confirm", TOKEN)
    check(needle in str(how) and "Nothing was written" in str(how) and c is None, f"{label} is refused, and the programmer is not even opened")
how, out, c = whole(dump_path, "--confirm", TOKEN)
check("name a full dump of this chip with --backup" in str(how) and c is None, "no --backup: refused, the programmer is not opened")
how, out, c = whole(dump_path, "--backup", str(TMP / "absent.bin"), "--confirm", TOKEN)
check("--backup" in str(how) and "No such file" in str(how) and c is None, "a --backup file that does not exist: refused")
how, out, c = whole(dump_path, "--backup", write("short.bin", DUMP[:0x1000]), "--confirm", TOKEN)
check("is not a usable dump" in str(how) and c is None, "a --backup that is not a full dump: refused")
how, out, c = whole(dump_path, "--backup", write("blank.bin", b"\xff" * ch.CHIP_SIZE), "--confirm", TOKEN)
check("is not a usable dump" in str(how) and c is None, "a --backup that is a failed read (all 0xFF): refused")
how, out, c = whole(dump_path, "--backup", dump_path, "--confirm", TOKEN)
check(how is None and c.mem == DUMP, "the dump itself named as the backup is allowed (an erased chip has no other)")
how, out, c = whole(str(TMP / "absent.bin"), "--backup", backup_path, "--confirm", TOKEN)
check("No such file" in str(how) and c is None, "an image file that does not exist is a message, not a traceback")

how, out, c = whole(dump_path, "--backup", backup_path, "--confirm", TOKEN, interrupt_at=0x200000)
check("INTERRUPTED" in str(how) and c.qe == 0 and c.mem[:0x200000] == DUMP[:0x200000] and c.mem[0x200000:] == BEFORE[0x200000:],
      "Ctrl-C half way: the first half is the dump, the rest is as it was, QE is not set")

# ================================================================================================
print("== ch341flash.py: one firmware slot ==")

image_path = write("esphome-slot.bin", ESPHOME)
OTA2 = "factory-ota2-0x170000"


def one_slot(*argv, typed=None, content=BEFORE, **attrs):
    cls = chip(content=content, **attrs)
    how, out = run(slot.main, list(argv), typed=typed)
    return how, out, cls.last


UNIT = synth_dump(VENDOR, VENDOR)                # a unit that still has the vendor image in both slots
how, out, c = one_slot(image_path, "--layout", "factory", "--slot", "2", "--backup", backup_path, typed=OTA2, content=UNIT)
want = bytearray(UNIT)
want[0x190000:0x300000] = ESPHOME + b"\xff" * (0x170000 - len(ESPHOME))
check(how is None and c.mem == want and c.qe == 1,
      "the image is in slot 2, erased flash fills the slot behind it, and not one byte outside the slot changed")
check(all(0x190000 <= a < 0x300000 for a in c.erased), "no sector outside the slot was erased")
check("slot 2 of the factory layout (0x190000..0x300000, 1507328 bytes)" in out and "serial 55933965" in out
      and "this slot holds serial 100, the other slot (0x010000) serial 100" in out,
      "before it asks, it names the slot, its range and size, the image's serial and what both slots hold now")
check("WARNING" not in out, "an image whose serial is above the other slot's gets no warning")
how, out, c = one_slot(write("low.bin", fake_image(50, body=0x30000)), "--layout", "factory", "--slot", "2", "--backup", backup_path,
                       "--confirm", OTA2, content=UNIT)
check(how is None and "WARNING: the other slot's serial is not below this image's" in out,
      "an image whose serial is not the highest is written with a warning: the bootloader will boot the other slot")
how, out, c = one_slot(write("raw-slot.bin", VENDOR + b"\xff" * 0x4000), "--layout", "factory", "--slot", "1", "--backup", backup_path,
                       "--confirm", "factory-ota1-0x170000")
check(how is None and c.mem[0x10000:0x10000 + len(VENDOR)] == VENDOR and c.mem[0x190000:] == BEFORE[0x190000:],
      "a raw slot capture goes back into slot 1, and slot 2 is untouched")

for typed in (None, "y", "factory-ota1-0x170000", "sdk-ota2-0x1ac000"):
    how, out, c = one_slot(image_path, "--layout", "factory", "--slot", "2", "--backup", backup_path, typed=typed)
    check(how == "not confirmed. Nothing was written" and c.mem == BEFORE and not c.erased,
          f"{'no terminal' if typed is None else repr(typed) + ' typed'} (another slot's token included): nothing is erased")
for layout, why in (("sdk", "the sdk layout on a unit with the factory layout"), ("native", "the native layout on the same unit")):
    for n in ("1", "2"):
        length = ch.layout_slot(layout, int(n))[1]
        how, out, c = one_slot(image_path, "--layout", layout, "--slot", n, "--backup", backup_path, "--confirm", f"{layout}-ota{n}-0x{length:X}")
        check("is not a slot of the %s layout" % layout in str(how) and not c.erased, f"refused, nothing erased: {why}, slot {n}")
how, out, c = one_slot(image_path, "--layout", "factory", "--slot", "2", "--backup", backup_path, "--confirm", OTA2,
                       content=b"\xff" * ch.CHIP_SIZE)
check("a blank or unknown chip" in str(how) and not c.erased, "a chip with no image at any slot address: refused, the layout cannot be checked")
how, out, c = one_slot(image_path, "--layout", "factory", "--slot", "2", "--backup", backup_path, "--confirm", OTA2, jedec_id="ef4016")
check("wrong chip id" in str(how) and not c.erased, "another chip on the clip stops it before anything is erased")
for label, data, needle in (
        ("a 17-byte file", b"not a flash image", "does not verify as an application image"),
        ("an image with a flipped byte", ESPHOME[:0x300] + bytes([ESPHOME[0x300] ^ 1]) + ESPHOME[0x301:], "does not verify as an application image"),
        ("a whole-chip dump", DUMP, "is a whole-chip dump, not a slot image"),
        ("an image larger than the slot", fake_image(7, body=0x80000), "slot 2 of the factory layout holds 0x170000")):
    how, out, c = one_slot(write("wrong.bin", data), "--layout", "factory", "--slot", "2", "--backup", backup_path, "--confirm", OTA2)
    check(needle in str(how) and "Nothing was written" in str(how) and c is None, f"{label} is refused, and the programmer is not opened")
how, out, c = one_slot(image_path, "--layout", "factory", "--slot", "2", "--confirm", OTA2)
check("--backup" in str(how) and c is None, "no --backup: refused")
how, out, c = one_slot(image_path, "--layout", "factory", "--slot", "2", "--backup", backup_path, "--confirm", OTA2, interrupt_at=0x200000, content=UNIT)
check("INTERRUPTED" in str(how) and c.qe == 0 and c.mem[:0x190000] == UNIT[:0x190000] and c.mem[0x300000:] == UNIT[0x300000:],
      "Ctrl-C half way through a slot: everything outside the slot is still as it was")

# ================================================================================================
print("== ch341dump.py, ch341_readverify.py, ch341_setqe.py ==")

out_path = TMP / "new-dump.bin"
chip(content=BEFORE)
how, out = run(dump.main, [str(out_path)])
check(how is None and out_path.read_bytes() == BEFORE and "two reads, identical" in out, "a dump is written after two reads that agree")
out_path.unlink()
chip(content=BEFORE, flaky_from=0x3F0000)
how, out = run(dump.main, [str(out_path)])
check("the two reads differ in 16 of 1024 sectors" in str(how) and not out_path.exists(),
      "a clip that reads differently the second time: no file is written")
chip(content=b"\xff" * ch.CHIP_SIZE)
how, out = run(dump.main, [str(out_path)])
check("read looks INVALID" in str(how) and not out_path.exists(), "an all-0xFF read is not written as a dump")
cls = chip(content=BEFORE, jedec_id="000000")
how, out = run(dump.main, [str(out_path)])
check("wrong chip id" in str(how) and not out_path.exists(), "no chip answering: no dump")
how, out = run(dump.main, [])
check(how == "usage: ch341dump.py <out.bin>", "ch341dump.py with no file name prints its usage")

chip(content=BEFORE)
how, out = run(readverify.main, [backup_path])
check(how is None and "SPOT-CHECK PASS" in out, "the spot-check passes when the chip holds the file")
how, out = run(readverify.main, [dump_path])
check(how == 1 and "SPOT-CHECK FAIL" in out and "0x190000: DIFF" in out,
      "and fails when only the second slot differs (the nine old points did not see that)")
how, out = run(readverify.main, [image_path])
check("a full dump is 4194304" in str(how), "a file that is not a full dump is refused, not compared")
how, out = run(readverify.main, [])
check(how == "usage: ch341_readverify.py <dump.bin>", "ch341_readverify.py with no file name prints its usage")

cls = chip()
how, out = run(setqe.main)
check(how is None and cls.last.qe == 1 and "SUCCESS" in out, "ch341_setqe.py sets the bit")
cls = chip(jedec_id="ef4016")
how, out = run(setqe.main)
check("wrong chip id" in str(how) and cls.last.status_writes == 0, "ch341_setqe.py writes no status register on another chip")
cls = chip(qe_sets=False)
how, out = run(setqe.main)
check(how == 1 and "FAILED" in out, "ch341_setqe.py exits 1 when the bit did not set")

# The real command lines, with a pyusb stand-in that finds no programmer.
stubs = TMP / "usb-stub"
(stubs / "usb").mkdir(parents=True)
(stubs / "usb/__init__.py").write_text("")
(stubs / "usb/core.py").write_text("class USBError(Exception):\n    pass\n\n\ndef find(**kw):\n    return None\n")
(stubs / "usb/util.py").write_text("")
ENV = {**os.environ, "PYTHONPATH": str(stubs), "PYTHONDONTWRITEBYTECODE": "1"}
for tool in ("ch341flash.py", "ch341flash-full.py", "ch341dump.py", "ch341_readverify.py"):
    r = subprocess.run([sys.executable, str(FLASHER / tool)], capture_output=True, text=True, env=ENV)
    check(r.returncode != 0 and "Traceback" not in r.stderr and "usage" in r.stderr.lower(), f"{tool} with no arguments prints how to call it")
r = subprocess.run([sys.executable, str(FLASHER / "ch341flash-full.py"), dump_path, "--backup", backup_path, "--confirm", TOKEN],
                   capture_output=True, text=True, env=ENV)
check(r.returncode != 0 and "CH341A not found" in r.stderr and "Traceback" not in r.stderr,
      "with no programmer plugged in a write ends in one line, after the file checks passed")
sources = "".join((FLASHER / f).read_text() for f in os.listdir(FLASHER) if f.endswith(".py"))
check("except:" not in sources, "no bare except in the flasher: a typo is not swallowed as a USB error")
check("flashrom" not in sources.replace("NOT flashrom", "").replace("NOT\nflashrom", ""), "no tool in the flasher calls flashrom")

tmp_holder.cleanup()
sys.exit(edge_harness.finish("FLASHER"))
