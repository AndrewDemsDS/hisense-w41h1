"""Synthetic AmebaZ2 images for the host tests: one builder instead of a copy in each test.

The layout (a 0xE0 manifest, then per sub-image a 0x60 header, the body and a 0x20 trailer) is
amebaz2_image.py's own, so a change there moves every test at once. Nothing here is a real
firmware: the bodies are a byte ramp, which also means no image holds a 4 KB run of 0xFF (the
end-of-image marker in a flash slot).
"""

import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
import amebaz2_image as az  # noqa: E402

_RAMP = bytes(range(256))


def step(size):
    """The link value that reaches the header after a sub-image with `size` body bytes."""
    return az.HDR_LEN + size + az.MAC_LEN


def chain(*subs, tail=b"", ramp=False):
    """An UNSIGNED payload from (size, link) pairs. Bodies are zeros, or a byte ramp."""
    out = bytearray(az.HDR0)
    for i, (size, link) in enumerate(subs):
        hdr = bytearray(az.HDR_LEN)
        struct.pack_into("<II", hdr, 0, size, link)
        start = (i * 37) & 0xFF
        body = (_RAMP * (size // 256 + 2))[start:start + size] if ramp else bytes(size)
        out += hdr + body + bytes(az.MAC_LEN)
    return bytes(out) + tail


def signed_payload(sizes, serial=100):
    """A payload of len(sizes) chained sub-images, signed the way the bootloader checks it."""
    subs = [(size, az.LINK_END if i == len(sizes) - 1 else step(size)) for i, size in enumerate(sizes)]
    return az.resign(chain(*subs, ramp=True), serial=serial)


def fake_image(serial, body=0x400):
    """An application image as a build writes it: three sub-images, then the 4-byte byte-sum."""
    signed = signed_payload([body] * 3, serial)
    return signed + az.bytesum(signed)
