#!/usr/bin/env python3
"""Write the seed corpus (seeds/<harness>/) and the known-failure reproducers (regressions/).

The files are committed; this script is how they were made and how to make them again after a
harness changes its input format. It is deterministic and reads the golden frames out of the
codec's own sources (hisense_protocol.h / .cpp) instead of repeating their bytes here:

    STATUS_REQUEST, LINK_INIT_0A, LINK_INIT_07, LINK_HEARTBEAT   captured from the vendor module
    FRAME_ON, FRAME_OFF                                          the literal power frames

A/C -> module frames are built the way the existing tests build them (test_codec.cpp
make_status(), test_esphome_setup_request.cpp link_reply(), test_esphome_bus.cpp ac_frame()),
with the byte values the RE doc gives (reverse-engineering/docs/03-rs485-ac-protocol.md).

Each harness documents its input format at the top of its .cpp; the builders below follow those.
"""

import re
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent.parent / "esphome" / "components" / "hisense_ac"


def c_array(name: str) -> bytes:
    """The initialiser of `name[...] = {...};` from the codec sources."""
    for src in ("hisense_protocol.h", "hisense_protocol.cpp"):
        text = (PORT / src).read_text()
        m = re.search(r"\b" + name + r"\[[A-Z_a-z0-9]*\]\s*=\s*\{(.*?)\};", text, re.S)
        if m:
            return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    raise SystemExit(f"{name} not found in the codec sources")


STATUS_REQUEST = c_array("STATUS_REQUEST")
LINK_INIT_0A = c_array("LINK_INIT_0A")
LINK_INIT_07 = c_array("LINK_INIT_07")
LINK_HEARTBEAT = c_array("LINK_HEARTBEAT")
FRAME_ON = c_array("FRAME_ON")
FRAME_OFF = c_array("FRAME_OFF")
assert (len(STATUS_REQUEST), len(LINK_INIT_0A), len(LINK_HEARTBEAT), len(FRAME_ON)) == (21, 20, 28, 50)


def seal(frame: bytearray) -> bytes:
    """LEN, 16 bit big-endian byte sum over [2, n-4), F4 FB."""
    n = len(frame)
    frame[0], frame[1], frame[4] = 0xF4, 0xF5, n - 9
    s = sum(frame[2 : n - 4]) & 0xFFFF
    frame[n - 4], frame[n - 3], frame[n - 2], frame[n - 1] = s >> 8, s & 0xFF, 0xF4, 0xFB
    return bytes(frame)


def stuff(frame: bytes) -> bytes:
    """Wire form as the A/C sends it: every 0xF4 between the tags doubled."""
    out = bytearray(frame[:2])
    for b in frame[2:-2]:
        out.append(b)
        if b == 0xF4:
            out.append(0xF4)
    return bytes(out + frame[-2:])


def status(mode_nibble=2, run=2, setpoint=22, room=21, fan=0x01, flags1=0, flags2=0, sleep=0, hz=0, outdoor=32,
           unit_f=False, sub=0x00, tune=None) -> bytes:
    """160 byte 0x66 status. Header bytes from test_codec.cpp make_status()."""
    f = bytearray(160)
    f[0:16] = bytes([0xF4, 0xF5, 0x01, 0x40, 0x97, 0x01, 0x00, 0xFE, 0x01, 0x01, 0x01, 0x01, 0x00, 0x66, sub, 0x01])
    f[16], f[17] = fan, sleep
    f[18] = (mode_nibble << 4) | (run << 2)  # RE doc 03: cool running = 0x28
    f[19], f[20] = setpoint & 0xFF, room & 0xFF
    f[26] = 0x02 if unit_f else 0x00  # bit 1: Fahrenheit panel
    f[35], f[36], f[42], f[44] = flags1, flags2, hz, outdoor & 0xFF
    if tune is not None:  # choose byte 100 so that the checksum's low byte is `tune`
        f[100] = (tune - sum(f[2:156])) & 0xFF
    return seal(f)


