"""The scenarios. Each one starts its own node (host binary, A/C model, API client), so they run
side by side and none depends on what another left behind.

Every check states what Home Assistant sees over the native API or which bytes reached the A/C.
Nothing reads the hub's internals: its timers are the real ones, and the only clock is the wall.

The firmware's timing, as the scenarios lean on it (hisense_bus.h, hisense_map.h, hisense_ac.h):
  1 s      one bus cycle: heartbeat, queued commands, status poll
  3.9 s    after a command left the wire, the first status that may confirm or refute it
  4 s      hold-off: a status frame does not move an entity the user just moved
  10 s     between two special-mode writes (eco, turbo, quiet, sleep)
  2 / 1    re-sends of a command the status did not show: main / special-mode
  5        status polls in a row without an answer before the link counts as lost
  10 s     after boot, and after a setup request, during which the "77" pulse is not taken
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass
import json
import math
import pathlib

from aioesphomeapi import (
    ClimateAction as Action,
    ClimateFanMode as Fan,
    ClimateMode as Mode,
    ClimatePreset as Preset,
    ClimateSwingMode as Swing,
)

from ac_model import CLASS_DEVTYPE, CLASS_LINK, CLASS_STATUS, SUB_PRODUCT, Unit, seal, unstuff
from harness import Node, combined_frame, command_frames, domain_of, power_frame, single_field_frame

HERE = pathlib.Path(__file__).resolve().parent
GOLDEN = HERE / "entities.golden.json"

CLIMATE = "climate.air_conditioner"
COUNTERS = ("bus_checksum_errors", "bus_reply_timeouts", "unanswered_commands", "command_retries",
            "failed_commands", "bus_link_losses")
SLEEP_OPTIONS = ["Off", "General", "Old", "Young", "Kids"]


@dataclass
class Scenario:
    name: str
    fn: object
    seconds: float  # the usual run time, for the summary and the timeout
    full: bool  # only with --full
    doc: str


SCENARIOS: list[Scenario] = []


def scenario(seconds: float, full: bool = False):
    def register(fn):
        SCENARIOS.append(Scenario(fn.__name__, fn, seconds, full, (fn.__doc__ or "").strip().splitlines()[0]))
        return fn

    return register


# ---- Shared pieces ---------------------------------------------------------------------------------
def counters(node: Node) -> dict:
    out = {}
    for name in COUNTERS:
        v = node.value(f"sensor.{name}")
        out[name] = None if v is None else int(v)
    return out


async def expect_counters(ctx, node: Node, want: dict, what: str, timeout: float = 4.0):
    """The counters go out with the next status frame, so they are waited for."""
    await node.wait(lambda: all(counters(node)[k] == v for k, v in want.items()), timeout)
    got = {k: counters(node)[k] for k in want}
    ctx.r.check(got == want, what, f"got {got}, expected {want}")


async def act_and_expect(ctx, node: Node, what: str, act, want: bytes | None, shows=None, timeout: float = 3.0):
    """Do `act` as Home Assistant would, then compare the next command frame the unit received.
    `want` None: no frame may follow."""
    mark = node.mark()
    act()
    if want is None:
        await node.quiet_cycles(2)
        got = command_frames(node, mark)
        ctx.r.check(not got, f"{what}: nothing is sent", f"{len(got)} frame(s): {got[0].hex(' ') if got else ''}")
    else:
        ctx.r.frame(await node.next_command(mark, timeout), want, f"{what}: the frame on the wire")
    if shows is not None:
        ctx.r.check(await node.wait(shows, 1.5), f"{what}: the entity shows it at once", repr(node.climate))
    return mark


def entity_record(info) -> dict:
    """What Home Assistant builds an entity from."""
    rec = {
        "domain": domain_of(info),
        "object_id": info.object_id,
        "name": info.name,
        "icon": info.icon,
        "entity_category": info.entity_category.name.lower(),
        "disabled_by_default": info.disabled_by_default,
    }
    for attr in ("device_class", "unit_of_measurement", "accuracy_decimals"):
        if hasattr(info, attr):
            rec[attr] = getattr(info, attr)
    if hasattr(info, "state_class"):
        rec["state_class"] = info.state_class.name.lower()
    if hasattr(info, "options"):
        rec["options"] = list(info.options)
    if rec["domain"] == "climate":
        rec["modes"] = [m.name.lower() for m in info.supported_modes]
        rec["fan_modes"] = [m.name.lower() for m in info.supported_fan_modes]
        rec["custom_fan_modes"] = list(info.supported_custom_fan_modes)
        rec["swing_modes"] = [m.name.lower() for m in info.supported_swing_modes]
        rec["presets"] = [m.name.lower() for m in info.supported_presets]
        rec["custom_presets"] = list(info.supported_custom_presets)
        rec["visual"] = [info.visual_min_temperature, info.visual_max_temperature,
                         info.visual_target_temperature_step, info.visual_current_temperature_step]
        rec["current_temperature"] = info.supports_current_temperature
        rec["action"] = info.supports_action
    return rec


def entity_list(node: Node) -> dict:
    return {
        "device": {"name": node.device_info.name, "friendly_name": node.device_info.friendly_name},
        "entities": [entity_record(node.info[k]) for k in sorted(node.info)],
    }


def polls(node: Node, cls: int, sub: int | None = None, since: int = 0):
    return [r for r in node.bus.rx[since:] if r.cls == cls and (sub is None or r.sub == sub)]


# ---- Boot ------------------------------------------------------------------------------------------
@scenario(8)
async def boot(ctx):
    """Link up, device type learned, first status decoded, every entity has a state, golden entity list."""
    r = ctx.r
    unit = Unit()
    unit.caps = {"cool_heat", "fan_mute"}
    node = await ctx.node(unit)
    rx = node.bus.rx

    r.equal(rx[0].frame.hex(" "), "f4 f5 00 40 0b 00 00 00 00 fe 01 00 00 0a 04 00 01 58 f4 fb",
            "the first frame is the DevType probe, with the pre-link 00 00 at bytes 7 and 8")
    r.equal(rx[1].frame.hex(" "), "f4 f5 00 40 0b 00 00 01 02 fe 01 00 00 07 01 00 01 55 f4 fb",
            "the 0x07 frame follows, stamped with the device type the unit answered (01 02)")
    await node.quiet_cycles(3)
    beats = polls(node, CLASS_LINK)
    r.check(beats[0].frame[16] == 0xF0 and all(b.frame[16] == 0xB0 for b in beats[1:]),
            "heartbeat byte 16 is F0 until the unit's 0x1E was heard, B0 after",
            " ".join(f"{b.frame[16]:02X}" for b in beats))
    r.check(all(b.frame[17] == 0x80 for b in beats), "heartbeat byte 17 is 80: no setup indication")
    r.check(all(x.frame[7:9] == b"\x01\x02" for x in rx[1:]), "every later frame carries the device type")
    r.equal(len(polls(node, CLASS_STATUS, SUB_PRODUCT)), 1, "the ProductType poll went out once")
    status = polls(node, CLASS_STATUS, 0)
    gaps = [b.at - a.at for a, b in zip(status, status[1:])]
    r.check(all(0.9 <= g <= 1.6 for g in gaps), "status polls are one second apart",
            " ".join(f"{g:.2f}" for g in gaps))

    got = entity_list(node)
    if ctx.update_golden:
        GOLDEN.write_text(json.dumps(got, indent=2) + "\n")
        r.note(f"wrote {GOLDEN.name}")
    want = json.loads(GOLDEN.read_text()) if GOLDEN.exists() else {"device": {}, "entities": []}
    r.equal(got["device"], want["device"], "device name and friendly name match the golden list")
    got_by = {f"{e['domain']}.{e['object_id']}": e for e in got["entities"]}
    want_by = {f"{e['domain']}.{e['object_id']}": e for e in want["entities"]}
    r.check(sorted(got_by) == sorted(want_by), "the entities are the golden list's, no more and no fewer",
            f"added {sorted(set(got_by) - set(want_by))}, missing {sorted(set(want_by) - set(got_by))}")
    changed = [f"{k}: {field} is {got_by[k].get(field)!r}, golden {want_by[k].get(field)!r}"
               for k in sorted(set(got_by) & set(want_by))
               for field in sorted(set(got_by[k]) | set(want_by[k]))
               if got_by[k].get(field) != want_by[k].get(field)]
    r.check(not changed, f"all {len(want_by)} entities match the golden list in every field", "; ".join(changed))

    await node.wait(lambda: node.value("text_sensor.ac_device_type") is not None, 3)
    # The setup request counter is the test's own and has nothing to show before a request.
    silent = [k for k in sorted(node.info) if k != "sensor.e2e_setup_requests"
              and (k not in node.states or getattr(node.states[k], "missing_state", False))]
    r.check(not silent, "every declared entity has published a state", f"none from {silent}")

    c = node.climate
    r.equal((c.mode, c.action, c.current_temperature, c.target_temperature, c.fan_mode, c.swing_mode, c.preset,
             c.custom_fan_mode, c.custom_preset),
            (Mode.COOL, Action.COOLING, 27.0, 24.0, Fan.AUTO, Swing.OFF, Preset.NONE, "", ""),
            "climate: cool, cooling, 27 in the room, target 24, fan auto, no swing, no preset")
    r.equal({k: node.value(f"sensor.{k}") for k in ("outdoor_temperature", "coil_temperature",
                                                     "compressor_frequency", "power", "voltage")},
            {"outdoor_temperature": 33.0, "coil_temperature": 41.0, "compressor_frequency": 45.0, "power": 1660.0,
             "voltage": 228.0}, "sensors: outdoor 33, coil 41, compressor 45 Hz, 1660 W, 228 V")
    r.check(abs(node.value("sensor.current") - 1660 / (228 * 0.95)) < 0.01, "current is power / (voltage * 0.95)",
            str(node.value("sensor.current")))
    r.equal(counters(node), dict.fromkeys(COUNTERS, 0), "all six bus counters read 0")
    r.equal({k: node.value(f"binary_sensor.{k}") for k in ("ac_bus_link", "aux_heat_relay", "fault")},
            {"ac_bus_link": True, "aux_heat_relay": False, "fault": False}, "bus link on, aux heat off, no fault")
    r.equal({k: node.value(f"binary_sensor.capability_{k}") for k in ("heat_pump", "eco", "quiet")},
            {"heat_pump": True, "eco": False, "quiet": True},
            "capabilities as the unit advertised them: heat pump and quiet, no eco")
    r.equal(node.value("text_sensor.ac_device_type"), "01 02", "the device type sensor reads 01 02")
    r.equal({k: node.value(f"switch.{k}") for k in ("eco", "turbo", "quiet", "panel_display", "beeper")},
            {"eco": False, "turbo": False, "quiet": False, "panel_display": True, "beeper": True},
            "switches: special modes off, display and beeper on")
    r.equal(node.value("select.sleep_profile"), "Off", "sleep profile: Off")
    r.equal(node.value("sensor.energy_today"), 0.0, "energy today starts at 0")
    r.equal(command_frames(node), [], "no command frame was sent by a node nobody touched")


@scenario(12)
async def telemetry(ctx):
    """Readings follow the status frame; the -20 C placeholder pair is published as unknown."""
    r = ctx.r
    unit = Unit()
    unit.outdoor, unit.coil = -20, -20  # 0xEC in both bytes: "not measured yet"
    node = await ctx.node(unit)
    await node.quiet_cycles(2)
    r.check(node.state("sensor.outdoor_temperature") is not None and node.value("sensor.outdoor_temperature") is not None
            and math.isnan(node.value("sensor.outdoor_temperature")) and math.isnan(node.value("sensor.coil_temperature")),
            "outdoor and coil are unknown while both bytes hold the placeholder",
            f"{node.value('sensor.outdoor_temperature')}, {node.value('sensor.coil_temperature')}")
    unit.outdoor, unit.coil = -20, 5
    r.check(await node.wait(lambda: node.value("sensor.outdoor_temperature") == -20.0
                            and node.value("sensor.coil_temperature") == 5.0, 3),
            "a lone -20 C is a reading: outdoor -20, coil 5")
    unit.outdoor, unit.coil = -20, -20
    await node.quiet_cycles(2)
    r.equal((node.value("sensor.outdoor_temperature"), node.value("sensor.coil_temperature")), (-20.0, -20.0),
            "once a real pair was seen the placeholder value is trusted as -20 C")

    # A power byte no split unit means: 4.15 * 40^2 = 6640 W, above the default max_power of 5 kW.
    unit.current = 40
    r.check(await node.wait(lambda: math.isnan(node.value("sensor.power"))
                            and math.isnan(node.value("sensor.current")), 3),
            "an estimate above max_power: power and current are unknown, not 6640 W",
            f"{node.value('sensor.power')}, {node.value('sensor.current')}")
    seen = len(node.history)
    await node.quiet_cycles(3)
    again = [k for _, k, _ in node.history[seen:] if k in ("sensor.power", "sensor.current")]
    r.check(not again, "the unknown is published once, not with every status frame", str(again))
    r.equal(node.value("sensor.voltage"), 228.0, "the voltage is a reading all the while")
    energy = node.value("sensor.energy_today")
    r.check(energy is not None and energy < 0.01, "energy today took nothing from the 6640 W", str(energy))
    unit.current = 20
    r.check(await node.wait(lambda: node.value("sensor.power") == 1660.0, 3), "the next plausible frame reads 1660 W")

    unit.indoor, unit.comp, unit.current, unit.voltage, unit.aux_heat = 19, 0, 0, 0, True
    r.check(await node.wait(lambda: node.climate.current_temperature == 19.0 and node.climate.action == Action.IDLE, 3),
            "compressor at 0 Hz: the climate action is idle and the room reads 19")
    r.check(await node.wait(lambda: node.value("sensor.power") == 0.0 and node.value("sensor.current") == 0.0
                            and node.value("sensor.voltage") == 0.0
                            and node.value("sensor.compressor_frequency") == 0.0, 3),
            "power, current, voltage and frequency read 0 (no division by the zero voltage)")
    r.check(await node.wait(lambda: node.value("binary_sensor.aux_heat_relay") is True, 3), "aux heat relay on")

    # What the unit does on its own shows without a command: the remote moved it.
    unit.mode, unit.setpoint, unit.fan, unit.vswing, unit.comp = "heat", 28, "med-high", True, 30
    r.check(await node.wait(lambda: (node.climate.mode, node.climate.target_temperature, node.climate.custom_fan_mode,
                                     node.climate.swing_mode, node.climate.action)
                            == (Mode.HEAT, 28.0, "medium_high", Swing.VERTICAL, Action.HEATING), 3),
            "a change at the remote shows: heat, 28, fan medium_high, vertical swing, heating", repr(node.climate))
    unit.mode, unit.comp = "dry", 20
    r.check(await node.wait(lambda: (node.climate.mode, node.climate.action) == (Mode.DRY, Action.DRYING), 3),
            "dry with the compressor running: action drying")
    unit.mode = "fan"
    r.check(await node.wait(lambda: (node.climate.mode, node.climate.action) == (Mode.FAN_ONLY, Action.FAN), 3),
            "fan only: action fan")
    unit.mode = "auto"
    r.check(await node.wait(lambda: node.climate.mode == Mode.AUTO, 3), "auto (status nibble 5) shows as auto")
    unit.status_patch = {18: (7 << 4) | 0x08}
    await node.quiet_cycles(2)
    r.equal(node.climate.mode, Mode.AUTO, "a status mode nibble with no meaning (7) leaves the mode shown as it was")
    unit.status_patch = {}
    unit.eco, unit.sleep, unit.mute = True, 2, False
    r.check(await node.wait(lambda: node.value("switch.eco") is True and node.value("select.sleep_profile") == "Old"
                            and node.climate.custom_preset == "eco_sleep_old", 3),
            "eco and sleep profile 2 from the remote: eco switch on, select Old, preset eco_sleep_old",
            f"{node.value('switch.eco')}, {node.value('select.sleep_profile')}, {node.climate.custom_preset!r}")
    unit.eco, unit.sleep, unit.power = False, 0, False
    r.check(await node.wait(lambda: (node.climate.mode, node.climate.action, node.climate.preset)
                            == (Mode.OFF, Action.OFF, Preset.NONE), 3), "switched off at the remote: off, action off")
    r.equal(command_frames(node), [], "none of this made the node send a command")


@scenario(10)
async def faults(ctx):
    """Fault bytes from the unit reach the problem sensor and the per-bit sensors."""
    r = ctx.r
    unit = Unit()
    unit.caps = set()
    node = await ctx.node(unit)
    names = {"indoor_temp_sensor": False, "condensate_tray_full": False, "indoor_to_outdoor_comms": False,
             "outdoor_temp_sensor": False}

    def shown():
        return {"fault": node.value("binary_sensor.fault"),
                **{k: node.value(f"binary_sensor.fault_{k}") for k in names}}

    async def expect(want_any, want_bits, what):
        want = {"fault": want_any, **names, **want_bits}
        await node.wait(lambda: shown() == want, 3)
        r.check(shown() == want, what, f"got {shown()}")

    await expect(False, {}, "a healthy unit: no fault")
    # Was a known failure, found by this test: the fault log tested the bitmap, whose valid bit
    # (bit 31) is always set, where it meant AcFaults::any.
    r.equal(node.log_count("A/C reports a fault"), 0, "a healthy unit is not logged as reporting a fault")
    await node.wait(lambda: node.value("binary_sensor.capability_heat_pump") is not None, 3)
    r.equal({k: node.value(f"binary_sensor.capability_{k}") for k in ("heat_pump", "eco", "quiet")},
            {"heat_pump": False, "eco": False, "quiet": False}, "a unit that advertises nothing: every capability off")
    unit.faults = {39: 0x80 | 0x10, 64: 0x08}
    await expect(True, {"indoor_temp_sensor": True, "condensate_tray_full": True, "outdoor_temp_sensor": True},
                 "byte 39 bits 7 and 4, byte 64 bit 3: indoor temp, tray full and outdoor temp faults")
    r.check(await node.wait(lambda: node.log_count("A/C reports a fault: bitmap 0xC0010009") == 1, 2),
            "the fault is logged once, as bitmap 0xC0010009 (valid, any, bits 16, 3 and 0)")
    unit.faults = {39: 0x01}
    await expect(True, {"indoor_to_outdoor_comms": True}, "byte 39 bit 0: indoor to outdoor comms, the others clear")
    unit.faults = {40: 0x01}
    await expect(True, {}, "a fault bit with no name (byte 40 bit 0) still raises the problem sensor")
    unit.faults = {66: 0x80}
    await expect(False, {}, "byte 66 bit 7 is the 8 C frost guard, not a fault")
    unit.faults = {66: 0x10}
    await expect(True, {}, "byte 66 bit 4 (over temperature) raises the problem sensor")
    unit.faults = {}
    await expect(False, {}, "all clear again")
    await node.quiet_cycles(2)
    r.equal(node.log_count("A/C fault cleared"), 2, "each of the two clearings is logged as 'A/C fault cleared'")
    r.equal(node.log_count("bitmap 0x80000000"), 0, "and never as a fault with an empty bitmap")
    logged = {b: node.log_count(f"A/C reports a fault: bitmap 0x{b:08X}") for b in (0xC0010009, 0xC0000080, 0xC0000000,
                                                                                    0xC0020000)}
    r.check(all(n == 1 for n in logged.values()), "each of the four fault patterns was logged once, with its bitmap",
            str({f"0x{b:08X}": n for b, n in logged.items()}))


# ---- Climate ---------------------------------------------------------------------------------------
@scenario(16)
async def climate_modes(ctx):
    """Every mode, off and on again: the frame on the wire and what the entity shows."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit
    cc = node.climate_command
    for mode, name in ((Mode.HEAT, "heat"), (Mode.DRY, "dry"), (Mode.FAN_ONLY, "fan_only"), (Mode.AUTO, "auto"),
                       (Mode.COOL, "cool")):
        await act_and_expect(ctx, node, f"mode {name}", lambda mode=mode: cc(mode=mode), combined_frame(name, 24),
                             lambda mode=mode: node.climate.mode == mode)
    await act_and_expect(ctx, node, "mode off", lambda: cc(mode=Mode.OFF), power_frame(False),
                         lambda: node.climate.mode == Mode.OFF)
    r.check(await node.wait(lambda: not unit.power, 1), "the unit is off")
    # Straight after: the status still reads "on" for a poll, and the unit ignores a mode while off.
    await act_and_expect(ctx, node, "mode heat on a unit just switched off", lambda: cc(mode=Mode.HEAT),
                         combined_frame("heat", 24, power_on=True), lambda: node.climate.mode == Mode.HEAT)
    r.check(await node.wait(lambda: unit.power and unit.mode == "heat", 1),
            "power and mode travelled in one frame (byte 18 = 3C): the unit is on, in heat")
    await asyncio.sleep(5.5)
    r.equal(len(command_frames(node)), 7, "seven requests, seven frames: nothing was sent twice")
    r.equal((node.climate.mode, node.climate.target_temperature, node.climate.action),
            (Mode.HEAT, 24.0, Action.HEATING), "after the hold-off the entity agrees with the unit: heat, 24, heating")
    await expect_counters(ctx, node, {"command_retries": 0, "failed_commands": 0, "unanswered_commands": 0},
                          "no retries, no failed command")

    await act_and_expect(ctx, node, "mode off again", lambda: cc(mode=Mode.OFF), power_frame(False))
    await asyncio.sleep(5.5)
    r.equal((node.climate.mode, node.climate.action), (Mode.OFF, Action.OFF), "off, confirmed by the status: action off")
    # Off for good now: the status says so, no power intent is pending.
    await act_and_expect(ctx, node, "mode cool from off", lambda: cc(mode=Mode.COOL),
                         combined_frame("cool", 24, power_on=True), lambda: node.climate.mode == Mode.COOL)
    await act_and_expect(ctx, node, "mode cool again while it runs", lambda: cc(mode=Mode.COOL),
                         combined_frame("cool", 24), None)
    await act_and_expect(ctx, node, "heat_cool, a mode the entity does not offer", lambda: cc(mode=Mode.HEAT_COOL),
                         None)
    r.equal(node.climate.mode, Mode.COOL, "and the entity stays on cool")


