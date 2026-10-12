#!/usr/bin/env python3
"""Whole-chip 4 MB read -> file: the backup to take before any write to the GD25Q32.

    ch341dump.py <out.bin>

Reads the chip twice in 4 KB chunks (a fresh READ-DATA command per chunk, explicit address, no
reliance on cross-call auto-increment) and writes the file only when the two reads agree and
look like a dump: a clip that reads unreliably gives two different reads. Checks the JEDEC id
first, same as the write tools, so a mis-clipped chip aborts instead of dumping garbage.
Read-only. Transport: ch341common (NOT flashrom).
"""
import sys

import ch341common as ch


def read_chip(c, label):
    out = bytearray()
    for a in range(0, ch.CHIP_SIZE, ch.SEC):
        out += c.read(a, ch.SEC)
        if (a + ch.SEC) % 0x40000 == 0:
            print("  %s read 0x%06X/0x%06X" % (label, a + ch.SEC, ch.CHIP_SIZE), flush=True)
    return bytes(out)


def main(argv):
    if len(argv) != 1 or argv[0].startswith("-"):
        raise SystemExit("usage: ch341dump.py <out.bin>")
    c = ch.open_chip()
    first = read_chip(c, "first")
    if len(first) != ch.CHIP_SIZE:
        raise SystemExit("short read: got %d of %d bytes" % (len(first), ch.CHIP_SIZE))
    # Sanity-check the CONTENT, not just the length: the kind of file someone restores to a unit
    # one day. Refuse to write it instead.
    why = ch.bad_read(first)
    if why:
        raise SystemExit(
            "read looks INVALID, refusing to write (%s).\n"
            "The JEDEC probe passed but the bulk reads did not: re-seat the SOIC-8 clip, check\n"
            "pin 1 orientation, and make sure nothing else is powering the module." % why)
    second = read_chip(c, "second")
    if second != first:
        differ = sum(1 for a in range(0, ch.CHIP_SIZE, ch.SEC) if first[a:a + ch.SEC] != second[a:a + ch.SEC])
        raise SystemExit("the two reads differ in %d of %d sectors, refusing to write: the clip does not read "
                         "reliably. Re-seat it and dump again." % (differ, ch.CHIP_SIZE // ch.SEC))
    with open(argv[0], "wb") as f:
        f.write(first)
    print("=== dumped %d bytes -> %s (two reads, identical) ===" % (len(first), argv[0]))
    c.close()


if __name__ == "__main__":
    main(sys.argv[1:])