def link_reply(flags: int, good=True) -> bytes:
    """28 byte 0x1E reply, request bits in byte 17 (test_esphome_setup_request.cpp link_reply())."""
    f = bytearray(28)
    f[2], f[3], f[13], f[17] = 0x01, 0x40, 0x1E, flags
    out = bytearray(seal(f))
    if not good:
        out[25] ^= 0x01
    return bytes(out)


def devtype_reply(hi=0x01, lo=0x02) -> bytes:
    """24 byte 0x0A reply with the device type at 16/17 (test_esphome_bus.cpp ac_frame(24, 0x0A, 0x04))."""
    f = bytearray(24)
    f[2], f[3], f[13], f[14], f[16], f[17] = 0x01, 0x40, 0x0A, 0x04, hi, lo
    return seal(f)


# test_codec.cpp, "#49 devtype from reply": the 20 byte literal, as written there.
DEVTYPE_20 = bytes([0xF4, 0xF5, 0x01, 0x40, 0x0B, 0x01, 0x00, 0xFE, 0x01, 0x02,
                    0x03, 0x04, 0x00, 0x0A, 0x00, 0x00, 0x01, 0x01, 0xF4, 0xFB])

STATUS_COOL = status()
STATUS_HEAT_F = status(mode_nibble=1, setpoint=72, room=70, unit_f=True, fan=0x0A, flags1=0x80, hz=45)
STATUS_AUTO = status(mode_nibble=5, fan=0x12, flags1=0x06, flags2=0x04, sleep=2)
STATUS_OFF = status(run=0)
STATUS_STUFFED = status(tune=0xF4)  # checksum low byte 0xF4: doubled on the wire
FEATURES = status(sub=0x40, flags1=0x03)
assert STATUS_STUFFED[157] == 0xF4

seeds: dict[str, dict[str, bytes]] = {h: {} for h in
                                      ("assembler", "parsers", "setup_request", "builders", "roundtrip",
                                       "differential", "bus")}
known: dict[str, bytes] = {}

# ---- assembler: [good-frame pick] + stream -------------------------------------------------------
a = seeds["assembler"]
a["status"] = b"\x01" + stuff(STATUS_COOL)
a["status_stuffed_checksum"] = b"\x01" + stuff(STATUS_STUFFED)
a["link_reply"] = b"\x00" + stuff(link_reply(0x20))
a["devtype_reply"] = b"\x02" + stuff(devtype_reply())
a["echo_of_our_poll"] = b"\x00" + STATUS_REQUEST
a["echo_then_reply"] = b"\x01" + LINK_HEARTBEAT + stuff(link_reply(0x00)) + stuff(STATUS_AUTO)
a["back_to_back"] = b"\x00" + stuff(STATUS_COOL) + stuff(link_reply(0x28)) + stuff(devtype_reply()) + stuff(STATUS_OFF)
a["noise_then_frame"] = b"\x00" + bytes([0x00, 0xFB, 0x12, 0xF4, 0xFB]) + stuff(link_reply(0x08))
a["oversize_len"] = b"\x00" + bytes([0xF4, 0xF5, 0x01, 0x40, 0xFF]) + stuff(link_reply(0x00))
a["max_len_header"] = b"\x00" + bytes([0xF4, 0xF5, 0x01, 0x40, 0xBF])
a["cut_status"] = b"\x01" + stuff(STATUS_COOL)[:70]
a["false_start"] = b"\x00" + bytes([0xF4, 0xF5]) + stuff(link_reply(0x20))
a["cut_then_short_frame"] = b"\x00" + stuff(STATUS_COOL)[:60] + stuff(link_reply(0x08))
a["cut_inside_end_tag"] = b"\x01" + stuff(STATUS_COOL)[:159]
a["greedy_header_twice"] = b"\x00" + bytes([0xF4, 0xF5, 0x01, 0x40, 0xBF]) * 2 + stuff(link_reply(0x00))[:11]
a["tiny_frame"] = b"\x02" + bytes([0xF4, 0xF5, 0x01, 0x40, 0x00, 0x00, 0x41, 0xF4, 0xFB])

