#!/usr/bin/env python3
"""HIL: capture and diff raw A/C status frames, annotated with the known field map.

Built for bench sessions where the question is "which byte moved when I did X". Every
derived field map in this repo has been wrong at least once, so the tool deliberately
reports RAW byte changes first and annotations second: an unannotated change is a finding,
not noise.

    # snapshot before the change
    hil_frame_diff.py capture before --node 14

    # ... pull the isolator / press the remote / send a tx probe ...

    hil_frame_diff.py capture after  --node 14
    hil_frame_diff.py diff before after

Node addresses and how to reach them come from --addr/--via, or the built-in defaults
below. The consoles sit on an IPv6 net the dev box cannot route, so the default is to
hop through the Pi with ssh.

Why not just eyeball `raw`: on a 160-byte frame, most bytes are zero and several drift on
their own (temperatures, current, compressor). Diffing two captures separates "changed
because I did something" from "changes constantly", which eyeballing does not.
"""

import argparse
import json
import os
import subprocess
import sys

# Known status-frame fields. Byte -> (name, {bit: meaning}).
# Everything here is CONFIRMED on hardware or derived from the stock capability table with
# an independent hardware anchor; see RE docs/10 §7. Bytes absent from this map are not
# "unused", they are UNIDENTIFIED, which is exactly what makes a diff hit interesting.
FIELDS = {
    16: ("fan_raw", {}),
    17: ("sleep_raw", {}),
    18: ("mode / run", {}),
    19: ("setpoint (7-bit)", {}),
    20: ("indoor temp", {}),
    26: ("temp unit / compensate", {1: "C/F unit (0=C)", 4: "temp_compensate"}),
    35: ("flags1", {7: "vswing", 6: "hswing", 4: "heat relay", 2: "eco", 1: "turbo"}),
    36: ("flags2", {7: "purify", 2: "mute"}),
    37: ("dimmer / filter", {7: "display dimmer", 3: "filter clean"}),
    39: (
        "FAULT group 0x18",
        {
            7: "in_temp",
            6: "in_coil",
            5: "in_humidity",
            4: "water_full",
            3: "fan_motor/up",
            2: "grille/dw",
            1: "in_vzero",
            0: "in_com",
        },
    ),
    40: (
        "FAULT group 0x19",
        {7: "in_display", 6: "in_keys", 5: "in_wifi", 4: "in_ele", 3: "in_eeprom"},
    ),
    41: ("compressor target", {}),
    42: ("compressor actual", {}),
    43: ("compressor target", {}),
    44: ("outdoor temp", {}),
    45: ("coil temp", {}),
    50: ("DC bus / voltage", {}),
    55: ("current A", {}),
    56: ("current B", {}),
    60: ("current / discharge", {}),
    64: (
        "FAULT group 0x31",
        {6: "out_eeprom", 5: "out_coil", 4: "out_gas", 3: "out_temp"},
    ),
    66: ("FAULT group 0x33", {4: "over_hot/over_cold"}),
    71: ("cooling counter", {}),
    77: ("8 C heat status", {0: "t_8heat engaged"}),
}

# Bytes that move on their own between captures, so a change here is usually not the
# thing you were testing. Reported separately rather than hidden.
NOISY = {20, 41, 42, 43, 44, 45, 50, 55, 56, 60, 144, 145, 152, 153, 156, 157}

DEFAULT_NODES = {
    "14": "fd00:4::e23e:cbff:fe31:64c6",  # AmebaZ2, SLAAC from MAC e0:3e:cb:31:64:c6
    "28": "fd00:4::3276:f5ff:fee5:5510",  # ESP32
}
DEFAULT_VIA = os.environ.get("HIL_VIA", "root@192.168.10.149")
DEFAULT_KEY = os.environ.get("HIL_SSH_KEY", os.path.expanduser("~/.ssh/id_ed25519_signing"))
STORE = os.environ.get("HIL_CAPTURE_DIR", "/tmp/hil-frames")