@scenario(30)
async def climate_values(ctx):
    """Setpoint (rounding and limits), all seven fan steps, swing, several fields in one call."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit
    cc = node.climate_command
    c = lambda: node.climate  # noqa: E731

    for asked, sent in ((22.0, 22), (22.5, 23), (21.4, 21), (16.0, 16), (32.0, 32), (3.0, 16), (45.0, 32), (24.0, 24)):
        await act_and_expect(ctx, node, f"target {asked:g}", lambda asked=asked: cc(target_temperature=asked),
                             combined_frame("cool", sent), lambda sent=sent: c().target_temperature == float(sent))
    r.equal(unit.setpoint, 24, "the unit ends on 24")

    for mode, custom, name in ((Fan.LOW, None, "low"), (None, "medium_low", "medium_low"), (Fan.MEDIUM, None, "medium"),
                               (None, "medium_high", "medium_high"), (Fan.HIGH, None, "high"), (Fan.AUTO, None, "auto")):
        def shows(mode=mode, custom=custom):
            return c().custom_fan_mode == custom if custom else (c().fan_mode == mode and c().custom_fan_mode == "")
        await act_and_expect(ctx, node, f"fan {name}",
                             lambda mode=mode, custom=custom: cc(fan_mode=mode, custom_fan_mode=custom),
                             combined_frame("cool", 24, fan=name), shows)
    await act_and_expect(ctx, node, "fan quiet, which is a preset and not an advertised fan mode",
                         lambda: cc(fan_mode=Fan.QUIET), None)
    await act_and_expect(ctx, node, "a custom fan mode that does not exist", lambda: cc(custom_fan_mode="warp"), None)

    await act_and_expect(ctx, node, "swing vertical", lambda: cc(swing_mode=Swing.VERTICAL),
                         combined_frame("cool", 24, vswing=True), lambda: c().swing_mode == Swing.VERTICAL)
    await act_and_expect(ctx, node, "swing horizontal, which the package does not offer",
                         lambda: cc(swing_mode=Swing.HORIZONTAL), None)
    await act_and_expect(ctx, node, "swing off", lambda: cc(swing_mode=Swing.OFF), combined_frame("cool", 24),
                         lambda: c().swing_mode == Swing.OFF)

    await act_and_expect(ctx, node, "heat, 26, fan high and vertical swing in one call",
                         lambda: cc(mode=Mode.HEAT, target_temperature=26.0, fan_mode=Fan.HIGH,
                                    swing_mode=Swing.VERTICAL),
                         combined_frame("heat", 26, fan="high", vswing=True),
                         lambda: (c().mode, c().target_temperature, c().fan_mode, c().swing_mode)
                         == (Mode.HEAT, 26.0, Fan.HIGH, Swing.VERTICAL))
    r.equal((unit.mode, unit.setpoint, unit.fan, unit.vswing), ("heat", 26, "high", True), "and the unit took all four")

    # In auto the unit keeps its own setpoint (measured 2026-10-09), so the entity refuses one.
    await act_and_expect(ctx, node, "mode auto", lambda: cc(mode=Mode.AUTO),
                         combined_frame("auto", 26, fan="high", vswing=True), lambda: c().mode == Mode.AUTO)
    await act_and_expect(ctx, node, "a target in auto", lambda: cc(target_temperature=20.0), None)
    r.equal(c().target_temperature, 26.0, "and the target shown stays 26")
    r.check("Target temperature refused" in node.log_text(), "the refusal is logged")
    await act_and_expect(ctx, node, "mode dry", lambda: cc(mode=Mode.DRY), combined_frame("dry", 26, vswing=True),
                         lambda: c().mode == Mode.DRY)
    await act_and_expect(ctx, node, "fan low in dry: the frame carries no fan (byte 16 = 01)",
                         lambda: cc(fan_mode=Fan.LOW), combined_frame("dry", 26, vswing=True))

    await asyncio.sleep(5.5)
    r.equal((c().mode, c().swing_mode), (Mode.DRY, Swing.VERTICAL),
            "after the hold-off the entity agrees with the unit: dry, vertical swing")
    # PINNED, and it looks wrong: the request is acknowledged, nothing on the wire carries it, and
    # the entity goes back about four seconds later. A target in auto is refused for that reason.
    r.equal(c().fan_mode, Fan.HIGH, "the fan asked for in dry was shown for the hold-off and is back on high")
    await expect_counters(ctx, node, {"command_retries": 0, "failed_commands": 0, "unanswered_commands": 0},
                          "no retries, no failed command")


@scenario(10)
async def fahrenheit(ctx):
    """A panel set to Fahrenheit: the status is converted, the command carries degrees F."""
    r = ctx.r
    unit = Unit()
    unit.status_patch = {26: 0x02, 19: 75, 20: 81}  # byte 26 bit 1: the panel shows F

    def panel(f):
        if f[19] & 1:
            unit.status_patch[19] = (f[19] - 1) // 2
        return True

    unit.on_command = panel
    node = await ctx.node(unit)
    r.equal((node.climate.target_temperature, node.climate.current_temperature), (24.0, 27.0),
            "75 F and 81 F on the wire show as 24 and 27 C")
    want = bytearray(combined_frame("cool", 24))
    want[19] = 72 * 2 + 1  # 22 C is 71.6 F, sent as 72
    await act_and_expect(ctx, node, "target 22 C", lambda: node.climate_command(target_temperature=22.0),
                         unstuff(seal(want)), lambda: node.climate.target_temperature == 22.0)
    await asyncio.sleep(5.5)
    r.equal(node.climate.target_temperature, 22.0, "72 F read back is 22 C: the entity stays on 22")
    r.equal(len(command_frames(node)), 1, "and the command was not sent again")
    await expect_counters(ctx, node, {"command_retries": 0, "failed_commands": 0}, "no retries, no failed command")


# ---- Switches, the sleep select, presets ------------------------------------------------------------
def paced(ctx, frames, what):
    gaps = [b.at - a.at for a, b in zip(frames, frames[1:])]
    # 10 s from one frame being queued to the next: each then waits for its cycle's command slot, so
    # on the wire it is 10 s give or take one bus cycle.
    ctx.r.check(all(g >= 8.9 for g in gaps), f"{what}: special-mode frames are 10 s apart, give or take a bus cycle",
                " ".join(f"{g:.1f}" for g in gaps))


@scenario(42)
async def switch_eco_turbo(ctx):
    """Eco and turbo switches: the byte 33 values, the 10 s pacing, the other switch following."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit
    sw = lambda k: node.value(f"switch.{k}")  # noqa: E731
    start = node.mark()

    await act_and_expect(ctx, node, "eco on", lambda: node.switch("eco", True), combined_frame("cool", 24, feature="eco"))
    r.check(await node.wait(lambda: sw("eco") is True, 1), "eco on: the switch shows it at once")
    r.check(await node.wait(lambda: node.climate.preset == Preset.ECO, 7), "the climate preset follows: eco")
    # Asked for at once, sent when the 10 s since the last special-mode write are over.
    await act_and_expect(ctx, node, "eco off", lambda: node.switch("eco", False),
                         combined_frame("cool", 24, feature="eco_off"), timeout=12)
    r.equal(sw("eco"), False, "eco off: shown while the frame waits its turn")
    await act_and_expect(ctx, node, "turbo on", lambda: node.switch("turbo", True),
                         combined_frame("cool", 24, feature="turbo"), timeout=12)
    r.check(await node.wait(lambda: node.climate.custom_preset == "turbo" and node.climate.fan_mode == Fan.HIGH, 7),
            "the climate shows preset turbo and the fan the unit forces (high)", repr(node.climate))
    await act_and_expect(ctx, node, "a fan speed while turbo holds the fan", lambda: node.climate_command(fan_mode=Fan.LOW),
                         None)
    r.check("Fan change refused" in node.log_text(), "the refusal is logged")
    # The status reported the fan on high while turbo held it, and the shadow followed: the frame
    # that ends turbo states high as the fan to keep.
    await act_and_expect(ctx, node, "turbo off", lambda: node.switch("turbo", False),
                         combined_frame("cool", 24, fan="high", feature="none"), timeout=12)
    await asyncio.sleep(5.5)
    r.equal((sw("eco"), sw("turbo"), node.climate.preset, node.climate.custom_preset, unit.eco, unit.turbo),
            (False, False, Preset.NONE, "", False, False), "all settled: both off on the unit and in Home Assistant")
    frames = node.bus.commands(start)
    r.equal(len(frames), 4, "four requests, four frames")
    paced(ctx, frames, "eco, eco off, turbo, turbo off")
    await expect_counters(ctx, node, {"command_retries": 0, "failed_commands": 0}, "no retries, no failed command")


