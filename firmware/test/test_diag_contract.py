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


def main() -> None:
    if not CONST.exists():
        print("[diag contract] SKIP: HACS submodule not checked out")
        return
    fw = firmware_macros()
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
