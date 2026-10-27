#!/usr/bin/env python3
"""End-to-end test of the hisense_ac component: the real component and the real entity package,
built for ESPHome's `host` platform, against a model of the A/C on a pseudo-terminal, driven over
the native API as Home Assistant drives a node.

    python3 firmware/test/host_e2e/run.py              build, then the default scenarios (about two minutes)
    python3 firmware/test/host_e2e/run.py --full       adds the long ones (about three minutes)
    python3 firmware/test/host_e2e/run.py --list       the scenarios and what each takes
    python3 firmware/test/host_e2e/run.py boot faults  only these
    python3 firmware/test/host_e2e/run.py --no-build   use the binary from the last build
    python3 firmware/test/host_e2e/run.py --coverage   build with gcov, print what the scenarios reached
    python3 firmware/test/host_e2e/run.py --update-golden boot   rewrite entities.golden.json
    python3 firmware/test/host_e2e/run.py --tidy       clang-tidy over the hub and the entities, no scenarios

What is built is host.yaml: packages/hisense-ac.yaml unmodified, on a UART that is a device path.
The scenarios are in scenarios.py and the A/C in ac_model.py (firmware/test/virtual_ac.py plus the
faults a test needs). Each scenario starts its own binary on its own pseudo-terminal, API port
(picked free, on 127.0.0.1) and temporary directory, so they run side by side, four at a time
unless --jobs says otherwise.

Needs esphome and a C++ compiler. ESPHOME names the esphome command (default: the one on PATH);
the Python that runs it also has aioesphomeapi, and this script re-runs itself under it. Without
esphome the test is skipped and says so, unless --require is given. The first build fetches
PlatformIO's `native` platform; after that nothing leaves the machine.

The hub's timers are real (a 1 s bus cycle, a 4 s hold-off, 10 s between special-mode writes), and
the host platform has no clock to turn forward, so a scenario lasts as long as what it watches.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
COMPONENT = ROOT / "firmware" / "esphome" / "components" / "hisense_ac"
BUILD_TIMEOUT = 1800  # seconds for `esphome compile`
DEFAULT_JOBS = 4  # scenarios, and so host programs, at a time
NODE_NAME = "hisense-ac"  # esphome: name: in host.yaml
GLUE = ("hisense_ac.cpp", "hisense_ac.h", "hisense_climate.cpp", "hisense_switch.cpp", "hisense_select.cpp")


def esphome_command() -> str | None:
    return os.environ.get("ESPHOME") or shutil.which("esphome")


def esphome_python(esphome: str) -> str | None:
    """The interpreter of the esphome command, from its first line."""
    try:
        with open(esphome, "rb") as f:
            first = f.readline().decode(errors="replace").strip()
    except OSError:
        return None
    if not first.startswith("#!"):
        return None
    parts = first[2:].split()
    if parts and pathlib.Path(parts[0]).name == "env":
        return shutil.which(parts[1]) if len(parts) > 1 else None
    return parts[0] if parts and os.path.exists(parts[0]) else None


def reexec_with_api_client():
    """aioesphomeapi is a dependency of esphome, at the version that release pins. Use that one."""
    try:
        import aioesphomeapi  # noqa: F401

        return
    except ImportError:
        pass
    require = "--require" in sys.argv
    esphome = esphome_command()
    python = esphome_python(esphome) if esphome else None
    if python is None or os.environ.get("HOST_E2E_REEXEC") == "1":
        print("== host e2e SKIPPED: no esphome with aioesphomeapi found (set ESPHOME, or pip install esphome) ==")
        sys.exit(1 if require else 0)
    os.environ["HOST_E2E_REEXEC"] = "1"
    os.execv(python, [python, str(pathlib.Path(__file__).resolve()), *sys.argv[1:]])


reexec_with_api_client()

import asyncio  # noqa: E402
import json  # noqa: E402
import logging  # noqa: E402
import signal  # noqa: E402
import subprocess  # noqa: E402
import tempfile  # noqa: E402
import time  # noqa: E402
import traceback  # noqa: E402

sys.path.insert(0, str(HERE))
import harness  # noqa: E402
import scenarios  # noqa: E402

# A child process gets none of the caller's git state.
CLEAN_ENV = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}


class Ctx:
    """What a scenario gets: its report, and nodes that are stopped whatever happens."""

    def __init__(self, name: str, binary: pathlib.Path, workdir: pathlib.Path, update_golden: bool):
        self.r = harness.Report(name)
        self.binary = binary
        self.workdir = workdir
        self.update_golden = update_golden
        self.nodes: list[harness.Node] = []

    async def node(self, unit=None, expect_status=True) -> harness.Node:
        n = harness.Node(self.binary, self.workdir / f"node{len(self.nodes)}", unit)
        self.nodes.append(n)
        await n.start(expect_status=expect_status)
        return n

    async def close(self):
        for n in self.nodes:
            await n.stop()


async def run_one(sc, binary, root, args, gate) -> harness.Report:
    ctx = Ctx(sc.name, binary, root / sc.name, args.update_golden)
    async with gate:
        started = time.monotonic()
        try:
            await asyncio.wait_for(sc.fn(ctx), timeout=sc.seconds * 2 + 30)
        except asyncio.CancelledError:
            ctx.r.ok = False
            ctx.r.lines.append("  FAIL interrupted")
            raise
        except TimeoutError:
            ctx.r.ok = False
            ctx.r.lines.append(f"  FAIL timed out after {sc.seconds * 2 + 30:.0f} s")
        except Exception:  # noqa: BLE001 - a scenario that dies is a failed scenario, with its trace
            ctx.r.ok = False
            ctx.r.lines.append("  FAIL the scenario died:")
            ctx.r.lines += ["      " + line for line in traceback.format_exc().rstrip().splitlines()]
        finally:
            await asyncio.shield(ctx.close())
            ctx.r.seconds = time.monotonic() - started
        for n in ctx.nodes:
            if n.exit_code not in (0, None):
                ctx.r.ok = False
                ctx.r.lines.append(f"  FAIL the host binary ended with exit code {n.exit_code}")
        if not ctx.r.ok:
            for n in ctx.nodes:
                ctx.r.lines.append(f"      last command frames the unit received ({n.workdir.name}):")
                ctx.r.lines += [f"      | {x!r}" for x in n.bus.commands()[-6:]]
                ctx.r.lines.append("      last log lines of the node:")
                ctx.r.lines.append(n.log_tail(20))
    print(f"== {sc.name}: {sc.doc} ({ctx.r.seconds:.0f} s) ==")
    print("\n".join(ctx.r.lines), flush=True)
    return ctx.r


async def run_all(selected, binary, root, args) -> list[harness.Report]:
    gate = asyncio.Semaphore(args.jobs)
    tasks = [asyncio.ensure_future(run_one(sc, binary, root, args, gate)) for sc in selected]
    loop = asyncio.get_running_loop()

    def stop():
        for t in tasks:
            t.cancel()

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        loop.add_signal_handler(sig, stop)
    done = await asyncio.gather(*tasks, return_exceptions=True)
    return [d for d in done if isinstance(d, harness.Report)]


def data_dir(args) -> pathlib.Path:
    """ESPHome's data directory for this build. The instrumented build has its own, so that the two
    do not rebuild each other."""
    return args.build_dir / "coverage" if args.coverage else args.build_dir


def build(args, esphome: str) -> pathlib.Path:
    config = "host-coverage.yaml" if args.coverage else "host.yaml"
    binary = data_dir(args) / "build" / NODE_NAME / ".pioenvs" / NODE_NAME / "program"
    if args.no_build:
        if not binary.exists():
            sys.exit(f"--no-build, and there is no binary at {binary}")
        return binary
    started = time.monotonic()
    env = {**CLEAN_ENV, "ESPHOME_DATA_DIR": str(data_dir(args))}
    data_dir(args).mkdir(parents=True, exist_ok=True)
    log = data_dir(args) / "compile.log"
    with open(log, "wb") as out:
        try:
            code = subprocess.run([esphome, "compile", config], cwd=HERE, env=env, stdout=out,
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=BUILD_TIMEOUT).returncode
        except subprocess.TimeoutExpired:
            code = f"no result in {BUILD_TIMEOUT} s"
    if code != 0 or not binary.exists():
        with open(log, "rb") as f:
            f.seek(max(0, log.stat().st_size - 6000))
            print(f.read().decode(errors="replace"))
        sys.exit(f"== host e2e FAILED: `esphome compile {config}` ended with {code} ==")
    print(f"built {config} in {time.monotonic() - started:.0f} s", flush=True)
    return binary


def tidy_report(args) -> int:
    """clang-tidy, with the repository's .clang-tidy, over the glue files. firmware/scripts/cpp-lint.py
    cannot reach them: its units compile with plain host flags and these need ESPHome's headers. The
    host build has those. From ESPHome 2026.10 it is a native ninja build that writes its compile
    database itself (esphome/esphome#19214); before that, PlatformIO writes one in a second or two.
    Report only: prints the findings and their count, and returns 0 unless the tools are missing."""
    build_dir = data_dir(args) / "build" / NODE_NAME
    pio = pathlib.Path(sys.executable).parent / "pio"
    tidy = os.environ.get("CLANG_TIDY") or shutil.which("clang-tidy")
    cached = pathlib.Path.home() / ".cache" / "hisense-w41h1" / "cpp-lint" / "bin" / "clang-tidy"
    if not os.environ.get("CLANG_TIDY") and cached.exists():
        tidy = str(cached)  # the pinned one firmware/scripts/cpp-lint.sh installs
    native_db = build_dir / ".pioenvs" / NODE_NAME / "compile_commands.json"
    if tidy is None or (not native_db.exists() and not pio.exists()):
        print(f"== clang-tidy on the glue SKIPPED: {'no clang-tidy (set CLANG_TIDY)' if tidy is None else 'no compile database and no pio beside ' + sys.executable} ==")
        return 1 if args.require else 0
    project = build_dir
    if native_db.exists():
        # The native build's database names every file by its absolute path: use it where it is.
        # Its commands include g++'s precompiled header, which clang reads as the plain header
        # after a warning that the repository's .clang-tidy turns into an error: silence that one.
        db_dir = native_db.parent
        extra = ["--extra-arg=-Wno-ignored-gch"]
        build_dir = data_dir(args) / "tidy" / NODE_NAME
        shutil.rmtree(build_dir, ignore_errors=True)
        build_dir.mkdir(parents=True)
    else:
        # On a copy of the generated project: for this target PlatformIO deletes the project's build
        # output (.pioenvs), and the program the scenarios run would go with it.
        build_dir = data_dir(args) / "tidy" / NODE_NAME
        shutil.rmtree(build_dir, ignore_errors=True)
        shutil.copytree(project, build_dir, ignore=shutil.ignore_patterns(".pio*", ".piolibdeps", "compile_commands.json"))
        with open(build_dir / "compiledb.log", "wb") as out:
            code = subprocess.run([str(pio), "run", "-t", "compiledb"], cwd=build_dir, env=CLEAN_ENV, stdout=out,
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=600).returncode
        if code != 0 or not (build_dir / "compile_commands.json").exists():
            print(f"== clang-tidy on the glue SKIPPED: `pio run -t compiledb` ended with {code} (see {build_dir / 'compiledb.log'}) ==")
            return 1 if args.require else 0
        db_dir = project = build_dir
        extra = []
    findings: dict[str, str] = {}
    for name in (g for g in GLUE if g.endswith(".cpp")):
        log = build_dir / f"tidy-{name}.log"
        with open(log, "wb") as out:
            subprocess.run([tidy, f"--config-file={ROOT / '.clang-tidy'}", "-p", str(db_dir), *extra,
                            str(project / "src" / "esphome" / "components" / "hisense_ac" / name)], cwd=project,
                           stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=900)
        for line in open(log, encoding="utf-8", errors="replace"):
            # A header's finding comes once per file that includes it.
            if (": warning: " in line or ": error: " in line) and "components/hisense_ac/" in line:
                where = line.split("components/hisense_ac/", 1)[1].strip()
                findings.setdefault(where.split(" ", 1)[0], where)
    version = subprocess.run([tidy, "--version"], stdout=subprocess.PIPE, text=True, timeout=30).stdout
    number = next((w for w in version.split() if w[:1].isdigit()), "?")
    print(f"== clang-tidy {number} on the glue: {len(findings)} finding(s) ==")
    for where in sorted(findings.values()):
        print(f"  {where}")
    print("  not covered: wifi_setup_mode.cpp, which needs wifi: and so is not part of a host build")
    return 0


def clear_coverage(args):
    """The counters add up across runs of the program, so the ones of an earlier run go first."""
    for gcda in (data_dir(args) / "build" / NODE_NAME / ".pioenvs").rglob("*.gcda"):
        gcda.unlink()


def coverage_report(args):
    """Per file of the component, the lines the scenarios reached, and per function of the glue
    files how often it ran. Report only: nothing here fails the run."""
    build_dir = data_dir(args) / "build" / NODE_NAME
    gcda = sorted(g for g in (build_dir / ".pioenvs" / NODE_NAME).rglob("*.gcda") if "hisense_ac" in str(g))
    print(f"\n== what the scenarios reached in components/hisense_ac ({len(gcda)} instrumented objects) ==")
    if not gcda:
        print("  no coverage data was written")
        return
    report = data_dir(args) / "gcov.jsonl"
    with open(report, "wb") as out:
        subprocess.run(["gcov", "--json-format", "--stdout", "--demangled-names", *map(str, gcda)], cwd=build_dir,
                       stdout=out, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, timeout=300)
    # A header is compiled into several objects: a line or a function counts as reached when any
    # of them ran it.
    lines: dict[str, dict[int, int]] = {}
    functions: dict[str, dict[str, int]] = {}
    for doc in open(report, encoding="utf-8", errors="replace"):  # one document per object, read one at a time
        if not doc.startswith("{"):
            continue
        for f in json.loads(doc).get("files", []):
            name = pathlib.Path(f["file"]).name
            if "components/hisense_ac/" not in f["file"]:
                continue
            for ln in f.get("lines", []):
                per = lines.setdefault(name, {})
                per[ln["line_number"]] = per.get(ln["line_number"], 0) + ln["count"]
            for fn in f.get("functions", []):
                per = functions.setdefault(name, {})
                label = fn.get("demangled_name") or fn["name"]
                per[label] = per.get(label, 0) + fn["execution_count"]
    for name in sorted(lines, key=lambda n: (n not in GLUE, n)):
        hit = sum(1 for c in lines[name].values() if c)
        total = len(lines[name])
        fns = functions.get(name, {})
        print(f"  {name:22} {100 * hit / total:5.1f}% of {total:3} lines, "
              f"{sum(1 for c in fns.values() if c)} of {len(fns)} functions{'' if name in GLUE else '  (not glue)'}")
    for name in GLUE:
        fns = functions.get(name, {})
        if not fns:
            continue
        print(f"  {name}:")
        for label, count in sorted(fns.items(), key=lambda kv: (kv[1] == 0, kv[0])):
            short = label.replace("esphome::hisense_ac::", "")
            print(f"      {count if count else 'never':>8}  {short}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("names", nargs="*", help="scenarios to run (default: all that are not --full only)")
    ap.add_argument("--full", action="store_true", help="also the long scenarios")
    ap.add_argument("--list", action="store_true", help="list the scenarios and exit")
    ap.add_argument("--no-build", action="store_true", help="use the binary of the last build")
    ap.add_argument("--build-dir", type=pathlib.Path, default=HERE / "build",
                    help="where esphome builds (default: build/ beside this file, which git ignores)")
    ap.add_argument("--jobs", type=int, default=DEFAULT_JOBS,
                    help=f"scenarios at a time (default: {DEFAULT_JOBS}; 0: all of them at once)")
    ap.add_argument("--keep", action="store_true", help="keep the temporary directory with every node's log")
    ap.add_argument("--coverage", action="store_true", help="build host-coverage.yaml and report what was reached")
    ap.add_argument("--tidy", action="store_true",
                    help="no scenarios: clang-tidy over the hub and the entities, with the build's compile database")
    ap.add_argument("--update-golden", action="store_true", help="rewrite entities.golden.json from this build (run boot)")
    ap.add_argument("--require", action="store_true", help="fail where the default is to skip (no esphome)")
    args = ap.parse_args()
    args.build_dir = args.build_dir.resolve()

    known = {s.name: s for s in scenarios.SCENARIOS}
    if args.list:
        for s in scenarios.SCENARIOS:
            print(f"  {s.name:36} {s.seconds:4.0f} s  {'--full  ' if s.full else '        '}{s.doc}")
        return 0
    unknown = [n for n in args.names if n not in known]
    if unknown:
        sys.exit(f"no such scenario: {', '.join(unknown)} (see --list)")
    selected = [known[n] for n in args.names] if args.names else [s for s in scenarios.SCENARIOS if args.full or not s.full]
    args.jobs = args.jobs or len(selected)
    # The longest first, so that the last slot to free up is not left with the longest scenario.
    selected.sort(key=lambda sc: -sc.seconds)

    esphome = esphome_command()
    if esphome is None and not args.no_build:
        print("== host e2e SKIPPED: esphome is not installed (set ESPHOME to its path) ==")
        return 1 if args.require else 0
    binary = build(args, esphome)
    if args.tidy:
        return tidy_report(args)
    if args.coverage:
        clear_coverage(args)

    logging.getLogger("aioesphomeapi").setLevel(logging.CRITICAL)
    root = pathlib.Path(tempfile.mkdtemp(prefix="hisense-e2e-"))
    started = time.monotonic()
    reports: list[harness.Report] = []
    try:
        reports = asyncio.run(run_all(selected, binary, root, args))
    except KeyboardInterrupt:
        pass
    finally:
        harness.kill_all()
    wall = time.monotonic() - started
    ok = len(reports) == len(selected) and all(r.ok for r in reports)
    if args.coverage and reports:
        coverage_report(args)
    if args.keep or not ok:
        print(f"logs kept in {root}")
    else:
        shutil.rmtree(root, ignore_errors=True)
    checks = sum(r.checks for r in reports)
    known_failures = sum(r.known for r in reports)
    failed = [s.name for s in selected if s.name not in {r.name for r in reports if r.ok}]
    if ok:
        print(f"== host e2e OK: {len(reports)} scenarios, {checks} checks, {known_failures} known failures, "
              f"{wall:.0f} s ==")
        return 0
    print(f"== host e2e FAILED: {', '.join(failed)} ({wall:.0f} s) ==")
    return 1


if __name__ == "__main__":
    sys.exit(main())
