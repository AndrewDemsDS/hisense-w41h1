#!/usr/bin/env python3
"""Field-mapper: capture live A/C status frames and report which byte offsets
change, so you can locate a field by toggling ONE control and diffing.

Listens on a serial port for `secs`, keeps every 160-byte 0x66 status frame,
and prints, per offset that varied, the sequence of value transitions. Boolean-
looking offsets (<=3 distinct values, returns to a base) are flagged as likely
flags/toggles; many-valued offsets are flagged as analog/telemetry drift.

  ./map_diff.py [secs]     (default 35; env PORT/BAUD override, defaults ttyUSB0/9600)

Toggle exactly one thing during the window, then read which offset moved.
"""
import os, sys, time, serial

# Same directory: reuse the driver-mirroring frame splitter instead of a second copy.
from decode_ac_frames import split_frames

PORT = os.environ.get("PORT", "/dev/ttyUSB0")
BAUD = int(os.environ.get("BAUD", "9600"))
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 35


def main():
    s = serial.Serial(PORT, BAUD, timeout=0.05); s.reset_input_buffer()
    print(f"# capturing {SECS:.0f}s on {PORT} @ {BAUD} -- toggle ONE control now ...")
    raw = bytearray(); t0 = time.time()
    while time.time() - t0 < SECS:
        c = s.read(512)
        if c:
            raw += c
    s.close()
    stamp = int(t0)
    open(f"/tmp/mapdiff_{stamp}.bin", "wb").write(raw)
    S = [f for f in split_frames(raw) if len(f) == 160 and f[13] == 0x66]
    print(f"# {len(S)} status(160B) frames; raw saved /tmp/mapdiff_{stamp}.bin")
    if len(S) < 2:
        print("# not enough frames -- is the RO tap connected and the A/C on?")
        return
    for o in range(16, 156):
        vals = [f[o] for f in S]
        if len(set(vals)) > 1:
            comp = [vals[0]] + [v for k, v in enumerate(vals[1:], 1) if v != vals[k - 1]]
            note = "  <-- TOGGLE/flag candidate" if len(set(vals)) <= 3 else "  (analog/telemetry drift)"
            bitnote = ""
            if len(set(vals)) == 2:
                a, b = sorted(set(vals))
                x = a ^ b
                if x and (x & (x - 1)) == 0:
                    bitnote = f"  [single bit 0x{x:02X} = bit{ x.bit_length()-1 }]"
            print(f"  off {o:3d}: {[hex(v) for v in comp]}{note}{bitnote}")


if __name__ == "__main__":
    main()
