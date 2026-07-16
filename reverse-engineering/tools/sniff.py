#!/usr/bin/env python3
"""RS-485 sniffer for the Hisense AEH-W41H1 <-> A/C bus.
Frames by inter-byte idle gap, timestamps each frame, logs hex to file + stdout.
Usage: sniff.py [baud] [seconds]   (defaults 9600, 60)
"""
import os, serial, time, sys

PORT = os.environ.get("SNIFF_PORT", "/dev/ttyUSB0")
baud = int(sys.argv[1]) if len(sys.argv) > 1 else 9600
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 60
GAP  = 0.010  # >=10ms idle => frame boundary (typical for these buses)

s = serial.Serial(PORT, baud, timeout=0.001)
s.reset_input_buffer()
log = open(f"capture_{baud}.log", "a")  # written to the current directory
print(f"# sniff baud={baud} for {secs}s  (Ctrl-C to stop early)")
log.write(f"\n# === session baud={baud} start ===\n")

frame = bytearray()
last = time.time()
t0 = time.time()
nframes = 0
try:
    while time.time() - t0 < secs:
        b = s.read(1)
        now = time.time()
        # Flush a completed frame once per iteration (whether or not a byte
        # arrived): an idle gap past GAP is the frame boundary.
        if frame and (now - last) > GAP:
            line = f"[{now-t0:8.3f}] ({len(frame):2d}) {frame.hex(' ')}"
            print(line); log.write(line + "\n"); log.flush()
            nframes += 1
            frame = bytearray()
        if b:
            frame += b
            last = now
except KeyboardInterrupt:
    pass
if frame:
    line = f"[{time.time()-t0:8.3f}] ({len(frame):2d}) {frame.hex(' ')}"
    print(line); log.write(line + "\n")
s.close(); log.close()
print(f"# done: {nframes} frames at baud {baud}")