# ---- parsers: the bytes are the frame ------------------------------------------------------------
p = seeds["parsers"]
p["status_cool"] = STATUS_COOL
p["status_heat_fahrenheit"] = STATUS_HEAT_F
p["status_auto"] = STATUS_AUTO
p["features"] = FEATURES
p["link_reply_smartcfg"] = link_reply(0x20)
p["link_reply_both"] = link_reply(0x28)
p["link_reply_bad_checksum"] = link_reply(0x20, good=False)
p["devtype_reply"] = devtype_reply()
p["status_request"] = STATUS_REQUEST
p["link_heartbeat"] = LINK_HEARTBEAT
p["power_on"] = FRAME_ON
p["status_short_45"] = seal(bytearray(STATUS_COOL[:45]))

# ---- setup_request: [start] + [control, request, dt hi, dt lo]* ----------------------------------
s = seeds["setup_request"]


def step(req, dt, indicating=False, lockout=False, slow=False):
    return bytes([(1 if indicating else 0) | (2 if lockout else 0) | (4 if slow else 0), req, dt >> 8, dt & 0xFF])


s["one_pulse"] = b"\x00" + step(0, 1000) + step(0x20, 1000) + step(0, 1000)
s["pulse_in_boot_lockout"] = b"\x04" + step(0x20, 1000) + step(0, 1000) + step(0x20, 200, slow=True)
s["reconfig_held"] = b"\x00" + step(0, 1000) + step(0x08, 1000) * 4 + step(0, 1000)
s["reconfig_echo_of_our_flag"] = b"\x00" + step(0x08, 1000, indicating=True) * 5 + step(0x08, 1000) * 4
s["leave_then_press"] = b"\x00" + step(0, 1000) + step(0x20, 1000) + step(0, 1000, lockout=True) + step(0x20, 1000)
s["across_the_wrap"] = b"\x05" + step(0x20, 4000) + step(0, 4000) + step(0x20, 4000) + step(0x20, 200, slow=True)
s["both_bits"] = b"\x02" + step(0, 1000) + step(0x28, 1000) * 5 + step(0x20, 1000) + step(0x08, 1000) * 3
s["helpers"] = b"\x08" + bytes([0x20, 2, 0, 3, 0x28, 0, 0, 0x27, 0x10, 0, 0, 0, 0, 1])

# ---- builders: [op, capacity, fields...]* (op | 0x80 = comfortable capacity) ----------------------
b = seeds["builders"]


def cmd(mode, fan, vs, hs, feat, disp, sp, unit_f=0, power=0, fold=1, cap=0, op=0x80):
    return bytes([op, cap, mode, fan, vs, hs, feat, disp, fold, sp & 0xFF, unit_f, power])


b["command_cool_24"] = cmd(2, 0, 0, 0, 0, 0, 24)
b["command_auto_power_on"] = cmd(4, 5, 2, 0, 1, 1, 22, power=1)
b["command_heat_fahrenheit"] = cmd(1, 6, 1, 2, 2, 2, 72, unit_f=1)
b["command_dry_stale_setpoint"] = cmd(3, 4, 0, 1, 3, 0, 5)
b["command_setpoint_out_of_range"] = cmd(2, 0, 0, 0, 0, 0, 40)
b["command_tight_buffer"] = cmd(2, 0, 0, 0, 0, 0, 24, op=0x00, cap=51)
b["fixed_builders"] = bytes([0x81, 0, 1, 0x82, 0, 1, 0x83, 0, 3, 0x84, 0, 0x85, 0, 1, 1, 0x01, 49, 0, 0x04, 20, 0x05, 29, 1])
b["single_field_mute"] = bytes([0x86, 0, 35, 0x30])
b["stamp_token_heartbeat"] = bytes([0x87, 0, 28 - 13]) + LINK_HEARTBEAT[2:28] + bytes([0, 0x01, 0x02, 0])
b["stamp_beep_command"] = bytes([0x88, 0, 50 - 13]) + FRAME_ON[2:50] + bytes([1, 0, 0])
b["stamp_beep_status_poll"] = bytes([0x88, 0, 21 - 13]) + STATUS_REQUEST[2:21] + bytes([0, 0, 0])
# A 13 byte non-command frame whose checksum low byte is 0xF4 (0x40 + 0x04 + 0xB0), so it sits in
# the transmit queue with that byte doubled, and stamp_beep has to copy it through intact.
b["stamp_beep_stuffed_passthrough"] = bytes([0x88, 0, 0]) + bytes([0, 0, 0, 0xB0, 0, 0, 0, 0, 0, 0, 0]) + bytes([0, 0])

