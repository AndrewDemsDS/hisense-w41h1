#!/usr/bin/env python3
"""Pre-flight and staging guards shared by ota-release.sh and esp32-release.sh.

Every decision here is a pure function with a host test (firmware/test/test_ota_guards.py);
ota-guards.sh only moves files and calls this CLI. Each guard maps to a near miss from the
2026-09-14 fleet update:

  flavour        the ESP32 release script defaulted to DEBUG while the node ran RELEASE, so the
                 unauthenticated :2323 console nearly shipped. The flavour is read from the
                 image bytes. It can only be read from the image BUILT here: nothing on the wire
                 reports the running node's flavour, so the expected flavour comes from config.
  stale          `package` died half way (detools missing) and left an August debug .ota/.json
                 in built-images/ that `stage` would have shipped.
  link           node at -76 dBm with 14 s reads: every update_node failed at AnnounceOTAProvider
                 after ~3.5 min of retries. One RSSI read up front refuses in a second.
  archive-plan   the provider dir held 90 active manifests. Only the manifest being shipped
                 should be offered for its product id; the rest are renamed, never deleted.
  repoint        the HTTP mirror's current-image path is what EVERY deployed node's break-glass
                 fetches, and ESP32_FLAVOUR defaults to debug, so a default stage repointed the
                 fleet's recovery image at the unauthenticated :2323 console. A debug image is
                 archived but only repoints the current path on an explicit override.

CLI (exit 0 = pass, 1 = refused, 2 = usage/IO):
  ota_guards.py flavour IMAGE MARKER                 print debug|release
  ota_guards.py check-flavour IMAGE MARKER WANT      refuse when the image is not WANT
  ota_guards.py stale INPUT OUTPUT...                refuse when an OUTPUT is older than INPUT
  ota_guards.py archive-plan KEEP < listing          names to archive (listing: name<TAB>json)
  ota_guards.py link WS NODE [MIN_RSSI] [MAX_S]      read 0/54/4 via matter-server (needs aiohttp)
  ota_guards.py repoint IMAGE MARKER [ALLOW_DEBUG]   refuse repointing the mirror at a debug image
"""

import json
import os
import sys
import time

MIN_RSSI_DBM = -70     # AnnounceOTAProvider / BDX failed at -76; succeeded at -58
MAX_READ_S = 1.0       # healthy node answers in ~0.1 s; the failing one took 14 s
ARCHIVE_SUFFIX = ".archived"


def image_flavour(data: bytes, marker: bytes) -> str:
    """debug when the console's log string is compiled in, else release."""
    return "debug" if marker and marker in data else "release"


def flavour_mismatch(have: str, want: str):
    """None when they match, else a human reason."""
    if want not in ("debug", "release"):
        return f"unknown flavour '{want}' (want debug or release)"
    if have == want:
        return None
    if have == "debug":
        return "image CONTAINS the unauthenticated :2323 console but the release flavour was requested"
    return "debug flavour requested but the image has NO console"


def stale_outputs(input_mtime: float, outputs: dict) -> list:
    """outputs: name -> mtime or None (missing). Missing counts as stale: stage would fail anyway."""
    return sorted(n for n, m in outputs.items() if m is None or m < input_mtime)


def link_verdict(rssi, read_s, min_rssi=MIN_RSSI_DBM, max_s=MAX_READ_S):
    """(ok, reason). rssi None means the read failed or the attribute is null."""
    if rssi is None:
        return False, "no RSSI reading (node unreachable or 0/54/4 null)"
    if rssi < min_rssi:
        return False, f"RSSI {rssi} dBm is below {min_rssi} dBm (OTA announce/BDX fails on this link)"
    if read_s is not None and read_s > max_s:
        return False, f"attribute read took {read_s:.1f} s (limit {max_s:.1f} s)"
    return True, f"RSSI {rssi} dBm, read {read_s:.2f} s"


