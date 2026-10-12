#!/usr/bin/env python3
"""Pack a libFuzzer corpus directory into one text file and back.

libFuzzer keeps a corpus as one small binary file per input, hundreds of them per harness. In git
that is noise nobody can review, so the committed corpus is corpus/<harness>.hex instead: one
input per line as hex digits, sorted, "-" for the empty input, "#" starts a comment. It diffs,
greps and reviews like any text file, and standalone_main.cpp replays it directly.

    corpus_tool.py pack <corpus dir> <out.hex> [<dir whose inputs are left out>...]
    corpus_tool.py unpack <in.hex> <corpus dir>

run.sh calls both: `unpack` before fuzzing (libFuzzer wants a directory), `pack` in --merge.
"""

import hashlib
import sys
from pathlib import Path


def read_dir(path: Path) -> set[bytes]:
    if not path.is_dir():
        return set()
    return {p.read_bytes() for p in path.iterdir() if p.is_file() and p.suffix != ".hex"}


def pack(src: Path, out: Path, skip_dirs: list[Path]) -> int:
    skip: set[bytes] = set()
    for d in skip_dirs:
        skip |= read_dir(d)
    inputs = sorted(read_dir(src) - skip, key=lambda b: (len(b), b))
    lines = [
        f"# Fuzz corpus for the `{out.stem}` harness: one input per line, hex. Written by",
        "# `run.sh --merge` (coverage-minimised by libFuzzer, seeds left out). Do not edit by hand.",
    ]
    lines += [b.hex() if b else "-" for b in inputs]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")
    return len(inputs)


def unpack(src: Path, dst: Path) -> int:
    dst.mkdir(parents=True, exist_ok=True)
    count = 0
    if not src.is_file():
        return 0
    for line in src.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        data = b"" if line == "-" else bytes.fromhex(line)
        (dst / hashlib.sha1(data).hexdigest()).write_bytes(data)
        count += 1
    return count


def main(argv: list[str]) -> int:
    if len(argv) >= 4 and argv[1] == "pack":
        n = pack(Path(argv[2]), Path(argv[3]), [Path(a) for a in argv[4:]])
    elif len(argv) == 4 and argv[1] == "unpack":
        n = unpack(Path(argv[2]), Path(argv[3]))
    else:
        print(__doc__, file=sys.stderr)
        return 2
    print(n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
