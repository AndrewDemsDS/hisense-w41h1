"""ms_ws.py -- shared Matter-server WebSocket helper.

One async call() (send a command, await the reply whose message_id matches) and
one async connect() context manager (open the WS, consume the server_info hello
frame). ws_url() and node_id() read $MS_WS / $NODE_ID, falling back to the same
ota-release.env that ota-release.sh uses (so a bare `python3 <script>.py` targets
the right Pi + node). There is NO silent default: matter-server runs on the Pi and
the node id is per-device, so a wrong endpoint/node quietly drives nothing -- both
raise a clear error instead (the old ws://localhost:5580 + node 9 defaults were
stale and silently mistargeted).

Factored out of mcli/mnodes/ota_go/drive_ac, which each previously carried their
own copy of call() plus a hardcoded ws://localhost:5580/ws + node 9.
"""

import os
import re
import json
from contextlib import asynccontextmanager

import aiohttp

# ota-release.env sits next to this module; it's the source of truth for MS_WS + NODE_ID.
_ENV_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ota-release.env")


def _env(key):
    """$key from the environment, else the same key parsed from ota-release.env, else None.
    The env always wins; the file is only a fallback for bare invocation."""
    v = os.environ.get(key)
    if v:
        return v
    try:
        with open(_ENV_FILE) as f:
            for line in f:
                m = re.match(rf'\s*{key}\s*=\s*"?([^"#\n]+?)"?\s*(?:#.*)?$', line)
                if m:
                    return m.group(1)
    except OSError:
        pass
    return None


def ws_url():
    """Matter-server WS endpoint from $MS_WS or ota-release.env (required, no default)."""
    v = _env("MS_WS")
    if not v:
        raise SystemExit(
            "MS_WS not set -- export it or set it in firmware/scripts/ota-release.env "
            "(e.g. ws://<pi-host>:5580/ws). matter-server runs on the Pi, not localhost."
        )
    return v


def node_id():
    """Target node id from $NODE_ID or ota-release.env (required, no default)."""
    v = _env("NODE_ID")
    if not v:
        raise SystemExit(
            "NODE_ID not set -- export it or set it in firmware/scripts/ota-release.env "
            "(per-device; e.g. kitchen AmebaZ2=14, ESP32=28)."
        )
    return int(v)


async def call(ws, cmd, args, mid, timeout=180):
    """Send {command, args, message_id} and return the reply matching message_id."""
    await ws.send_json({"message_id": mid, "command": cmd, "args": args})
    while True:
        msg = await ws.receive(timeout=timeout)
        if msg.type != aiohttp.WSMsgType.TEXT:
            print("WS closed/err:", msg.type)
            return None
        d = json.loads(msg.data)
        if d.get("message_id") == mid:
            return d


@asynccontextmanager
async def connect(heartbeat=30, hello_timeout=30):
    """Open the matter-server WS, consume the server_info hello frame, yield (ws, hello)."""
    async with aiohttp.ClientSession() as s:
        async with s.ws_connect(ws_url(), heartbeat=heartbeat) as ws:
            hello = await ws.receive(timeout=hello_timeout)
            yield ws, hello
