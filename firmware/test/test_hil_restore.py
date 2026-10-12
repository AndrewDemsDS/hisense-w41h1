#!/usr/bin/env python3
"""hil_esphome_actuation.py: that a real air conditioner is put back as it was found, whatever
stops the run, and that a unit in use is left alone.

The HIL script itself needs a unit and is not part of this suite. This runs its run() against a
stand-in for the ESPHome API client (a unit that does what it is told, at once) with the waits
removed, and stops the steps in every way they can stop: a step that raises, the hard timeout, a
connection that dies. What the unit really does is NOT covered: that is the manual run.
"""

import asyncio
import contextlib
import io
import sys
import types

import edge_harness
from edge_harness import ROOT, check, load_script

OFF, COOL = 0, 2


class Entity:
    def __init__(self, key, name=""):
        self.key, self.name = key, name


class ClimateInfo(Entity): pass       # noqa: E701 - the script tells entities apart by class name
class SwitchInfo(Entity): pass        # noqa: E701
class SelectInfo(Entity): pass        # noqa: E701
class BinarySensorInfo(Entity): pass  # noqa: E701
class SensorInfo(Entity): pass        # noqa: E701


class FakeClient:
    """An ESPHome node with an A/C that obeys every command at once."""
    start_mode = OFF
    fail_after = None      # raise from the command with this number, once
    dead_after = None      # raise from every command from this number on
    last = None

    def __init__(self, *args, **kwargs):
        self.commands, self.cb, self.disconnected = [], None, False
        self.entities = [ClimateInfo(1), SwitchInfo(2, "Panel display"), SwitchInfo(3, "Eco"), SwitchInfo(4, "Turbo"),
                         SwitchInfo(5, "Quiet"), SelectInfo(6, "Sleep profile"), BinarySensorInfo(7, "A/C bus link"),
                         SensorInfo(8, "Command retries"), SensorInfo(9, "Failed commands"), SensorInfo(10, "Unanswered commands")]
        ns = types.SimpleNamespace
        self.state = {1: ns(key=1, mode=self.start_mode, target_temperature=24.0, custom_fan_mode="", fan_mode=2,
                            swing_mode=0, preset=0, custom_preset=""),
                      2: ns(key=2, state=True), 3: ns(key=3, state=False), 4: ns(key=4, state=False), 5: ns(key=5, state=False),
                      6: ns(key=6, state="Off"), 7: ns(key=7, state=True), 8: ns(key=8, state=0), 9: ns(key=9, state=0),
                      10: ns(key=10, state=0)}
        FakeClient.last = self

    async def connect(self, login=True):
        pass

    async def disconnect(self):
        self.disconnected = True

    async def list_entities_services(self):
        return self.entities, []

    def subscribe_states(self, cb):
        self.cb = cb
        for s in self.state.values():
            cb(s)

    def _command(self, kind, key, **fields):
        n = len(self.commands)
        if n == self.fail_after:
            self.commands.append(("raised", key, {}))
            raise RuntimeError("the step broke")
        if self.dead_after is not None and n >= self.dead_after:
            raise ConnectionError("the node is gone")
        self.commands.append((kind, key, fields))
        s = self.state[key]
        for f, v in fields.items():
            if f == "custom_fan_mode":
                builtin = {"auto": 2, "low": 3, "medium": 4, "high": 5}.get(str(v).lower())
                s.fan_mode, s.custom_fan_mode = (builtin, "") if builtin else (s.fan_mode, v)
            elif f == "custom_preset":
                s.custom_preset, s.preset = v, 0
            elif f == "preset":
                s.preset, s.custom_preset = v, ""
            else:
                setattr(s, f, v)
        self.cb(s)

    def climate_command(self, key, **fields):
        self._command("climate", key, **fields)

    def switch_command(self, key, state):
        self._command("switch", key, state=state)

    def select_command(self, key, state):
        self._command("select", key, state=state)


sys.modules["aioesphomeapi"] = types.SimpleNamespace(APIClient=FakeClient)
hil = load_script("hil_esphome_actuation", ROOT / "firmware/test/hil_esphome_actuation.py")
real_sleep = asyncio.sleep
hang_at = [None]           # the number of the wait that never ends, or None


