#!/usr/bin/env python3
"""List every uncovered line and every branch not taken from a gcovr JSON report.

    uncovered.py coverage.json REPO_ROOT

One row per source line: LINE when it never ran, BRANCH i,j/n when it ran and outcomes i and j
of its n branch outcomes were never taken.
"""
import json
import sys


def main() -> int:
    report, repo = sys.argv[1], sys.argv[2]
    with open(report, encoding="utf-8") as fh:
        data = json.load(fh)
    rows = 0
    for entry in sorted(data["files"], key=lambda e: e["file"]):
        with open(f"{repo}/{entry['file']}", encoding="utf-8") as fh:
            src = fh.read().splitlines()
        for line in entry["lines"]:
            n = line["line_number"]
            branches = line.get("branches", [])
            missed = [str(i) for i, b in enumerate(branches) if b["count"] == 0]
            text = src[n - 1].strip()
            if line["count"] == 0:
                print(f"{entry['file']}:{n}: LINE    {text}")
                rows += 1
            elif missed:
                print(f"{entry['file']}:{n}: BRANCH {','.join(missed)}/{len(branches)}  {text}")
                rows += 1
    print(f"{rows} source lines with a gap")
    return 0


if __name__ == "__main__":
    sys.exit(main())