# ---- roundtrip: [class, length pick, lead-in, repeats] + frame bytes -----------------------------
r = seeds["roundtrip"]
r["status_160"] = bytes([0, 160 - 49, 0, 0]) + STATUS_COOL
r["status_fahrenheit_twice"] = bytes([0, 160 - 49, 3, 1]) + STATUS_HEAT_F
r["status_all_f4"] = bytes([0, 200 - 49, 0, 2]) + b"\xF4" * 200
r["features_160"] = bytes([1, 160 - 40, 0, 0]) + FEATURES
r["link_reply_28"] = bytes([2, 28 - 22, 5, 0]) + link_reply(0x28)
r["devtype_24"] = bytes([3, 24 - 22, 0, 0]) + devtype_reply()

# ---- differential: [group] + group input ---------------------------------------------------------
d = seeds["differential"]
# Group 0, byte 1: 0 = arbitrary stream, 1 = whole frames ([pick, ...]: odd pick = pick >> 4 noise
# bytes, even pick = [length - 9, frame bytes]).
d["stream_status"] = b"\x00\x00" + stuff(STATUS_STUFFED) + stuff(link_reply(0x20))
d["stream_echo"] = b"\x00\x00" + STATUS_REQUEST + LINK_HEARTBEAT + bytes([0xF4, 0xF5, 0x01, 0x40, 0xFF])
d["stream_false_start"] = b"\x00\x00" + bytes([0xF4, 0xF5]) + stuff(link_reply(0x20))
d["stream_cut_then_whole"] = b"\x00\x00" + stuff(STATUS_COOL)[:60] + stuff(link_reply(0x08)) + stuff(STATUS_COOL)
d["stream_greedy_header"] = b"\x00\x00" + bytes([0xF4, 0xF5, 0x01, 0x40, 0xBF]) + stuff(link_reply(0x00)) * 3
d["clean_frames"] = (b"\x00\x01" + bytes([0x00, 28 - 9]) + link_reply(0x28) + bytes([0x31, 0x00, 0xFB, 0x12])
                     + bytes([0x00, 160 - 9]) + STATUS_STUFFED + bytes([0x00, 0]) + bytes(9))
d["parse_status"] = b"\x01" + STATUS_AUTO
d["parse_features"] = b"\x01" + FEATURES
d["parse_devtype_20"] = b"\x01" + DEVTYPE_20
d["parse_link_reply"] = b"\x01" + link_reply(0x08)
d["stamp_heartbeat"] = b"\x02" + bytes([0x00, 80, 0x01, 0x02]) + LINK_HEARTBEAT
d["stamp_power_on_quiet"] = b"\x02" + bytes([0x00, 80, 0x36, 0x00]) + FRAME_ON
d["stamp_garbage_forced_header"] = b"\x02" + bytes([0x07, 60, 0xF4, 0xF4]) + bytes(range(40))
d["build_cool"] = b"\x03" + bytes([64, 2, 24, 0, 0, 0, 0, 0, 0, 1, 2, 35, 0x30, 1, 1])
d["build_auto_f"] = b"\x03" + bytes([79, 4, 72, 1, 9, 3, 3, 1, 1, 0, 9, 17, 9, 0, 0])
d["helpers"] = b"\x04" + bytes([0x00, 72, 22, 1, 0x20, 3, 2, 0, 0, 0, 0x10, 0, 0, 0, 0x27, 0x10, 0x1E, 0x00, 0xDE, 0xAD, 0xBE, 0xEF])

# ---- bus: [options] + [op, args...]* -------------------------------------------------------------
u = seeds["bus"]


