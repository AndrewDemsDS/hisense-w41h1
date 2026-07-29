#!/usr/bin/env python3
"""HIL: play a synthetic A/C to a module running STOCK firmware, and capture the bus.

Layer 5 (needs hardware): a USB-TTL tap on the module's A/C-bus UART. Unlike
virtual_ac.py --port, this adds what a stock-side sniffing session actually needs:

  * both-direction raw capture with timestamps, frames split on a >=10ms idle gap
  * an on-demand "77" (smart-config pairing) pulse
  * auto-reconnect, so a module power-cycle or an adapter re-enumeration does not
    end the session (stock throws SerialException: "device reports readiness to
    read but returned no data")

Stock is stricter than our own firmware. Its RX parser device_package_get_cmd
(0x9b6f2e00) enforces THREE response flags that the module's own TX path leaves
at 0x00, so a reply built by echoing the request is rejected before any class
handler runs:

    frame[2]  link resp       must be 0x01   (else -2)
    frame[5]  net resp        must be 0x01   (else -5)
    frame[11] transport resp  must be 0x01   (else -6)

plus payload[2] (frame[15]) = 0x01 as the A/C->module marker. See
reverse-engineering/docs/10 section 3.2. virtual_ac.py's handshake path was fixed
for this; the frames here are built the same way.

Baud note: stock alternates 9600<->115200 every DevType attempt until a reply
parses (docs/10 section 4.6). Pick either; it stops toggling once it hears you.

Usage:
  hil_stock_ac.py --port /dev/ttyUSB0 [--baud 115200] [--log bus.log]
                  [--trigger /tmp/trig77] [--caps ai,swing_direction_8]

  Open a pairing window:  touch <trigger>
"""

import argparse, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import virtual_ac
from virtual_ac import checksum, split_frames, stuff, DEVTYPE

SMARTCFG = 0x20  # HISENSE_LINK_REQ_SMARTCFG, payload[4] bit5 of the 0x1E reply

# The A/C asserts this for EXACTLY ONE ~1Hz frame per remote press
# (hisense_rs485.cpp:1198-1202). Do not hold it: issue #69 is the re-entry hazard.


def wire(f):
    """Set LEN, append the big-endian checksum, stuff interior 0xF4, add the trailer."""
    f = bytearray(f)
    f[4] = len(f) + 4 - 9  # LEN = total - 9
    ck = checksum(f, len(f))
    f.append(ck >> 8)
    f.append(ck & 0xFF)
    return bytes(f[:2]) + stuff(bytes(f[2:])) + bytes([0xF4, 0xFB])


def gated(payload, src=(0x00, 0x00)):
    """A/C -> module frame with all three response gates asserted."""
    f = bytearray(
        [
            0xF4,
            0xF5,
            0x01,
            0x40,
            0x00,
            0x01,
            0x00,
            0xFE,
            0x01,
            src[0],
            src[1],
            0x01,
            0x00,
        ]
    )
    f.extend(payload)
    return wire(f)


def devtype_reply(req):
    """0x0A reply. MUST be longer than the 20-byte request: device-type lives at
    frame[16] and sub-type at [17], which in a 20-byte frame are the checksum."""
    f = bytearray(req[:16])
    f[2] = f[5] = f[11] = 0x01
    f[9] = f[10] = 0x00  # source address; a real A/C sends 00 00 here (docs/10 4.5)
    f[15] = 0x01
    f.extend(DEVTYPE)
    return wire(f)


def serve(args):
    import serial

    s = serial.Serial(args.port, args.baud, timeout=0.02)
    log = open(args.log, "a", buffering=1)
    ac = virtual_ac.VirtualAC()
    ac.caps = {c.strip() for c in args.caps.split(",") if c.strip()}
    t0 = time.time()
    log.write(
        f"\n# === hil_stock_ac baud={args.baud} caps={sorted(ac.caps)} "
        f"{time.strftime('%Y-%m-%d %H:%M:%S')} ===\n"
    )
    print(
        f"# up on {args.port} @{args.baud}. pair with: touch {args.trigger}", flush=True
    )

    buf = bytearray()
    linked = False

    def wr(d, tag=""):
        log.write(
            f"[{time.time() - t0:9.3f}] AC->MOD ({len(d):3d}) {bytes(d).hex(' ')} {tag}\n"
        )
        s.write(d)

    while True:
        chunk = s.read(512)
        if not chunk:
            time.sleep(0.002)
            continue
        buf += chunk
        frames = list(split_frames(bytes(buf)))
        cut = bytes(buf).rfind(bytes([0xF4, 0xFB]))
        if cut >= 0:
            del buf[: cut + 2]
        for f in frames:
            if len(f) < 14 or f[2] != 0x00:
                continue  # module->A/C only; drops any tap echo of our own replies
            cls = f[13]
            log.write(
                f"[{time.time() - t0:9.3f}] AC<-MOD ({len(f):3d}) {bytes(f).hex(' ')}\n"
            )
            if cls != 0x0A and not linked:
                linked = True
                print(f"*** LINK UP (module advanced to 0x{cls:02X})", flush=True)
            if cls == 0x0A:
                wr(devtype_reply(f), "devtype")
            elif cls == 0x66 and len(f) > 14 and f[14] == 0x40:
                wr(ac.producttype_frame(), "producttype")
            elif cls == 0x66:
                ac._sim_physics(time.time())
                wr(ac.status_frame(), "status")
            elif cls == 0x65:
                ac.apply_command(f)
                wr(ac.status_frame(), "cmd-echo")
            elif cls == 0x1E:
                pl = bytearray([0x1E, f[14] if len(f) > 14 else 0x00, 0x01])
                pl.extend(f[16 : len(f) - 4])
                tag = "link"
                if os.path.exists(args.trigger) and len(pl) > 4:
                    pl[4] |= SMARTCFG
                    os.unlink(args.trigger)
                    tag = "link+77"
                    print("*** 77 ASSERTED (one frame, payload[4] bit5)", flush=True)
                wr(gated(pl, src=(0x01, 0x01)), tag)
            else:
                # 0x07 capability query, 0x67 (issue #110, undecoded), anything new
                wr(
                    gated([cls, f[14] if len(f) > 14 else 0x00, 0x01]),
                    f"gated-0x{cls:02X}",
                )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--log", default="bus_capture.log")
    ap.add_argument("--trigger", default="/tmp/hil_stock_ac.trigger77")
    ap.add_argument(
        "--caps", default="", help="ProductType flags, e.g. ai,swing_direction_8"
    )
    ap.add_argument("--once", action="store_true", help="do not auto-reconnect")
    args = ap.parse_args()

    n = 0
    while True:
        while not os.path.exists(args.port):
            print(f"# waiting for {args.port}", flush=True)
            time.sleep(2)
        n += 1
        try:
            serve(args)
        except KeyboardInterrupt:
            return
        except Exception as e:  # SerialException on power-cycle, and anything else
            print(f"# run {n} died: {type(e).__name__}: {e}", flush=True)
            if args.once:
                raise
            time.sleep(2)


if __name__ == "__main__":
    main()
