#!/usr/bin/env python3
"""Contract test (docs/14): the HACS integration's Faults1/Features1 bit layout MUST match
the firmware packers' HISENSE_FAULT1_* / HISENSE_FEAT1_* macros in hisense_rs485.h.

The two live in separate repos (the integration is a submodule), so nothing but this test
stops them drifting. Exits non-zero on any mismatch; skips cleanly if the submodule is absent.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HDR = ROOT / "firmware/src/rs485-driver/hisense_rs485.h"
CONST = (
    ROOT
    / "integrations/hisense-unified-ac/custom_components/hisense_unified_ac/const.py"
)
ESPHOME_BS = ROOT / "firmware/esphome/components/hisense_ac/binary_sensor.py"

# The ESPHome entity keys read as entity names ("fault_indoor_temp"), the macros read as struct
# fields ("IN_TEMP"). Neither is derivable from the other, so the pairing is spelled out once
# here; that is what makes a renumbered bit a test failure instead of a mislabelled sensor.
ALIASES = {
    "fault_indoor_temp": "IN_TEMP",
    "fault_indoor_coil_temp": "IN_COIL_TEMP",
    "fault_indoor_humidity": "IN_HUMIDITY",
    "fault_water_full": "WATER_FULL",
    "fault_indoor_fan_motor": "IN_FAN_MOTOR",
    "fault_grille": "GRILLE",
    "fault_indoor_vzero": "IN_VZERO",
    "fault_indoor_comms": "IN_COM",
    "fault_indoor_display": "IN_DISPLAY",
    "fault_indoor_keys": "IN_KEYS",
    "fault_indoor_wifi": "IN_WIFI",
    "fault_indoor_ele": "IN_ELE",
    "fault_indoor_eeprom": "IN_EEPROM",
    "fault_outdoor_eeprom": "OUT_EEPROM",
    "fault_outdoor_coil_temp": "OUT_COIL_TEMP",
    "fault_outdoor_gas_temp": "OUT_GAS_TEMP",
    "fault_outdoor_temp": "OUT_TEMP",
    "fault_over_temp": "OVER_TEMP",
    "capability_cool_heat": "COOL_HEAT",
    "capability_ai": "AI",
    "capability_infinite_fan": "INFINITE_FAN",
    "capability_power_save": "POWER_SAVE",
    "capability_fan_mute": "FAN_MUTE",
    "capability_swing_dir_8": "SWING_DIR_8",
    "capability_swing_follow": "SWING_FOLLOW",
    "capability_humidity": "HUMIDITY",
    "capability_heat_8c": "HEAT_8C",
    "capability_purify": "PURIFY",
    "capability_q_display": "Q_DISPLAY",
    "capability_enable_8heat": "ENABLE_8HEAT",
    "capability_trans_102_64": "TRANS_102_64",
}


def firmware_macros() -> dict[str, int]:
    text = HDR.read_text()
    return {
        m: int(v)
        for m, v in re.findall(
            r"#define\s+(HISENSE_(?:FAULT1|FEAT1)_\w+)\s+(\d+)", text
        )
    }


def const_namespace() -> dict:
    ns: dict = {}
    # const.py is pure data (no imports), so it execs standalone without Home Assistant.
    exec(compile(CONST.read_text(), str(CONST), "exec"), ns)  # noqa: S102
    return ns


def check_esphome_bits(fw: dict[str, int]) -> list[str]:
    """The ESPHome component names one binary sensor per bit, so its dicts are a third copy of
    the layout. Unlike the HACS integration this one lives in-tree, but it drifts just as
    silently: a renumbered bit would relabel a fault rather than fail anything."""
    errors: list[str] = []
    ns: dict = {}
    # Parse rather than import: importing would pull in esphome, which is not a test dependency.
    text = ESPHOME_BS.read_text()
    for dict_name, prefix in (
        ("FAULT_BITS", "HISENSE_FAULT1_"),
        ("CAPABILITY_BITS", "HISENSE_FEAT1_"),
    ):
        body = re.search(rf"^{dict_name} = \{{(.*?)^\}}", text, re.S | re.M)
        if body is None:
            errors.append(f"{ESPHOME_BS.name}: {dict_name} not found")
            continue
        entries = re.findall(r'"(\w+)":\s*\((\d+),', body.group(1))
        ns[dict_name] = entries
        for key, bit in entries:
            # "fault_indoor_temp" -> HISENSE_FAULT1_IN_TEMP is not mechanical, so the component
            # key carries the macro suffix after its prefix only when they agree; compare on the
            # bit index via the name the component chose.
            macro_suffix = ALIASES.get(key)
            if macro_suffix is None:
                errors.append(f"{dict_name}: {key} has no macro alias in this test")
                continue
            macro = prefix + macro_suffix
            if fw.get(macro) != int(bit):
                errors.append(
                    f"esphome {key}: bit {bit} != firmware {macro}={fw.get(macro)}"
                )
    if len(ns.get("FAULT_BITS", [])) != 18:
        errors.append(f"esphome FAULT_BITS has {len(ns.get('FAULT_BITS', []))} entries, want 18")
    return errors


def main() -> None:
    fw = firmware_macros()
    esphome_errors = check_esphome_bits(fw)
    if esphome_errors:
        print("[diag contract] FAIL (ESPHome component)")
        for e in esphome_errors:
            print("  -", e)
        sys.exit(1)
    print("[diag contract] OK: ESPHome binary_sensor bit map matches hisense_rs485.h")

    if not CONST.exists():
        print("[diag contract] SKIP: HACS submodule not checked out")
        return
    ns = const_namespace()
    errors: list[str] = []

    faults = ns["FAULT1_BITS"]
    if len(faults) != 18:
        errors.append(f"FAULT1_BITS has {len(faults)} entries, want 18")
    for bit, key, _name in faults:
        macro = "HISENSE_FAULT1_" + key.upper()
        if fw.get(macro) != bit:
            errors.append(
                f"fault {key}: const bit {bit} != firmware {macro}={fw.get(macro)}"
            )

    feats = ns["FEAT1_BITS"]
    for bit, key, _name, _ext in feats:
        macro = "HISENSE_FEAT1_" + key.upper()
        if fw.get(macro) != bit:
            errors.append(
                f"feat {key}: const bit {bit} != firmware {macro}={fw.get(macro)}"
            )

    meta = [
        ("FAULTS1_VALID_BIT", "HISENSE_FAULT1_VALID"),
        ("FAULTS1_ANY_BIT", "HISENSE_FAULT1_ANY"),
        ("FEATURES1_VALID_BIT", "HISENSE_FEAT1_VALID"),
        ("FEATURES1_EXT_VALID_BIT", "HISENSE_FEAT1_EXT_VALID"),
        ("FEATURES1_POWER_DISPLAY_SHIFT", "HISENSE_FEAT1_POWER_DISPLAY_SHIFT"),
        ("FEATURES1_DEMAND_RESP_SHIFT", "HISENSE_FEAT1_DEMAND_RESP_SHIFT"),
    ]
    for const_name, macro in meta:
        if ns[const_name] != fw.get(macro):
            errors.append(
                f"{const_name} ({ns[const_name]}) != {macro} ({fw.get(macro)})"
            )

    if errors:
        print("[diag contract] FAIL")
        for e in errors:
            print("  -", e)
        sys.exit(1)
    print(
        f"[diag contract] OK: 18 fault bits + {len(feats)} feature flags + meta "
        "match hisense_rs485.h"
    )


if __name__ == "__main__":
    main()
