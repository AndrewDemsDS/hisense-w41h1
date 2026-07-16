# Whole-chip 4MB read -> file: a pre-reflash safety backup of the GD25Q32.
# Reads in 4KB chunks (fresh READ-DATA command per chunk, explicit address -- no
# reliance on cross-call auto-increment). Verifies the JEDEC id first, same as the
# write tools, so a mis-clipped chip aborts instead of dumping garbage.
# Usage: python3 ch341dump.py <out.bin>
# Transport: ch341common (NOT flashrom).
import sys
from ch341common import CH341, GD25Q32_JEDEC

TOTAL = 0x400000  # 4 MB GD25Q32
STEP = 0x1000  # 4 KB per read (matches the flasher's sector granularity)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: ch341dump.py <out.bin>")
    c = CH341()
    jid = c.jedec().hex()
    print("JEDEC:", jid, "OK" if jid == GD25Q32_JEDEC else "*** WRONG ***")
    if jid != GD25Q32_JEDEC:
        raise SystemExit("wrong chip id -- check the clip/orientation before dumping")
    out = bytearray()
    a = 0
    while a < TOTAL:
        out += c.read(a, STEP)
        a += STEP
        if a % 0x40000 == 0 or a >= TOTAL:
            print("  read 0x%06X/0x%06X" % (a, TOTAL), flush=True)
    if len(out) != TOTAL:
        raise SystemExit("short read: got %d of %d bytes" % (len(out), TOTAL))
    with open(sys.argv[1], "wb") as f:
        f.write(out)
    print("=== dumped %d bytes -> %s ===" % (len(out), sys.argv[1]))


if __name__ == "__main__":
    main()
