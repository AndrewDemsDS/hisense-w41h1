#!/usr/bin/env python3
"""HIL: ESPHome actuation + no-collateral regression (needs a real A/C).

The ESPHome counterpart of hil_display_actuation.py, which drives Matter nodes through
matter-server. This one drives the node over the ESPHome native API, so it needs the A/C
online and the node adopted.

    python3 hil_esphome_actuation.py <host> [--key <noise_psk>]

NOT part of run_tests.sh: that suite is deliberately hardware-free.

Two properties per control, the same pair the Matter HIL script checks:

1. ACTUATION. The command lands AND the entity really transitions. A command matching the
   current value is a no-op that proves nothing, so every step forces a genuine change.

2. NO COLLATERAL. Every control except quiet and sleep rides the COMBINED command frame,
   which rebuilds mode + setpoint + fan + swing + display from the command shadow. If the
   shadow drifts, driving one control retunes the A/C. That is a real bug, and it is the
   class the 2026-08-19 display report belongs to: eco/turbo/quiet were re-lighting a panel
   the user had switched off, because byte 36 rides every frame and 0x00 means "on", not
   "leave alone".

The panel itself is NOT verified here. The A/C reports no live display state, so whether the
photons changed is a human observation. This proves the command path and the absence of side
effects; a person at the unit proves the panel.

State is snapshotted at the start and restored at the end, so a real unit is left as found.

CONFIRM AND RETRY. The firmware re-sends a command the unit did not answer or did not apply, and
holds the entity on the request while it does. A step that reads back during a re-send would pass
on the echo of its own command, so every wait watches the node's `Command retries` counter and
waits longer when it moved. At the end the run reports the three counters: retries are the firmware
doing its job and are listed, a rise of `Failed commands` is a failed check.
"""

import argparse
import asyncio
import time

from aioesphomeapi import APIClient

# Fields the collateral check watches. Anything the test did not deliberately change must
# still match after the step.
# esphome ClimateFanMode values for the built-in names the ladder uses
FAN_ENUM = {"auto": 2, "low": 3, "medium": 4, "high": 5}
CLIMATE_FIELDS = ("mode", "target_temperature", "custom_fan_mode", "fan_mode", "swing_mode")

# Names of the diagnostic counters in w41h1.yaml.
RETRIES = "Command retries"
FAILED = "Failed commands"
UNANSWERED = "Unanswered commands"

results: list[tuple[str, bool, str]] = []


def record(name: str, ok: bool, detail: str = "") -> None:
    results.append((name, ok, detail))
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}{(' -- ' + detail) if detail else ''}")


