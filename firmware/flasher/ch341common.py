"""Shared CH341A SPI transport and write guards for the GD25Q32 flasher tools.

One CH341 class (USB open, kernel-detach, SPI stream, bit-reversal), the checks every writer
makes before the first erase, and the verify/retry flash_region(). All ch341*.py entry points
import from here so the transport and the guards live in one place. Use these tools, NOT
flashrom (silent partial writes on this CH341A).

What stands between a command line and an erase:
  * the JEDEC id of the chip on the clip (wrong chip: nothing is written);
  * the file: a whole-chip image has to be exactly the size of the chip, hold an application
    image whose every signature verifies, and carry the module's system data; a slot image has
    to verify as the bootloader will check it (amebaz2_image.verify) and fit the slot;
  * a full dump named with --backup, which has to look like a real dump of a W41H1;
  * a typed confirmation that names the target and the size (--confirm for scripted use).
An interrupted or failed run leaves the chip half written with the QE bit clear. Running the
same command again resumes: sectors that already hold the image are skipped.

The host test (firmware/test/test_flasher.py) runs all of this against a chip that is a
bytearray. It cannot see what a real GD25Q32 does, so after a change to this file check it by
hand, on a spare module and never on a unit in service:
  1. ch341dump.py a.bin, then ch341dump.py b.bin, then cmp a.bin b.bin: identical.
  2. ch341flash-full.py a.bin --backup a.bin, and type anything but the token: it stops, and
     ch341_readverify.py a.bin still passes with the QE bit as it was.
  3. The same with the token: the chip already holds a.bin, so all 1024 sectors pass with no
     erase, QE is set and the module boots.
  4. ch341flash.py <a slot image> --layout <the unit's> --slot 2 --backup a.bin, then
     ch341dump.py c.bin: c.bin equals a.bin everywhere outside the slot, and the module boots
     the slot with the higher serial.
  5. ch341flash-full.py a.bin --backup c.bin puts the module back, and it boots as in step 3.
  6. Once, pull the USB plug during step 5 and run the command again: it resumes and finishes.
"""

import json
import sys
import time
from pathlib import Path

import usb.core
import usb.util

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "scripts"))
import amebaz2_image  # noqa: E402

VID, PID = 0x1a86, 0x5512
EP_OUT, EP_IN = 0x02, 0x82
SPI = 0xA8; UIO = 0xAB; OUT = 0x80; DIR = 0x40; END = 0x20; I2C = 0xAA; SET = 0x60
_REV = [int('{:08b}'.format(i)[::-1], 2) for i in range(256)]   # LSB-first bit-reverse LUT
GD25Q32_JEDEC = "c84016"
CHIP_SIZE = 0x400000   # 4 MB GD25Q32
SEC = 0x1000           # sector: the erase unit, and the unit of verify and retry
PAGE = 256             # page program unit
# The module's system data. Present and identical on every dump taken so far (two modules),
# erased in a full-flash image that a build produced. Such an image must not go onto a module
# (firmware/docs/10-stock-module-boot-and-recovery.md, section 5).
SYSTEM_DATA = (0x1000, 0x2000)
LAYOUT_DIR = HERE.parent / "esphome" / "components" / "w41h1_module"
LAYOUTS = ("factory", "sdk", "native")
MAX_IMAGE = 0x1AC000   # the longest firmware slot of any layout


class FlashTimeout(Exception):
    """The chip still reported write-in-progress after the time an operation may take."""


