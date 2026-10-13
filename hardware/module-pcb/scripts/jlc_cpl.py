#!/usr/bin/env python3
"""Convert KiCad's placement CSV to the column names JLCPCB's assembly upload expects.

Rotations are passed through unchanged. JLCPCB's part library does not always agree with
KiCad's footprint orientation: check every part in their placement preview before paying.

Usage: python3 scripts/jlc_cpl.py kicad-pos.csv jlc-cpl.csv
"""

import csv
import sys

# Not placed by the assembly house: holes, bare test pads, the unpopulated programming pads.
SKIP_PREFIXES = ("H", "TP", "J2")


def main():
    src, dst = sys.argv[1:3]
    with open(src, newline="") as f:
        rows = list(csv.DictReader(f))
    with open(dst, "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for row in rows:
            if row["Ref"].startswith(SKIP_PREFIXES):
                continue
            layer = "Top" if row["Side"] == "top" else "Bottom"
            out.writerow(
                [
                    row["Ref"],
                    f"{float(row['PosX']):.4f}mm",
                    f"{float(row['PosY']):.4f}mm",
                    layer,
                    row["Rot"],
                ]
            )


if __name__ == "__main__":
    main()
