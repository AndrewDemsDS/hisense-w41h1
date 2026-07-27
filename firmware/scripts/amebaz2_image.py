#!/usr/bin/env python3
"""AmebaZ2 firmware image format: sub-image chain walk, verify, re-sign (issue #75).

Single definition of the signing recipe, shared by firmware/scripts/ota-release.sh
(revert --backup / --repackage) and firmware/test/test_image_chain.py so the two cannot
drift.

An image is a 0xE0-byte manifest followed by a CHAIN of sub-images. boot_load checks
three independent things and HANGS on any failure, with no fall-back to the other slot
(it routes through the shared failure sink -> hal_flash_return_spi, which leaves the
flash QE bit CLEARED -- the only reliable "did the bootloader reject this?" signal):

  1. manifest signature  HMAC-SHA256(KEY, img[0xE0:0x140]) == img[0:0x20]
  2. one trailer PER SUB-IMAGE i at header offset H, with S = u32le(img[H]):
        END   = H + 0x60 + S
        START = 0 if i == 0 else H
        HMAC-SHA256(KEY, img[START:END]) == img[END:END+0x20]
     Sub-image 0 starts at 0, so its trailer covers the manifest signature AND the FWHS
     serial at 0xF4: patching the serial invalidates it. Recomputing only that one is
     what bricked the office unit on 2026-07-21 (#75).
  3. a 4-byte little-endian sum of every payload byte, appended after the payload.

The chain is linked by u32le(img[H+4]): the next header sits at H + that value, and
0xFFFFFFFF terminates it. Every image seen so far (custom and stock) carries three
sub-images. KEY is the partition-table entry's hash_key (+0x20), identical for FW1/FW2.
"""

import hashlib
import hmac
import struct

KEY = bytes.fromhex('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e5f')
HDR0 = 0xE0            # first sub-image header, immediately after the manifest
HDR_LEN = 0x60         # sub-image header length
MAC_LEN = 0x20         # HMAC-SHA256 trailer length
SERIAL_OFF = 0xF4      # FWHS serial: what the bootloader A/B-selects on (docs/10 section 7)
LINK_END = 0xFFFFFFFF  # chain terminator in the u32 at H+4
MAX_SUB_IMAGES = 8     # real images carry 3; cap so a corrupt chain cannot spin
ERASED_RUN = b'\xff' * 4096   # end-of-image marker inside a raw slot / flash dump


class ImageFormatError(ValueError):
    """The sub-image chain is malformed (bad offset, size or link)."""


def sub_images(payload):
    """Walk the sub-image chain. Returns [(index, header_off, start, end), ...].

    Every offset is bounds-checked against len(payload) BEFORE it is used, so a corrupt
    or hostile header raises ImageFormatError instead of reading past the buffer, and the
    iteration count is capped so a cyclic chain cannot spin.
    """
    n = len(payload)
    out = []
    off = HDR0
    for i in range(MAX_SUB_IMAGES):
        if off + HDR_LEN > n:
            raise ImageFormatError(f"sub-image {i}: header at {off:#x} outside payload ({n:#x} bytes)")
        size, link = struct.unpack_from('<II', payload, off)
        end = off + HDR_LEN + size
        if end + MAC_LEN > n:
            raise ImageFormatError(f"sub-image {i}: size {size:#x} at header {off:#x} runs past payload ({n:#x} bytes)")
        out.append((i, off, 0 if i == 0 else off, end))
        if link == LINK_END:
            return out
        if link == 0 or off + link <= off:
            raise ImageFormatError(f"sub-image {i}: link {link:#x} at {off + 4:#x} does not advance")
        off += link
    raise ImageFormatError(f"sub-image chain longer than {MAX_SUB_IMAGES} entries -- malformed")


def manifest_ok(blob):
    """True when blob starts with a signature-valid manifest (cheap image detector)."""
    return (len(blob) >= HDR0 + HDR_LEN
            and hmac.new(KEY, blob[HDR0:HDR0 + HDR_LEN], hashlib.sha256).digest() == blob[0:MAC_LEN])


def read_serial(payload):
    if len(payload) < SERIAL_OFF + 4:
        raise ImageFormatError(f"payload too short ({len(payload):#x}) to hold a serial")
    return struct.unpack_from('<I', payload, SERIAL_OFF)[0]


def bytesum(payload):
    """The 4-byte transport trailer, a plain sum of every payload byte."""
    return struct.pack('<I', sum(payload) & 0xFFFFFFFF)


def verify(payload, trailer=None):
    """Check the manifest signature + EVERY sub-image trailer + (if given) the byte-sum.

    Returns (ok, lines) with one human-readable line per check, so a caller can print
    exactly WHICH trailer went stale instead of a bare pass/fail.
    """
    if len(payload) < HDR0 + HDR_LEN:
        return False, [f"payload too short ({len(payload):#x} bytes) to hold a manifest"]
    sig_ok = manifest_ok(payload)
    ok = sig_ok
    lines = [f"manifest sig = {'OK' if sig_ok else 'MISMATCH'}"]
    try:
        subs = sub_images(payload)
    except ImageFormatError as exc:
        return False, lines + [f"sub-image chain = MALFORMED ({exc})"]
    for i, hdr, start, end in subs:
        good = hmac.new(KEY, payload[start:end], hashlib.sha256).digest() == payload[end:end + MAC_LEN]
        ok = ok and good
        lines.append(f"sub{i} hdr@{hdr:#x} covers [{start:#x},{end:#x}) trailer@{end:#x} = "
                     f"{'OK' if good else 'MISMATCH'}")
    if trailer is not None:
        sum_ok = bytesum(payload) == trailer
        ok = ok and sum_ok
        lines.append(f"byte-sum = {'OK' if sum_ok else 'MISMATCH'}")
    return ok, lines


def failures(lines):
    """The subset of verify() lines that report a problem (for a one-line failure summary)."""
    return [ln for ln in lines if 'MISMATCH' in ln or 'MALFORMED' in ln or 'too short' in ln]


def resign(payload, serial=None):
    """Re-sign a payload the way the bootloader checks it. Returns new bytes.

    Order matters: sub-image 0 covers [0, END0), i.e. the serial at 0xF4 AND the manifest
    signature at [0:0x20], so both must be final before its trailer is written. Later
    sub-images start at their own header; recomputing ascending keeps any future overlap
    correct too. The caller appends bytesum(result).
    """
    buf = bytearray(payload)
    if len(buf) < HDR0 + HDR_LEN:
        raise ImageFormatError(f"payload too short ({len(buf):#x} bytes) to sign")
    subs = sub_images(buf)                       # validate the chain BEFORE mutating anything
    if serial is not None:
        struct.pack_into('<I', buf, SERIAL_OFF, serial)
    buf[0:MAC_LEN] = hmac.new(KEY, bytes(buf[HDR0:HDR0 + HDR_LEN]), hashlib.sha256).digest()
    for _i, _hdr, start, end in subs:
        buf[end:end + MAC_LEN] = hmac.new(KEY, bytes(buf[start:end]), hashlib.sha256).digest()
    return bytes(buf)


def split_payload(blob, min_len=0x100000, max_len=0x180000):
    """Carve (payload, trailer) out of a raw slot image or flash-dump region.

    The image ends at the first 4 KB run of erased flash; the 4 bytes before it are the
    byte-sum trailer. Returns None when there is no plausibly-sized image here.
    """
    i = blob.find(ERASED_RUN)
    if i < 4:
        return None
    n = i - 4
    if not min_len <= n <= max_len:
        return None
    return blob[:n], blob[n:n + 4]
