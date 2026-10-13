"""One node under test: the host binary, the A/C model on its UART and an API client where Home
Assistant would be. Plus the check counters every scenario reports through.

A known failure is a check that is expected to fail today: `known_failure()` passes while the
defect is there and fails the day it is fixed, so the marker cannot outlive the defect (the same
rule as firmware/test/edge_harness.py).
"""

from __future__ import annotations

import asyncio
import atexit
import ctypes
import math
import os
import pathlib
import re
import signal
import socket
import subprocess
import time

import aioesphomeapi
from aioesphomeapi import APIClient

from ac_model import CLASS_COMMAND, Bus, Unit, seal, unstuff

# Every host binary started and not yet reaped, so that no exit path leaves one behind.
_LIVE: set[subprocess.Popen] = set()
# Bounds on what one node's output may take: on disk, and of the searchable part in memory.
LOG_FILE_MAX = 64 * 1024 * 1024
LOG_MEMORY_MAX = 4 * 1024 * 1024
_ANSI = re.compile(rb"\x1b\[[0-9;]*m")


def kill_all():
    for proc in list(_LIVE):
        try:
            proc.kill()
        except OSError:
            pass
    _LIVE.clear()


atexit.register(kill_all)


def _die_with_parent():
    """In the child: ask the kernel for SIGKILL when the runner dies, however it dies."""
    try:
        ctypes.CDLL(None).prctl(1, signal.SIGKILL)  # PR_SET_PDEATHSIG
    except (OSError, AttributeError):
        pass


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# ---- Expected frames -------------------------------------------------------------------------------
# Written out here from the protocol document (reverse-engineering/docs/03-rs485-ac-protocol.md) and
# the component's builders, byte by byte, so that the test states what must be on the wire instead
# of asking the code under test.

MODE_BYTE = {"fan_only": 0x10, "heat": 0x30, "cool": 0x50, "dry": 0x70, "auto": 0x90}
FAN_BYTE = {"auto": 0x01, "low": 0x0B, "medium_low": 0x0D, "medium": 0x0F, "medium_high": 0x11, "high": 0x13}
FEATURE_BYTE = {"none": 0x04, "eco": 0x30, "turbo": 0x0C, "eco_off": 0x10}


def _command(token, beep=True):
    f = bytearray(50)
    f[:16] = bytes.fromhex("f4f5004029000001 01fe01000065 0000".replace(" ", ""))
    f[7], f[8] = token
    f[23] = 0x04 if beep else 0x00  # the buzzer bit
    f[-2:] = b"\xf4\xfb"
    return f


def combined_frame(mode, setpoint, fan="auto", vswing=False, feature="none", display=True, power_on=False,
                   token=(1, 2), beep=True):
    """The combined 0x65 frame: fan at 16, mode (and power-on) at 18, setpoint * 2 + 1 at 19, the
    marker at 31, swing at 32, eco or turbo at 33, the display preference at 36."""
    f = _command(token, beep)
    f[16] = 0x01 if mode == "dry" else FAN_BYTE[fan]
    f[18] = MODE_BYTE[mode] | (0x0C if power_on else 0)
    f[19] = setpoint * 2 + 1 if mode not in ("dry", "fan_only") else 0
    f[31] = 0x01
    f[32] = 0xC0 if vswing else 0x40
    f[33] = FEATURE_BYTE[feature]
    f[36] = 0xC0 if display else 0x40
    return unstuff(seal(f))


def single_field_frame(offset, value, token=(1, 2), beep=True):
    """The minimal frame for mute (byte 35) and sleep (byte 17): the marker and the one field."""
    f = _command(token, beep)
    f[31] = 0x01
    f[offset] = value
    return unstuff(seal(f))


def power_frame(on, token=(1, 2), beep=True):
    """The literal power frames (FRAME_ON, FRAME_OFF in hisense_protocol.cpp), re-stamped."""
    f = _command(token, beep)
    if on:
        f[18] = 0x0C
    else:
        f[17], f[18], f[24], f[29], f[32], f[33], f[35] = 0x01, 0x04, 0x01, 0x01, 0x01, 0x55, 0x01
    return unstuff(seal(f))


def frame_diff(got: bytes, want: bytes) -> str:
    if len(got) != len(want):
        return f"length {len(got)}, expected {len(want)}: {got.hex(' ')}"
    return ", ".join(f"byte {i}: 0x{g:02X}, expected 0x{w:02X}" for i, (g, w) in enumerate(zip(got, want)) if g != w)


