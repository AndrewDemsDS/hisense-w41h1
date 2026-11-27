#!/usr/bin/env python3
"""The Python side of hisense_ac names the same things as the C++ side.

The schema files (binary_sensor.py, climate.py, switch.py, select.py, __init__.py) refer to
constants of the C++ headers: fault and capability bits, the preset support bits, the switch
kinds, the setpoint range, the sleep profiles, the baud rate. For the bits and the kinds the
generated code passes the C++ name, so a wrong one fails the build. This test says so without a
build, and it checks what no compiler sees: that every bit the unit reports has an entity, that
none is listed twice, and that the numbers Python needs for validation equal the header's.

Reads the sources as text. Needs neither ESPHome nor a compiler.
"""

from __future__ import annotations

import ast
from pathlib import Path
import re
import sys

COMPONENT = Path(__file__).resolve().parents[1] / "esphome" / "components" / "hisense_ac"

failures = 0
checks = 0


def check(condition: bool, message: str) -> None:
    global failures, checks
    checks += 1
    if not condition:
        failures += 1
        print(f"  FAIL {message}")


def source(name: str) -> str:
    return (COMPONENT / name).read_text(encoding="utf-8")


def cpp_constants(text: str, prefix: str) -> dict[str, int]:
    """`static constexpr <type> PREFIX_NAME = <integer>;` lines, name -> value."""
    found = re.findall(
        rf"^\s*static constexpr \w+ ({prefix}\w+) = (0[xX][0-9a-fA-F]+|\d+);", text, re.MULTILINE
    )
    return {name: int(value, 0) for name, value in found}


def cpp_enum(text: str, name: str) -> list[str]:
    body = re.search(rf"enum {name}\b[^{{]*\{{([^}}]*)\}}", text)
    if body is None:
        return []
    return re.findall(r"^\s*(\w+)\b", body.group(1), re.MULTILINE)


def python_int(text: str, name: str) -> int | None:
    match = re.search(rf"^{name} = (\d+)$", text, re.MULTILINE)
    return int(match.group(1)) if match else None


def bit_table(label: str, python_names: list[str], cpp: dict[str, int], expected: set[str]) -> None:
    check(len(python_names) > 0, f"{label}: no constant found in the Python table")
    for name in python_names:
        check(name in cpp, f"{label}: {name} is not a constant of the C++ header")
    check(
        len(set(python_names)) == len(python_names),
        f"{label}: a constant is listed twice ({sorted(n for n in set(python_names) if python_names.count(n) > 1)})",
    )
    bits = [cpp[name] for name in python_names if name in cpp]
    check(len(set(bits)) == len(bits), f"{label}: two entries share a bit")
    missing = sorted(expected - set(python_names))
    check(not missing, f"{label}: the header has {missing}, the Python table does not")
    extra = sorted(set(python_names) - expected)
    check(not extra, f"{label}: {extra} is not a bit an entity can show")


