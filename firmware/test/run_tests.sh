#!/usr/bin/env bash
# QA runner: every host layer, in the order of the labels it prints. Layers 1a to 1p are the
# codec, the component's pure code, the fuzz replay and the configuration matrix. Layer 2 is the
# simulator self-check, layer 3 the image signing chain, layers 4 to 6 the tooling. The table is
# in firmware/docs/04-qa-strategy.md.
# Host-only, no A/C, no chip. Exit non-zero on any failure (CI-friendly).
set -euo pipefail
cd "$(dirname "$0")"

echo "== Layer 1a: codec golden regression =="
g++ -std=c++11 -Wall -Istubinc -I. -Ireference-driver \
    test_codec.cpp reference-driver/hisense_rs485.cpp -o test_codec
./test_codec

echo
echo "== Layer 1b: reference map (fan ladder, setpoint clamp, running state, power estimate) =="
g++ -std=c++11 -Wall -Istubinc -I. -Ireference-driver \
    test_reference_map.cpp reference-driver/hisense_rs485.cpp -o test_reference_map
./test_reference_map

echo
echo "== Layer 1c: ESPHome <-> A/C mapping (ESPHome-side QA, no chip, no ESPHome) =="
g++ -std=c++11 -Wall -Istubinc -I. -Ireference-driver \
    test_esphome_map.cpp reference-driver/hisense_rs485.cpp -o test_esphome_map
./test_esphome_map
echo
echo "== Layer 1d: ESPHome codec port == reference driver (parity, see the test's header) =="
# The port is C++17 (ESPHome's nested namespaces); the original stays C++11 as it ships.
g++ -std=c++11 -Wall -Istubinc -I. -Ireference-driver -c reference-driver/hisense_rs485.cpp \
    -o parity_driver.o
g++ -std=c++17 -Wall -c ../esphome/components/hisense_ac/hisense_protocol.cpp -o parity_port.o
g++ -std=c++17 -Wall -Istubinc -I. -Ireference-driver -I../esphome/components/hisense_ac \
    test_esphome_codec_parity.cpp parity_port.o parity_driver.o -o test_esphome_codec_parity
rm -f parity_driver.o parity_port.o
./test_esphome_codec_parity
echo
echo "== Layer 1e: ESPHome bus scheduler vs the original bus task's contract (simulated A/C) =="
g++ -std=c++17 -Wall -I../esphome/components/hisense_ac test_esphome_bus.cpp \
    ../esphome/components/hisense_ac/hisense_bus.cpp ../esphome/components/hisense_ac/hisense_protocol.cpp \
    -o test_esphome_bus
./test_esphome_bus
echo
echo "== Layer 1f: ESPHome component edge cases (shadow sync rules, boundaries, damaged byte streams) =="
g++ -std=c++17 -Wall -I../esphome/components/hisense_ac test_esphome_edge_cases.cpp \
    ../esphome/components/hisense_ac/hisense_protocol.cpp -o test_esphome_edge_cases
./test_esphome_edge_cases
echo
echo "== Layer 1g: ESPHome confirm and retry, power and mode in one frame (decisions and scenarios) =="
g++ -std=c++17 -Wall -I../esphome/components/hisense_ac test_esphome_confirm.cpp \
    ../esphome/components/hisense_ac/hisense_protocol.cpp -o test_esphome_confirm
./test_esphome_confirm
echo
echo "== Layer 1h: ESPHome entities (the climate request as a plan, what a status frame shows, switches, the sleep select) =="
g++ -std=c++17 -Wall -I../esphome/components/hisense_ac test_esphome_entities.cpp \
    ../esphome/components/hisense_ac/hisense_protocol.cpp -o test_esphome_entities.bin
./test_esphome_entities.bin
python3 ./test_esphome_tables.py  # the Python schema tables against the C++ constants they name
g++ -std=c++17 -Wall -I../esphome/components/hisense_ac test_esphome_setup_request.cpp ../esphome/components/hisense_ac/hisense_bus.cpp ../esphome/components/hisense_ac/hisense_protocol.cpp -o test_esphome_setup_request.bin; ./test_esphome_setup_request.bin  # prints its own "Layer 1i" heading; .bin is gitignored
echo
echo "== Layer 1j: stock module under ESPHome, when the A/C bus serial port is restarted =="
g++ -std=c++17 -Wall -I../esphome/components/w41h1_module test_esphome_uart_policy.cpp -o test_esphome_uart_policy
./test_esphome_uart_policy

echo
echo "== Layer 1k: ESPHome setup request edge cases (every ordering of the two request bits, the lockout, link loss) =="
g++ -std=c++17 -Wall -I../esphome/components/hisense_ac test_esphome_setup_edges.cpp \
    ../esphome/components/hisense_ac/hisense_bus.cpp ../esphome/components/hisense_ac/hisense_protocol.cpp \
    -o test_esphome_setup_edges.bin
./test_esphome_setup_edges.bin

echo
echo "== Layer 1l: ESPHome bus scheduler edge cases and stress (clock wrap, damaged replies, seeded random runs) =="
g++ -std=c++17 -Wall -O1 -I../esphome/components/hisense_ac test_esphome_bus_stress.cpp \
    ../esphome/components/hisense_ac/hisense_bus.cpp ../esphome/components/hisense_ac/hisense_protocol.cpp \
    -o test_esphome_bus_stress.bin
./test_esphome_bus_stress.bin

echo
echo "== Layer 1m: coverage and mutation gap tests (codec, map, serial port policy) =="
g++ -std=c++17 -Wall -I../esphome/components/hisense_ac test_esphome_codec_gaps.cpp \
    ../esphome/components/hisense_ac/hisense_protocol.cpp -o test_esphome_codec_gaps.bin
./test_esphome_codec_gaps.bin
g++ -std=c++17 -Wall -I../esphome/components/hisense_ac -I../esphome/components/w41h1_module \
    test_esphome_map_gaps.cpp ../esphome/components/hisense_ac/hisense_protocol.cpp -o test_esphome_map_gaps.bin
./test_esphome_map_gaps.bin

echo
# Prints its own "Layer 1n" heading.
g++ -std=c++17 -Wall -I../esphome/components/wifi_setup_mode test_esphome_setup_mode.cpp \
    -o test_esphome_setup_mode.bin
./test_esphome_setup_mode.bin

echo
echo "== Layer 1o: fuzz corpus replay, known failures, framing edge cases (ASan + UBSan) =="
./fuzz/run.sh --smoke

echo
echo "== Layer 1p: ESPHome configuration matrix (skipped when esphome is not installed) =="
python3 ./test_esphome_config_matrix.py

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
echo "== Layer 3: AmebaZ2 image signing chain (#75) =="
python3 ./test_image_chain.py

echo
echo "== Layer 4: dev.py (build cache, board files, command line) and matter-convert.py =="
python3 ./test_dev_release.py
python3 ./test_matter_convert.py

echo
echo "== Layer 5: the firmware version, the changelog and the ESPHome pin =="
python3 ../scripts/version-lint.py
python3 ./test_version_lint.py

echo
echo "== Layer 6: tooling edge cases (dev.py, the scripts, the hook, the lint lists, the clip flasher) =="
python3 ./test_dev_edges.py
python3 ./test_scripts_edges.py
python3 ./test_flasher.py
python3 ./test_hil_restore.py

echo
echo "ALL QA LAYERS PASSED"