def run(timeout=60.0, hang=None, power_on=False, allow_running=False, **client):
    """One run of the HIL script against a fresh stand-in. Returns (exit code, output, client)."""
    waits = [0]

    async def no_wait(seconds):
        waits[0] += 1
        await real_sleep(3600 if waits[0] == hang else 0)

    hil.APIClient = type("Client", (FakeClient,), client)
    hil.results.clear()
    out = io.StringIO()
    asyncio.sleep = no_wait
    try:
        with contextlib.redirect_stdout(out):
            try:
                code = asyncio.run(hil.run("unit.example", None, power_on, allow_running, timeout))
            except BaseException as e:  # noqa: BLE001 - which one is what the checks look at
                code = type(e).__name__
    finally:
        asyncio.sleep = real_sleep
    return code, out.getvalue(), FakeClient.last


def sent(c, kind, key, **fields):
    return [n for n, (k, at, f) in enumerate(c.commands) if k == kind and at == key and all(f.get(a) == v for a, v in fields.items())]


def verdict(name):
    return next((ok for n, ok, _ in hil.results if n == name), None)


print("== hil_esphome_actuation.py: a unit in use, and the restore ==")

code, out, c = run(start_mode=COOL)
check(code == 2 and not c.commands and "taken to be in use" in out and c.disconnected,
      "a unit that is running is taken to be in use: exit 2, not one command sent")
code, out, c = run(start_mode=COOL, allow_running=True)
check(len(c.commands) > 30 and verdict("the unit is as it was found") is True and c.state[1].mode == COOL and c.state[1].target_temperature == 24.0
      and c.state[1].fan_mode == 2 and c.state[1].custom_fan_mode == "",
      "--allow-running tests it, and at the end it is as it was found: that is read back and checked")
check(c.state[2].state is True and c.state[6].state == "Off" and c.disconnected, "the display switch and the sleep profile are back too")

code, out, c = run()
check(not sent(c, "climate", 1, mode=COOL) and c.state[1].mode == OFF and verdict("the unit is as it was found") is True,
      "a unit found off and no --power-on: it is never switched on, and it is as found at the end")
code, out, c = run(power_on=True)
check(sent(c, "climate", 1, mode=COOL) and sent(c, "climate", 1, mode=OFF) and c.state[1].mode == OFF
      and verdict("the unit is as it was found") is True, "--power-on switches it on for the run and off again")

# Whatever stops the steps, the restore runs.
for at in (0, 1, 5, 12, 20):
    code, out, c = run(power_on=True, fail_after=at)
    check(code == 1 and verdict("the steps ran to the end") is False and c.state[1].mode == OFF and c.state[1].target_temperature == 24.0
          and c.state[2].state is True and c.state[6].state == "Off" and verdict("the unit is as it was found") is True,
          f"a step that raises at command {at}: the unit is switched off again and put back, and the run fails")
code, out, c = run(power_on=True, fail_after=0)
check(sent(c, "climate", 1, mode=OFF), "even when the power-on itself raised, the restore sends the power-off: it cannot know what reached the unit")
for at in (2, 4, 12, 30):          # wait 1 is the connect, before the steps
    code, out, c = run(start_mode=COOL, allow_running=True, hang=at, timeout=0.3)
    check(code == 1 and verdict("the steps finished inside the hard timeout") is False and c.state[1].mode == COOL
          and c.state[1].target_temperature == 24.0 and verdict("the unit is as it was found") is True,
          f"the hard timeout during wait {at}: the steps stop, the unit is put back, the run fails")
code, out, c = run(start_mode=COOL, allow_running=True, dead_after=12)
check(code == 1 and "RESTORE DID NOT FINISH" in out and "It was found as:" in out and verdict("the unit was put back as found") is False
      and c.disconnected, "a node that stops answering: the run fails and prints what the unit was like, so it can be put back by hand")

default = hil.Path(hil.__file__).resolve().parents[1] / "esphome" / "secrets.yaml"
source = (ROOT / "firmware/test/hil_esphome_actuation.py").read_text()
check('default="/repo/' not in source and str(default).endswith("firmware/esphome/secrets.yaml"),
      "the default secrets file is this checkout's, not a path inside a container")

sys.exit(edge_harness.finish("HIL RESTORE"))