class CH341:
    def __init__(s):
        s._open()

    def _open(s):
        d = usb.core.find(idVendor=VID, idProduct=PID)
        if d is None:
            raise SystemExit("CH341A not found - replug")
        s.d = d
        try:
            if s.d.is_kernel_driver_active(0):
                s.d.detach_kernel_driver(0)
        except (usb.core.USBError, NotImplementedError):
            pass   # no kernel driver bound, or a platform that has none to detach
        s.d.set_configuration()
        usb.util.claim_interface(s.d, 0)
        s.d.write(EP_OUT, bytes([I2C, SET | 0x02, 0x00]))

    def close(s):
        usb.util.release_interface(s.d, 0)

    def cs(s, e):
        s.d.write(EP_OUT, bytes([UIO, OUT | 0x37, DIR | 0x3F, OUT | 0x36, END]) if e else bytes([UIO, OUT | 0x37, END]))

    def xfer(s, out):
        s.cs(True); res = bytearray(); i = 0
        while i < len(out):
            ch = out[i:i + 31]
            s.d.write(EP_OUT, bytes([SPI]) + bytes(_REV[b] for b in ch))
            got = bytearray()
            while len(got) < len(ch):
                got += bytes(s.d.read(EP_IN, len(ch) - len(got), timeout=3000))
            res += bytes(_REV[b] for b in got); i += 31
        s.cs(False); return bytes(res)

    def jedec(s): return s.xfer(bytes([0x9F, 0, 0, 0]))[1:4]
    def rdsr1(s): return s.xfer(bytes([0x05, 0xFF]))[1]
    def rdsr2(s): return s.xfer(bytes([0x35, 0xFF]))[1]
    def wren(s): s.xfer(bytes([0x06]))

    def wip_wait(s, ms=2000):
        """True once the chip is idle, False when it is still busy after `ms`."""
        t = time.time()
        while (time.time() - t) * 1000 < ms:
            if not (s.rdsr1() & 1):
                return True
            time.sleep(0.001)
        return False

    def _idle(s, what):
        # Going on while the chip is busy would send the next command into the void: the chip
        # ignores everything but a status read until the operation is over.
        if not s.wip_wait():
            raise FlashTimeout(what)

    def read(s, addr, n):
        return s.xfer(bytes([0x03, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF]) + bytes(n))[4:]

    def sector_erase(s, addr):
        s.wren(); s.xfer(bytes([0x20, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF])); s._idle("sector erase")

    def page_prog(s, addr, data):
        s.wren(); s.xfer(bytes([0x02, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF]) + data); s._idle("page program")

    def set_qe(s):
        sr2 = s.rdsr2(); s.wren(); s.xfer(bytes([0x31, sr2 | 0x02])); s._idle("status register write")
        return (s.rdsr2() >> 1) & 1


def open_chip():
    """The programmer, once the chip on the clip has answered with the GD25Q32's id. Every tool
    that writes anything (data or the status register) starts here."""
    c = CH341()
    jid = c.jedec().hex()
    print("JEDEC:", jid, "OK" if jid == GD25Q32_JEDEC else "*** WRONG ***")
    if jid != GD25Q32_JEDEC:
        raise SystemExit("wrong chip id -- check the clip and its orientation. Nothing was written")
    return c


