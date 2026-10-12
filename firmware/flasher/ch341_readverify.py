#!/usr/bin/env python3
"""Spot-check that the chip holds a full dump, + report QE. Read-only (no erase, no write).

    ch341_readverify.py <dump.bin>

Compares 64 bytes at the start of every 64 KB block and the chip's last 256 bytes with the file.
The points are an even grid on purpose. Until 2026-10-12 they were nine hand-picked offsets of
the retired Matter clip image (its slot at 0xC000, its end at 0x137000). Tried on real dumps,
those nine reported a match between a module with the vendor image in its second slot and the
same module with ESPHome there, and between the dumps of two different modules: none of them
falls where a slot of the factory layout differs. The grid tells both pairs apart.
A pass says the write did not go badly wrong. It is not a full compare: the writers verify every
sector they write. Transport: ch341common.
"""
import sys
from pathlib import Path

import ch341common as ch

STEP = 0x10000


def main(argv):
    if len(argv) != 1 or argv[0].startswith("-"):
        raise SystemExit("usage: ch341_readverify.py <dump.bin>")
    try:
        img = Path(argv[0]).read_bytes()
    except OSError as e:
        raise SystemExit("%s: %s" % (argv[0], e.strerror))
    if len(img) != ch.CHIP_SIZE:
        raise SystemExit("%s is %d bytes, a full dump is %d: nothing to compare the chip with" % (argv[0], len(img), ch.CHIP_SIZE))
    c = ch.open_chip()
    bad = 0
    for off, n in [(a, 64) for a in range(0, ch.CHIP_SIZE, STEP)] + [(ch.CHIP_SIZE - 256, 256)]:
        got = c.read(off, n)
        if got != img[off:off + n]:
            bad += 1
            print("0x%06X: DIFF  chip=%s\n            want=%s" % (off, got[:16].hex(), img[off:off + 16].hex()))
    print("\n=== SPOT-CHECK %s ===" % ("PASS - the chip matches the file at every point checked" if not bad
                                     else "FAIL - %d points differ" % bad))
    print("QE bit currently:", (c.rdsr2() >> 1) & 1, "(0=needs setting before it will boot)")
    c.close()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
