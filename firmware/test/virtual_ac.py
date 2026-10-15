#!/usr/bin/env python3
"""virtual_ac.py -- a software model of the Hisense W41H1 A/C indoor unit.

Layer-2 QA: emulates the *device the driver talks to* so the RS-485 command/
status loop can be exercised without a real A/C. It speaks the validated bus
protocol -- responds to status-request polls with a 160-byte status frame,
applies command frames to its internal state, and prints every state change.

Two ways to wire it up:
  --pty              create a PTY pair; prints the slave path for the peer
                     (a host-compiled driver, or `decode_ac_frames.py --port`).
  --port /dev/ttyX   attach to a real serial port (e.g. a USB-TTL loopback, or
                     the module's DI/RO tap for on-hardware cross-checking).

This is the executable spec of the status encoding: field offsets and value
maps match firmware/test/reference-driver and its README.md (all hardware-confirmed).
"""

import argparse, os, sys, time

# Reuse the driver-mirroring frame splitter + checksum from the RE decoder
# (same approach as reverse-engineering/tools/map_diff.py) rather than a 2nd copy.
sys.path.insert(
    0,
    os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..",
        "..",
        "reverse-engineering",
        "tools",
    ),
)
from decode_ac_frames import checksum, split_frames

STX1, STX2, ETX1, ETX2 = 0xF4, 0xF5, 0xF4, 0xFB
STATUS_HDR = bytes(
    [
        0xF4,
        0xF5,
        0x01,
        0x40,
        0x97,
        0x01,
        0x00,
        0xFE,
        0x01,
        0x01,
        0x01,
        0x01,
        0x00,
        0x66,
        0x00,
        0x01,
    ]
)

# ---- ProductType (`66 40`) capability flags -------------------------------
# Source: reverse-engineering/docs/10-stock-fw-init-and-comms.md section 5a,
# handle_producttype_cmd_result @ 0x9b6f0c4c [PROVEN]. Guard there is
# payload[0]==0x66 && payload[1]==0x40; frame_byte = payload_off + 13.
# Advertising a flag here makes the MODULE believe the A/C supports it, which is
# how we exercise features no A/C we own reports (issue #15: ai, swing_direction_8).
# Nothing is inferred: every (byte, mask) below is copied from that table.
CAPS = {
    "cool_heat": (18, 0x80),
    "purify": (23, 0x08),
    "power_save": (23, 0x40),
    "fan_mute": (24, 0x40),
    "infinite_fan_speed": (25, 0x08),
    "swing_follow": (26, 0x02),
    "8heat": (26, 0x80),
    "power_display": (27, 0xC0),  # bits6-7, read as (b >> 6)
    "swing_direction_8": (28, 0x10),
    "ai": (28, 0x40),
    "humidity": (32, 0x01),
    "dr": (35, 0x03),  # bits0-1, read as (b & 3)
    "trans_102_64": (38, 0x08),
    "enable_8heat": (39, 0x04),
    "q_display": (39, 0x40),
}

# ---- Device ID (`67 00 00`) ------------------------------------------------
# Source: dumps/issue-110-class-0x67-analysis.md (stock dump of 2026-08-19, rizin).
# The stock module sends `67 00 00` once after every ProductType exchange and
# parses the reply in handle_deviceID_cmd_result: inner payload (frame byte 13
# onwards) of exactly 43 bytes, inner[3]=0x10, inner[4]=0x24, a 23-byte ASCII id
# at inner[0x12..0x28] and the big-endian 16-bit sum of inner[3..0x28] at
# inner[0x29..0x2a]. A missing reply is harmless: the module logs and carries on.
DEVICE_ID_LEN = 23
DEVICE_ID_FRAME_LEN = 13 + 43 + 4  # envelope + inner payload + checksum + end tag

# command index (2n+1 on the wire) <-> names
CMD_MODE = {1: "fan", 3: "heat", 5: "cool", 7: "dry", 9: "auto"}  # byte18 nibble
CMD_FAN = {
    1: "auto",
    3: "quiet",
    0x0B: "low",
    0x0D: "med-low",
    0x0F: "mid",
    0x11: "med-high",
    0x13: "high",
}  # byte16
# status encodings
STAT_MODE = {
    "fan": 0,
    "heat": 1,
    "cool": 2,
    "dry": 3,
    "auto": 5,
}  # byte18 nibble (auto=5!)
STAT_FAN = {
    "auto": 0x01,
    "quiet": 0x02,
    "low": 0x0A,
    "med-low": 0x0C,
    "mid": 0x0E,
    "med-high": 0x10,
    "high": 0x12,
}


