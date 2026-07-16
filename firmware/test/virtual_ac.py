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
maps match firmware/src/rs485-driver + INTEGRATION.md (all hardware-confirmed).
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
        return stuff(bytes(s[:158])) + bytes([ETX1, ETX2])

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


def run(read_fn, write_fn):
    ac = VirtualAC()
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
                if cls == 0x66 and len(f) < 30:  # status-request poll
                    ac._sim_physics(time.time())
                    write_fn(ac.status_frame())
                elif cls == 0x65:  # command
                    ac.apply_command(f)
                    write_fn(ac.status_frame())  # echo new state
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
    a = ap.parse_args()

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

        run(rd, lambda d: sk.sendall(d))
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

        run(rd, wr)
    else:
        import serial

        s = serial.Serial(a.port, a.baud, timeout=0.02)
        run(lambda: s.read(512), lambda d: s.write(d))


if __name__ == "__main__":
    main()
