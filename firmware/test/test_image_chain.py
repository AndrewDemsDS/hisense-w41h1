#!/usr/bin/env python3
"""AmebaZ2 image signing chain regression (#75).

The bootloader verifies a manifest signature, one HMAC trailer PER SUB-IMAGE, and a
byte-sum. `revert --repackage` used to recompute only sub-image 0's trailer; a stale
trailer anywhere hangs boot_load with no fall-back slot (the 2026-07-21 office brick).
This locks the recipe down two ways:

  SYNTHETIC (always runs, so CI covers it): build a multi-sub-image payload from scratch,
    re-sign it, then flip one byte inside each sub-image in turn and assert verify() fails
    AND names the right trailer. Also replays the exact #75 bug and two malformed chains.
  ARCHIVE (skips when absent): every archived image in firmware/built-images/ and any raw
    dump in dumps/ must verify end to end, and re-signing one with its serial unchanged
    must reproduce the original bytes.

built-images/ and dumps/ are gitignored, so on CI only the synthetic half runs.

Archive files are graded, not all-or-nothing:
  STRICT  firmware_is-v*.bin (custom build output) and stock-backup-*.bin (read off a
          booted unit) must verify; a failure there is a real regression.
  REPORT  rac-stock-v*-payload.bin, flash_rac-*.bin and dumps/*.bin are printed but do not
          fail the run. Pre-#75 repackages and post-brick dumps legitimately carry stale
          trailers, and spotting them is the point -- deleting stale generated artifacts
          should not block a commit.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "firmware/scripts"))

import amebaz2_image as az  # noqa: E402

BUILT = ROOT / "firmware/built-images"
DUMPS = ROOT / "dumps"
STRICT_GLOBS = ("firmware_is-v*.bin", "stock-backup-*.bin")
REPORT_GLOBS = ("rac-stock-v*-payload.bin", "flash_rac-*.bin")
SCAN_STEP = 0x1000        # flash slots are 4 KB aligned; scan for every image in a dump

fails = []


def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)


def synth_payload(sizes):
    """Build a payload with len(sizes) chained sub-images, correctly signed."""
    parts = []
    for i, size in enumerate(sizes):
        link = az.LINK_END if i == len(sizes) - 1 else az.HDR_LEN + size + az.MAC_LEN
        body = bytes((i * 37 + j) & 0xFF for j in range(size))
        parts.append(size.to_bytes(4, "little") + link.to_bytes(4, "little")
                     + bytes(az.HDR_LEN - 8) + body + bytes(az.MAC_LEN))
    return az.resign(bytes(az.HDR0) + b"".join(parts), serial=100)


def containers(blob):
    """Every image inside a file: a bare payload+trailer, plus each signature-valid
    manifest found at a flash-slot boundary (a full dump holds FW1 and FW2)."""
    out = []
    if len(blob) > 4 and az.bytesum(blob[:-4]) == blob[-4:] and az.manifest_ok(blob):
        out.append(("bare", blob[:-4], blob[-4:]))
    for off in range(0, max(0, len(blob) - (az.HDR0 + az.HDR_LEN)) + 1, SCAN_STEP):
        if not az.manifest_ok(blob[off:off + az.HDR0 + az.HDR_LEN]):
            continue
        got = az.split_payload(blob[off:])
        if got and not any(p == got[0] for _, p, _ in out):
            out.append((f"@{off:#x}", got[0], got[1]))
    return out


def grade(path, strict):
    found = containers(path.read_bytes())
    if not found:
        print(f"  --   {path.name}: holds no AmebaZ2 image, skipped")
        return
    for how, payload, trailer in found:
        # The byte-sum is an OTA TRANSPORT trailer. A transport artifact (bare payload, or a
        # raw slot fetched by revert --backup) carries it; a slot carved out of a multi-slot
        # flash image or dump may not, so there it is reported, not gated.
        gated = strict and how in ("bare", "@0x0")
        ok, lines = az.verify(payload, trailer if gated else None)
        if not gated:
            lines.append("byte-sum (transport only) = "
                         f"{'OK' if az.bytesum(payload) == trailer else 'differs, not gated'}")
        serial = az.read_serial(payload)
        if strict and ok:
            # The signing recipe must be a byte-exact identity when nothing changes, and must
            # round-trip a serial patch: patch to serial+1, patch back, get the original bytes.
            if az.resign(payload) != payload:
                ok = False
                lines.append("re-sign with no change = MISMATCH (recipe does not reproduce the original)")
            elif az.resign(az.resign(payload, serial + 1), serial) != payload:
                ok = False
                lines.append("serial-patch round-trip = MISMATCH (patch to serial+1 and back is lossy)")
        tag = f"{path.name} [{how}] serial={serial}"
        if strict:
            check(ok, f"{tag}: manifest + {len(az.sub_images(payload))} sub-image trailers verify, "
                      f"re-sign and serial-patch round-trip byte-exact")
        else:
            print(("  ok   " if ok else "  warn ") + tag)
        for ln in az.failures(lines):
            print(f"         {ln}")


print("== synthetic sub-image chain ==")
p = synth_payload([0x200, 0x400, 0x100])
subs = az.sub_images(p)
check(len(subs) == 3, f"chain walk finds 3 sub-images: {[(hex(h), hex(s), hex(e)) for _, h, s, e in subs]}")
check(subs[0][2] == 0, "sub0 covers from offset 0 (so it covers the manifest sig + the serial)")
check(all(s == h for _, h, s, _ in subs[1:]), "sub1.. cover from their own header")
ok, lines = az.verify(p, az.bytesum(p))
check(ok, "a freshly signed payload verifies: " + "; ".join(lines))
check(az.resign(p) == p, "re-signing an unchanged payload is a byte-exact no-op")

for i, (_, _hdr, _start, end) in enumerate(subs):
    b = bytearray(p)
    b[end - 1] ^= 0xFF                                   # corrupt inside sub-image i only
    ok, lines = az.verify(bytes(b), az.bytesum(bytes(b)))
    named = [ln for ln in az.failures(lines) if ln.startswith(f"sub{i} ")]
    check(not ok and len(named) == 1, f"a flipped byte in sub{i} is caught and named: {named}")

b = bytearray(p)                                         # the #75 bug, reproduced exactly
b[az.SERIAL_OFF] ^= 0xFF
b[0:az.MAC_LEN] = az.resign(bytes(b))[0:az.MAC_LEN]      # manifest sig refreshed, trailers not
ok, lines = az.verify(bytes(b), az.bytesum(bytes(b)))
check(not ok and any(ln.startswith("sub0 ") for ln in az.failures(lines)),
      "serial patched with only the manifest sig refreshed leaves a stale sub0 trailer (#75)")

b = bytearray(p)                                         # size past the buffer must raise
b[az.HDR0:az.HDR0 + 4] = (0x7FFFFFFF).to_bytes(4, "little")
try:
    az.sub_images(bytes(b)); check(False, "an out-of-range sub-image size raises ImageFormatError")
except az.ImageFormatError:
    check(True, "an out-of-range sub-image size raises ImageFormatError")

b = bytearray(p)                                         # self-referential link must raise
b[az.HDR0 + 4:az.HDR0 + 8] = (0).to_bytes(4, "little")
try:
    az.sub_images(bytes(b)); check(False, "a non-advancing chain link raises ImageFormatError")
except az.ImageFormatError:
    check(True, "a non-advancing chain link raises ImageFormatError")

print()
print("== archived images (built-images/ and dumps/ are gitignored -- skipped when absent) ==")
seen = 0
for glob, strict in [(g, True) for g in STRICT_GLOBS] + [(g, False) for g in REPORT_GLOBS]:
    for f in sorted(BUILT.glob(glob)):
        seen += 1
        grade(f, strict)
for f in sorted(DUMPS.glob("*.bin")) if DUMPS.is_dir() else []:
    seen += 1
    grade(f, strict=False)
if seen == 0:
    print("  --   SKIP: no archived images or dumps on this host")

print()
print("== IMAGE CHAIN OK ==" if not fails else f"== IMAGE CHAIN FAILED ({len(fails)}) ==")
sys.exit(1 if fails else 0)
