"""The A/C for the host end-to-end test: firmware/test/virtual_ac.py on a pseudo-terminal, with the
knobs a test needs to make the unit misbehave.

virtual_ac.py supplies the status and ProductType frames and the capability table. What is added
here, without touching that file:

- a DevType reply in the layout captured from a deployed unit on 2026-10-12 (22 bytes, the device
  type at bytes 16 and 17). virtual_ac.py echoes the 20-byte probe, which has its checksum where
  the device type belongs, so the component never learns a type from it;
- the request bits of the 0x1E reply (the remote's "77" gesture);
- the status bytes virtual_ac.py leaves at zero: coil temperature, voltage, the power proxy, the
  aux heat relay and the four fault bytes;
- the command rules the component's own bus tests use (Wire::apply_command_ in
  test_esphome_bus_stress.cpp): a powered-down unit ignores a mode change, byte 16 = 0x01 in a
  combined frame is fan auto, byte 32 bits 4-5 = 00 is horizontal swing off. virtual_ac.py
  switches the unit on for any mode byte and never returns the fan to auto from a combined frame;
- faults on the wire: silence, a deaf or stubborn unit, damaged replies.

Nothing here is timed. The unit answers as soon as a request is complete, as fast as a real one
looks from 9600 baud.
"""

from __future__ import annotations

import os
import pathlib
import sys
import time
import tty

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from virtual_ac import CAPS, CMD_FAN, CMD_MODE, VirtualAC, stuff  # noqa: E402

# virtual_ac.py put reverse-engineering/tools on the path for its own import of this.
from decode_ac_frames import checksum  # noqa: E402

__all__ = ["CAPS", "Bus", "Rx", "Unit", "seal", "unstuff"]

CLASS_DEVTYPE = 0x0A
CLASS_VERSION = 0x07
CLASS_LINK = 0x1E
CLASS_COMMAND = 0x65
CLASS_STATUS = 0x66
SUB_PRODUCT = 0x40


def unstuff(wire: bytes) -> bytes:
    """Undo the doubling of 0xF4 between the start and end tags of one frame."""
    body = wire[2:-2]
    out = bytearray()
    i = 0
    while i < len(body):
        out.append(body[i])
        if body[i] == 0xF4 and i + 1 < len(body) and body[i + 1] == 0xF4:
            i += 1
        i += 1
    return bytes(wire[:2]) + bytes(out) + bytes(wire[-2:])


def seal(frame: bytearray | bytes) -> bytes:
    """Write the checksum of an un-stuffed frame (16-bit sum of bytes 2 to n-5, big-endian, at
    n-4) and return the frame as it goes on the wire."""
    f = bytearray(frame)
    ck = checksum(f, len(f) - 4)
    f[-4], f[-3] = ck >> 8, ck & 0xFF
    return bytes(f[:2]) + stuff(bytes(f[2:-2])) + bytes(f[-2:])