# ---- what a file is, checked before the programmer is opened ------------------------------------
def bad_read(blob):
    """Why a whole-chip read is not a dump, or None. A marginal clip can pass the JEDEC probe (one
    short transaction) and then fail every bulk read, returning all-0xFF or echoing the id back.
    Both give a full-size file that looks like a backup and is worthless (2026-08-19)."""
    blank = blob.count(0xFF) * 100 // max(1, len(blob))
    distinct = len(set(blob))
    id_echo = blob[:64] == (bytes.fromhex(GD25Q32_JEDEC) + b"\x40") * (64 // 4)
    if blank > 95 or distinct < 64 or id_echo:
        return "%d%% 0xFF, %d distinct byte values%s" % (blank, distinct, ", id echo" if id_echo else "")
    return None


def images_in(blob):
    """Every application image at a sector boundary of a flash image: [(offset, serial, verifies)].
    The slots of every layout start on one, so this needs to know no layout."""
    found = []
    head = amebaz2_image.HDR0 + amebaz2_image.HDR_LEN
    for off in range(0, max(0, len(blob) - head) + 1, SEC):
        if amebaz2_image.manifest_ok(blob[off:off + head]):
            good, _ = amebaz2_image.verify(blob[off:off + MAX_IMAGE])
            found.append((off, amebaz2_image.read_serial(blob[off:off + head]), good))
    return found


def chip_image_problems(blob, system_data=True):
    """Why a file must not be written over the whole chip. Empty when it may. With
    system_data=False it is only asked whether the file is a dump one could go back to."""
    if len(blob) != CHIP_SIZE:
        return ["it is %d bytes, the chip is %d (0x%X): a whole-chip write takes a full dump and nothing else"
                % (len(blob), CHIP_SIZE, CHIP_SIZE)]
    why = bad_read(blob)
    if why:
        return ["it does not look like a dump (%s)" % why]
    problems = []
    images = images_in(blob)
    if not any(good for _, _, good in images):
        problems.append("no firmware slot in it holds an image whose signatures all verify (%s): the bootloader "
                        "would hang on it" % (", ".join("0x%X: stale" % off for off, _, _ in images) or "no image found"))
    lo, hi = SYSTEM_DATA
    if system_data and blob[lo:hi] == b"\xff" * (hi - lo):
        problems.append("0x%X..0x%X is erased: it lacks the module's system data, like a full-flash image from a "
                        "build. Write the unit's own dump" % (lo, hi - 1))
    return problems


def slot_data(blob):
    """(bytes to write, serial) from an application image file: the image as a build writes it
    (payload and byte-sum), or a raw slot (the same followed by erased flash). ValueError with the
    reason when it is neither, or when a signature in it does not verify."""
    candidates = [(blob[:-4], blob[-4:])]
    carved = amebaz2_image.split_payload(blob, min_len=amebaz2_image.HDR0 + amebaz2_image.HDR_LEN, max_len=MAX_IMAGE)
    if carved:
        candidates.append(carved)
    lines = []
    for payload, trailer in candidates:
        good, lines = amebaz2_image.verify(payload, trailer)
        if good:
            return payload + trailer, amebaz2_image.read_serial(payload)
    raise ValueError("; ".join(amebaz2_image.failures(lines)) or "not an application image")


def layout_slot(layout, slot):
    """(address, length) of firmware slot 1 or 2 in a flash layout, from the component's own file."""
    doc = json.loads((LAYOUT_DIR / ("layout-%s.json" % layout)).read_text())
    addr, length = (int(x, 16) for x in doc["flash"]["ota%d" % slot].split("+"))
    return addr, length


def need_backup(path):
    """The dump named with --backup, or SystemExit. It has to be a real dump: the size of the
    chip, not a failed read, with a firmware image in it that verifies."""
    if not path:
        raise SystemExit("name a full dump of this chip with --backup <dump.bin> (take one with ch341dump.py). "
                         "It is the only complete way back. Nothing was written")
    try:
        blob = Path(path).read_bytes()
    except OSError as e:
        raise SystemExit("--backup %s: %s. Nothing was written" % (path, e.strerror))
    problems = chip_image_problems(blob, system_data=False)
    if problems:
        raise SystemExit("--backup %s is not a usable dump: %s. Nothing was written" % (path, "; ".join(problems)))
    return blob


def confirm(token, given):
    """True when the operator names the target and the size. `given` is --confirm, for a script:
    it has to be the same token, so a script cannot confirm a write it did not mean."""
    if given is not None:
        if given != token:
            raise SystemExit("--confirm has to be %s for this write (got %s). Nothing was written" % (token, given))
        return True
    try:
        typed = input("type %s to write (anything else stops): " % token).strip()
    except EOFError:
        typed = ""
    return typed == token


# ---- the write ----------------------------------------------------------------------------------
def reopen(c):
    """After a USB error: let go of the interface and claim the programmer again. Failing here
    is not the end by itself: the next attempt on the sector fails the same way and counts
    against its four, so a programmer that is gone ends in the half-written message."""
    try:
        usb.util.release_interface(c.d, 0)
    except usb.core.USBError:
        pass
    try:
        c._open()
    except (usb.core.USBError, SystemExit):
        pass


def flash_region(c, data, start):
    """Write `data` at `start`, verify and retry each sector, then set QE. The last sector is
    padded with 0xFF. A sector that already holds its bytes is not erased, so a run that was cut
    short resumes where it stopped. SystemExit when a sector will not verify or QE will not set."""
    length = -(-len(data) // SEC) * SEC
    end = start + length
    if start % SEC or not data or end > CHIP_SIZE:
        raise SystemExit("refusing to write 0x%X bytes at 0x%X: not a sector-aligned range inside the chip" % (len(data), start))
    nsec = length // SEC
    unfinished = ("The chip is half written and the QE bit is not set: the module does not boot until this same "
                  "command has run to the end (sectors that are already written are skipped).")
    print("writing 0x%X..0x%X (%d sectors, verify+retry each)" % (start, end, nsec))
    done = 0
    try:
        for a in range(start, end, SEC):
            want = data[a - start:a - start + SEC]
            want += b'\xff' * (SEC - len(want))
            ok = False
            for _attempt in range(4):
                try:
                    if c.read(a, SEC) == want:
                        ok = True; break
                    c.sector_erase(a)
                    for p in range(0, SEC, PAGE):
                        c.page_prog(a + p, want[p:p + PAGE])
                    if c.read(a, SEC) == want:
                        ok = True; break
                except FlashTimeout as e:
                    print("  0x%X: chip still busy after a %s, trying the sector again" % (a, e), flush=True)
                except usb.core.USBError:
                    time.sleep(0.3)
                    reopen(c)
            if not ok:
                raise SystemExit("SECTOR 0x%X failed after retries, %d of %d sectors written. %s" % (a, done, nsec, unfinished))
            done += 1
            if done % 16 == 0 or a + SEC >= end:
                print("  %d/%d sectors ok (0x%X)" % (done, nsec, a), flush=True)
    except KeyboardInterrupt:
        raise SystemExit("INTERRUPTED, %d of %d sectors written. %s" % (done, nsec, unfinished))
    print("=== all sectors written+verified ===")
    try:
        qe = c.set_qe()
    except FlashTimeout:
        qe = 0
    print("QE bit set:", qe, "-> WILL BOOT" if qe else "-> QE FAILED")
    if not qe:
        raise SystemExit("every sector is written, the QE bit did not set: run ch341_setqe.py, the module does not boot without it")
