#!/usr/bin/env python3
"""Whole-chip 4 MB write + set QE: every byte of the chip is replaced, per-unit data included.
Use it to put a unit's OWN full dump back. Transport and guards: ch341common.

    ch341flash-full.py <dump.bin> --backup <dump-of-the-chip-as-it-is-now.bin> [--confirm chip-0x400000]

Refused before anything is erased: a file that is not exactly the size of the chip, one that
does not look like a dump, one with no firmware image that verifies, one without the module's
system data (a full-flash image from a build), a missing or unusable --backup, another chip on
the clip, and anything but the typed confirmation.

--backup is a dump of the chip as it is now (ch341dump.py). When the chip is erased or cannot be
read and the file to write is this unit's own dump, name that file for both.

Nothing here can tell two modules apart: the low sectors are the same on every unit dumped so
far. That the dump belongs to THIS unit is yours to know. Another unit's dump leaves a module
with the wrong per-unit data (firmware/docs/10-stock-module-boot-and-recovery.md, section 5).
"""
import argparse
import sys
from pathlib import Path

import ch341common as ch


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", metavar="dump.bin", help="the full dump to write (exactly 4 MB)")
    ap.add_argument("--backup", metavar="dump.bin", help="a full dump of the chip as it is now")
    ap.add_argument("--confirm", metavar="TOKEN", help="the confirmation the prompt asks for, for scripted use")
    a = ap.parse_args(argv)
    try:
        image = Path(a.image).read_bytes()
    except OSError as e:
        raise SystemExit("%s: %s" % (a.image, e.strerror))
    problems = ch.chip_image_problems(image)
    if problems:
        raise SystemExit("refusing to write %s over the whole chip: %s. Nothing was written" % (a.image, "; ".join(problems)))
    ch.need_backup(a.backup)

    c = ch.open_chip()
    print("about to replace ALL of the chip (0x0..0x%X, %d bytes) with %s" % (ch.CHIP_SIZE, ch.CHIP_SIZE, a.image))
    for off, serial, good in ch.images_in(image):
        print("  the file holds an image at 0x%06X, serial %d, %s" % (off, serial, "verifies" if good else "DOES NOT VERIFY"))
    print("  the way back is %s. Is the file this unit's own dump? Nothing here can check that." % a.backup)
    if not ch.confirm("chip-0x%X" % ch.CHIP_SIZE, a.confirm):
        raise SystemExit("not confirmed. Nothing was written")
    ch.flash_region(c, image, 0x0)
    c.close()


if __name__ == "__main__":
    main(sys.argv[1:])