@scenario(48)
async def switch_quiet_sleep_display_beeper(ctx):
    """Quiet switch, sleep select, display and beeper switches."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit
    start = node.mark()

    await act_and_expect(ctx, node, "quiet on", lambda: node.switch("quiet", True), single_field_frame(35, 0x30))
    r.check(await node.wait(lambda: node.value("switch.quiet") is True, 1), "quiet on: the switch shows it at once")
    r.check(await node.wait(lambda: node.climate.custom_preset == "quiet" and node.climate.fan_mode == Fan.LOW, 7),
            "the climate shows preset quiet, and the unit's quiet fan step as low", repr(node.climate))

    # The display preference is not a special mode: it goes out at once, on a combined frame.
    await act_and_expect(ctx, node, "display off", lambda: node.switch("panel_display", False),
                         combined_frame("cool", 24, display=False))
    r.check(await node.wait(lambda: node.value("switch.panel_display") is False and unit.display is False, 1),
            "display off: shown, and the unit took it")
    # The beeper is a setting of the node: nothing is sent, the next frames carry the bit clear.
    await act_and_expect(ctx, node, "beeper off", lambda: node.switch("beeper", False), None)
    r.equal(node.value("switch.beeper"), False, "beeper off: shown")
    await act_and_expect(ctx, node, "display on, with the beeper off", lambda: node.switch("panel_display", True),
                         combined_frame("cool", 24, beep=False))
    await asyncio.sleep(5.0)
    r.equal(node.value("switch.panel_display"), True, "the display switch holds its value through status frames")

    await act_and_expect(ctx, node, "quiet off", lambda: node.switch("quiet", False),
                         single_field_frame(35, 0x10, beep=False), timeout=12)
    await act_and_expect(ctx, node, "sleep profile General", lambda: node.select("sleep_profile", "General"),
                         single_field_frame(17, 3, beep=False), timeout=12)
    r.equal(node.value("select.sleep_profile"), "General", "the select shows General")
    r.check(await node.wait(lambda: node.climate.custom_preset == "sleep_general", 7),
            "the climate preset follows: sleep_general")
    await act_and_expect(ctx, node, "sleep profile Kids", lambda: node.select("sleep_profile", "Kids"),
                         single_field_frame(17, 9, beep=False), timeout=12)
    node.switch("beeper", True)
    await act_and_expect(ctx, node, "sleep profile Off, with the beeper back on",
                         lambda: node.select("sleep_profile", "Off"), single_field_frame(17, 1), timeout=12)
    await act_and_expect(ctx, node, "a sleep option that does not exist", lambda: node.select("sleep_profile", "Nap"),
                         None)
    await asyncio.sleep(5.5)
    r.equal((node.value("switch.quiet"), node.value("select.sleep_profile"), node.climate.preset, unit.mute, unit.sleep),
            (False, "Off", Preset.NONE, False, 0), "all settled: quiet off, sleep off, on the unit too")
    special = [f for f in node.bus.commands(start) if f.frame[18] == 0]
    r.equal(len(special), 5, "five special-mode requests, five frames")
    paced(ctx, special, "quiet, quiet off, General, Kids, Off")
    await expect_counters(ctx, node, {"command_retries": 0, "failed_commands": 0}, "no retries, no failed command")


async def run_presets(ctx, node, steps):
    """`steps`: (what to ask, name shown, frames it takes in order). The preset shown must never
    be anything but the one asked for while its frames are on their way."""
    r = ctx.r
    start = node.mark()
    for ask, shown, frames in steps:
        t0 = node.bus.now()
        mark = node.mark()
        node.climate_command(**ask)

        def is_shown(shown=shown):
            c = node.climate
            return c.custom_preset == shown if shown not in ("none", "eco") else (
                c.custom_preset == "" and c.preset == (Preset.ECO if shown == "eco" else Preset.NONE))

        r.check(await node.wait(is_shown, 1.5), f"preset {shown}: shown at once", repr(node.climate))
        await node.wait(lambda: len(node.bus.commands(mark)) >= len(frames), 12.0 * len(frames) + 3)
        r.frames([x.frame for x in node.bus.commands(mark)], frames, f"preset {shown}: {len(frames)} frame(s), in order")
        await asyncio.sleep(5.5)
        seen = [(c.preset, c.custom_preset) for t, k, c in node.history if k == CLIMATE and t >= t0]
        r.check(is_shown() and len(set(seen)) == 1, f"preset {shown}: never showed an intermediate preset",
                str(sorted({p.name if not cp else cp for p, cp in seen})))
    paced(ctx, node.bus.commands(start), "all preset frames")
    await expect_counters(ctx, node, {"command_retries": 0, "failed_commands": 0}, "no retries, no failed command")


@scenario(70)
async def presets(ctx):
    """Climate presets: the plan of frames each one takes, and that the entity shows only the target."""
    node = await ctx.node()
    await run_presets(ctx, node, [
        ({"preset": Preset.ECO}, "eco", [combined_frame("cool", 24, feature="eco")]),
        # One byte 33 write switches eco to turbo.
        ({"custom_preset": "turbo"}, "turbo", [combined_frame("cool", 24, feature="turbo")]),
        # Byte 33 to eco, then the profile. The fan is high from here on: the status reported it
        # so while turbo held it, and the command shadow followed.
        ({"custom_preset": "eco_sleep_general"}, "eco_sleep_general",
         [combined_frame("cool", 24, fan="high", feature="eco"), single_field_frame(17, 3)]),
        # Sleep off first, then the explicit eco clear.
        ({"preset": Preset.NONE}, "none",
         [single_field_frame(17, 1), combined_frame("cool", 24, fan="high", feature="eco_off")]),
    ])
    ctx.r.equal((node.unit.eco, node.unit.turbo, node.unit.mute, node.unit.sleep), (False, False, False, 0),
                "the unit ends with every special mode off")


@scenario(200, full=True)
async def presets_all(ctx):
    """Every preset the entity offers, from none and back."""
    node = await ctx.node()
    eco = combined_frame("cool", 24, feature="eco")
    eco_off = combined_frame("cool", 24, feature="eco_off")
    mute_on, mute_off = single_field_frame(35, 0x30), single_field_frame(35, 0x10)
    sleep = lambda n: single_field_frame(17, n * 2 + 1)  # noqa: E731
    await run_presets(ctx, node, [
        ({"custom_preset": "quiet"}, "quiet", [mute_on]),
        ({"custom_preset": "eco_quiet"}, "eco_quiet", [eco]),
        ({"custom_preset": "sleep_old"}, "sleep_old", [eco_off, mute_off, sleep(2)]),
        ({"custom_preset": "sleep_young"}, "sleep_young", [sleep(3)]),
        ({"custom_preset": "eco_sleep_kids"}, "eco_sleep_kids", [eco, sleep(4)]),
        ({"custom_preset": "eco_sleep_old"}, "eco_sleep_old", [sleep(2)]),
        ({"custom_preset": "eco_sleep_young"}, "eco_sleep_young", [sleep(3)]),
        ({"custom_preset": "sleep_kids"}, "sleep_kids", [eco_off, sleep(4)]),
        ({"custom_preset": "sleep_general"}, "sleep_general", [sleep(1)]),
        ({"custom_preset": "turbo"}, "turbo", [sleep(0), combined_frame("cool", 24, feature="turbo")]),
        # With the fan on high, where the status had it while turbo held it.
        ({"preset": Preset.NONE}, "none", [combined_frame("cool", 24, fan="high", feature="none")]),
    ])
    await act_and_expect(ctx, node, "a preset that does not exist", lambda: node.climate_command(custom_preset="party"),
                         None)


# ---- Hold-off, confirm and retry --------------------------------------------------------------------
@scenario(12)
async def holdoff(ctx):
    """A status that contradicts a command does not move the entity for 4 s, and does after."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit

    def remote_wins(f):
        unit.setpoint = 26  # the unit does not take 22: someone at the remote set 26 instead
        return True

    unit.on_command = remote_wins
    mark = await act_and_expect(ctx, node, "target 22", lambda: node.climate_command(target_temperature=22.0),
                                combined_frame("cool", 22), lambda: node.climate.target_temperature == 22.0)
    sent = node.bus.commands(mark)[0].at
    unit.indoor = 30
    r.check(await node.wait(lambda: node.climate.current_temperature == 30.0, 2.5),
            "inside the hold-off the room temperature still follows the status")
    r.equal(node.climate.target_temperature, 22.0, "while the target stays on what was asked, against a status of 26")
    r.check(await node.holds(lambda: node.climate.target_temperature == 22.0, sent + 3.7 - node.bus.now()),
            "it stays there until 3.7 s after the frame")
    r.check(await node.wait(lambda: node.climate.target_temperature == 26.0, 3.0),
            "then it shows what the unit reports: 26", repr(node.climate))
    flip = next(t for t, k, s in node.history if k == CLIMATE and t > sent and s.target_temperature == 26.0)
    r.check(3.9 <= flip - sent <= 6.2, "the change came between 3.9 and 6.2 s after the frame", f"{flip - sent:.2f} s")
    r.check("Unit was changed from elsewhere" in node.log_text(),
            "a third value is the remote's doing: logged, and the command is not sent again")
    await node.quiet_cycles(3)
    r.equal(len(command_frames(node)), 1, "one frame in all")
    await expect_counters(ctx, node, {"command_retries": 0, "failed_commands": 0},
                          "which counts as neither a retry nor a failed command")

    # The same for a switch: a status frame from before the request cannot flip it back.
    unit.on_command = None
    unit.ignore_commands = -1
    mark = node.mark()
    node.switch("quiet", True)
    r.check(await node.wait(lambda: node.value("switch.quiet") is True, 1), "quiet on: shown at once")
    r.check(await node.holds(lambda: node.value("switch.quiet") is True, 3.5),
            "and held for 3.5 s against a status that says off")