def main() -> int:
    print("== hisense_ac: the Python tables against the C++ constants ==")
    protocol = source("hisense_protocol.h")
    special = source("hisense_special.h")
    entities = source("hisense_entities.h")

    # Fault bits: every FAULT1_* below the two summary bits (ANY, VALID) is one binary sensor.
    faults = cpp_constants(protocol, "FAULT1_")
    check(len(faults) == 20, f"the header declares 18 fault bits and 2 summary bits (found {len(faults)})")
    fault_bits = {name for name, bit in faults.items() if bit < faults.get("FAULT1_ANY", 0)}
    binary_sensor = source("binary_sensor.py")
    bit_table("fault bits", re.findall(r'"(FAULT1_\w+)"', binary_sensor), faults, fault_bits)
    # The fault detail text names a stock fault bit as its binary sensor option, less "fault_".
    names_table = re.search(r"FAULT_NAMES\[\] = \{(.*?)\n\};", source("hisense_protocol.cpp"), re.DOTALL)
    detail_names = re.findall(
        r'\{FAULT_BYTE_\w+, 0x[0-9A-Fa-f]{2}, FAULT_NAME_FAULT, "(\w+)"', names_table.group(1) if names_table else ""
    )
    options = [
        key.removeprefix("fault_")
        for key in re.findall(r'^\s*"(fault_\w+)": \("FAULT1_', binary_sensor, re.MULTILINE)
    ]
    check(
        sorted(detail_names) == sorted(options) and len(set(detail_names)) == len(detail_names),
        f"FAULT_NAMES names {sorted(detail_names)}, binary_sensor.py has {sorted(options)}",
    )
    # Every other fault bit has a FAULT_RAW_BITS entry, the frost-guard flag (byte 66 0x80) excepted,
    # and its name says what FAULT_NAMES knows of the bit: reserved, a guess ("?"), or unknown.
    offsets = cpp_constants(protocol, "FAULT_BYTE_")
    rows = {
        (offsets.get(byte), int(mask, 16)): kind
        for byte, mask, kind in re.findall(
            r"\{(FAULT_BYTE_\w+), (0x[0-9A-Fa-f]{2}), (FAULT_NAME_\w+)", names_table.group(1) if names_table else ""
        )
    }
    raw = re.findall(
        r'^\s*"(fault_byte(\d+)_0x([0-9a-f]{2}))": \(\s*"(FAULT_BYTE_\w+)",\s*(0x[0-9A-Fa-f]{2}),\s*"([^"]+)",?\s*\),',
        binary_sensor,
        re.MULTILINE,
    )
    check(len(raw) > 0, "binary_sensor.py: FAULT_RAW_BITS found")
    every = {(offsets[name], 1 << bit) for name in offsets for bit in range(8)} - {(66, 0x80)}
    stock = {key for key, kind in rows.items() if kind == "FAULT_NAME_FAULT"}
    seen = []
    for key, num, hexmask, byte, mask, label in raw:
        at = (offsets.get(byte), int(mask, 16))
        seen.append(at)
        check(at == (int(num), int(hexmask, 16)), f"{key}: the option name says byte {num} 0x{hexmask}, the entry {at}")
        kind = rows.get(at)
        want = {"FAULT_NAME_RESERVED": "reserved", "FAULT_NAME_GUESS": "?"}.get(kind, "unknown")
        check(kind != "FAULT_NAME_FAULT" and want in label, f"{key}: FAULT_NAMES has {kind}, the name is {label!r}")
    check(len(set(seen)) == len(seen), "FAULT_RAW_BITS: a bit is listed twice")
    check(
        set(seen) == every - stock,
        f"FAULT_RAW_BITS misses {sorted(every - stock - set(seen))}, or has {sorted(set(seen) - (every - stock))}",
    )

    # Capability bits: the one-bit FEAT1_* fields. The two-bit fields (_SHIFT) and the validity
    # bits are not booleans and have no entity.
    features = cpp_constants(protocol, "FEAT1_")
    lowest_other = min(
        bit for name, bit in features.items() if name.endswith(("_SHIFT", "_VALID")) or name == "FEAT1_VALID"
    )
    feature_bits = {name for name, bit in features.items() if bit < lowest_other}
    check(len(feature_bits) == 13, f"the header declares 13 one-bit capabilities (found {len(feature_bits)})")
    bit_table("capability bits", re.findall(r'"(FEAT1_\w+)"', binary_sensor), features, feature_bits)

    # Preset support: one option per SUPPORT_* bit.
    support = cpp_constants(special, "SUPPORT_")
    climate = source("climate.py")
    bit_table("preset support bits", re.findall(r'"(SUPPORT_\w+)"', climate), support, set(support))
    check(
        sorted(support.values()) == [1, 2, 4, 8],
        f"the SUPPORT_* bits are four distinct single bits ({support})",
    )

    # The setpoint range the schema refuses to go beyond.
    setpoints = cpp_constants(protocol, "SETPOINT_")
    for name in ("SETPOINT_MIN_C", "SETPOINT_MAX_C", "SETPOINT_MAX_F"):
        check(
            python_int(climate, name) == setpoints.get(name),
            f"climate.py {name} = {python_int(climate, name)}, hisense_protocol.h says {setpoints.get(name)}",
        )
    for key, default in (("CONF_MIN_TEMPERATURE", "SETPOINT_MIN_C"), ("CONF_MAX_TEMPERATURE", "SETPOINT_MAX_C")):
        match = re.search(rf"cv\.Optional\({key}, default=(\d+)\)", climate)
        check(
            match is not None and int(match.group(1)) == setpoints.get(default),
            f"climate.py: the default of {key} is {default}",
        )

    # Switch kinds: every SwitchKind has a switch, and the Python side names no other.
    kinds = cpp_enum(entities, "SwitchKind")
    check(len(kinds) == 5, f"hisense_entities.h declares five switch kinds (found {kinds})")
    used = re.findall(r"SwitchKind\.(SWITCH_\w+)", source("switch.py"))
    check(sorted(used) == sorted(kinds), f"switch.py names {sorted(used)}, the enum has {sorted(kinds)}")

    # The sleep select: the option index is the profile number on the wire.
    profiles = re.search(r"^SLEEP_PROFILES = (\[[^\]]*\])$", source("select.py"), re.MULTILINE)
    options = ast.literal_eval(profiles.group(1)) if profiles else []
    profile_max = cpp_constants(entities, "SLEEP_PROFILE_").get("SLEEP_PROFILE_MAX")
    check(
        profile_max is not None and len(options) == profile_max + 1,
        f"select.py has {len(options)} sleep options, SLEEP_PROFILE_MAX is {profile_max}",
    )
    check(options[:1] == ["Off"], "the first sleep option is Off (profile 0)")

    # The baud rate the schema demands is the one the scheduler's timing is computed for.
    baud = cpp_constants(protocol, "BUS_BAUD_").get("BUS_BAUD_RATE")
    schema_baud = re.search(r"baud_rate=(\d+)", source("__init__.py"))
    check(
        baud is not None and schema_baud is not None and int(schema_baud.group(1)) == baud,
        f"__init__.py demands baud rate {schema_baud.group(1) if schema_baud else None}, BUS_BAUD_RATE is {baud}",
    )

    # The power limit: the default and the top of the range max_power takes.
    limits = cpp_constants(source("hisense_telemetry.h"), "POWER_LIMIT_W_")
    hub = source("__init__.py")
    for name, constant in (("MAX_POWER_DEFAULT_W", "POWER_LIMIT_W_DEFAULT"), ("MAX_POWER_MAX_W", "POWER_LIMIT_W_MAX")):
        check(
            python_int(hub, name) is not None and python_int(hub, name) == limits.get(constant),
            f"__init__.py {name} = {python_int(hub, name)}, hisense_telemetry.h says {limits.get(constant)}",
        )
    low = python_int(hub, "MAX_POWER_MIN_W")
    check(
        low is not None and 0 < low < limits.get("POWER_LIMIT_W_DEFAULT", 0),
        f"__init__.py MAX_POWER_MIN_W = {low} is below the default",
    )

    print(f"  {checks} checks, {failures} failed")
    print("== TABLES FAILED ==" if failures else "== TABLES OK ==")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
