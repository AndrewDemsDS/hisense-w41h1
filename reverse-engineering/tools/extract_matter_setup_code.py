#!/usr/bin/env python3
"""Extract Matter commissioning info from an AmebaZ2 (RTL8710C) flash dump.

Looks for the connectedhomeip factory/KV data and the compiled-in default
(test) commissioning values, and reports the setup discriminator / passcode.

    ./extract_matter_setup_code.py w41h1_dump1.bin

Note: if the device was provisioned with a per-device spake2p *verifier* (not a
raw pin-code), the passcode is not recoverable from the dump by design. Devices
built from the example app use the well-known test values instead.
"""
import struct
import sys

data = open(sys.argv[1], "rb").read()

DEFAULT_PASSCODE = 20202021
DEFAULT_DISCRIMINATOR = 3840  # 0x0F00
KNOWN_TEST_MANUAL = b"34970112332"
KNOWN_TEST_QR = "MT:Y.K9042C00KA0648G00"

print(f"dump size: {len(data)} bytes")

# 1) compiled-in test defaults present?
has_pc = struct.pack("<I", DEFAULT_PASSCODE) in data
has_disc = struct.pack("<I", DEFAULT_DISCRIMINATOR) in data
print(f"default test passcode {DEFAULT_PASSCODE} present: {has_pc}")
print(f"default test discriminator {DEFAULT_DISCRIMINATOR} present: {has_disc}")

# 2) the generated manual pairing code string stored verbatim?
i = data.find(KNOWN_TEST_MANUAL)
if i >= 0:
    print(f"manual pairing code {KNOWN_TEST_MANUAL.decode()} stored at 0x{i:x}")

# 3) any custom commissioning values in the KV/factory store?
custom = [k for k in (b"verifier", b"salt", b"iteration-count", b"pin-code")
          if data.count(k) > 1]  # >1 => beyond the code's key-name table
print("custom factory commissioning KV entries:", custom or "none (using test defaults)")

if has_pc and has_disc and not custom:
    print("\n=> Device uses the STANDARD MATTER TEST CREDENTIALS:")
    print("   Manual pairing code: 34970112332  (3497-011-2332)")
    print(f"   QR payload:          {KNOWN_TEST_QR}")
    print(f"   Discriminator/Passcode: {DEFAULT_DISCRIMINATOR} / {DEFAULT_PASSCODE}")
else:
    print("\n=> Custom/per-device factory data detected; inspect the KV/factory "
          "partition. If only a spake2p verifier is stored, the passcode is not "
          "recoverable from the dump.")