@scenario(36)
async def retry_main(ctx):
    """The unit answers a command and does not apply it: two re-sends, then the entity reverts."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit
    c = lambda: node.climate  # noqa: E731
    want = combined_frame("cool", 22)

    unit.ignore_commands = -1
    t0 = node.bus.now()
    mark = await act_and_expect(ctx, node, "target 22 to a unit that ignores it",
                                lambda: node.climate_command(target_temperature=22.0), want,
                                lambda: c().target_temperature == 22.0)
    r.check(await node.wait(lambda: counters(node)["failed_commands"] == 1, 20), "the command is given up on")
    gave_up = node.bus.now()
    frames = node.bus.commands(mark)
    r.check(len(frames) == 3 and all(f.frame == want for f in frames), "three identical frames: the send and two re-sends",
            f"{len(frames)} frame(s)")
    gaps = [b.at - a.at for a, b in zip(frames, frames[1:])]
    r.check(all(3.9 <= g <= 6.0 for g in gaps), "each re-send follows the last by the 3.9 s settle and one status",
            " ".join(f"{g:.2f}" for g in gaps))
    r.check(12.0 <= gave_up - frames[0].at <= 16.0, "given up 12 to 16 s after the first frame",
            f"{gave_up - frames[0].at:.1f} s")
    shown = [s.target_temperature for t, k, s in node.history if k == CLIMATE and t0 < t < gave_up - 0.3]
    r.check(set(shown) == {22.0}, "all that time the entity showed 22, without a flicker", str(sorted(set(shown))))
    r.check(await node.wait(lambda: c().target_temperature == 24.0, 2), "then it reverts to what the unit reports: 24")
    await expect_counters(ctx, node, {"command_retries": 2, "failed_commands": 1, "unanswered_commands": 0},
                          "counters: 2 retries, 1 failed command, none unanswered")
    r.check("Command not taken by the unit after 2 re-send(s)" in node.log_text(), "the give-up is logged")
    await node.quiet_cycles(3)
    r.equal(len(node.bus.commands(mark)), 3, "and nothing more is sent")

    unit.ignore_commands = 1
    mark = await act_and_expect(ctx, node, "target 21, lost once", lambda: node.climate_command(target_temperature=21.0),
                                combined_frame("cool", 21))
    r.check(await node.wait(lambda: unit.setpoint == 21, 7), "the re-send lands: the unit is on 21")
    await asyncio.sleep(5.5)
    r.equal(len(node.bus.commands(mark)), 2, "two frames")
    r.equal(c().target_temperature, 21.0, "the entity never left 21")
    await expect_counters(ctx, node, {"command_retries": 3, "failed_commands": 1}, "counters: one more retry, no new failure")
    r.check("Command taken by the unit after 1 re-send(s)" in node.log_text(), "taken on a re-send: logged")

    unit.ignore_commands = 1
    mark = await act_and_expect(ctx, node, "off, lost once", lambda: node.climate_command(mode=Mode.OFF),
                                power_frame(False))
    r.check(await node.wait(lambda: not unit.power, 7), "the re-send lands: the unit is off")
    r.frames([f.frame for f in node.bus.commands(mark)], [power_frame(False)] * 2, "the literal power-off frame, twice")
    await asyncio.sleep(5.0)

    # First send: power and mode in one frame. The re-send goes back to the pair of frames.
    unit.ignore_commands = 1
    mark = await act_and_expect(ctx, node, "cool from off, lost once", lambda: node.climate_command(mode=Mode.COOL),
                                combined_frame("cool", 21, power_on=True))
    r.check(await node.wait(lambda: unit.power and unit.mode == "cool", 7), "the re-send lands: the unit is on, in cool")
    await node.wait(lambda: len(node.bus.commands(mark)) >= 3, 3)
    r.frames([f.frame for f in node.bus.commands(mark)],
             [combined_frame("cool", 21, power_on=True), power_frame(True), combined_frame("cool", 21)],
             "re-sent as the literal power-on frame followed by the plain combined frame")
    await asyncio.sleep(5.0)
    r.equal((c().mode, c().target_temperature), (Mode.COOL, 21.0), "the entity shows cool, 21")
    await expect_counters(ctx, node, {"command_retries": 5, "failed_commands": 1}, "counters: 5 retries, still 1 failure")


@scenario(14)
async def retry_unrelated_frame(ctx):
    """An unrelated frame leaving the queue must not restart the wait of a command already sent."""
    r = ctx.r
    node = await ctx.node()
    node.unit.ignore_commands = -1
    mark = await act_and_expect(ctx, node, "target 22 to a unit that ignores it",
                                lambda: node.climate_command(target_temperature=22.0), combined_frame("cool", 22))
    first = node.bus.commands(mark)[0].at
    await asyncio.sleep(max(0.0, first + 2.5 - node.bus.now()))
    node.switch("panel_display", False)  # an unchecked frame of its own, 2.5 s into the wait
    display = combined_frame("cool", 22, display=False)
    await node.wait(lambda: len(node.bus.commands(mark)) >= 3, 10)
    frames = node.bus.commands(mark)
    r.check(len(frames) >= 3 and frames[1].frame == display and frames[2].frame == display,
            "the display frame goes out, and the re-send of the setpoint follows", f"{len(frames)} frame(s)")
    if len(frames) >= 3:
        # Was a known failure (static-analysis W3) until the command tracker credited a finished
        # frame to its own command only: the re-send used to come 3.9 s after the display frame.
        gap = frames[2].at - first
        r.check(3.9 <= gap < 5.5, "the re-send comes 3.9 s and one status after the command, whatever else was sent",
                f"{gap:.1f} s")


@scenario(24)
async def retry_special(ctx):
    """A special-mode write the unit ignores: one re-send after the pacing, then the switch reverts."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit
    want = combined_frame("cool", 24, feature="eco")
    unit.ignore_commands = -1
    t0 = node.bus.now()
    mark = await act_and_expect(ctx, node, "eco on to a unit that ignores it", lambda: node.switch("eco", True), want)
    r.check(await node.wait(lambda: counters(node)["failed_commands"] == 1, 24), "the write is given up on")
    gave_up = node.bus.now()
    frames = node.bus.commands(mark)
    r.check(len(frames) == 2 and frames[1].frame == want, "two identical frames: the send and one re-send",
            f"{len(frames)} frame(s)")
    paced(ctx, frames, "the re-send")
    shown = [s.state for t, k, s in node.history if k == "switch.eco" and t0 < t < gave_up - 0.3]
    r.check(set(shown) == {True}, "all that time the switch showed on", str(shown))
    r.check(await node.wait(lambda: node.value("switch.eco") is False, 6), "then it reverts to off")
    await expect_counters(ctx, node, {"command_retries": 1, "failed_commands": 1}, "counters: 1 retry, 1 failed command")
    r.check("Special-mode command not taken by the unit after 1 re-send(s)" in node.log_text(), "the give-up is logged")


