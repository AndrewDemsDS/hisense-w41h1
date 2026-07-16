#!/usr/bin/env bash
# gen-creds.sh -- generate a UNIQUE Matter commissioning discriminator + passcode for your
# own build, so every flashed unit isn't the shared test code 34970112332. Still uses the
# CSA *test* attestation certs (VID 0xFFF1) -- this is dev-only, not a certified device.
#
# It prints the values + (if the connectedhomeip env is activated and its tools are built)
# the SPAKE2+ verifier and the QR/manual pairing code. Wire the discriminator + passcode +
# verifier into your build's factory data / CHIPProjectConfig before building.
set -euo pipefail

# --- random, spec-valid discriminator (0..4095) + passcode (1..99999998, no trivial values) ---
INVALID_PIN="00000000 11111111 22222222 33333333 44444444 55555555 66666666 77777777 88888888 99999999 12345678 87654321"
DISCRIMINATOR=$(( (RANDOM<<4 ^ RANDOM) & 0x0FFF ))
while :; do
  PASSCODE=$(( ( (RANDOM<<15 ^ RANDOM<<2 ^ RANDOM) % 99999998 ) + 1 ))
  printf -v P8 '%08d' "$PASSCODE"
  case " $INVALID_PIN " in *" $P8 "*) continue;; esac
  break
done

echo "== your unique Matter commissioning credentials =="
echo "  discriminator : $DISCRIMINATOR   (0x$(printf '%03X' "$DISCRIMINATOR"))"
echo "  passcode      : $PASSCODE"
echo

# --- optional: SPAKE2+ verifier + pairing codes, if the CHIP tools are available ---
SPAKE2P="$(command -v spake2p || true)"
CHIPTOOL="$(command -v chip-tool || true)"
if [ -n "$SPAKE2P" ]; then
  SALT_B64=$(head -c 32 /dev/urandom | base64)
  ITER=10000
  echo "== SPAKE2+ verifier (iterations=$ITER) =="
  "$SPAKE2P" gen-verifier --count 1 --iteration-count "$ITER" --salt "$SALT_B64" --pin-code "$PASSCODE" 2>/dev/null \
    || echo "  (spake2p failed -- generate manually; salt(b64)=$SALT_B64 iter=$ITER)"
  echo "  salt (base64) : $SALT_B64"
  echo "  iterations    : $ITER"
else
  echo "(spake2p not on PATH -- activate the connectedhomeip env to also emit the SPAKE2+ verifier)"
fi
echo
if [ -n "$CHIPTOOL" ]; then
  echo "== pairing codes (VID 0xFFF1 / PID 0x8001, on-network) =="
  "$CHIPTOOL" setup-payload generate -d "$DISCRIMINATOR" -p "$PASSCODE" -v 65521 -i 32769 -c 2 -w 0 2>/dev/null \
    || echo "  (chip-tool payload generate failed)"
else
  echo "(chip-tool not on PATH -- run 'chip-tool setup-payload generate -d $DISCRIMINATOR -p $PASSCODE -v 65521 -i 32769' for the QR/manual code)"
fi
echo
echo "Next: set discriminator=$DISCRIMINATOR, passcode=$PASSCODE (+ the verifier/salt/iter above)"
echo "in your factory data / CHIPProjectConfig, then rebuild. See RELEASE.md and sdk-edits/README.md."
