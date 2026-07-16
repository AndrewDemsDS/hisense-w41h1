#!/usr/bin/env python3
"""Decode Hisense/AirconIntl A/C-bus frames captured off the UM3352E RO/DI taps.

This is the validation half of the sniffing operation: it takes raw bytes (from
sniff.py's hex log, a raw dump, a pasted hex string, or a live serial port),
reassembles F4 F5 .. F4 FB frames (un-stuffing doubled F4), verifies the 2-byte
big-endian checksum, and decodes every field using the SAME byte offsets as the
firmware driver (firmware/src/rs485-driver/hisense_rs485.{h,cpp}).

Point: if the driver's assumptions (borrowed from the AEH-W4A1 reference) don't
match the real W41H1 wire, this prints the mismatch loudly -- wrong checksum,
unexpected status-frame length, or a status header that differs from the one the
driver's RX task hard-matches. That's the open item from the driver review.

Inputs (pick one):
  --log FILE     parse sniff.py's "(NN) f4 f5 .." hex log (framing ignored;
                 all hex bytes are concatenated and re-split by markers)
  --raw FILE     raw binary byte dump (e.g. from a logic analyzer export)
  --hex "..."    decode a single pasted frame / byte run
  --port DEV     live decode from a serial port (needs pyserial)
                   --baud N   (default 9600)   --secs N (default: run until Ctrl-C)

Direction tag (optional, cosmetic): --dir tx|rx|di|ro  labels the source.
DI (the SoC's UART0 TX) carries 0x65 "set" commands; RO (A/C -> SoC) carries
0x66 status responses.

No dependencies for file/hex modes; pyserial only for --port.
"""

import argparse
import re
import sys
import time

# ---- constants, mirrored from hisense_rs485.h -----------------------------
STX1, STX2 = 0xF4, 0xF5
ETX1, ETX2 = 0xF4, 0xFB
CLASS_OFFSET = 13  # byte[13]: 0x65 = set command, 0x66 = status class
CLASS_SET, CLASS_STATUS = 0x65, 0x66