def stuff(payload):
    """double any 0xF4 inside the frame body/checksum (not the F4 F5 / F4 FB markers)."""
    out = bytearray()
    for b in payload:
        out.append(b)
        if b == 0xF4:
            out.append(0xF4)
    return bytes(out)


class VirtualAC:
    def __init__(self):
        self.power = True
        self.mode = "cool"
        self.setpoint = 24  # C
        self.indoor = 25  # C
        self.outdoor = 32  # C
        self.fan = "auto"
        self.vswing = False
        self.hswing = False
        self.eco = False
        self.turbo = False
        self.mute = False
        self.sleep = 0  # 0 off, 1..4 profile
        self.comp = 0  # compressor Hz
        self.caps = set()  # advertised in the `66 40` ProductType reply
        self.device_id = "virtual-ac-00000000000"  # 23 chars, `67` reply
        self._t0 = None

    def _sim_physics(self, now):
        # trivially drift indoor toward setpoint when running; ramp compressor.
        if self._t0 is None:
            self._t0 = now
        if self.power and self.mode in ("cool", "heat", "auto"):
            self.comp = min(60, self.comp + 6)
            if self.indoor > self.setpoint:
                self.indoor -= 1
            elif self.indoor < self.setpoint:
                self.indoor += 1
        else:
            self.comp = max(0, self.comp - 10)

    def status_frame(self):
        s = bytearray(160)
        s[0:16] = STATUS_HDR
        s[16] = STAT_FAN.get(self.fan, 0x01)
        s[17] = self.sleep * 2  # status sleep = profile*2
        run = 2 if self.power else 0
        s[18] = (STAT_MODE.get(self.mode, 2) << 4) | (run << 2)
        s[19] = self.setpoint & 0xFF
        s[20] = self.indoor & 0xFF
        f1 = 0
        if self.vswing:
            f1 |= 0x80
        if self.hswing:
            f1 |= 0x40
        if self.eco:
            f1 |= 0x04
        if self.turbo:
            f1 |= 0x02
        s[35] = f1
        s[36] = 0x04 if self.mute else 0x00
        s[42] = self.comp & 0xFF
        s[44] = self.outdoor & 0xFF
        ck = checksum(s, 156)
        s[156], s[157] = ck >> 8, ck & 0xFF
        s[158], s[159] = ETX1, ETX2
        # Stuff the BODY + checksum only. The F4 F5 / F4 FB markers must not be
        # escaped (stuff() says so); stuffing from index 0 doubled the leading STX,
        # emitting "f4 f4 f5 ..." which a real module rejects. Host tests missed it
        # because both ends share the tolerant splitter; the bench module did not.
        return bytes([STX1, STX2]) + stuff(bytes(s[2:158])) + bytes([ETX1, ETX2])

    def producttype_frame(self):
        """Reply to the module's `66 40` ProductType query, advertising self.caps.

        Same envelope as status_frame() (which the module demonstrably accepts):
        only the subtype byte and the flag payload differ. Length gates in the
        stock parser are len-2 > 0x14 / 0x17 / 0x18, all satisfied here.
        """
        s = bytearray(160)
        s[0:16] = STATUS_HDR
        s[14] = 0x40  # subtype: ProductType, not the `66 00` status poll
        for name in self.caps:
            byte, mask = CAPS[name]
            s[byte] |= mask
        ck = checksum(s, 156)
        s[156], s[157] = ck >> 8, ck & 0xFF
        s[158], s[159] = ETX1, ETX2
        # Stuff the BODY + checksum only. The F4 F5 / F4 FB markers must not be
        # escaped (stuff() says so); stuffing from index 0 doubled the leading STX,
        # emitting "f4 f4 f5 ..." which a real module rejects. Host tests missed it
        # because both ends share the tolerant splitter; the bench module did not.
        return bytes([STX1, STX2]) + stuff(bytes(s[2:158])) + bytes([ETX1, ETX2])

    def deviceid_frame(self):
        """Reply to the module's `67 00 00` device-ID query.

        Layout from the stock parser (see the DEVICE_ID_* note above). Frame bytes:
        16/17 = 10 24, 31..53 = the id, 54/55 = big-endian sum of bytes 16..53,
        then the usual frame checksum and end tag. 60 bytes in all, LEN 0x33.
        No real unit's reply has been captured yet; the envelope bytes 5..12 copy
        the status reply, which the module demonstrably accepts.
        """
        n = DEVICE_ID_FRAME_LEN
        s = bytearray(n)
        s[0:16] = STATUS_HDR
        s[4] = n - 9  # LEN: the whole frame is LEN + 9 bytes long (doc 03)
        s[13] = 0x67
        s[16], s[17] = 0x10, 0x24
        did = self.device_id.encode("ascii", "replace")[:DEVICE_ID_LEN]
        did = did.ljust(DEVICE_ID_LEN, b"0")
        s[31 : 31 + DEVICE_ID_LEN] = did
        inner = sum(s[16:54]) & 0xFFFF
        s[54], s[55] = inner >> 8, inner & 0xFF
        ck = checksum(s, n - 4)
        s[n - 4], s[n - 3] = ck >> 8, ck & 0xFF
        s[n - 2], s[n - 1] = ETX1, ETX2
        return bytes([STX1, STX2]) + stuff(bytes(s[2 : n - 2])) + bytes([ETX1, ETX2])

    def apply_command(self, f):
        """f is an un-stuffed command frame (0x65). Update state from the fields set."""
        before = self._snapshot()
        # power on/off literals: on[] has byte18=0x0C, off[] has byte17=0x01 & byte33=0x55
        if len(f) >= 34 and f[18] == 0x0C and f[13] == 0x65:
            self.power = True
        elif len(f) >= 34 and f[33] == 0x55 and f[13] == 0x65:
            self.power = False
        else:
            if f[16] in CMD_FAN and f[16] != 0x01:
                self.fan = CMD_FAN[f[16]]
            elif f[16] == 0x01 and f[18] == 0 and f[19] == 0 and f[33] in (0, 4):
                self.fan = "auto"
            n = f[18] >> 4
            if n in CMD_MODE:
                self.mode = CMD_MODE[n]
                self.power = True
            if f[19] & 1:
                self.setpoint = (f[19] - 1) // 2
            if f[17] & 1:
                self.sleep = (f[17] - 1) // 2
            if f[33] == 0x30:
                self.eco = True
            elif f[33] == 0x10:
                self.eco = False
            elif f[33] == 0x0C:
                self.turbo = True
            elif f[33] == 0x04 and before.get("turbo"):
                self.turbo = False
            if f[32] & 0xC0:
                self.vswing = (f[32] & 0xC0) == 0xC0
            if f[32] & 0x30:
                self.hswing = (f[32] & 0x30) == 0x30
            if len(f) > 35 and f[35] in (0x30, 0x10):
                self.mute = f[35] == 0x30
        after = self._snapshot()
        if after != before:
            diff = {k: after[k] for k in after if after[k] != before[k]}
            print(f"  [state] {diff}")

    def _snapshot(self):
        return dict(
            power=self.power,
            mode=self.mode,
            setpoint=self.setpoint,
            fan=self.fan,
            vswing=self.vswing,
            hswing=self.hswing,
            eco=self.eco,
            turbo=self.turbo,
            mute=self.mute,
            sleep=self.sleep,
        )


