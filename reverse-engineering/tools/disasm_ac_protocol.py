#!/usr/bin/env python3
"""Locate and disassemble the Hisense A/C-bus frame parser in an AmebaZ2 dump.

Reproduces how the frame format (F4 F5 ... F4 FB) was recovered:
  1. detect the XIP base by finding the pointer table to a cluster of known strings
  2. disassemble (ARM Thumb) around that literal pool to read the parser

    pip install capstone
    ./disasm_ac_protocol.py w41h1_dump1.bin
"""
import struct
import sys
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_LITTLE_ENDIAN

data = open(sys.argv[1], "rb").read()

# a cluster of parser error strings; offsets are found dynamically
NEEDLES = [
    b"aircond get cmd error: invalid frame",
    b"aircond get cmd link resp error",
    b"aircond get cmd error: no end tag",
    b"aircond get cmd crc error",
    b"aircond get cmd net resp error",
    b"aircond get cmd trans resp error",
]
offs = [data.find(n) for n in NEEDLES]
anchor = offs[3]

# consensus base: a 4KB-aligned base whose pointers reference >=3 of the strings
from collections import Counter
cnt = Counter()
mv = memoryview(data)
for p in range(0, len(data) - 4, 4):
    base = int.from_bytes(mv[p:p+4], "little") - anchor
    if base >= 0 and not (base & 0xFFF) and base <= 0xA0000000:
        cnt[base] += 1
BASE = None
for base, _ in cnt.most_common(50):
    if sum(struct.pack("<I", base + o) in data for o in offs if o > 0) >= 3:
        BASE = base
        break
print(f"detected XIP base: 0x{BASE:08x}")

# literal pool = where pointers to the strings live; parser code precedes it
pool = [data.find(struct.pack("<I", BASE + o)) for o in offs if o > 0]
lo = min(pool)
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN)
start = (lo - 0x260) & ~1
print(f"disassembling parser near file 0x{start:x} (runtime 0x{BASE+start:08x}):\n")
for ins in md.disasm(data[start:lo], BASE + start):
    note = ""
    if ins.mnemonic == "cmp" and any(h in ins.op_str for h in ("#0xf4", "#0xf5", "#0xfb")):
        note = "   <-- frame marker"
    print(f"  0x{ins.address:08x}  {ins.mnemonic:8s} {ins.op_str}{note}")
