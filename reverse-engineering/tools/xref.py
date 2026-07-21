#!/usr/bin/env python3
"""Find xrefs to a XIP address (string pointer) in a carved fw image.

Finds:
  1. literal-pool loads: a 4-byte LE word == target, preceded by an LDR Rt,[PC,#imm]
     whose computed address matches the pool location.
  2. movw/movt pairs constructing the target address.

Usage: xref.py <binfile> <base> <target_hex>
"""

import sys


def main():
    path, base, target = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    data = open(path, "rb").read()
    needle = target.to_bytes(4, "little")
    hits = []
    off = 0
    while True:
        i = data.find(needle, off)
        if i < 0:
            break
        hits.append(i)
        off = i + 1
    print(f"[*] {len(hits)} raw 4-byte occurrences of 0x{target:08x}:")
    for i in hits:
        # check for LDR Rt, [PC, #imm] referencing this pool entry (Thumb)
        # literal pool entry address = base + i ; scan backwards up to 1024+4 bytes
        refs = []
        for j in range(max(0, i - 1050), i, 2):
            hw = int.from_bytes(data[j : j + 2], "little")
            # LDR Rt,[PC,#imm] T1: 01001tttiiiiiiii ; addr = (pc&~3)+imm*4, pc=j+4
            if (hw & 0xF800) == 0x4800:
                imm = hw & 0xFF
                addr = ((base + j + 4) & ~3) + imm * 4
                if addr == base + i:
                    refs.append(j)
        print(
            f"  pool @ file 0x{i:06x} xip 0x{base + i:08x}  ldr-refs: "
            + ", ".join(f"file 0x{r:06x} / 0x{base + r:08x}" for r in refs)
        )
    # movw/movt search
    lo = target & 0xFFFF
    hi = target >> 16
    movts = {}
    movws = {}
    for j in range(0, len(data) - 4, 2):
        h1 = int.from_bytes(data[j : j + 2], "little")
        h2 = int.from_bytes(data[j + 2 : j + 4], "little")
        # MOVW: 11110 i 100100 imm4 | 0 imm3 Rd imm8  => 0xF240 pattern
        # MOVT: 11110 i 101100 imm4 | 0 imm3 Rd imm8  => 0xF2C0
        if (h1 & 0xFBF0) in (0xF240, 0xF2C0):
            imm4 = h1 & 0xF
            i = (h1 >> 10) & 1
            imm3 = (h2 >> 12) & 0x7
            rd = (h2 >> 8) & 0xF
            imm8 = h2 & 0xFF
            val = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8
            if (h1 & 0xFBF0) == 0xF240 and val == lo:
                movws.setdefault(rd, []).append(j)
            if (h1 & 0xFBF0) == 0xF2C0 and val == hi:
                movts.setdefault(rd, []).append(j)
    for rd, locs in movws.items():
        for tj in movts.get(rd, []):
            for wj in locs:
                if 0 < tj - wj <= 24:
                    print(
                        f"  movw/movt r{rd} @ file 0x{wj:06x} / xip 0x{base + wj:08x}"
                    )


if __name__ == "__main__":
    main()