def run(read_fn, write_fn, caps=frozenset(), device_id=None):
    ac = VirtualAC()
    ac.caps = set(caps)
    if device_id is not None:
        ac.device_id = device_id
    if ac.caps:
        print(f"# advertising ProductType capabilities: {sorted(ac.caps)}")
    print(f"# virtual A/C up. initial: {ac._snapshot()}")
    buf = bytearray()
    while True:
        chunk = read_fn()
        if chunk:
            buf += chunk
            frames = list(split_frames(bytes(buf)))
            if frames:
                # drop everything up to and including the last complete frame's
                # F4 FB end tag (same trim the decoder's own live loop uses).
                cut = bytes(buf).rfind(bytes([ETX1, ETX2]))
                if cut >= 0:
                    del buf[: cut + 2]
            for f in frames:
                if len(f) < 14:
                    continue
                cls = f[13]
                if cls == 0x66 and len(f) > 14 and f[14] == 0x40:
                    # ProductType query -- advertise the capability flags
                    write_fn(ac.producttype_frame())
                    print(
                        f"  [66 40] ProductType query -> advertised {sorted(ac.caps) or 'none'}"
                    )
                elif cls == 0x67:
                    # device-ID query, sent once after each ProductType exchange
                    write_fn(ac.deviceid_frame())
                    print(f"  [67 00] device-ID query -> replied {ac.device_id!r}")
                elif cls == 0x66 and len(f) < 30:  # status-request poll
                    ac._sim_physics(time.time())
                    write_fn(ac.status_frame())
                elif cls == 0x65:  # command
                    ac.apply_command(f)
                    write_fn(ac.status_frame())  # echo new state
                elif cls in (0x0A, 0x07, 0x1E):
                    # Handshake / keepalive frames (DevType, Version, LINK). The
                    # A/C is a pure slave: the module matches replies to requests
                    # by the transaction primitive, not by class, and RX frames
                    # are hard-gated on byte[2]==0x01 (docs/10 section: "no
                    # class-demux on RX"). So a faithful slave reply is the request
                    # echoed back with byte[2] flipped to the RX direction. Without
                    # this the module's `heard_ac` never latches and it never issues
                    # the `66 40` ProductType query. (Found running against the real
                    # module: the status/command replies alone are not enough.)
                    reply = bytearray(f)
                    reply[2] = 0x01
                    ck = checksum(reply, len(reply) - 4)
                    reply[-4], reply[-3] = ck >> 8, ck & 0xFF
                    write_fn(
                        bytes(reply[:2]) + stuff(bytes(reply[2:-2])) + bytes(reply[-2:])
                    )
                    print(f"  [0x{cls:02X}] handshake poll -> echoed slave reply")
        else:
            time.sleep(0.02)


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument(
        "--pty", action="store_true", help="create a PTY pair for a peer to attach"
    )
    g.add_argument("--port", help="attach to an existing serial port")
    g.add_argument(
        "--connect",
        help="attach to a TCP socket HOST:PORT (e.g. Renode UART: 127.0.0.1:3456)",
    )
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument(
        "--capabilities",
        default="",
        help="comma-separated flags to advertise in the `66 40` ProductType reply "
        "(e.g. ai,swing_direction_8). Known: " + ",".join(sorted(CAPS)),
    )
    ap.add_argument(
        "--device-id",
        default=None,
        help="23-character id returned to the module's `67 00 00` device-ID query "
        "(shorter values are padded with 0, longer ones cut)",
    )
    a = ap.parse_args()
    caps = {c.strip() for c in a.capabilities.split(",") if c.strip()}
    unknown = caps - set(CAPS)
    if unknown:
        sys.exit(
            f"unknown capability flag(s): {sorted(unknown)}; known: {sorted(CAPS)}"
        )

    if a.connect:
        import socket

        host, port = a.connect.rsplit(":", 1)
        sk = socket.create_connection((host, int(port)))
        sk.setblocking(False)
        print(f"# connected to {a.connect} (Renode UART bridge)")

        def rd():
            try:
                return sk.recv(512)
            except BlockingIOError:
                return b""

        run(rd, lambda d: sk.sendall(d), caps, a.device_id)
        return

    if a.pty:
        master, slave = os.openpty()
        import tty

        tty.setraw(master)
        print(f"# PTY ready -- point the driver/decoder at: {os.ttyname(slave)}")
        print(f"#   e.g. ./decode_ac_frames.py --port {os.ttyname(slave)}")
        os.set_blocking(master, False)

        def rd():
            try:
                return os.read(master, 512)
            except BlockingIOError:
                return b""

        def wr(d):
            os.write(master, d)

        run(rd, wr, caps, a.device_id)
    else:
        import serial

        s = serial.Serial(a.port, a.baud, timeout=0.02)
        run(lambda: s.read(512), lambda d: s.write(d), caps, a.device_id)


if __name__ == "__main__":
    main()