def repoint_verdict(flavour: str, allow_debug: bool = False):
    """(ok, reason): may this image become the mirror's current-image path? Only release may,
    unless the override is set. Anything but a known flavour is refused, never assumed release."""
    if flavour == "release":
        return True, "release image: repointing the current-image path"
    if flavour == "debug" and allow_debug:
        return True, "WARNING: OTA_HTTP_REPOINT_DEBUG=1, every node's break-glass now fetches a debug image"
    if flavour == "debug":
        return False, ("debug image (unauthenticated :2323 console): archived only, the current-image "
                       "path still serves the last release. OTA_HTTP_REPOINT_DEBUG=1 overrides")
    return False, f"unknown flavour '{flavour}': archived only"


def archive_plan(keep: str, manifests: dict) -> list:
    """Manifests to archive so KEEP is the only active one for its vid/pid.

    manifests: file name -> parsed JSON (or None when unreadable). Unreadable files and other
    products are left alone. Stock-revert manifests (rac-stock-*) are kept: `revert --apply`
    relies on them and removing a way back is not this guard's call.
    """
    def ident(doc):
        mv = (doc or {}).get("modelVersion") or {}
        return mv.get("vid"), mv.get("pid")
    if keep not in manifests or manifests[keep] is None:
        raise ValueError(f"keep manifest {keep} not in listing")
    want = ident(manifests[keep])
    return sorted(n for n, d in manifests.items()
                  if n != keep and d is not None and ident(d) == want and not n.startswith("rac-stock-"))


def parse_listing(text: str) -> dict:
    out = {}
    for line in text.splitlines():
        if "\t" not in line:
            continue
        name, body = line.split("\t", 1)
        name = os.path.basename(name.strip())
        try:
            out[name] = json.loads(body)
        except ValueError:
            out[name] = None
    return out


async def read_link(ws_url: str, node: int):
    import aiohttp  # only the link guard needs the network
    async with aiohttp.ClientSession() as s:
        async with s.ws_connect(ws_url, heartbeat=30) as ws:
            await ws.receive(timeout=10)
            t0 = time.monotonic()
            await ws.send_json({"message_id": "rssi", "command": "read_attribute",
                                "args": {"node_id": node, "attribute_path": "0/54/4"}})
            while True:
                d = json.loads((await ws.receive(timeout=30)).data)
                if d.get("message_id") == "rssi":
                    break
            dt = time.monotonic() - t0
            res = d.get("result")
            val = res.get("0/54/4") if isinstance(res, dict) else res
            return (val if isinstance(val, int) else None), dt


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    cmd, args = argv[1], argv[2:]
    if cmd in ("flavour", "check-flavour") and len(args) >= 2:
        with open(args[0], "rb") as f:
            have = image_flavour(f.read(), args[1].encode())
        if cmd == "flavour":
            print(have)
            return 0
        why = flavour_mismatch(have, args[2] if len(args) > 2 else "")
        print(why or f"flavour ok: {have}")
        return 1 if why else 0
    if cmd == "repoint" and len(args) >= 2:
        with open(args[0], "rb") as f:
            have = image_flavour(f.read(), args[1].encode())
        ok, why = repoint_verdict(have, len(args) > 2 and args[2] == "1")
        print(why)
        return 0 if ok else 1
    if cmd == "stale" and len(args) >= 2:
        src = os.path.getmtime(args[0])
        bad = stale_outputs(src, {o: (os.path.getmtime(o) if os.path.exists(o) else None) for o in args[1:]})
        for b in bad:
            print(f"stale or missing (older than {os.path.basename(args[0])}): {b}")
        return 1 if bad else 0
    if cmd == "archive-plan" and len(args) == 1:
        try:
            for n in archive_plan(args[0], parse_listing(sys.stdin.read())):
                print(n)
        except ValueError as e:
            print(e, file=sys.stderr)
            return 2
        return 0
    if cmd == "link" and len(args) >= 2:
        import asyncio
        min_rssi = int(args[2]) if len(args) > 2 else MIN_RSSI_DBM
        max_s = float(args[3]) if len(args) > 3 else MAX_READ_S
        try:
            rssi, dt = asyncio.run(read_link(args[0], int(args[1])))
        except Exception as e:  # unreachable server/node is a refusal, not a crash
            rssi, dt = None, None
            print(f"link read failed: {e!r}"[:160])
        ok, why = link_verdict(rssi, dt, min_rssi, max_s)
        print(why)
        return 0 if ok else 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
