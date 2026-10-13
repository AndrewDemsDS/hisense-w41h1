#!/usr/bin/env python3
"""The failing gates of a firmware/scripts/esphome-upstream-check.sh run, as Markdown.

    upstream_gate_report.py <log of the run>

The script prints `[upstream] <gate>` before each gate, the gate's own output, and
`[upstream] FAILED: <command>` after one that failed. This keeps the failed gates only, each with
its output cut to the first and last lines, so the text fits an issue and shows the rule that
fired and not a thousand validated configurations. Terminal colours are removed and long lines
are cut.

Exit 0 with the report on stdout. Exit 3 when the log names no failed gate: the run stopped
somewhere else (the clone, the tool install), which says nothing about the component.
"""

from __future__ import annotations

import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
MARK = re.compile(r"^\[upstream\] (.*)$")
HEAD, TAIL = 45, 20  # lines kept of one gate's output, halved until the report fits
WIDTH = 300  # characters kept of one line
LIMIT = 40000  # characters: an issue body holds 65536


def failed_gates(text: str) -> list[tuple[str, list[str]]]:
    gates: list[tuple[str, list[str]]] = []
    label, lines = None, []
    for raw in text.splitlines():
        line = ANSI.sub("", raw).rstrip()
        mark = MARK.match(line)
        if mark is None:
            lines.append(line)
        elif mark.group(1).startswith("FAILED:"):
            gates.append((label or mark.group(1)[len("FAILED:"):].strip(), lines))
            label, lines = None, []
        else:
            label, lines = mark.group(1), []
    return gates


def cut(lines: list[str], head: int, tail: int) -> list[str]:
    while lines and not lines[0].strip():
        lines = lines[1:]
    while lines and not lines[-1].strip():
        lines = lines[:-1]
    lines = [ln if len(ln) <= WIDTH else ln[:WIDTH] + " [...]" for ln in lines]
    if len(lines) <= head + tail:
        return lines
    return lines[:head] + [f"[... {len(lines) - head - tail} lines left out ...]"] + lines[-tail:]


def report(text: str) -> str:
    head, tail = HEAD, TAIL
    while True:
        parts = []
        for label, lines in failed_gates(text):
            # A fence longer than any run of backticks in the output, so the output cannot close it.
            body = "\n".join(cut(lines, head, tail))
            fence = "`" * max(3, max((len(m) for m in re.findall(r"`+", body)), default=0) + 1)
            parts.append(f"### {label}\n\n{fence}text\n{body}\n{fence}\n")
        out = "\n".join(parts)
        if len(out) <= LIMIT or head <= 1:
            return out
        head, tail = head // 2, max(tail // 2, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
        out = report(f.read())
    if not out:
        print("upstream_gate_report.py: the log names no failed gate", file=sys.stderr)
        return 3
    sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
