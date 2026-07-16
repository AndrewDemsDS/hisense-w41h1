#!/usr/bin/env bash
# QA runner -- Layer 1 (codec golden tests) + Layer 2 (simulator self-check).
# Host-only, no A/C, no chip. Exit non-zero on any failure (CI-friendly).
set -euo pipefail
cd "$(dirname "$0")"

echo "== Layer 1a: codec golden regression =="
g++ -std=c++11 -Wall -Istubinc -I. -I../src/rs485-driver \
    test_codec.cpp ../src/rs485-driver/hisense_rs485.cpp -o test_codec
./test_codec

echo
echo "== Layer 1b: Matter <-> A/C mapping (Matter-side QA, no chip) =="
g++ -std=c++11 -Wall -Istubinc -I. -I../src/rs485-driver \
    test_matter_map.cpp ../src/rs485-driver/hisense_rs485.cpp -o test_matter_map
./test_matter_map

echo
echo "== Layer 2: virtual A/C <-> decoder round-trip =="
python3 - <<'PY'
import importlib.util, sys
def load(name, path):
    s = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(s); s.loader.exec_module(m); return m
vac = load("vac", "virtual_ac.py")
dec = load("dec", "../../reverse-engineering/tools/decode_ac_frames.py")

ok = True
def check(cond, msg):
    global ok
    print(("  ok  " if cond else "  FAIL ") + msg); ok = ok and cond

ac = vac.VirtualAC(); ac.mode="cool"; ac.setpoint=22; ac.indoor=21; ac.outdoor=32; ac.fan="auto"; ac.comp=45
f = list(dec.split_frames(ac.status_frame()))[0]
n = len(f); check(dec.checksum(f, n-4) == ((f[n-4]<<8)|f[n-3]), "simulator status frame checksum valid")
d = dec.decode_status(f)
check("mode=COOL" in d and "setpoint=22C" in d and "indoor=21C" in d and "comp=45Hz" in d, f"decoder reads sim state: {d}")

# command -> state mutation, checked via the sim's decode of the driver's golden bytes
for b16,name in [(0x0B,"low"),(0x13,"high")]:
    ac2=vac.VirtualAC(); cmd=bytearray(50); cmd[13]=0x65; cmd[16]=b16; ac2.apply_command(cmd)
    check(ac2.fan==name, f"cmd byte16=0x{b16:02X} -> fan {ac2.fan}")
cmd=bytearray(50); cmd[13]=0x65; cmd[18]=0x90; ac3=vac.VirtualAC(); ac3.apply_command(cmd)
check(ac3.mode=="auto", "cmd byte18=0x90 -> mode auto")
cmd=bytearray(50); cmd[13]=0x65; cmd[33]=0x30; ac4=vac.VirtualAC(); ac4.apply_command(cmd)
check(ac4.eco, "cmd byte33=0x30 -> eco on")

print("== ROUND-TRIP OK ==" if ok else "== ROUND-TRIP FAILED ==")
sys.exit(0 if ok else 1)
PY

echo
echo "ALL QA LAYERS PASSED"