class Node:
    def __init__(self, client: APIClient) -> None:
        self.client = client
        self.by_key: dict[int, object] = {}
        self.states: dict[int, object] = {}
        self.climate = None
        self.switches: dict[str, object] = {}
        self.selects: dict[str, object] = {}
        self.binary_sensors: dict[str, object] = {}
        self.sensors: dict[str, object] = {}

    async def connect(self) -> None:
        entities, _ = await self.client.list_entities_services()
        for e in entities:
            self.by_key[e.key] = e
            kind = type(e).__name__
            name = getattr(e, "name", "")
            if kind.startswith("Climate"):
                self.climate = e
            elif kind.startswith("Switch"):
                self.switches[name] = e
            elif kind.startswith("Select"):
                self.selects[name] = e
            elif kind.startswith("BinarySensor"):
                self.binary_sensors[name] = e
            elif kind.startswith("Sensor"):
                self.sensors[name] = e
        self.client.subscribe_states(lambda s: self.states.__setitem__(s.key, s))
        await asyncio.sleep(3)

    def climate_snapshot(self) -> dict:
        s = self.states.get(self.climate.key)
        if s is None:
            return {}
        snap = {}
        for f in CLIMATE_FIELDS:
            v = getattr(s, f, None)
            snap[f] = str(v) if v is not None else None
        return snap

    def bus_link(self) -> bool | None:
        """The bus-link binary sensor, or None when the node does not declare one."""
        for name, e in self.binary_sensors.items():
            if "bus link" in name.lower():
                s = self.states.get(e.key)
                return None if s is None else bool(getattr(s, "state", False))
        return None

    def counter(self, name: str) -> int | None:
        """A diagnostic counter sensor by name, or None when the node does not declare it."""
        e = self.sensors.get(name)
        if e is None:
            return None
        s = self.states.get(e.key)
        try:
            return int(getattr(s, "state", None))
        except (TypeError, ValueError):
            return None

    async def settle(self, seconds: float, extra: float) -> None:
        """Wait `seconds`, then `extra` more for each re-send the firmware made meanwhile.

        During a re-send the entity still shows the request, so a readback proves nothing until the
        firmware has either confirmed the command or given up on it. Up to three extra waits: the
        firmware makes at most two re-sends of one command.
        """
        seen = self.counter(RETRIES)
        await asyncio.sleep(seconds)
        for _ in range(3):
            now = self.counter(RETRIES)
            if seen is None or now is None or now == seen:
                return
            print(f"    (firmware sent a command again: retries {seen} -> {now}, waiting {extra:.0f} s more)")
            seen = now
            await asyncio.sleep(extra)

    def mode(self) -> int | None:
        """The climate mode as ESPHome's ClimateMode int (0 off, 2 cool, 3 heat, ...)."""
        s = self.states.get(self.climate.key)
        try:
            return int(getattr(s, "mode", None))
        except (TypeError, ValueError):
            return None

    def fan_is(self, name: str) -> bool:
        """True when the entity shows exactly the ladder step `name`."""
        snap = self.climate_snapshot()
        if name in FAN_ENUM:
            return snap.get("fan_mode") == str(FAN_ENUM[name]) and not snap.get("custom_fan_mode")
        return snap.get("custom_fan_mode") == name

    def switch_state(self, name: str) -> bool | None:
        e = self.switches.get(name)
        if e is None:
            return None
        s = self.states.get(e.key)
        return None if s is None else bool(getattr(s, "state", False))

    def select_state(self, name: str) -> str | None:
        e = self.selects.get(name)
        if e is None:
            return None
        s = self.states.get(e.key)
        return None if s is None else getattr(s, "state", None)

    def no_collateral(self, label: str, before: dict, allowed: set[str]) -> None:
        after = self.climate_snapshot()
        drifted = [
            f"{f}: {before.get(f)} -> {after.get(f)}"
            for f in CLIMATE_FIELDS
            if f not in allowed and before.get(f) != after.get(f)
        ]
        record(f"{label}: no collateral change", not drifted, "; ".join(drifted))


# Phases in run order with rough durations (s), for the progress bar. The run takes ~12 minutes,
# mostly waiting out the firmware's 10 s special-mode pacing, and is silent while it waits.
PHASES = [
    ("prepare", 35), ("setpoint", 7), ("fan: built-in enum", 7), ("fan: custom name", 7), ("swing", 7),
    ("fan ladder", 42), ("mode sweep", 35), ("display + special modes", 103),
    ("sleep profile", 16), ("presets", 245), ("restore", 31),
]
_T0 = time.monotonic()


def phase(name: str) -> None:
    """Print a progress bar line for the phase about to start: done/total weighted by time."""
    names = [n for n, _ in PHASES]
    idx = names.index(name) if name in names else 0
    total = sum(d for _, d in PHASES)
    done = sum(d for _, d in PHASES[:idx])
    width = 30
    filled = round(width * done / total)
    elapsed = int(time.monotonic() - _T0)
    left = max(0, total - done)
    print(f"\n[{'#' * filled}{'-' * (width - filled)}] {round(100 * done / total):3d}%  "
          f"{idx + 1}/{len(PHASES)} {name}  elapsed {elapsed // 60}m{elapsed % 60:02d}s, "
          f"~{left // 60}m{left % 60:02d}s left", flush=True)