def fetch(addr, via, key=None, port=2323, timeout=25):
    """Run `raw` on the console and return the frame bytes."""
    cmd = "printf 'raw\\nquit\\n' | timeout %d nc -w %d %s %d" % (
        timeout,
        timeout - 5,
        addr,
        port,
    )
    if via:
        # The Pi rejects password auth, so the hop needs an explicit key unless the agent
        # already holds it. Silently falling back to no key just produces an empty capture.
        argv = ["ssh", "-o", "BatchMode=yes"]
        if key and os.path.exists(key):
            argv += ["-i", key]
        argv += [via, cmd]
    else:
        argv = ["sh", "-c", cmd]
    r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout + 20)
    out = r.stdout
    if not out.strip() and r.stderr.strip():
        out = "(stderr) " + r.stderr.strip()
    frame = {}
    for line in out.splitlines():
        s = line.strip()
        if ":" not in s:
            continue
        head, _, rest = s.partition(":")
        if not head.strip().isdigit():
            continue
        base = int(head.strip())
        for i, tok in enumerate(rest.split()):
            if len(tok) == 2 and all(c in "0123456789abcdefABCDEF" for c in tok):
                frame[base + i] = int(tok, 16)
    if not frame:
        sys.exit(
            "no frame parsed. Is the console reachable and the DEBUG flavour flashed?\n"
            "raw output was:\n" + out[:400]
        )
    return frame


def annotate(byte, old, new):
    name, bits = FIELDS.get(byte, (None, {}))
    parts = []
    changed = old ^ new
    for bit in range(8):
        if changed & (1 << bit):
            meaning = bits.get(bit)
            state = "set" if new & (1 << bit) else "cleared"
            parts.append(
                "bit%d %s%s"
                % (bit, state, " = " + meaning if meaning else " (UNNAMED)")
            )
    label = name if name else "UNIDENTIFIED BYTE"
    return label, parts


def cmd_capture(a):
    addr = a.addr or DEFAULT_NODES.get(a.node)
    if not addr:
        sys.exit("unknown node %r; pass --addr" % a.node)
    frame = fetch(addr, None if a.via == "none" else a.via, a.key)
    os.makedirs(STORE, exist_ok=True)
    path = os.path.join(STORE, a.label + ".json")
    with open(path, "w") as f:
        json.dump(
            {
                "node": a.node,
                "addr": addr,
                "frame": {str(k): v for k, v in sorted(frame.items())},
            },
            f,
            indent=1,
        )
    print("captured %d bytes from node %s -> %s" % (len(frame), a.node, path))
    for b in sorted(
        k for k in frame if k in FIELDS and FIELDS[k][0].startswith("FAULT")
    ):
        print("  fault byte %d = 0x%02x" % (b, frame[b]))


def cmd_diff(a):
    def load(lbl):
        p = os.path.join(STORE, lbl + ".json")
        if not os.path.exists(p):
            sys.exit("no capture %r (looked in %s)" % (lbl, STORE))
        with open(p) as f:
            d = json.load(f)
        return {int(k): v for k, v in d["frame"].items()}, d

    before, mb = load(a.before)
    after, ma = load(a.after)
    if mb["node"] != ma["node"]:
        print(
            "WARNING: comparing different nodes (%s vs %s)" % (mb["node"], ma["node"])
        )

    keys = sorted(set(before) | set(after))
    interesting, noisy, missing = [], [], []
    for k in keys:
        o, n = before.get(k), after.get(k)
        if o is None or n is None:
            missing.append(k)
            continue
        if o != n:
            (noisy if k in NOISY else interesting).append((k, o, n))

    print("=== %s -> %s ===" % (a.before, a.after))
    if not interesting:
        print("\nNO meaningful byte changed.")
        print("If you expected one, that is a real result: the thing you did is not")
        print("reflected in this frame, or not at the offset we think it is.")
    else:
        print(
            "\nCHANGED (%d byte%s):"
            % (len(interesting), "" if len(interesting) == 1 else "s")
        )
        for k, o, n in interesting:
            label, parts = annotate(k, o, n)
            print("  byte %3d: 0x%02x -> 0x%02x   %s" % (k, o, n, label))
            for p in parts:
                print("             %s" % p)
    if noisy and not a.quiet:
        print(
            "\nalso changed, but these drift on their own (temps, current, compressor):"
        )
        print("  " + ", ".join("byte %d 0x%02x->0x%02x" % t for t in noisy))
    if missing:
        print("\nbytes present in only one capture: %s" % missing[:12])


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("capture", help="grab a raw frame and store it under a label")
    c.add_argument("label")
    c.add_argument("--node", default="14", help="14 (AmebaZ2) or 28 (ESP32)")
    c.add_argument("--addr", help="override the console address")
    c.add_argument("--via", default=DEFAULT_VIA, help="ssh hop, or 'none' for direct")
    c.set_defaults(func=cmd_capture)
    c.add_argument("--key", default=DEFAULT_KEY, help="ssh identity for the hop")

    d = sub.add_parser("diff", help="diff two captures, annotated")
    d.add_argument("before")
    d.add_argument("after")
    d.add_argument("--quiet", action="store_true", help="hide the self-drifting bytes")
    d.set_defaults(func=cmd_diff)

    a = p.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