def tick(ms):
    """Advance the clock and poll; ops 0, 1, 2 are 1, 8 and 64 ms units."""
    out = b""
    while ms >= 64 * 255:
        out += bytes([2, 255])
        ms -= 64 * 255
    if ms >= 64:
        out += bytes([2, ms // 64])
        ms %= 64
    if ms:
        out += bytes([0, ms])
    return out


def reply(*args):
    return bytes([4, 0, *args])


# One transaction with DE: 5 ms settle, write, drain (wire time + 25 ms), listen.
EXCHANGE = tick(5) + tick(90)
boot = EXCHANGE + reply(0x01, 0x02, 0) + tick(1) + EXCHANGE + reply(0, 0) + tick(1)  # DevType, then 0x07
cycle = (EXCHANGE + reply(0x00, 0) + tick(1)  # heartbeat -> 0x1E reply
         + EXCHANGE + reply(0, 0) + tick(1)  # status poll -> status
         + EXCHANGE + reply(0, 0) + tick(1)  # ProductType poll -> features
         + tick(700))
u["boot_and_two_cycles"] = b"\x01" + boot + cycle * 2
u["boot_no_de"] = b"\x00" + tick(1) + reply(0x01, 0x02, 0) + tick(1) + reply(0, 0) + tick(1) + cycle
u["silent_unit"] = b"\x01" + tick(64 * 100) * 12
u["command_and_echo"] = (b"\x01" + boot + bytes([5, 0, 2, 8, 0, 1]) + EXCHANGE + reply(0x00, 0) + tick(1)
                         + EXCHANGE + reply(0x01, 0) + tick(1) + cycle)
u["command_unanswered"] = b"\x01" + boot + cycle + bytes([5, 1, 1]) + (tick(64 * 20)) * 8
u["setup_gesture"] = (b"\x03" + boot + tick(64 * 200) + EXCHANGE + reply(0x20, 0) + tick(1) + bytes([6, 1])
                      + tick(1000) + EXCHANGE + reply(0x08, 0) + tick(1) + bytes([6, 0]) + cycle)
u["damaged_replies"] = b"\x01" + EXCHANGE + reply(0x01, 0x02, 17) + tick(1) + EXCHANGE + bytes([3, 6, 0xF4, 0xF5, 0x01, 0x40, 0xBF, 0x00]) + tick(600)
u["raw_enqueue"] = b"\x01" + boot + bytes([7, 5, 1, 2, 3, 4, 5]) + cycle

# ---- known failures: one reproducer each, see regressions/KNOWN_FAILURES --------------------------
bad = bytearray(link_reply(0x20))
bad[24] ^= 0x40
known["assembler/asm-no-checksum"] = b"\x00" + bytes(bad)
# Fixed (devtype_from_reply needs 22 bytes). Kept as a plain regression input for the parsers.
known["parsers/devtype-overlaps-trailer"] = DEVTYPE_20
# Both fixed (the builders refuse the input). Kept as plain regression inputs for the builders.
known["builders/builder-enum-range"] = cmd(8, 0, 0, 0, 0, 0, 24, fold=0)
known["builders/single-field-offset"] = bytes([0x86, 0, 0, 0x00])
known["builders/tx-f4-in-body"] = bytes([0x87, 0, 28 - 13]) + LINK_HEARTBEAT[2:28] + bytes([0, 0xF4, 0xF4, 0])
# Boot with DE; the DevType reply 01 02 arrives with bit 0 of byte 17 flipped (damage 17) and a
# checksum that no longer matches. The scheduler used to adopt 01 03 as the link token. Fixed, kept
# as a plain regression input for the bus harness.
known["bus/bus-token-no-checksum"] = b"\x01" + EXCHANGE + reply(0x01, 0x02, 17) + tick(1)


def write_tree(root: Path, files: dict[str, bytes], wipe: list[str]):
    for sub in wipe:
        shutil.rmtree(root / sub, ignore_errors=True)
    for rel, data in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


if __name__ == "__main__":
    flat = {f"{h}/{name}": data for h, entries in seeds.items() for name, data in entries.items()}
    write_tree(HERE / "seeds", flat, list(seeds))
    write_tree(HERE / "regressions", known, [])
    print(f"{len(flat)} seeds, {len(known)} known-failure reproducers")
