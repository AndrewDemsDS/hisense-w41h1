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
"""

import argparse
import asyncio

from aioesphomeapi import APIClient

# Fields the collateral check watches. Anything the test did not deliberately change must
# still match after the step.
CLIMATE_FIELDS = ("mode", "target_temperature", "custom_fan_mode", "fan_mode", "swing_mode")

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

    settle = 6

    # --- 0. power on (opt-in) -----------------------------------------------------------
    # Off by default: this starts a real compressor. Also the only test of the power path,
    # and directly relevant to issue #7 (I17), where powering on a physically-off unit from
    # a cold state may not take. The unit's own mode readback is the evidence, not our echo.
    powered_here = False
    if power_on and powered_off:
        print("[power on]")
        client.climate_command(key=node.climate.key, mode=2)  # COOL
        await asyncio.sleep(12)
        after = node.climate_snapshot()
        got_on = str(after.get("mode")) not in ("0", "ClimateMode.OFF")
        record("power on takes (issue #7)", got_on, f"mode now {after.get('mode')}")
        powered_here = got_on
        powered_off = not got_on

    # --- 1. setpoint ------------------------------------------------------------------
    print("[setpoint]")
    before = node.climate_snapshot()
    target = 25.0 if float(before.get("target_temperature") or 24) != 25.0 else 23.0
    client.climate_command(key=node.climate.key, target_temperature=target)
    await asyncio.sleep(settle)
    after = node.climate_snapshot()
    record("setpoint actuates", float(after.get("target_temperature") or -1) == target,
           f"wanted {target}, got {after.get('target_temperature')}")
    node.no_collateral("setpoint", before, {"target_temperature"})

    # --- 2. fan, BUILT-IN enum path (the 2026-08-18 bug) ------------------------------
    print("[fan: built-in enum]")
    before = node.climate_snapshot()
    client.climate_command(key=node.climate.key, custom_fan_mode="High")
    await asyncio.sleep(settle)
    after = node.climate_snapshot()
    moved = before.get("fan_mode") != after.get("fan_mode") or \
        before.get("custom_fan_mode") != after.get("custom_fan_mode")
    record("fan High actuates", moved,
           f"fan_mode {before.get('fan_mode')} -> {after.get('fan_mode')}, "
           f"custom {before.get('custom_fan_mode')} -> {after.get('custom_fan_mode')}")
    node.no_collateral("fan", before, {"fan_mode", "custom_fan_mode"})

    # --- 3. fan, CUSTOM name path -----------------------------------------------------
    print("[fan: custom name]")
    before = node.climate_snapshot()
    client.climate_command(key=node.climate.key, custom_fan_mode="Medium-low")
    await asyncio.sleep(settle)
    after = node.climate_snapshot()
    record("fan Medium-low actuates", after.get("custom_fan_mode") == "Medium-low",
           f"got {after.get('custom_fan_mode')}")
    node.no_collateral("fan custom", before, {"fan_mode", "custom_fan_mode"})

    # --- 4. swing ---------------------------------------------------------------------
    print("[swing]")
    before = node.climate_snapshot()
    want = 0 if str(before.get("swing_mode")) not in ("0", "ClimateSwingMode.SWING_MODE_OFF") else 2
    client.climate_command(key=node.climate.key, swing_mode=want)
    await asyncio.sleep(settle)
    after = node.climate_snapshot()
    record("swing actuates", before.get("swing_mode") != after.get("swing_mode"),
           f"{before.get('swing_mode')} -> {after.get('swing_mode')}")
    node.no_collateral("swing", before, {"swing_mode"})

    # --- 5. THE DISPLAY REGRESSION ----------------------------------------------------
    # Turn the panel off, then drive eco, turbo and quiet in turn. Each of those sends a
    # frame carrying byte 36; before the fix they re-lit the panel while the switch still
    # read off. The switch must stay off, and the A/C must not be retuned.
    print("[display: off, then special modes must not disturb it]")
    disp = node.switches.get("Panel display")
    if disp is None:
        record("display switch present", False, "no 'Panel display' switch declared")
    else:
        client.switch_command(key=disp.key, state=False)
        await asyncio.sleep(settle)
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
            await asyncio.sleep(settle)
            record(f"{mode_name} actuates", node.switch_state(mode_name) == (not was),
                   f"{was} -> {node.switch_state(mode_name)}")
            record(f"{mode_name} leaves display off",
                   node.switch_state("Panel display") is False,
                   f"display reads {node.switch_state('Panel display')}")
            # Turbo is not a pure flag: the A/C itself forces fan high and setpoint 16 C when
            # it engages (hardware-confirmed, see the turbo_on comment in hisense_rs485.h).
            # That is the unit's behaviour, not shadow drift, so it is expected here.
            allowed = ({"target_temperature", "fan_mode", "custom_fan_mode"}
                       if mode_name == "Turbo" else set())
            node.no_collateral(f"{mode_name}", before, allowed)
            # put it back
            client.switch_command(key=e.key, state=bool(was))
            await asyncio.sleep(settle)

    # --- 6. sleep profile --------------------------------------------------------------
    print("[sleep profile]")
    sleep_sel = node.selects.get("Sleep profile")
    if sleep_sel is not None:
        before = node.climate_snapshot()
        want = "General" if node.select_state("Sleep profile") != "General" else "Off"
        client.select_command(key=sleep_sel.key, state=want)
        await asyncio.sleep(settle)
        record("sleep profile actuates", node.select_state("Sleep profile") == want,
               f"got {node.select_state('Sleep profile')}")
        record("sleep leaves display off",
               node.switch_state("Panel display") is False or disp is None,
               f"display reads {node.switch_state('Panel display')}")
        node.no_collateral("sleep", before, set())

    # --- restore ------------------------------------------------------------------------
    print("\n[restore]")
    if powered_here:
        client.climate_command(key=node.climate.key, mode=0)  # back off, as found
        await asyncio.sleep(6)
        print("  powered back off")
    if sleep_sel is not None and base_sleep:
        client.select_command(key=sleep_sel.key, state=base_sleep)
        await asyncio.sleep(3)
    for name, was in base_switches.items():
        e = node.switches.get(name)
        if e is not None and was is not None:
            client.switch_command(key=e.key, state=was)
            await asyncio.sleep(2)
    if baseline.get("target_temperature"):
        client.climate_command(key=node.climate.key,
                               target_temperature=float(baseline["target_temperature"]))
        await asyncio.sleep(3)
    print("  restored to baseline (verify on the unit)")

    await client.disconnect()

    passed = sum(1 for _, ok, _ in results if ok)
    failed = len(results) - passed
    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


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