@scenario(9)
async def deaf_unit(ctx):
    """A command frame that gets no reply is sent again in the next cycles, three sends in all."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit
    want = combined_frame("cool", 22)
    unit.deaf_commands = 2
    mark = await act_and_expect(ctx, node, "target 22, not heard twice",
                                lambda: node.climate_command(target_temperature=22.0), want)
    r.check(await node.wait(lambda: unit.setpoint == 22, 4), "the third send is heard: the unit is on 22")
    frames = node.bus.commands(mark)
    r.check(len(frames) == 3 and all(f.frame == want for f in frames), "three identical frames", f"{len(frames)}")
    gaps = [b.at - a.at for a, b in zip(frames, frames[1:])]
    r.check(all(0.9 <= g <= 1.6 for g in gaps), "one bus cycle apart", " ".join(f"{g:.2f}" for g in gaps))
    await asyncio.sleep(5.5)
    r.equal(len(node.bus.commands(mark)), 3, "and not sent again once the status shows it")
    r.equal(node.climate.target_temperature, 22.0, "the entity stayed on 22")
    await expect_counters(ctx, node, {"unanswered_commands": 2, "command_retries": 2, "failed_commands": 0,
                                      "bus_reply_timeouts": 2, "bus_link_losses": 0},
                          "counters: 2 unanswered, 2 retries, 2 reply timeouts, nothing failed")
    r.equal(node.log_count("Command frame got no reply from the unit, sending it again"), 2, "both losses are logged")

    unit.command_reply = "echo"
    await act_and_expect(ctx, node, "target 23, answered with the 20-byte echo of the stock protocol",
                         lambda: node.climate_command(target_temperature=23.0), combined_frame("cool", 23))
    await asyncio.sleep(5.0)
    r.equal((node.climate.target_temperature, len(command_frames(node))), (23.0, 4), "taken, and sent once")


@scenario(34, full=True)
async def deaf_unit_forever(ctx):
    """A unit that answers its status poll and never a command: 3 x 3 frames, then given up."""
    r = ctx.r
    node = await ctx.node()
    node.unit.deaf_commands = -1
    want = combined_frame("cool", 22)
    mark = await act_and_expect(ctx, node, "target 22", lambda: node.climate_command(target_temperature=22.0), want)
    r.check(await node.wait(lambda: counters(node)["failed_commands"] == 1, 30), "the command is given up on")
    frames = node.bus.commands(mark)
    r.check(len(frames) == 9 and all(f.frame == want for f in frames), "nine identical frames", f"{len(frames)}")
    r.check(await node.wait(lambda: node.climate.target_temperature == 24.0, 3), "the entity reverts to 24")
    await expect_counters(ctx, node, {"unanswered_commands": 9, "command_retries": 8, "failed_commands": 1},
                          "counters: 9 unanswered, 8 retries (6 on the bus, 2 by the hub), 1 failed")


@scenario(14)
async def queue_full(ctx):
    """More commands than the 8-frame queue holds: frames are dropped, and the entity ends on the truth."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit
    asked = [17.0 + (i % 15) for i in range(40)]
    mark = node.mark()
    for target in asked:
        node.climate_command(target_temperature=target)
    r.check(await node.wait(lambda: "Command frame dropped: TX queue full" in node.log_text(), 3),
            "the overflow is logged as a dropped frame")
    await node.wait(lambda: len(node.bus.commands(mark)) >= 8, 6)
    await asyncio.sleep(6.5)
    frames = node.bus.commands(mark)
    dropped = node.log_count("Command frame dropped: TX queue full")
    r.check(len(frames) + dropped == len(asked) and dropped > 0,
            "every request was either sent or logged as dropped",
            f"{len(frames)} sent, {dropped} dropped, {len(asked)} asked")
    sent = [(f.frame[19] - 1) // 2 for f in frames]
    r.equal(sent, [int(a) for a in asked[:len(sent)]] if dropped else sent,
            "the frames that went out are the first requests, in order")
    r.equal(node.climate.target_temperature, float(unit.setpoint),
            "after the hold-off the entity shows the unit's setpoint")
    r.equal(unit.setpoint, sent[-1], "which is the last frame that went out")
    per_cycle = {}
    for f in frames:
        per_cycle[int(f.at - frames[0].at + 0.4)] = per_cycle.get(int(f.at - frames[0].at + 0.4), 0) + 1
    r.note(f"frames per second while the queue drained: {list(per_cycle.values())}")
    status = polls(node, CLASS_STATUS, 0, mark)
    gaps = [b.at - a.at for a, b in zip(status, status[1:])]
    r.check(max(gaps) <= 1.6, "the status poll kept its pace while the queue drained", f"longest gap {max(gaps):.2f} s")
    # Was a known failure (static-analysis W4) until a request whose frame could not be queued
    # stopped being published: the entity used to show the last target asked for, for four seconds.
    shown = [s.target_temperature for t, k, s in node.history if k == CLIMATE and not math.isnan(s.target_temperature)]
    never_sent = sorted(set(shown) - {float(v) for v in sent} - {24.0})
    r.check(not never_sent, "the entity never showed a target whose frame was dropped", str(never_sent))


# ---- The link ----------------------------------------------------------------------------------------
@scenario(30)
async def link_never_up(ctx):
    """A node that boots with no A/C answering: what is refused, what is shown, and the recovery."""
    r = ctx.r
    unit = Unit()
    unit.silent = True
    node = await ctx.node(unit, expect_status=False)
    await asyncio.sleep(1.0)
    c = lambda: node.climate  # noqa: E731
    r.check(CLIMATE in node.states and c().mode == Mode.OFF and math.isnan(c().target_temperature)
            and math.isnan(c().current_temperature),
            "before any status the climate shows off, with no temperatures", repr(node.states.get(CLIMATE)))
    r.equal(node.value("binary_sensor.ac_bus_link"), None, "the link sensor has no state yet: unknown, not off")

    await act_and_expect(ctx, node, "a target before the first status", lambda: node.climate_command(target_temperature=22.0),
                         None)
    r.check(math.isnan(c().target_temperature), "and the entity does not show it")
    r.check("Command refused: no status from the unit yet" in node.log_text(), "the refusal is logged")
    await act_and_expect(ctx, node, "mode cool before the first status", lambda: node.climate_command(mode=Mode.COOL), None)
    r.equal(c().mode, Mode.OFF, "and the entity stays off")

    mark = node.mark()
    node.switch("eco", True)
    await asyncio.sleep(2.5)
    r.equal(command_frames(node, mark), [], "eco on before the first status: nothing is sent")
    r.check("Command not sent: no status from the unit yet" in node.log_text(), "the refused frame is logged")
    # Was a known failure (what was left of static-analysis W4) until the command tracker refused
    # an eco or turbo write that cannot be sent for want of a status.
    r.check(node.value("switch.eco") is not True, "a switch whose frame was refused does not show on",
            repr(node.value("switch.eco")))

    # Switching off needs no shadow, so it is let through.
    await act_and_expect(ctx, node, "mode off before the first status", lambda: node.climate_command(mode=Mode.OFF),
                         power_frame(False, token=(1, 1)), timeout=12)

    # Ten boot probes of a 1000 ms window and a 500 ms wait, then five silent cycles of two windows.
    r.check(await node.wait(lambda: node.value("binary_sensor.ac_bus_link") is False, 32),
            "after five unanswered status polls the link sensor reads off")
    probes = [x for x in node.bus.rx if x.cls == CLASS_DEVTYPE]
    first_07 = next(i for i, x in enumerate(node.bus.rx) if x.cls == 0x07)
    r.equal(sum(1 for x in node.bus.rx[:first_07] if x.cls == CLASS_DEVTYPE), 10,
            "boot tried the DevType probe ten times before going on")
    r.check(all(x.frame[7:9] == b"\x01\x01" for x in node.bus.rx if x.cls != CLASS_DEVTYPE),
            "with no device type learned, frames carry the seeded 01 01")
    r.check(len(probes) > 10 or await node.wait(lambda: len([x for x in node.bus.rx if x.cls == CLASS_DEVTYPE]) > 10, 5),
            "while the link is down every cycle starts with a DevType probe")
    await expect_counters(ctx, node, {"bus_link_losses": 1}, "a unit that was never there counts as one link loss")
    r.check("A/C bus link lost" in node.log_text(), "the loss is logged")

    unit.silent = False
    r.check(await node.wait(lambda: node.value("binary_sensor.ac_bus_link") is True, 8), "the unit answers: link on")
    r.check(await node.wait(lambda: c().current_temperature == 27.0 and c().mode == Mode.COOL, 6),
            "and the climate shows the unit: cool, 27 in the room", repr(c()))
    r.check(await node.wait(lambda: node.value("text_sensor.ac_device_type") == "01 02", 4),
            "the device type is learned from the recovery probe: 01 02")
    r.check(await node.wait(lambda: node.bus.rx[-1].frame[7:9] == b"\x01\x02", 3), "and stamped into the frames that follow")
    r.check("A/C bus link restored" in node.log_text(), "the recovery is logged")
    r.check(await node.wait(lambda: node.value("switch.eco") is False, 6), "the eco switch falls back to what the unit reports")
    r.equal(node.unit.power, True, "the power-off sent into the silence was lost: the unit is still on")
    r.equal(counters(node)["bus_link_losses"], 1, "still one link loss")


@scenario(40)
async def link_loss(ctx):
    """The unit goes silent and comes back: the link sensor, the counters, pending and new commands."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit
    c = lambda: node.climate  # noqa: E731

    # The unit dies under a command: that frame and everything after it go unanswered.
    def die(f):
        unit.silent = True
        return True

    unit.on_command = die
    mark = node.mark()
    node.climate_command(target_temperature=22.0)
    await node.wait(lambda: unit.silent, 3)
    t_silent = node.bus.now()
    unit.on_command = None
    # A silent cycle is a heartbeat window and a status window of 1000 ms each, plus the frames:
    # five of them, behind the command's own window, put the loss about 12 s after the silence.
    r.check(await node.wait(lambda: node.value("binary_sensor.ac_bus_link") is False, 18), "the link sensor goes off")
    lost = node.bus.now() - t_silent
    r.check(10.0 <= lost <= 15.0, "five unanswered status polls: lost 10 to 15 s after the unit went silent",
            f"{lost:.1f} s")
    # The first send saw no reply while the status poll had not missed yet, so the bus re-sent it
    # once, in the next cycle. By then a poll had gone unanswered and it was left at that.
    r.frames([f.frame for f in node.bus.commands(mark)], [combined_frame("cool", 22)] * 2,
             "the command went out, once more in the next cycle, and no further into the silence")
    await expect_counters(ctx, node, {"bus_link_losses": 1, "failed_commands": 1, "unanswered_commands": 2,
                                      "command_retries": 1},
                          "counters at the loss: 1 link loss, the pending command failed, 2 unanswered sends, 1 retry")
    r.check(counters(node)["bus_reply_timeouts"] >= 10, "reply timeouts were counted", str(counters(node)))
    r.check("command(s) unconfirmed or unsent when the link dropped" in node.log_text(), "the dropped command is logged")
    r.equal((c().mode, c().target_temperature, c().current_temperature, c().action),
            (Mode.COOL, 22.0, 27.0, Action.COOLING),
            "the climate keeps its last values, with the unconfirmed target: it does not become unavailable")
    r.equal(node.value("sensor.power"), 1660.0, "the sensors keep their last values too")
    probes = len([x for x in node.bus.rx if x.cls == CLASS_DEVTYPE])
    # With the link down a cycle also carries the probe's window: about 3.3 s each.
    await node.wait(lambda: len([x for x in node.bus.rx if x.cls == CLASS_DEVTYPE]) >= probes + 2, 8)
    r.check(len([x for x in node.bus.rx if x.cls == CLASS_DEVTYPE]) >= probes + 2,
            "while the link is down every cycle starts with a DevType probe")

    unit.silent = False
    r.check(await node.wait(lambda: node.value("binary_sensor.ac_bus_link") is True, 8), "the unit answers again: link on")
    r.check(await node.wait(lambda: c().target_temperature == 24.0, 6),
            "the target falls back to what the unit reports: 24")
    await asyncio.sleep(5.0)
    r.equal((unit.setpoint, len(node.bus.commands(mark))), (24, 2), "the command dropped at the loss is not sent after it")
    r.check("A/C bus link restored" in node.log_text(), "the recovery is logged")

    # A command issued while the link is already down.
    unit.silent = True
    r.check(await node.wait(lambda: node.value("binary_sensor.ac_bus_link") is False, 18), "silent again: link off")
    mark = node.mark()
    node.climate_command(target_temperature=21.0)
    await asyncio.sleep(2.5)
    # Was a known failure (bus-stress D2) until such a command was refused: it used to go out once
    # into the silence and be sent again, and taken, when the unit returned.
    r.equal((len(node.bus.commands(mark)), c().target_temperature), (0, 24.0),
            "a target asked for while the link is down is refused: nothing is sent, the entity is left alone")
    r.check("Command refused: the unit is not answering" in node.log_text(), "the refusal is logged")
    node.switch("eco", True)
    await asyncio.sleep(1.0)
    r.equal(node.value("switch.eco"), False, "a switch flipped while the link is down is refused as well")
    unit.silent = False
    r.check(await node.wait(lambda: node.value("binary_sensor.ac_bus_link") is True, 8), "the unit is back")
    await asyncio.sleep(8.0)
    r.equal((unit.setpoint, unit.eco, len(node.bus.commands(mark)), c().target_temperature), (24, False, 0, 24.0),
            "and nothing asked for during the outage is carried out after it")
    await expect_counters(ctx, node, {"bus_link_losses": 2}, "two link losses in all")


# ---- Setup request ("77") ---------------------------------------------------------------------------
@scenario(40)
async def setup_request(ctx):
    """The remote's setup gesture fires on_setup_request once; set_setup_indicator shows in the heartbeat."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit
    count = lambda: node.value("sensor.e2e_setup_requests")  # noqa: E731
    taken = lambda: node.log_count("Setup requested from the remote")  # noqa: E731

    async def pulse(bits, replies):
        unit.link_request, unit.link_request_replies = bits, replies
        await node.wait(lambda: unit.link_request_replies == 0, replies + 2)
        await node.quiet_cycles(2)

    await pulse(0x20, 1)
    r.check("0x1E reply request bits now 0x20" in node.log_text(), "the request bit is seen in the unit's 0x1E reply")
    r.equal((count(), taken()), (None, 0), "in the first 10 s after boot the pulse is not taken")

    await asyncio.sleep(max(0.0, 10.8 - node.bus.now()))
    await pulse(0x20, 1)
    r.equal((count(), taken()), (1.0, 1), "after that one reply with bit 5 runs on_setup_request once")
    r.check("Setup requested from the remote (0x1E reply request bits 0x20)" in node.log_text(),
            "and the request is logged with its bits")

    mark = node.mark()
    node.switch("e2e_setup_indicator", True)
    r.check(await node.wait(lambda: any(b.frame[17] == 0x88 for b in polls(node, CLASS_LINK, since=mark)), 3),
            "set_setup_indicator(true): the next heartbeat carries the provisioning flag (byte 17 = 88)")
    await node.quiet_cycles(3)
    beats = [b.frame[17] for b in polls(node, CLASS_LINK, since=mark)]
    r.check(beats.count(0x88) >= 3 and beats[beats.index(0x88):] == [0x88] * (len(beats) - beats.index(0x88)),
            "and every heartbeat after it", " ".join(f"{b:02X}" for b in beats))
    r.equal(node.value("switch.e2e_setup_indicator"), True, "setup_indicator() reads true")
    r.check('Panel setup indication ("77") on' in node.log_text(), "the indication is logged")

    # The press that leaves the mode makes a pulse of its own: inside the lockout it changes nothing.
    await pulse(0x20, 1)
    r.equal((count(), taken()), (1.0, 1), "a second pulse within 10 s of the first is not taken")
    mark = node.mark()
    node.switch("e2e_setup_indicator", False)
    await node.quiet_cycles(3)
    beats = [b.frame[17] for b in polls(node, CLASS_LINK, since=mark)]
    r.check(beats[-2:] == [0x80, 0x80], "set_setup_indicator(false): the flag is gone from the heartbeat",
            " ".join(f"{b:02X}" for b in beats))
    await pulse(0x20, 1)
    r.equal((count(), taken()), (1.0, 1), "a pulse right after the indication was switched off is not taken")

    # Bit 3 has to hold for three replies in a row.
    await pulse(0x08, 2)
    r.equal((count(), taken()), (1.0, 1), "bit 3 in two replies in a row is not a request")
    await pulse(0x08, 4)
    r.equal((count(), taken()), (2.0, 2), "bit 3 in four replies in a row is one request, lockout or not")
    r.check("Setup requested from the remote (0x1E reply request bits 0x08)" in node.log_text(), "logged with its bits")

    # That request started a lockout of its own.
    await asyncio.sleep(10.8)
    await pulse(0x20, 3)
    r.equal((count(), taken()), (3.0, 3), "bit 5 held for three replies is still one request")
    r.equal(command_frames(node), [], "none of this sent a command to the unit")


# ---- Damaged traffic ----------------------------------------------------------------------------------
@scenario(34)
async def damaged_traffic(ctx):
    """Bad checksums, truncated replies, garbage and missing replies: counted, and nothing false shown."""
    r = ctx.r
    node = await ctx.node()
    unit = node.unit
    c = lambda: node.climate  # noqa: E731
    want = dict.fromkeys(COUNTERS, 0)

    async def step(what, **moved):
        await node.quiet_cycles(2)
        for k, v in moved.items():
            want[k] += v
        await expect_counters(ctx, node, dict(want), what)

    unit.damage[(CLASS_STATUS, 0)] = ["bad_checksum"]
    await step("a status reply with a bad checksum: counted as a checksum error, not as a timeout", bus_checksum_errors=1)
    r.check(28.0 not in [s.target_temperature for t, k, s in node.history if k == CLIMATE] and unit.setpoint == 24,
            "and its content (a setpoint of 28) never reached the entity")
    r.check("Frame with a bad checksum dropped" in node.log_text(), "the dropped frame is logged")

    unit.damage[(CLASS_STATUS, 0)] = ["truncate"]
    await step("half a status reply: one reply timeout", bus_reply_timeouts=1)

    unit.indoor = 28
    unit.damage[(CLASS_STATUS, 0)] = ["garbage_before"]
    await step("nine bytes of noise ahead of a status reply cost nothing")
    unit.indoor = 29
    unit.damage[(CLASS_STATUS, 0)] = ["false_start"]
    await step("a false frame start (F4 F5 01) ahead of a status reply costs nothing")
    r.equal(c().current_temperature, 29.0, "both replies were decoded: the room reads 29")

    unit.damage[(CLASS_STATUS, 0)] = ["garbage_only"]
    await step("noise instead of a status reply: one reply timeout", bus_reply_timeouts=1)

    unit.damage[(CLASS_LINK, 0)] = ["bad_checksum"]
    await step("a heartbeat reply with a bad checksum: one checksum error", bus_checksum_errors=1)
    unit.damage[(CLASS_LINK, 0)] = ["drop"]
    await step("a missing heartbeat reply: one reply timeout", bus_reply_timeouts=1)

    # Bytes nobody asked for, between two transactions: flushed before the next frame goes out.
    await asyncio.sleep(0.45)
    node.bus.send_raw(bytes.fromhex("f4 f5 01 40 97 de ad be ef") + bytes(40))
    other = Unit()
    other.setpoint, other.mode = 30, "heat"
    node.bus.send_raw(other.status_frame())
    await step("noise and a whole unsolicited status frame between transactions change nothing")
    r.check(not any(s.target_temperature == 30.0 or s.mode == Mode.HEAT for t, k, s in node.history if k == CLIMATE),
            "the unsolicited frame (heat, 30) never reached the entity")

    unit.damage[(CLASS_STATUS, 0)] = ["drop"] * 4
    await node.quiet_cycles(6, 10)
    for k, v in (("bus_reply_timeouts", 4),):
        want[k] += v
    await expect_counters(ctx, node, dict(want), "four status polls in a row unanswered: four timeouts, and the link holds")
    r.equal(node.value("binary_sensor.ac_bus_link"), True, "the link sensor stayed on")
    r.check(not any(s.state is False and not s.missing_state for t, k, s in node.history
                    if k == "binary_sensor.ac_bus_link"), "at every moment")

    # A command whose reply is damaged still counts as answered, and the status confirms it.
    unit.damage[(0x65, 0)] = ["bad_checksum"]
    mark = await act_and_expect(ctx, node, "target 22, reply damaged", lambda: node.climate_command(target_temperature=22.0),
                                combined_frame("cool", 22))
    await asyncio.sleep(5.5)
    want["bus_checksum_errors"] += 1
    await expect_counters(ctx, node, dict(want), "one more checksum error, no unanswered command, no retry")
    r.equal((unit.setpoint, c().target_temperature, len(node.bus.commands(mark))), (22, 22.0, 1),
            "the unit took it, the entity shows it, one frame")


# ---- Long runs ----------------------------------------------------------------------------------------
@scenario(70, full=True)
async def periodic(ctx):
    """What happens once a minute: the telemetry refresh, and no ProductType re-poll."""
    r = ctx.r
    node = await ctx.node()
    await asyncio.sleep(64)
    # Without the states the API hands a new client: those are from before the first status.
    power = [(round(t, 1), s.state) for t, k, s in node.history if k == "sensor.power" and not s.missing_state]
    r.check(len(power) == 2 and power[0][1] == power[1][1] == 1660.0 and 59.0 <= power[1][0] - power[0][0] <= 62.0,
            "an unchanged power reading is published again after 60 s, and only then", str(power))
    # Once per handshake, like the vendor module (#230): never again while the link holds.
    product = polls(node, CLASS_STATUS, SUB_PRODUCT)
    r.check(len(product) == 1, "the ProductType poll goes out once, and not again in the minute",
            " ".join(f"{p.at:.1f}" for p in product))
    energy = node.value("sensor.energy_today")
    r.check(0.025 <= energy <= 0.031, "energy today integrated 1660 W over the minute: about 0.028 kWh", str(energy))
    climate = [s for t, k, s in node.history if k == CLIMATE and not math.isnan(s.current_temperature)]
    r.equal(len(climate), 1, "the climate entity was published once in a minute of identical status frames")
    r.equal(counters(node), dict.fromkeys(COUNTERS, 0), "a quiet minute: every counter still 0")
