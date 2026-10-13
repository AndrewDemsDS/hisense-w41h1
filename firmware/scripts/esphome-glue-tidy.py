#!/usr/bin/env python3
"""clang-tidy over this repo's ESPHome components as a board build compiles them. Report only.

    esphome-glue-tidy.py <compile_commands.json>

The hub and the entities are checked on the host build (firmware/test/host_e2e/run.py --tidy,
which gates). wifi_setup_mode.cpp needs wifi: and w41h1_module.cpp needs LibreTiny, so neither is
part of a host build, and code under a platform #ifdef is not seen there either. A board build
compiles all of it, and its compile database says how:

    ESP-IDF      firmware/esphome/.esphome/build/<name>/build/compile_commands.json, written by
                 the build itself
    PlatformIO   `pio run -t compiledb` in a copy of .esphome/build/<name> (in the project itself
                 that target deletes the build output)

The database names a GCC cross compiler. clang-tidy is given the same defines, include paths and
language standard, the target of that compiler, and that compiler's own header search path. The
other flags are GCC's and are left out. The repository's .clang-tidy applies, at the clang-tidy
cpp-lint.sh pins ($CLANG_TIDY, or the one cpp-lint.sh installed, or the one on PATH).

Prints the findings in this repo's components and their count, and exits 0 whatever the count:
the checks that only fire under a cross target have not been triaged, so this is not a gate. A
file clang could not parse is counted apart, so that a zero is not taken for a clean file. Exit 2
when the database or a tool is missing.
"""

from __future__ import annotations

import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
COMPONENTS = sorted(p.name for p in (ROOT / "firmware/esphome/components").iterdir() if p.is_dir())
OWN = re.compile(r"/components/(?:%s)/" % "|".join(map(re.escape, COMPONENTS)))

# What clang takes over from the GCC command line. Flags with a separate value first.
WITH_VALUE = ("-D", "-U", "-I", "-isystem", "-iquote", "-include")
PREFIXES = WITH_VALUE + ("-std=", "-march=", "-mabi=", "-mcpu=", "-mfloat-abi=", "-mfpu=")
EXACT = ("-mthumb", "-fno-exceptions", "-fno-rtti", "-fno-short-enums", "-fsigned-char")
MACHINE = ("-march=", "-mabi=", "-mcpu=", "-mfloat-abi=", "-mfpu=", "-mthumb")


def find_tidy() -> str | None:
    if os.environ.get("CLANG_TIDY"):
        return os.environ["CLANG_TIDY"]
    cached = pathlib.Path.home() / ".cache/hisense-w41h1/cpp-lint/bin/clang-tidy"
    return str(cached) if cached.exists() else shutil.which("clang-tidy")


def expand(args: list[str], directory: pathlib.Path) -> list[str]:
    """ESP-IDF passes most of its flags through @response files."""
    out: list[str] = []
    for arg in args:
        if arg.startswith("@"):
            out += shlex.split((directory / arg[1:]).read_text())
        else:
            out.append(arg)
    return out


def clang_command(entry: dict) -> list[str]:
    directory = pathlib.Path(entry["directory"])
    args = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])
    gxx, flags = args[0], expand(args[1:], directory)
    kept: list[str] = []
    take_next = False
    for flag in flags:
        if take_next:
            kept.append(flag)
            take_next = False
        elif flag in WITH_VALUE:
            kept.append(flag)
            take_next = True
        elif flag in EXACT or flag.startswith(PREFIXES):
            kept.append(flag)
    machine = [f for f in kept if f in MACHINE or f.startswith(MACHINE[:-1])]
    # The cross compiler's own header search path, in its order, for the same machine flags.
    probe = subprocess.run([gxx, *machine, "-x", "c++", "-E", "-v", "-"], input="", capture_output=True, text=True)
    listed = re.search(r"#include <\.\.\.> search starts here:\n(.*?)End of search list", probe.stderr, re.S)
    triple = subprocess.run([gxx, "-dumpmachine"], capture_output=True, text=True).stdout.strip()
    if listed is None or not triple:
        sys.exit(f"esphome-glue-tidy.py: {gxx} did not say where its headers are")
    system = [s for d in listed.group(1).split() for s in ("-isystem", str(pathlib.Path(d).resolve()))]
    return ["clang++", f"--target={triple}", "-nostdinc", "-nostdinc++", *kept, *system, "-c", entry["file"]]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    database = pathlib.Path(sys.argv[1])
    tidy = find_tidy()
    if not database.is_file() or tidy is None:
        print(f"esphome-glue-tidy.py: {'no clang-tidy (set CLANG_TIDY)' if tidy is None else f'no {database}'}", file=sys.stderr)
        return 2
    units = [e for e in json.loads(database.read_text())
             if OWN.search("/" + e["file"]) and "/src/esphome/components/" in "/" + e["file"]]
    if not units:
        print(f"esphome-glue-tidy.py: {database} compiles no file of this repo's components", file=sys.stderr)
        return 2
    triple = "?"
    findings: dict[str, str] = {}
    unparsed: dict[str, str] = {}
    with tempfile.TemporaryDirectory(prefix="glue-tidy-") as tmp:
        entries = []
        for e in units:
            command = clang_command(e)
            triple = command[1].split("=", 1)[1]
            entries.append({"directory": e["directory"], "file": e["file"], "arguments": command})
        (pathlib.Path(tmp) / "compile_commands.json").write_text(json.dumps(entries, indent=1))
        for e in entries:
            name = e["file"].split("/components/", 1)[1]
            r = subprocess.run([tidy, f"--config-file={ROOT / '.clang-tidy'}", "-p", tmp, e["file"]],
                               cwd=e["directory"], capture_output=True, text=True, timeout=900)
            for line in (r.stdout + r.stderr).splitlines():
                if not re.search(r": (warning|error): ", line):
                    continue
                if "clang-diagnostic-error" in line:
                    unparsed.setdefault(name, line.strip())
                elif OWN.search(line.split(": ", 1)[0]):
                    # A header's finding comes once per file that includes it.
                    where = line.split("/components/", 1)[1].strip()
                    findings.setdefault(where.split(" ", 1)[0], where)
    version = subprocess.run([tidy, "--version"], capture_output=True, text=True, timeout=30).stdout
    number = next((w for w in version.split() if w[:1].isdigit()), "?")
    print(f"== clang-tidy {number} on this repo's components as built for {triple}: "
          f"{len(findings)} finding(s) in {len(units)} file(s), {len(unparsed)} file(s) not parsed ==")
    for where in sorted(findings.values()):
        print(f"  {where}")
    for name, line in sorted(unparsed.items()):
        print(f"  not parsed: {name}: {line[-200:]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
