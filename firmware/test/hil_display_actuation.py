#!/usr/bin/env python3
"""HIL: ep9 Display actuation + no-collateral regression (needs real hardware).

NOT part of run_tests.sh -- that suite is deliberately no-hardware. This one drives
commissioned nodes through matter-server, so it needs the A/C modules online.

    python3 firmware/test/hil_display_actuation.py            # every configured node
    python3 firmware/test/hil_display_actuation.py 28         # one node

Two properties are checked per node:

1. ACTUATION. On/Off commands land AND the attribute really transitions. OnOff.OnOff
   is read-only (a write returns 0x88 UNSUPPORTED_WRITE), so the switch must be driven
   with commands, which is also what Home Assistant does. A command that matches the
   current value is a no-op: no attribute change, no update callback, no frame on the
   wire. This script forces a genuine transition in both directions so it cannot pass
   on a no-op, which is exactly the false negative that made the original bench test
   look like a failure.

2. NO COLLATERAL. `display` rides the COMBINED command frame, so every display command
   rebuilds and resends mode + setpoint + fan + swing from the command shadow. If the
   shadow has drifted, or the one-shot reset to HISENSE_DISPLAY_NOCHANGE leaks, those
   fields move. Any drift here is a real bug: driving the panel must never retune the
   A/C.

The panel itself is NOT verified here. The A/C reports no display state (power_display
and q_display are ProductType CAPABILITY bits, not live status), so whether the panel
actually lit is a human observation. This script proves the command path and the
absence of side effects; a person at the unit proves the photons.

Node ids come from ota-release.env (NODE_ID / ESP32_NODE_ID) via ms_ws, so there is no
hardcoded fleet here.
"""

import asyncio
import json
import os
import sys

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts")
)
import ms_ws  # noqa: E402

DISPLAY_EP = 9
ONOFF_CLUSTER = 6
DISPLAY_ATTR = "9/6/0"

# Attributes that MUST be identical before and after a display-only actuation.
WATCH = {
    "SystemMode": "1/513/28",
    "CoolSetpoint": "1/513/17",
    "HeatSetpoint": "1/513/18",
    "LocalTemp": "1/513/0",
}

SETTLE_S = 3  # command -> attribute readback; covers the driver's sync hold-off


def configured_nodes():
    """Node ids from the environment, deduped, in a stable order."""
    ids = []
    for key in ("NODE_ID", "ESP32_NODE_ID"):
        raw = os.environ.get(key)
        if raw and raw.strip().isdigit():
            n = int(raw)
            if n not in ids:
                ids.append(n)
    if not ids:
        # ms_ws.node_id() raises a clear error rather than defaulting, which is the point.
        ids = [ms_ws.node_id()]
    return ids


async def read_attr(ws, node, path, mid):
    r = await ms_ws.call(
        ws, "read_attribute", {"node_id": node, "attribute_path": path}, mid, timeout=45
    )
    return r.get("result", {}).get(path)


async def onoff(ws, node, command, mid):
    await ms_ws.call(
        ws,
        "device_command",
        {
            "node_id": node,
            "endpoint_id": DISPLAY_EP,
            "cluster_id": ONOFF_CLUSTER,
            "command_name": command,
            "payload": {},
        },
        mid,
        timeout=45,
    )


async def snapshot(ws, node, tag):
    out = {}
    for i, (name, path) in enumerate(WATCH.items()):
        out[name] = await read_attr(ws, node, path, "%d-%s-%d" % (node, tag, i))
    return out


async def check_node(ws, node):
    print("=== node %d ===" % node)
    before = await snapshot(ws, node, "pre")
    print("  baseline      : %s" % json.dumps(before))

    start = await read_attr(ws, node, DISPLAY_ATTR, "%d-d0" % node)
    print("  ep9 OnOff     : %s" % start)

    # Drive to a known state first so BOTH legs below are genuine transitions even if
    # the switch already happened to sit where we want it.
    if start is not False:
        await onoff(ws, node, "Off", "%d-seed" % node)
        await asyncio.sleep(SETTLE_S)

    await onoff(ws, node, "On", "%d-on" % node)
    await asyncio.sleep(SETTLE_S)
    on_val = await read_attr(ws, node, DISPLAY_ATTR, "%d-d1" % node)
    ok_on = on_val is True
    print("  false -> On   : %s  %s" % (on_val, "OK" if ok_on else "FAIL"))

    await onoff(ws, node, "Off", "%d-off" % node)
    await asyncio.sleep(SETTLE_S)
    off_val = await read_attr(ws, node, DISPLAY_ATTR, "%d-d2" % node)
    ok_off = off_val is False
    print("  true  -> Off  : %s  %s" % (off_val, "OK" if ok_off else "FAIL"))

    after = await snapshot(ws, node, "post")
    clean = after == before
    print("  after actuate : %s" % json.dumps(after))
    print("  collateral    : %s" % ("none (OK)" if clean else "CHANGED -> FAIL"))
    if not clean:
        for name in WATCH:
            if before[name] != after[name]:
                print("      %s: %s -> %s" % (name, before[name], after[name]))

    # Leave the panel lit: that is the A/C's own default, and a dark panel looks broken
    # to whoever walks past next.
    await onoff(ws, node, "On", "%d-restore" % node)
    print("  restored      : Display=On\n")
    return ok_on and ok_off and clean


async def main():
    nodes = [int(a) for a in sys.argv[1:]] or configured_nodes()
    failed = []
    async with ms_ws.connect(heartbeat=30, hello_timeout=30) as (ws, _):
        for node in nodes:
            if not await check_node(ws, node):
                failed.append(node)
    if failed:
        print("RESULT: FAILED on node(s) %s" % ", ".join(str(n) for n in failed))
        return 1
    print("RESULT: PASS on %d node(s)" % len(nodes))
    print("NOTE: the panel itself is a human observation -- see the module docstring.")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