# ---- Check counters --------------------------------------------------------------------------------
class Report:
    """What one scenario found. Printed as a block when the scenario ends."""

    def __init__(self, name: str):
        self.name = name
        self.lines: list[str] = []
        self.ok = True
        self.checks = 0
        self.known = 0
        self.seconds = 0.0

    def check(self, cond, msg, detail="") -> bool:
        self.checks += 1
        if cond:
            self.lines.append(f"  ok   {msg}")
        else:
            self.ok = False
            self.lines.append(f"  FAIL {msg}" + (f": {detail}" if detail != "" else ""))
        return bool(cond)

    def equal(self, got, want, msg) -> bool:
        return self.check(got == want, msg, f"got {got!r}, expected {want!r}")

    def frame(self, got: bytes | None, want: bytes, msg) -> bool:
        if got is None:
            return self.check(False, msg, "no frame reached the unit")
        return self.check(got == want, msg, frame_diff(got, want))

    def frames(self, got: list[bytes], want: list[bytes], msg) -> bool:
        if len(got) != len(want):
            return self.check(False, msg, f"{len(got)} frame(s), expected {len(want)}: " + " | ".join(f.hex(" ") for f in got))
        diffs = [f"frame {i + 1}: {frame_diff(g, w)}" for i, (g, w) in enumerate(zip(got, want)) if g != w]
        return self.check(not diffs, msg, "; ".join(diffs))

    def known_failure(self, fixed, msg, defect):
        """`fixed` is the condition that holds once the defect is gone."""
        self.checks += 1
        if fixed:
            self.ok = False
            self.lines.append(f"  FAIL {msg}: passes now, so the defect is fixed. Turn this into a plain check ({defect})")
        else:
            self.known += 1
            self.lines.append(f"  xfail {msg}  [{defect}]")

    def note(self, msg):
        self.lines.append(f"  note {msg}")