class Deframer:
    """Module to A/C byte stream to un-stuffed frames, sized by the LEN byte as the unit does."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data: bytes) -> list[bytes]:
        self.buf += data
        frames = []
        while True:
            start = self.buf.find(b"\xf4\xf5")
            if start < 0:
                del self.buf[: max(0, len(self.buf) - 1)]
                return frames
            del self.buf[:start]
            out = bytearray(self.buf[:2])
            i = 2
            done = False
            while i < len(self.buf):
                b = self.buf[i]
                if b == 0xF4:
                    if i + 1 >= len(self.buf):
                        break  # the next byte decides: a doubled F4 or the end tag
                    if self.buf[i + 1] == 0xF4:
                        i += 1
                out.append(b)
                i += 1
                if len(out) >= 5 and len(out) == out[4] + 9:
                    done = True
                    break
            if not done:
                return frames
            frames.append(bytes(out))
            del self.buf[:i]


class Rx:
    """One frame the unit received."""

    def __init__(self, at: float, frame: bytes):
        self.at = at
        self.frame = frame
        self.cls = frame[13] if len(frame) > 13 else 0
        self.sub = frame[14] if len(frame) > 14 else 0

    def __repr__(self):
        return f"<{self.at:8.3f} {self.frame.hex(' ')}>"


class Unit(VirtualAC):
    def __init__(self):
        super().__init__()
        # A unit cooling a warm room, with every reading different from every other so a sensor
        # wired to the wrong byte shows.
        self.setpoint = 24
        self.indoor = 27
        self.outdoor = 33
        self.coil = 41
        self.comp = 45
        self.voltage = 228  # byte 50, whole volts
        self.current = 20  # byte 55, the power proxy: 4.15 * 20^2 = 1660 W
        self.aux_heat = False  # byte 35 bit 4
        self.faults = {}  # status byte offset (39, 40, 64, 66) -> value
        self.devtype = (0x01, 0x02)
        self.display = None  # byte 36 of the last combined frame
        self.status_patch = {}  # status byte offset -> value, applied last

        # ---- what a test turns ----
        self.silent = False  # answers nothing at all
        self.devtype_silent = False  # answers everything but the DevType probe
        self.deaf_commands = 0  # this many command frames get no reply and do nothing (-1: all)
        self.ignore_commands = 0  # this many are answered and not applied (-1: all)
        self.command_reply = "status"  # or "echo": the 20-byte echo the stock module expects
        self.link_request = 0  # request bits (byte 17) of the next 0x1E replies
        self.link_request_replies = 0  # how many replies carry them (-1: until cleared)
        self.damage = {}  # (cls, sub) -> list of damage kinds, one per reply, see Bus._damage
        self.on_command = None  # called with each command frame heard; True = answer it, do not apply it

    def _sim_physics(self, now):
        """No drift: a test sets every reading it asserts."""

    def status_frame(self):
        f = bytearray(unstuff(super().status_frame()))
        # Quiet shows as fan step 0x02 and turbo as high (hisense_map.h, forced_fan_index).
        if self.mute:
            f[16] = 0x02
        elif self.turbo:
            f[16] = 0x12
        if self.aux_heat:
            f[35] |= 0x10
        f[45] = self.coil & 0xFF
        f[50] = self.voltage & 0xFF
        f[55] = self.current & 0xFF
        for offset, value in {**self.faults, **self.status_patch}.items():
            f[offset] = value & 0xFF
        return seal(f)

    def devtype_frame(self):
        # F4 F5 01 40 0D 01 00 FE 01 00 00 01 00 0A 04 01 <type> <sub> <sum> F4 FB, as captured.
        f = bytearray.fromhex("f4f501400d0100fe01000001000a04010000 0000 f4fb")
        f[16], f[17] = self.devtype
        return seal(f)

    def link_frame(self, request: bytes):
        f = bytearray(request)
        f[2] = 0x01
        f[16:24] = bytes(8)
        bits = 0
        if self.link_request_replies != 0:
            bits = self.link_request
            if self.link_request_replies > 0:
                self.link_request_replies -= 1
        f[17] = bits
        return seal(f)

    def echo_frame(self, request: bytes, length: int = 20):
        f = bytearray(length)
        f[:16] = request[:16]
        f[2] = 0x01
        f[4] = length - 9
        f[-2:] = b"\xf4\xfb"
        return seal(f)

    def apply_command(self, f):
        b18 = f[18]
        if b18 & 0xF0 == 0:
            if b18 & 0x0C == 0x0C:
                self.power = True  # the literal power-on frame
            elif b18 & 0x0C == 0x04:
                self.power = False  # the literal power-off frame
            else:  # a single-field frame: sleep at 17, mute at 35
                if f[17] & 1:
                    self.sleep = (f[17] - 1) // 2
                if f[35] in (0x30, 0x10):
                    self.mute = f[35] == 0x30
            return
        if b18 & 0x0C == 0x0C:
            self.power = True
        if not self.power:
            return
        self.mode = CMD_MODE.get(b18 >> 4, self.mode)
        if f[19] & 1 and self.mode in ("cool", "heat"):
            self.setpoint = (f[19] - 1) // 2
        if self.mode != "dry" and f[16] in CMD_FAN:
            self.fan = CMD_FAN[f[16]]
        self.vswing = f[32] & 0xC0 == 0xC0
        self.hswing = f[32] & 0x30 == 0x30
        if f[33] == 0x30:
            self.eco, self.turbo = True, False
        elif f[33] == 0x0C:
            self.eco, self.turbo = False, True
        elif f[33] == 0x10:
            self.eco = False
        elif f[33] == 0x04:
            self.turbo = False
        if f[36] in (0xC0, 0x40):
            self.display = f[36] == 0xC0


class Bus:
    """The unit's end of the wire. `path` is what the host binary opens as its UART."""

    def __init__(self, unit: Unit):
        self.unit = unit
        self.master, self.slave = os.openpty()
        tty.setraw(self.master)
        tty.setraw(self.slave)
        os.set_blocking(self.master, False)
        self.path = os.ttyname(self.slave)
        self.rx: list[Rx] = []
        self.deframer = Deframer()
        self.t0 = time.monotonic()
        self.replies = 0

    def close(self):
        for fd in (self.master, self.slave):
            try:
                os.close(fd)
            except OSError:
                pass

    def now(self) -> float:
        return time.monotonic() - self.t0

    def on_readable(self):
        try:
            data = os.read(self.master, 4096)
        except (BlockingIOError, OSError):
            return
        for frame in self.deframer.feed(data):
            self.rx.append(Rx(self.now(), frame))
            self._answer(frame)

    def send_raw(self, data: bytes):
        try:
            os.write(self.master, data)
        except (BlockingIOError, OSError):
            pass  # nobody is reading: the binary is gone, and the test will say so

    def commands(self, since: int = 0) -> list[Rx]:
        return [r for r in self.rx[since:] if r.cls == CLASS_COMMAND]

    def _answer(self, f: bytes):
        u = self.unit
        if u.silent or len(f) < 16:
            return
        cls, sub = f[13], f[14]
        if cls == CLASS_DEVTYPE:
            if u.devtype_silent:
                return
            reply = u.devtype_frame()
        elif cls == CLASS_VERSION:
            reply = u.echo_frame(f)
        elif cls == CLASS_LINK:
            reply = u.link_frame(f)
        elif cls == CLASS_STATUS and sub == SUB_PRODUCT:
            reply = u.producttype_frame()
        elif cls == CLASS_STATUS:
            reply = u.status_frame()
        elif cls == CLASS_COMMAND:
            if u.deaf_commands != 0:
                if u.deaf_commands > 0:
                    u.deaf_commands -= 1
                return
            swallowed = u.on_command is not None and u.on_command(f)
            if u.silent:
                return  # the hook switched the unit off under this very frame
            if u.ignore_commands != 0:
                if u.ignore_commands > 0:
                    u.ignore_commands -= 1
            elif not swallowed:
                u.apply_command(f)
            reply = u.status_frame() if u.command_reply == "status" else u.echo_frame(f)
        else:
            return
        queue = u.damage.get((cls, sub))
        kind = queue.pop(0) if queue else None
        reply = self._damage(reply, kind)
        if reply:
            self.replies += 1
            self.send_raw(reply)

    @staticmethod
    def _damage(reply: bytes, kind: str | None) -> bytes:
        if kind is None:
            return reply
        if kind == "drop":
            return b""
        if kind == "bad_checksum":
            # A different setpoint under the old checksum: a hub that skipped the check would show it.
            f = bytearray(reply)
            f[19] ^= 0x04
            return bytes(f)
        if kind == "truncate":
            return reply[: len(reply) // 2]
        if kind == "garbage_before":
            return bytes.fromhex("00 ff 13 37 f4 a5 5a 65 66") + reply
        if kind == "false_start":
            return bytes.fromhex("f4 f5 01") + reply
        if kind == "garbage_only":
            return bytes.fromhex("de ad be ef 00 ff f5 f4 12 34") * 4
        raise ValueError(kind)