async def run(host: str, key: str | None, power_on: bool) -> int:
    client = APIClient(host, 6053, None, noise_psk=key)
    await client.connect(login=True)
    node = Node(client)
    await node.connect()
    if node.climate is None:
        print("no climate entity: is this the A/C node?")
        return 1

    baseline = node.climate_snapshot()
    base_switches = {n: node.switch_state(n) for n in node.switches}
    base_sleep = node.select_state("Sleep profile")
    print(f"baseline: {baseline}")
    print(f"switches: {base_switches}  sleep: {base_sleep}")
    # A powered-off A/C accepts a setpoint but ignores fan, quiet and sleep, so those steps
    # report a false failure. Say so up front rather than leaving it to be re-derived.
    powered_off = str(baseline.get("mode")) in ("0", "ClimateMode.OFF")
    if powered_off:
        print("  NOTE: unit reads OFF. fan / quiet / sleep are expected to be no-ops;\n"
              "        rerun with the A/C running to exercise them.\n")
    else:
        print()

    # Every readback below is the entity, and the entity shows the REQUEST for the firmware's 4 s
    # hold-off. Only a status frame after that replaces it with what the unit does. With the bus
    # link down no such frame arrives, so every step would pass on the echo of its own command.
    link = node.bus_link()
    if link is None:
        print("  NOTE: no bus-link binary sensor declared; a dead bus cannot be told from a pass.")
    else:
        record("bus link is up before the run", link, f"bus link reads {link}")
        if not link:
            print("\n0 steps run: the unit is not answering, so no readback would mean anything.")
            await client.disconnect()
            return 1

    # 4 s hold-off, plus up to ~1.7 s for the command to reach its slot in the 1 s bus cycle and
    # for the poll after it. 6 s left almost nothing over; 7 s leaves a full cycle.
    settle = 7
    # A re-send restarts the 4 s check, a cycle after the verdict.
    retry_extra = 6
    # Eco / turbo / quiet / sleep go through the firmware's paced special-mode queue: each write
    # waits 10 s after the previous one (the A/C swallows faster ones), then ~4 s until its status
    # check, plus a cycle.
    special_settle = 16
    # A special-mode re-send waits out the 10 s pacing again, then its own 4 s check.
    special_retry_extra = 16

    counters_start = {n: node.counter(n) for n in (UNANSWERED, RETRIES, FAILED)}
    if counters_start[RETRIES] is None or counters_start[FAILED] is None:
        print("  NOTE: no 'Command retries' / 'Failed commands' sensors declared; a step read during a\n"
              "        re-send cannot be told from a pass. Add them from w41h1.yaml.")

    # --- 0. power on (opt-in) -----------------------------------------------------------
    # Off by default: this starts a real compressor. Also the only test of the power path,
    # and directly relevant to issue #7 (I17), where powering on a physically-off unit from
    # a cold state may not take. The unit's own mode readback is the evidence, not our echo.
    powered_here = False
    if power_on and powered_off:
        print("[power on]")
        client.climate_command(key=node.climate.key, mode=2)  # COOL
        await node.settle(12, retry_extra)
        after = node.climate_snapshot()
        got_on = str(after.get("mode")) not in ("0", "ClimateMode.OFF")
        record("power on takes (issue #7)", got_on, f"mode now {after.get('mode')}")
        powered_here = got_on
        powered_off = not got_on

    # Special modes pin the fan (quiet/sleep low, turbo high) and the firmware refuses other
    # speeds while they do, so start from no special mode. The baseline preset is put back at
    # the end through the climate entity, which is one ordered plan rather than switch writes.
    phase("prepare")
    base_preset = _preset_name(node.states.get(node.climate.key))
    if base_preset not in ("", "none"):
        client.climate_command(key=node.climate.key, preset=0)
        await asyncio.sleep(35)
        print(f"  cleared baseline preset {base_preset} for the run")

    # --- 1. setpoint ------------------------------------------------------------------
    phase("setpoint")
    before = node.climate_snapshot()
    if not powered_off and node.mode() not in (2, 3):
        # The unit takes a setpoint in cool and heat only, and the firmware refuses it elsewhere.
        print(f"  SKIP setpoint: mode {node.mode()} takes none (cool and heat only)")
    else:
        target = 25.0 if float(before.get("target_temperature") or 24) != 25.0 else 23.0
        client.climate_command(key=node.climate.key, target_temperature=target)
        await node.settle(settle, retry_extra)
        after = node.climate_snapshot()
        record("setpoint actuates", float(after.get("target_temperature") or -1) == target,
               f"wanted {target}, got {after.get('target_temperature')}")
        node.no_collateral("setpoint", before, {"target_temperature"})

    # --- 2. fan, BUILT-IN enum path (the 2026-08-18 bug) ------------------------------
    phase("fan: built-in enum")
    before = node.climate_snapshot()
    # A command matching the current value proves nothing, so pick a step that is not active.
    enum_step = "Medium" if before.get("fan_mode") == str(FAN_ENUM["high"]) else "High"
    client.climate_command(key=node.climate.key, custom_fan_mode=enum_step)
    await node.settle(settle, retry_extra)
    after = node.climate_snapshot()
    # "It moved" is not enough: a unit that went to some other speed did not obey.
    record(f"fan {enum_step} actuates", node.fan_is(enum_step.lower()),
           f"fan_mode {before.get('fan_mode')} -> {after.get('fan_mode')}, "
           f"custom {before.get('custom_fan_mode')} -> {after.get('custom_fan_mode')}")
    node.no_collateral("fan", before, {"fan_mode", "custom_fan_mode"})

    # --- 3. fan, CUSTOM name path -----------------------------------------------------
    phase("fan: custom name")
    before = node.climate_snapshot()
    client.climate_command(key=node.climate.key, custom_fan_mode="medium_low")
    await node.settle(settle, retry_extra)
    after = node.climate_snapshot()
    record("fan medium_low actuates", after.get("custom_fan_mode") == "medium_low",
           f"got {after.get('custom_fan_mode')}")
    node.no_collateral("fan custom", before, {"fan_mode", "custom_fan_mode"})

    # --- 4. swing ---------------------------------------------------------------------
    phase("swing")
    before = node.climate_snapshot()
    want = 0 if str(before.get("swing_mode")) not in ("0", "ClimateSwingMode.SWING_MODE_OFF") else 2
    client.climate_command(key=node.climate.key, swing_mode=want)
    await node.settle(settle, retry_extra)
    after = node.climate_snapshot()
    record("swing actuates", before.get("swing_mode") != after.get("swing_mode"),
           f"{before.get('swing_mode')} -> {after.get('swing_mode')}")
    node.no_collateral("swing", before, {"swing_mode"})

    # --- 4b. FULL FAN LADDER ----------------------------------------------------------
    # All six advertised steps, not just a sample. Four arrive as ESPHome's built-in enum and two
    # stay custom, and the split is invisible to the caller -- which is exactly why the enum path
    # went unnoticed when it was broken. Quiet is the `quiet` preset, not a fan mode.
    phase("fan ladder")
    for name in ("auto", "low", "medium_low", "medium", "medium_high", "high"):
        before = node.climate_snapshot()
        client.climate_command(key=node.climate.key, custom_fan_mode=name)
        await node.settle(settle, retry_extra)
        after = node.climate_snapshot()
        record(f"fan {name}", node.fan_is(name),
               f"fan_mode={after.get('fan_mode')} custom={after.get('custom_fan_mode')}")
        node.no_collateral(f"fan {name}", before, {"fan_mode", "custom_fan_mode"})

    # --- 4c. MODE SWEEP ----------------------------------------------------------------
    # HEAT is deliberately skipped: it would heat the room to prove a mapping the host tests
    # already cover. Cool / dry / fan-only / auto exercise the same code path.
    phase("mode sweep")
    # A mode write powers the unit on. Without --power-on that must not happen, and the sweep
    # used to start the compressor on a unit found off and leave it running in auto.
    base_mode = node.mode()
    sweep = () if powered_off else (("cool", 2), ("dry", 5), ("fan_only", 4), ("auto", 6))
    if powered_off:
        print("  SKIP mode sweep: unit is off (use --power-on to run it)")
    for label, mode_val in sweep:
        before = node.climate_snapshot()
        client.climate_command(key=node.climate.key, mode=mode_val)
        await node.settle(settle, retry_extra)
        after = node.climate_snapshot()
        record(f"mode {label}", str(after.get("mode")) == str(mode_val),
               f"wanted {mode_val}, got {after.get('mode')}")
        # A mode change legitimately moves the action, and dry/turbo may move the fan.
        node.no_collateral(f"mode {label}", before,
                           {"mode", "fan_mode", "custom_fan_mode", "target_temperature"})
    # Put the mode back now: the sweep ends in auto, and the steps below (and the unit's owner)
    # expect the mode the run started in. Heat is not re-entered by the test.
    if sweep and base_mode not in (None, 0, 3, 6):
        client.climate_command(key=node.climate.key, mode=base_mode)
        await node.settle(settle, retry_extra)
        record("mode restored after the sweep", node.mode() == base_mode,
               f"wanted {base_mode}, got {node.mode()}")
    elif sweep and base_mode == 3:
        print("  NOTE: the run started in heat, which the test does not re-enter. Unit left in auto.")

    # --- 5. THE DISPLAY REGRESSION ----------------------------------------------------
    # Turn the panel off, then drive eco, turbo and quiet in turn. Each of those sends a
    # frame carrying byte 36; before the fix they re-lit the panel while the switch still
    # read off. The switch must stay off, and the A/C must not be retuned.
    phase("display + special modes")
    disp = node.switches.get("Panel display")
    if disp is None:
        record("display switch present", False, "no 'Panel display' switch declared")
    else:
        client.switch_command(key=disp.key, state=False)
        await node.settle(settle, retry_extra)
        record("display switches off", node.switch_state("Panel display") is False,
               f"switch reads {node.switch_state('Panel display')}")

        for mode_name in ("Eco", "Turbo", "Quiet"):
            e = node.switches.get(mode_name)
            if e is None:
                record(f"{mode_name} present", False, "not declared")
                continue
            before = node.climate_snapshot()
            was = node.switch_state(mode_name)
            client.switch_command(key=e.key, state=not was)
            await node.settle(special_settle, special_retry_extra)
            record(f"{mode_name} actuates", node.switch_state(mode_name) == (not was),
                   f"{was} -> {node.switch_state(mode_name)}")
            record(f"{mode_name} leaves display off",
                   node.switch_state("Panel display") is False,
                   f"display reads {node.switch_state('Panel display')}")
            # Turbo is not a pure flag: the A/C itself forces fan high and setpoint 16 C when
            # it engages (hardware-confirmed, see the turbo_on comment in hisense_rs485.h).
            # That is the unit's behaviour, not shadow drift, so it is expected here.
            # Turbo and Quiet are not pure flags: the A/C forces fan high + setpoint 16 C for
            # turbo, and drops the fan to quiet for mute (both hardware-confirmed in
            # hisense_rs485.h). Those are the unit's behaviour, not shadow drift.
            if mode_name == "Turbo":
                allowed = {"target_temperature", "fan_mode", "custom_fan_mode", "mode"}
            elif mode_name == "Quiet":
                allowed = {"fan_mode", "custom_fan_mode"}
            else:
                allowed = set()
            node.no_collateral(f"{mode_name}", before, allowed)
            # put it back, and let turbo's forced cool / 16 C land before the next mode's snapshot
            client.switch_command(key=e.key, state=bool(was))
            await node.settle(special_settle, special_retry_extra)

    # --- 6. sleep profile --------------------------------------------------------------
    phase("sleep profile")
    sleep_sel = node.selects.get("Sleep profile")
    if sleep_sel is not None:
        before = node.climate_snapshot()
        want = "General" if node.select_state("Sleep profile") != "General" else "Off"
        client.select_command(key=sleep_sel.key, state=want)
        await node.settle(special_settle, special_retry_extra)
        record("sleep profile actuates", node.select_state("Sleep profile") == want,
               f"got {node.select_state('Sleep profile')}")
        record("sleep leaves display off",
               node.switch_state("Panel display") is False or disp is None,
               f"display reads {node.switch_state('Panel display')}")
        # A sleep profile owns the fan and drops it to its low profile (docs/05 "Special
        # functions"), so a fan change is the unit's behaviour, not shadow drift.
        node.no_collateral("sleep", before, {"fan_mode", "custom_fan_mode"})

    # --- 6. climate presets ------------------------------------------------------------
    # The special modes as climate presets, named like hisense-unified-ac. One preset can take
    # several writes 10 s apart (the A/C swallows a faster special-mode command), so each step
    # waits out the longest plan. The order walks through every kind of transition: set, combine,
    # replace eco with turbo, turbo to sleep, add eco under a running profile, sleep to quiet.
    phase("presets")
    # The longest plan here is three writes: 10 s pacing between them, 4 s check after the last.
    preset_wait = 35.0
    for name in ("eco", "eco_quiet", "turbo", "sleep_general", "eco_sleep_general", "quiet", "none"):
        if name in ("none", "eco"):
            client.climate_command(key=node.climate.key, preset=0 if name == "none" else 5)
        else:
            client.climate_command(key=node.climate.key, custom_preset=name)
        await node.settle(preset_wait, special_retry_extra)
        got = _preset_name(node.states.get(node.climate.key))
        record(f"preset {name}", got == name, f"got {got}")

    # --- restore ------------------------------------------------------------------------
    phase("restore")
    if powered_here:
        client.climate_command(key=node.climate.key, mode=0)  # back off, as found
        await asyncio.sleep(6)
        print("  powered back off")
    if sleep_sel is not None and base_sleep:
        client.select_command(key=sleep_sel.key, state=base_sleep)
        await asyncio.sleep(3)
    if base_swing := baseline.get("swing_mode"):
        client.climate_command(key=node.climate.key, swing_mode=int(base_swing))
        await asyncio.sleep(3)
    # Special modes come back as ONE preset plan. Restoring them switch by switch replayed eco on
    # and then turbo off, and byte33's neutral turbo-off write cleared eco again.
    if base_preset not in ("", "none"):
        if base_preset == "eco":
            client.climate_command(key=node.climate.key, preset=5)
        else:
            client.climate_command(key=node.climate.key, custom_preset=base_preset)
        print(f"  preset {base_preset} queued (settles over ~30 s)")
    else:
        client.climate_command(key=node.climate.key, preset=0)
    for name, was in base_switches.items():
        if name in ("Eco", "Turbo", "Quiet"):
            continue
        e = node.switches.get(name)
        if e is not None and was is not None:
            client.switch_command(key=e.key, state=was)
            await asyncio.sleep(2)
    # Turbo forces cool at 16 C and the unit stays in cool once turbo is off, so the mode can
    # have moved again since the sweep put it back.
    # Not when this run switched the unit on: it is off again by now, and base_mode is the mode it
    # came up in, so "restoring" it would switch the unit back on and leave it running.
    if not powered_off and not powered_here and base_mode not in (None, 0, 3) and node.mode() != base_mode:
        client.climate_command(key=node.climate.key, mode=base_mode)
        await asyncio.sleep(settle)
    if baseline.get("target_temperature") and (powered_off or node.mode() in (2, 3)):
        client.climate_command(key=node.climate.key,
                               target_temperature=float(baseline["target_temperature"]))
        await asyncio.sleep(3)
    link = node.bus_link()
    if link is not None:
        record("bus link still up after the run", link, f"bus link reads {link}")
    # What the firmware had to do to get there. Retries are listed, not failed: a re-send that
    # lands is the fix working. A command the unit never took is a failure even if every readback
    # above happened to pass.
    await asyncio.sleep(special_retry_extra)  # let the restore finish its own checks
    counters_end = {n: node.counter(n) for n in counters_start}
    delta = {n: (None if counters_start[n] is None or counters_end[n] is None
                 else counters_end[n] - counters_start[n]) for n in counters_start}
    print(f"  counters over the run: unanswered +{delta[UNANSWERED]}, retries +{delta[RETRIES]}, "
          f"failed +{delta[FAILED]}")
    if delta[FAILED] is not None:
        record("no command was given up on", delta[FAILED] == 0,
               f"'Failed commands' rose by {delta[FAILED]} (see the node's WARN log for which)")
    print("  restored to baseline (verify on the unit)")

    await client.disconnect()

    passed = sum(1 for _, ok, _ in results if ok)
    failed = len(results) - passed
    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


def _preset_name(state) -> str:
    """The climate preset as its Home Assistant name. Built-ins arrive as enum ints."""
    if state is None:
        return ""
    custom = getattr(state, "custom_preset", "") or ""
    if custom:
        return custom
    builtin = getattr(state, "preset", None)
    try:
        return {0: "none", 5: "eco"}.get(int(builtin), str(builtin))
    except (TypeError, ValueError):
        text = str(builtin)
        return "eco" if text.endswith("ECO") else "none" if text.endswith("NONE") else text


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--key", default=None, help="API encryption key (noise_psk)")
    ap.add_argument("--secrets", default="/repo/firmware/esphome/secrets.yaml")
    ap.add_argument("--power-on", action="store_true",
                    help="power the A/C on for the run if it is off, then restore. Starts a real "
                         "compressor; also the only test of the power path (issue #7).")
    args = ap.parse_args()

    key = args.key
    if key is None:
        try:
            for line in open(args.secrets, encoding="utf-8"):
                if line.strip().startswith("hisense_ac__encryption_key:"):
                    key = line.split(":", 1)[1].strip().strip('"').strip("'")
        except OSError:
            pass

    raise SystemExit(asyncio.run(run(args.host, key, args.power_on)))


if __name__ == "__main__":
    main()