# ---- The node --------------------------------------------------------------------------------------
class Node:
    """Start with `await Node(binary, workdir).start()`, always `await stop()`."""

    BOOT_TIMEOUT = 20.0

    def __init__(self, binary: pathlib.Path, workdir: pathlib.Path, unit: Unit | None = None):
        self.binary = binary
        self.workdir = workdir
        self.unit = unit or Unit()
        self.bus = Bus(self.unit)
        self.proc: subprocess.Popen | None = None
        self.client: APIClient | None = None
        self.port = 0
        self.log_path = workdir / "node.log"
        # The binary's stdout is a pseudo-terminal too, so that libc flushes it line by line and a
        # log line can be waited for. What comes out is kept here and written to log_path.
        self.log_master, self.log_slave = os.openpty()
        os.set_blocking(self.log_master, False)
        self._log = bytearray()
        self._log_partial = b""
        self._log_written = 0
        self._log_file = None
        self.info: dict[str, object] = {}  # "domain.object_id" -> entity info
        self.by_key: dict[int, str] = {}
        self.states: dict[str, object] = {}  # "domain.object_id" -> last state message
        self.history: list[tuple[float, str, object]] = []
        self.device_info = None
        self.exit_code: int | None = None

    # -- life cycle --
    async def start(self, expect_status=True):
        self.workdir.mkdir(parents=True, exist_ok=True)
        self._log_file = open(self.log_path, "ab")
        loop = asyncio.get_running_loop()
        loop.add_reader(self.bus.master, self.bus.on_readable)
        loop.add_reader(self.log_master, self._on_log)
        last_error = None
        # The port is free when it is picked and may be taken by the time the binary binds it.
        for _ in range(4):
            self.port = free_port()
            env = {
                "PATH": os.environ.get("PATH", ""),
                "HOME": str(self.workdir),
                "ESPHOME_PREFDIR": str(self.workdir / "prefs"),
                "HISENSE_E2E_UART": self.bus.path,
                "HISENSE_E2E_API_PORT": str(self.port),
            }
            for key in ("GCOV_PREFIX", "GCOV_PREFIX_STRIP"):
                if key in os.environ:
                    env[key] = os.environ[key]
            self.proc = subprocess.Popen([str(self.binary)], cwd=self.workdir, env=env, stdout=self.log_slave,
                                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                         start_new_session=True, preexec_fn=_die_with_parent)
            _LIVE.add(self.proc)
            try:
                await self._connect()
                break
            except (aioesphomeapi.APIConnectionError, OSError, TimeoutError) as e:
                last_error = e
                await self._stop_process()
        else:
            raise RuntimeError(f"the host binary did not come up: {last_error}\n{self.log_tail()}")
        if expect_status:
            if not await self.wait(lambda: "climate.air_conditioner" in self.states
                                   and not math.isnan(self.climate.current_temperature), self.BOOT_TIMEOUT):
                raise RuntimeError(f"no status reached the climate entity\n{self.log_tail()}")
        return self

    async def _connect(self):
        deadline = time.monotonic() + self.BOOT_TIMEOUT
        while True:
            if self.proc.poll() is not None:
                raise OSError(f"the host binary exited with {self.proc.returncode}")
            client = APIClient("127.0.0.1", self.port, None, client_info="host-e2e")
            try:
                await client.connect(login=True)
                self.client = client
                break
            except aioesphomeapi.APIConnectionError:
                await client.disconnect(force=True)
                if time.monotonic() > deadline:
                    raise
                await asyncio.sleep(0.05)
        self.device_info = await self.client.device_info()
        entities, _services = await self.client.list_entities_services()
        for e in entities:
            ident = f"{domain_of(e)}.{e.object_id}"
            self.info[ident] = e
            self.by_key[e.key] = ident
        self.client.subscribe_states(self._on_state)

    def _on_log(self):
        try:
            data = os.read(self.log_master, 65536)
        except (BlockingIOError, OSError):
            return
        data = self._log_partial + _ANSI.sub(b"", data).replace(b"\r", b"")
        whole, _, self._log_partial = data.rpartition(b"\n")
        if not whole:
            return
        whole += b"\n"
        # Everything goes to the file, up to a limit. In memory only what a scenario searches: at
        # VERY_VERBOSE the raw frames and the DE pin are most of the output, and nothing reads them.
        if self._log_written < LOG_FILE_MAX:
            self._log_file.write(whole)
            self._log_written += len(whole)
        if len(self._log) < LOG_MEMORY_MAX:
            self._log += b"".join(line for line in whole.splitlines(keepends=True)
                                  if b"[VV]" not in line and b"[host:" not in line and b"][V][" not in line)

    def _on_state(self, state):
        ident = self.by_key.get(state.key)
        if ident is None:
            return
        self.states[ident] = state
        self.history.append((self.bus.now(), ident, state))

    async def _stop_process(self):
        proc, self.proc = self.proc, None
        if proc is None:
            return
        if proc.poll() is None:
            proc.terminate()  # the host platform exits its loop on SIGTERM
            for _ in range(60):
                if proc.poll() is not None:
                    break
                await asyncio.sleep(0.05)
            else:
                proc.kill()
                proc.wait()
        self.exit_code = proc.returncode
        _LIVE.discard(proc)

    async def stop(self):
        if self.client is not None:
            try:
                await self.client.disconnect(force=True)
            except Exception:  # noqa: BLE001 - a dead peer must not hide the scenario's own result
                pass
            self.client = None
        await self._stop_process()
        self._on_log()
        loop = asyncio.get_running_loop()
        for fd in (self.bus.master, self.log_master):
            try:
                loop.remove_reader(fd)
            except (ValueError, OSError):
                pass
        self.bus.close()
        for fd in (self.log_master, self.log_slave):
            try:
                os.close(fd)
            except OSError:
                pass
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None

    # -- waiting --
    async def wait(self, pred, timeout: float, step: float = 0.01) -> bool:
        """True as soon as `pred()` holds, False when `timeout` seconds pass first."""
        deadline = time.monotonic() + timeout
        while True:
            if pred():
                return True
            if time.monotonic() >= deadline:
                return False
            await asyncio.sleep(step)

    async def holds(self, pred, seconds: float, step: float = 0.02) -> bool:
        """True when `pred()` held at every look for `seconds`."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if not pred():
                return False
            await asyncio.sleep(step)
        return pred()

    async def next_command(self, since: int, timeout: float = 3.0) -> bytes | None:
        """The first command frame the unit received after position `since` of bus.rx."""
        await self.wait(lambda: self.bus.commands(since), timeout)
        got = self.bus.commands(since)
        return got[0].frame if got else None

    async def quiet_cycles(self, cycles: int = 2, timeout: float = 6.0):
        """Wait for `cycles` further status polls, so that whatever was sent has been answered."""
        polls = lambda: sum(1 for r in self.bus.rx if r.cls == 0x66 and r.sub == 0)  # noqa: E731
        start = polls()
        await self.wait(lambda: polls() >= start + cycles, timeout)

    def mark(self) -> int:
        return len(self.bus.rx)

    # -- reading --
    def state(self, ident: str):
        return self.states.get(ident)

    def value(self, ident: str):
        """The state value Home Assistant would show, None for unknown."""
        s = self.states.get(ident)
        if s is None or getattr(s, "missing_state", False):
            return None
        return s.state

    @property
    def climate(self):
        return self.states["climate.air_conditioner"]

    def key(self, ident: str) -> int:
        return self.info[ident].key

    def log_text(self) -> str:
        """The node's log without the VERBOSE and VERY_VERBOSE lines; node.log has all of it."""
        return self._log.decode(errors="replace")

    def log_count(self, needle: str) -> int:
        return self.log_text().count(needle)

    def log_tail(self, lines: int = 25) -> str:
        return "\n".join("      | " + line for line in self.log_text().splitlines()[-lines:])

    # -- acting, as Home Assistant does --
    def climate_command(self, **kwargs):
        self.client.climate_command(self.key("climate.air_conditioner"), **kwargs)

    def switch(self, object_id: str, on: bool):
        self.client.switch_command(self.key(f"switch.{object_id}"), on)

    def select(self, object_id: str, option: str):
        self.client.select_command(self.key(f"select.{object_id}"), option)


_DOMAINS = {
    "BinarySensorInfo": "binary_sensor",
    "ClimateInfo": "climate",
    "SelectInfo": "select",
    "SensorInfo": "sensor",
    "SwitchInfo": "switch",
    "TextSensorInfo": "text_sensor",
}


def domain_of(info) -> str:
    return _DOMAINS.get(type(info).__name__, type(info).__name__)


def command_frames(node: Node, since: int = 0) -> list[bytes]:
    return [r.frame for r in node.bus.rx[since:] if r.cls == CLASS_COMMAND]
