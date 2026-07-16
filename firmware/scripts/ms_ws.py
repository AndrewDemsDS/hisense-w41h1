"""ms_ws.py -- shared Matter-server WebSocket helper.

One async call() (send a command, await the reply whose message_id matches) and
one async connect() context manager (open the WS, consume the server_info hello
frame). Both read the matter-server WS endpoint from $MS_WS (default
ws://localhost:5580/ws); node_id() reads the target node from $NODE_ID.

Factored out of mcli/mnodes/ota_go/drive9, which each previously carried their
own copy of call() plus a hardcoded ws://localhost:5580/ws + node 9.
"""

import os
import json
from contextlib import asynccontextmanager

import aiohttp

DEFAULT_WS = "ws://localhost:5580/ws"


def ws_url():
    """Matter-server WS endpoint ($MS_WS, default ws://localhost:5580/ws)."""
    return os.environ.get("MS_WS", DEFAULT_WS)


def node_id(default=9):
    """Target node id ($NODE_ID if set, else the caller's default)."""
    v = os.environ.get("NODE_ID")
    return int(v) if v else default


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