# The exact 16-byte status header the driver's RX task hard-matches. If a real
# W41H1 status frame differs here, the driver silently drops it -- so we flag it.
STATUS_HEADER = bytes(
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
CMD_FRAME_LEN = 50

MODE_NAMES = {0: "FAN", 1: "HEAT", 2: "COOL", 3: "DRY", 4: "AUTO"}
# command-side fan INDEX (2n+1 on the wire) -> name, W41H1-confirmed six speeds
FAN_NAMES = {
    0: "AUTO",
    1: "QUIET",
    5: "LOW",
    6: "MED_LOW",
    7: "MID",
    8: "MED_HIGH",
    9: "HIGH",
}


# ---- checksum (2-byte big-endian sum over [2, len-4), like the driver) -----
def checksum(frame, end_exclusive):
    return sum(frame[2:end_exclusive]) & 0xFFFF


def decode_2n1(raw):
    """Inverse of the driver's encode(x) = x*2+1. Returns (value, is_flagged)
    where is_flagged means the low bit was set ('write this field')."""
    return (raw - 1) // 2 if raw & 1 else raw // 2, bool(raw & 1)


# ---- frame reassembly: un-stuff doubled F4, split on F4 F5 .. F4 FB ---------
def split_frames(raw):
    """Yield un-stuffed frames (each starts F4 F5, ends F4 FB) from a raw byte
    stream. Mirrors the driver's RX un-stuffing: a doubled F4 collapses to one;
    the end tag is a lone F4 followed by FB."""
    n = len(raw)
    i = 0
    while i < n - 1:
        if raw[i] == STX1 and raw[i + 1] == STX2:
            buf = bytearray([STX1, STX2])
            j = i + 2
            ok = False
            while j < n:
                b = raw[j]
                if b == STX1:
                    if j + 1 < n and raw[j + 1] == ETX2:  # F4 FB = end tag
                        buf += bytes([ETX1, ETX2])
                        j += 2
                        ok = True
                        break
                    elif j + 1 < n and raw[j + 1] == STX1:  # F4 F4 = stuffed
                        buf.append(STX1)
                        j += 2
                    else:  # lone F4 -> desync
                        break
                else:
                    buf.append(b)
                    j += 1
            if ok:
                yield bytes(buf)
                i = j
            else:
                i += 1
        else:
            i += 1


# ---- field decoders --------------------------------------------------------
def decode_command(f):
    out = []
    if len(f) < CMD_FRAME_LEN:
        return f"SHORT command ({len(f)}B, expected {CMD_FRAME_LEN})"
    fan_v, fan_set = decode_2n1(f[16])
    mode_v, mode_set = decode_2n1(f[18] >> 4)
    temp_v, temp_set = decode_2n1(f[19])
    if fan_set:
        out.append(f"fan={FAN_NAMES.get(fan_v, '?%d' % fan_v)}")
    if f[17] & 1:
        out.append(f"sleep={decode_2n1(f[17])[0]}")
    if mode_set:
        out.append(f"mode={MODE_NAMES.get(mode_v, '?%d' % mode_v)}")
    if temp_set:
        out.append(f"temp={temp_v}C")
    # W41H1-confirmed byte33 feature values (differ from the AEH-W4A1 reference)
    feat = {0x0C: "TURBO_ON", 0x04: "none", 0x30: "ECO_ON", 0x10: "ECO_OFF"}.get(f[33])
    if feat and feat != "none":
        out.append(feat)
    if f[31] or (f[32] & 0xF0):
        out.append(f"vswing/hswing byte31=0x{f[31]:02X} byte32=0x{f[32]:02X}")
    disp = {0xC0: "DISPLAY_ON", 0x40: "DISPLAY_OFF"}.get(f[36])
    if disp:
        out.append(disp)
    # power on/off literal frames (matched by their distinctive bytes)
    if f[18] == 0x0C:
        out.append("POWER_ON(literal)")
    if f[33] == 0x55 or (f[24] == 0x01 and f[29] == 0x01):
        out.append("POWER_OFF?(literal)")
    return "  ".join(out) if out else "(no recognized field set)"


def decode_status(f):
    # Field map fully confirmed by W41H1 sniffing (see INTEGRATION.md). Frame is
    # LEN-driven (byte[4]+9 = 160), temps are DIRECT integer C, flags1 bits are
    # vswing7/hswing6/heat4/eco2/turbo1, sleep@17, compressor@42, outdoor@44.
    out = []
    if f[:16] != STATUS_HEADER:
        diff = [i for i in range(min(16, len(f))) if f[i] != STATUS_HEADER[i]]
        out.append(
            f"!! STATUS HEADER MISMATCH at {diff} "
            f"(got {' '.join('%02X' % b for b in f[:16])})"
        )
    expected = f[4] + 9
    if len(f) != expected:
        out.append(f"!! LENGTH {len(f)} != byte[4]+9 ({expected})")
    if len(f) < 45:
        return "  ".join(out) or f"SHORT status ({len(f)}B)"
    packed = f[18]
    run = (packed >> 2) & 0x3
    mode = (packed >> 4) & 0xF
    if (
        mode in (5, 6)
    ):  # status reports AUTO as nibble 5 or 6 (nibble 6 = commanded-auto, tap-confirmed)
        mode = 4
    flags1, flags2 = f[35], f[36]
    sleep = {
        0x00: "off",
        0x02: "General",
        0x04: "Old",
        0x06: "Young",
        0x08: "Kids",
    }.get(f[17], f"0x{f[17]:02X}")
    out.append(
        f"power={'on' if run else 'off'} mode={MODE_NAMES.get(mode, '?%d' % mode)} "
        f"setpoint={_s8(f[19])}C indoor={_s8(f[20])}C outdoor={_s8(f[44])}C "
        f"fan_raw=0x{f[16]:02X} comp={f[42]}Hz "
        f"vswing={bool(flags1 & 0x80)} hswing={bool(flags1 & 0x40)} "
        f"eco={bool(flags1 & 0x04)} turbo={bool(flags1 & 0x02)} heat={bool(flags1 & 0x10)} "
        f"mute={bool(flags2 & 0x04)} sleep={sleep}"
    )
    return "  ".join(out)


def _s8(b):
    return b - 256 if b >= 128 else b


# ---- top-level per-frame print --------------------------------------------
def report(frame, idx, tag):
    n = len(frame)
    ts = time.strftime("%H:%M:%S")
    prefix = f"[{ts}] {tag + ' ' if tag else ''}#{idx:<4} ({n:2d}B) {frame.hex(' ')}"
    if n < 5 or frame[-2:] != bytes([ETX1, ETX2]):
        print(f"{prefix}\n     -> MALFORMED (no F4 FB end tag)")
        return
    cls = frame[CLASS_OFFSET] if n > CLASS_OFFSET else None
    comp = checksum(frame, n - 4)
    rxd = (frame[n - 4] << 8) | frame[n - 3]
    ck = "OK" if comp == rxd else f"BAD(calc=0x{comp:04X} rx=0x{rxd:04X})"
    if cls == CLASS_SET:
        kind, body = "CMD ", decode_command(frame)
    elif cls == CLASS_STATUS:
        kind = "STAT" if n != 21 else "POLL"  # 21B 0x66 = status-request poll
        body = decode_status(frame) if n != 21 else "status-request (poll)"
    else:
        kind, body = f"cls=0x{cls:02X}", "(non 0x65/0x66 -- LINK/NET envelope?)"
    print(f"{prefix}\n     -> {kind} chk={ck}  {body}")


# ---- input adapters --------------------------------------------------------
def bytes_from_log(path):
    """Extract every hex byte pair from a sniff.py-style log, in order."""
    out = bytearray()
    with open(path) as fh:
        for line in fh:
            if line.lstrip().startswith("#"):
                continue
            # strip a leading "[t] (NN)" prefix if present, then grab hex pairs
            for tok in re.findall(r"\b[0-9a-fA-F]{2}\b", line):
                out.append(int(tok, 16))
    return bytes(out)


def bytes_from_hex(s):
    toks = re.findall(r"[0-9a-fA-F]{2}", s.replace("0x", " "))
    return bytes(int(t, 16) for t in toks)


def run_static(raw, tag):
    idx = 0
    for fr in split_frames(raw):
        report(fr, idx, tag)
        idx += 1
    if idx == 0:
        print("# no F4 F5 .. F4 FB frames found in input", file=sys.stderr)
    else:
        print(f"# {idx} frames decoded", file=sys.stderr)


def run_live(port, baud, secs, tag):
    import serial

    s = serial.Serial(port, baud, timeout=0.02)
    s.reset_input_buffer()
    print(f"# live decode {port} @ {baud} 8N1 (Ctrl-C to stop)", file=sys.stderr)
    buf = bytearray()
    idx = 0
    t0 = time.time()
    try:
        while secs is None or time.time() - t0 < secs:
            chunk = s.read(256)
            if chunk:
                buf += chunk
                # decode any complete frames, then keep the unparsed tail
                for fr in split_frames(bytes(buf)):
                    report(fr, idx, tag)
                    idx += 1
                # keep only bytes after the last end tag to avoid re-decoding
                cut = bytes(buf).rfind(bytes([ETX1, ETX2]))
                if cut >= 0:
                    del buf[: cut + 2]
    except KeyboardInterrupt:
        pass
    finally:
        s.close()
        print(f"# {idx} frames decoded", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description="Decode Hisense A/C-bus frames.")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--log")
    g.add_argument("--raw")
    g.add_argument("--hex")
    g.add_argument("--port")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--secs", type=float, default=None)
    ap.add_argument("--dir", default="", help="tx|rx|di|ro label for the source")
    a = ap.parse_args()
    tag = a.dir.upper()
    if a.log:
        run_static(bytes_from_log(a.log), tag)
    elif a.raw:
        run_static(open(a.raw, "rb").read(), tag)
    elif a.hex:
        run_static(bytes_from_hex(a.hex), tag)
    elif a.port:
        run_live(a.port, a.baud, a.secs, tag)


if __name__ == "__main__":
    main()
