#!/usr/bin/env python3
"""Mutation testing of the pure ESPHome product code against the host tests.

    firmware/test/coverage/mutate.py [--target NAME ...] [--sample N] [--seed S]
                                     [--baseline] [--jobs J] [--mem-limit MB] [--out DIR]

Targets: protocol_cpp, protocol_h, mapping_h, special_h, confirm_h, telemetry_h, commands_h, entities_h,
uart_h
(default: all of them). hisense_map.h only includes the four parts, so it is not a target.

For each target the mutants come from universalmutator (`uvx --from universalmutator mutate`).
Each mutant then goes through three stages:

  1. It is compiled. A mutant that does not compile is INVALID and is not counted.
  2. Every product translation unit is compiled at -O2 and compared with the unmutated build
     (a probe unit emits every inline function and default-constructs every struct of the
     headers). Byte-identical object code means the product code the host can compile did not
     change. For a .cpp target that settles it: NO_EFFECT, not counted.
  3. The host tests that read the mutated file are rebuilt and run. A non-zero exit or a timeout
     KILLS the mutant. A mutant every test passes is a SURVIVOR, unless stage 2 found no change:
     then it is NO_EFFECT (a respelled number, or a header constant that only the ESPHome glue
     reads, which the host cannot compile).

Score = killed / (killed + survivors).

--baseline leaves out the gap tests (test_esphome_*_gaps.cpp), to see what they add.
--sample N takes N mutants per target, drawn with random.Random(seed), instead of all of them.
--survivors-of LABEL runs only the mutants an earlier run in the same --out left as survivors
  (LABEL is `baseline` or `with-gaps`): quick, for checking a new test against them.
--sanitize builds the tests with AddressSanitizer and UBSan, so a mutant that only reads or
  writes out of bounds is killed too. About three times slower.

Slow (minutes), needs uvx: not part of the default suite. Everything is written to --out
(default: a fresh temporary directory), never into the tree.

A mutant is a program nobody wrote: it may loop while it prints, or allocate without bound. So
every test binary runs fenced in, and the limits are printed at the start:

  - its output goes to /dev/null and is never read into this process, and a file it writes
    itself cannot grow past 16 MB (RLIMIT_FSIZE). No core file is written;
  - it has a memory limit of its own (--mem-limit, 512 MB by default). With systemd-run and a
    reachable user manager it runs in a scope with MemoryMax and no swap, so the kernel kills
    that process and nothing else. Otherwise the limit is RLIMIT_AS on the process. Under
    --sanitize the fallback is skipped, with a warning: the sanitizers reserve terabytes of
    address space and would not start;
  - it is killed after 20 s;
  - at most 4 mutants run at once unless --jobs says otherwise, and the run is refused when
    --jobs times --mem-limit is more than a quarter of the machine's memory.

A mutant that is killed by a limit counts as KILLED, like one that fails a check.

The compilers are not fenced in. Each worker runs one g++ at a time, which is what --jobs
bounds.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import random
import re
import resource
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent.parent
REPO = TEST_DIR.parent.parent
COMP = REPO / "firmware/esphome/components/hisense_ac"
MODULE = REPO / "firmware/esphome/components/w41h1_module"
REF = TEST_DIR / "reference-driver"

# Product files, as they are named inside a work directory.
PRODUCT = {
    "hisense_protocol.cpp": COMP / "hisense_protocol.cpp",
    "hisense_protocol.h": COMP / "hisense_protocol.h",
    "hisense_map.h": COMP / "hisense_map.h",
    "hisense_mapping.h": COMP / "hisense_mapping.h",
    "hisense_special.h": COMP / "hisense_special.h",
    "hisense_confirm.h": COMP / "hisense_confirm.h",
    "hisense_telemetry.h": COMP / "hisense_telemetry.h",
    "hisense_commands.h": COMP / "hisense_commands.h",
    "hisense_entities.h": COMP / "hisense_entities.h",
    "hisense_bus.cpp": COMP / "hisense_bus.cpp",
    "hisense_bus.h": COMP / "hisense_bus.h",
    "w41h1_uart_policy.h": MODULE / "w41h1_uart_policy.h",
}
TARGETS = {
    "protocol_cpp": "hisense_protocol.cpp",
    "protocol_h": "hisense_protocol.h",
    "mapping_h": "hisense_mapping.h",
    "special_h": "hisense_special.h",
    "confirm_h": "hisense_confirm.h",
    "telemetry_h": "hisense_telemetry.h",
    "commands_h": "hisense_commands.h",
    "entities_h": "hisense_entities.h",
    "uart_h": "w41h1_uart_policy.h",
}

P_H = "hisense_protocol.h"
# hisense_map.h and the four headers it includes: who reads one reads them all.
M_H = ("hisense_map.h", "hisense_mapping.h", "hisense_special.h", "hisense_confirm.h", "hisense_telemetry.h")
C_H = "hisense_commands.h"
E_H = "hisense_entities.h"
B_H = "hisense_bus.h"
U_H = "w41h1_uart_policy.h"
REF_INC = [f"-I{TEST_DIR / 'stubinc'}", f"-I{TEST_DIR}", f"-I{REF}"]

# Object files: name -> (source, product files it reads, extra flags). A source given as a bare
# product name is taken from the work directory, so the mutant is what gets compiled.
UNITS = {
    "protocol.o": ("hisense_protocol.cpp", {"hisense_protocol.cpp", P_H}, []),
    "bus.o": ("hisense_bus.cpp", {"hisense_bus.cpp", B_H, P_H}, []),
    "t_parity.o": (TEST_DIR / "test_esphome_codec_parity.cpp", {P_H, *M_H}, REF_INC),
    "t_bus.o": (TEST_DIR / "test_esphome_bus.cpp", {P_H, *M_H, B_H}, []),
    "t_edge.o": (TEST_DIR / "test_esphome_edge_cases.cpp", {P_H, *M_H}, []),
    "t_confirm.o": (TEST_DIR / "test_esphome_confirm.cpp", {P_H, *M_H, C_H}, []),
    "t_entities.o": (TEST_DIR / "test_esphome_entities.cpp", {P_H, *M_H, E_H}, []),
    "t_setup.o": (TEST_DIR / "test_esphome_setup_request.cpp", {P_H, B_H}, []),
    "t_uart.o": (TEST_DIR / "test_esphome_uart_policy.cpp", {U_H}, []),
    "t_codec_gaps.o": (TEST_DIR / "test_esphome_codec_gaps.cpp", {P_H}, []),
    "t_map_gaps.o": (TEST_DIR / "test_esphome_map_gaps.cpp", {P_H, *M_H, U_H}, []),
}
# Tests: name -> objects linked. Order is the order they run in: quick and broad first.
TESTS = {
    "edge": ["t_edge.o", "protocol.o"],
    "codec_gaps": ["t_codec_gaps.o", "protocol.o"],
    "map_gaps": ["t_map_gaps.o", "protocol.o"],
    "uart": ["t_uart.o"],
    "entities": ["t_entities.o", "protocol.o"],
    "confirm": ["t_confirm.o", "protocol.o"],
    "parity": ["t_parity.o", "protocol.o", "driver.o"],
    "setup": ["t_setup.o", "bus.o", "protocol.o"],
    "bus": ["t_bus.o", "bus.o", "protocol.o"],
}
GAP_TESTS = {"codec_gaps", "map_gaps"}
# What stage 2 compares. probe.cpp includes every header, so each inline function is emitted.
EFFECT_UNITS = ["protocol.o", "bus.o", "probe.o"]
TEST_TIMEOUT_S = 20
DEFAULT_JOBS = 4            # more is --jobs, within what max_jobs() allows
DEFAULT_MEM_LIMIT_MB = 512  # per test binary. The host tests peak at a few MB
MAX_FILE_BYTES = 16 << 20   # the largest file a test binary may write
TIMED_OUT = -999


def run(cmd, cwd, timeout=120):
    try:
        r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
        return r.returncode
    except subprocess.TimeoutExpired:
        return TIMED_OUT


# ---- Running a mutated binary ---------------------------------------------------------------
def total_memory_bytes() -> int:
    return os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")


def max_jobs(mem_limit_mb: int, total_bytes: int) -> int:
    """How many mutants may run at once: together they may claim a quarter of the machine."""
    return int((total_bytes // 4) // (mem_limit_mb << 20))


def systemd_scope_works() -> bool:
    """Whether a memory-limited scope can be started: systemd-run is there and the user manager
    answers. Asked once, by running `true` in one."""
    if shutil.which("systemd-run") is None:
        return False
    cmd = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=64M", "-p", "MemorySwapMax=0", "--", "true"]
    try:
        return subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=20).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def choose_limits(mem_limit_mb: int, sanitize: bool, mode: str = "auto") -> dict:
    """The fence around a test binary, as a plain dict so it travels to the worker processes.
    mode: auto (a scope when one can be started, else the rlimit), scope, rlimit or none."""
    if mode == "auto":
        mode = "scope" if systemd_scope_works() else "rlimit"
    warning = None
    if mode == "rlimit" and sanitize:
        mode = "none"
        warning = ("WARNING: no memory limit on the test binaries. systemd-run cannot start a scope here, and "
                   "RLIMIT_AS cannot be used with --sanitize (the sanitizers reserve terabytes of address space). "
                   "Keep --jobs low, or run the whole command in a container or scope with a memory limit.")
    return {"mode": mode, "mem_bytes": mem_limit_mb << 20, "timeout_s": TEST_TIMEOUT_S, "warning": warning}


def describe_limits(limits: dict, jobs: int) -> str:
    how = {"scope": "systemd-run scope, MemoryMax with no swap: the kernel kills that process alone",
           "rlimit": "RLIMIT_AS on the process (no systemd user scope here)",
           "none": "NONE"}[limits["mode"]]
    return (f"limits per test binary: memory {limits['mem_bytes'] >> 20} MB ({how}), "
            f"time {limits['timeout_s']} s, output to /dev/null, files it writes {MAX_FILE_BYTES >> 20} MB, no core file. "
            f"Workers: {jobs} (at most {jobs * (limits['mem_bytes'] >> 20)} MB of test binaries at once)")


def run_fenced(exe, cwd, limits: dict) -> int:
    """Run one test binary inside the limits. Returns its exit status, a negative signal number
    when it was killed, or TIMED_OUT. Its output is never read: it goes to /dev/null."""
    def fence():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_FILE_BYTES, MAX_FILE_BYTES))
        if limits["mode"] == "rlimit":
            resource.setrlimit(resource.RLIMIT_AS, (limits["mem_bytes"], limits["mem_bytes"]))

    cmd = [str(exe)]
    if limits["mode"] == "scope":
        # --scope makes systemd-run exec the command in the new scope, so the child is the test
        # binary itself: the rlimits above are inherited and the timeout below kills the right process.
        cmd = ["systemd-run", "--user", "--scope", "-q", "-p", f"MemoryMax={limits['mem_bytes']}",
               "-p", "MemorySwapMax=0", "--", str(exe)]
    proc = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, preexec_fn=fence, start_new_session=True)
    try:
        return proc.wait(timeout=limits["timeout_s"])
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait()
        return TIMED_OUT


def kills(exe, cwd, limits: dict) -> bool:
    """Whether running this test binary kills the mutant: a failed check, a crash, a limit or the
    timeout. Only a clean exit lets it live."""
    return run_fenced(exe, cwd, limits) != 0


SANITIZE = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]


def compile_unit(work: Path, name: str, out: Path, extra_flags: list) -> bool:
    src, _deps, extra = UNITS[name]
    src = work / src if isinstance(src, str) else src
    cmd = ["g++", "-std=c++17", "-w", "-O0", *extra_flags, f"-I{work}", *extra, "-c", str(src), "-o", str(out)]
    return run(cmd, work) == 0


def compile_effect(work: Path, name: str, out: Path) -> bool:
    """One product translation unit at -O2, for the object-code comparison."""
    src = "probe.cpp" if name == "probe.o" else UNITS[name][0]
    cmd = ["g++", "-std=c++17", "-w", "-O2", "-fkeep-inline-functions", "-I.", "-c", src, "-o", str(out)]
    return run(cmd, work) == 0


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fresh_work(work: Path):
    work.mkdir(parents=True, exist_ok=True)
    for name, src in PRODUCT.items():
        shutil.copyfile(src, work / name)
    (work / "probe.cpp").write_text(
        '#include "hisense_map.h"\n#include "hisense_commands.h"\n#include "hisense_entities.h"\n#include "hisense_bus.h"\n'
        '#include "w41h1_uart_policy.h"\n'
        "bool probe_reply_class_ok(unsigned char a, unsigned char b) "
        "{ return esphome::hisense_ac::reply_class_ok(a, b); }\n"
        "bool probe_uart(W41H1UartWatch *w, bool a, bool b, unsigned c) "
        "{ return w41h1_uart_restart_due(w, a, b, c); }\n"
        # Default member initialisers only show where a struct is constructed.
        "namespace H = esphome::hisense_ac;\n"
        "void probe_defaults(H::AcCommand *a, H::AcState *b, H::AcFeatures *c, H::AcFaults *d,\n"
        "    H::SetupRequestTracker *e, H::FrameAssembler *f, H::PowerIntent *g, H::SpecialState *h,\n"
        "    H::UnitView *i, H::CommandIntent *j, H::PendingCommand *k, H::OutdoorTempGate *l,\n"
        "    W41H1UartWatch *m) {\n"
        "  *a = H::AcCommand{}; *b = H::AcState{}; *c = H::AcFeatures{}; *d = H::AcFaults{};\n"
        "  *e = H::SetupRequestTracker{}; *f = H::FrameAssembler{}; *g = H::PowerIntent{};\n"
        "  *h = H::SpecialState{}; *i = H::UnitView{}; *j = H::CommandIntent{};\n"
        "  *k = H::PendingCommand{}; *l = H::OutdoorTempGate{}; *m = W41H1UartWatch{};\n"
        "}\n"
    )


def build_pristine(base: Path, tests: list, flags: list, limits: dict) -> dict:
    """Unmutated objects, the -O2 reference digests, and a check that every test passes."""
    fresh_work(base)
    cmd = ["g++", "-std=c++11", "-w", *REF_INC, "-c", str(REF / "hisense_rs485.cpp"), "-o", str(base / "driver.o")]
    if run(cmd, base) != 0:
        sys.exit("reference driver does not compile")
    needed = {o for t in tests for o in TESTS[t] if o != "driver.o"}
    for name in sorted(needed):
        if not compile_unit(base, name, base / name, flags):
            sys.exit(f"unmutated {name} does not compile")
    for t in tests:
        exe = base / f"test_{t}"
        if run(["g++", *flags, *[str(base / o) for o in TESTS[t]], "-o", str(exe)], base) != 0:
            sys.exit(f"unmutated test {t} does not link")
        if kills(exe, base, limits):
            sys.exit(f"unmutated test {t} fails inside the limits: fix the suite, or raise --mem-limit, before mutating")
    digests = {}
    for name in EFFECT_UNITS:
        out = base / f"ref_{name}"
        if not compile_effect(base, name, out):
            sys.exit(f"unmutated {name} does not compile at -O2")
        digests[name] = digest(out)
    return digests


def describe(original: list, mutant_path: Path) -> tuple:
    mutated = mutant_path.read_text(encoding="utf-8", errors="replace").splitlines()
    for i, line in enumerate(original):
        if i >= len(mutated) or mutated[i] != line:
            after = mutated[i].strip() if i < len(mutated) else ""
            # A mutant that adds a line (break; / continue;) shows it after the one it follows.
            if len(mutated) > len(original) and i + 1 < len(mutated) and after == line.strip():
                after += " " + mutated[i + 1].strip()
            return i + 1, line.strip(), after
    if len(mutated) > len(original):
        return len(original), original[-1].strip(), original[-1].strip() + " " + mutated[-1].strip()
    return 0, "", ""


def judge(job):
    """Run one mutant. Executed in a worker process."""
    base, work_root, product_name, mutant_path, tests, ref_digests, flags, limits = job
    work = Path(work_root) / f"w{os.getpid()}"
    base = Path(base)
    fresh_work(work)
    shutil.copyfile(mutant_path, work / product_name)

    # Stages 1 and 2: does it compile, and does any product object code change.
    changed = False
    for name in EFFECT_UNITS:
        out = work / f"eff_{name}"
        if not compile_effect(work, name, out):
            return "INVALID", None
        if digest(out) != ref_digests[name]:
            changed = True
    if not changed and product_name.endswith(".cpp"):
        return "NO_EFFECT", None
    unkilled = "SURVIVED" if changed else "NO_EFFECT"

    # Stage 3. For a header it runs even when no product object changed: a test may still read
    # the mutated constant or default.
    rebuilt = {}
    for t in tests:
        objs = []
        relink = False
        for o in TESTS[t]:
            if o != "driver.o" and product_name in UNITS[o][1]:
                if o not in rebuilt:
                    out = work / o
                    rebuilt[o] = out if compile_unit(work, o, out, flags) else None
                if rebuilt[o] is None:
                    return "INVALID", None
                objs.append(str(rebuilt[o]))
                relink = True
            else:
                objs.append(str(base / o))
        if not relink:
            continue
        exe = work / f"test_{t}"
        if run(["g++", *flags, *objs, "-o", str(exe)], work) != 0:
            return "INVALID", None
        if kills(exe, work, limits):
            return "KILLED", t
    return unkilled, None


def generate(product_name: str, out: Path) -> list:
    mdir = out / "mutants" / product_name
    if not mdir.exists():
        mdir.mkdir(parents=True)
        cmd = ["uvx", "--from", "universalmutator", "mutate", str(PRODUCT[product_name]), "cpp",
               "--noCheck", "--mutantDir", str(mdir)]
        if run(cmd, out, timeout=1800) != 0:
            sys.exit(f"universalmutator failed on {product_name}")
    key = lambda p: int(re.search(r"\.mutant\.(\d+)\.", p.name).group(1))  # noqa: E731
    return sorted(mdir.iterdir(), key=key)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", action="append", choices=sorted(TARGETS))
    ap.add_argument("--sample", type=int, default=0, help="mutants per target (0 = all)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--baseline", action="store_true", help="leave out the gap tests")
    ap.add_argument("--survivors-of", metavar="LABEL", help="only the survivors of an earlier run in --out")
    ap.add_argument("--sanitize", action="store_true", help="build the tests with ASan and UBSan")
    ap.add_argument("--jobs", type=int, default=None,
                    help=f"mutants judged at once (default {DEFAULT_JOBS}, or fewer on a small machine)")
    ap.add_argument("--mem-limit", type=int, default=DEFAULT_MEM_LIMIT_MB, metavar="MB",
                    help=f"memory limit of each test binary (default {DEFAULT_MEM_LIMIT_MB})")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()

    # The limits come first: nothing is generated, compiled or run before they are settled.
    if args.mem_limit < 16:
        sys.exit("--mem-limit below 16 MB: no test binary starts in that")
    total = total_memory_bytes()
    allowed = max_jobs(args.mem_limit, total)
    if allowed < 1:
        sys.exit(f"refused: one test binary at --mem-limit {args.mem_limit} MB is more than a quarter of this "
                 f"machine's memory ({total >> 20} MB). Lower --mem-limit.")
    jobs = args.jobs if args.jobs is not None else max(1, min(DEFAULT_JOBS, os.cpu_count() or 1, allowed))
    if jobs < 1:
        sys.exit("--jobs must be at least 1")
    if jobs > allowed:
        sys.exit(f"refused: --jobs {jobs} at --mem-limit {args.mem_limit} MB is {jobs * args.mem_limit} MB, more than "
                 f"a quarter of this machine's memory ({total >> 20} MB). At most {allowed} here.")
    limits = choose_limits(args.mem_limit, args.sanitize)
    print(describe_limits(limits, jobs), flush=True)
    if limits["warning"]:
        print(limits["warning"], flush=True)

    # Absolute: the compilers run with the work directory as their own, where a relative --out
    # would name another place.
    out = (args.out or Path(tempfile.mkdtemp(prefix="w41h1-mutation."))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    label = "baseline" if args.baseline else "with-gaps"
    if args.sanitize:
        label += "-san"
    if args.survivors_of:
        label += f"-of-{args.survivors_of}"
    flags = SANITIZE if args.sanitize else []
    tests = [t for t in TESTS if not (args.baseline and t in GAP_TESTS)]
    base = out / f"base-{label}"
    ref_digests = build_pristine(base, tests, flags, limits)
    print(f"tests: {', '.join(tests)}", flush=True)

    summary = {}
    for target in args.target or list(TARGETS):
        product_name = TARGETS[target]
        mutants = generate(product_name, out)
        total = len(mutants)
        if args.survivors_of:
            earlier = json.loads((out / f"{target}-{args.survivors_of}.json").read_text())
            keep = {r["mutant"] for r in earlier if r["verdict"] == "SURVIVED"}
            mutants = [m for m in mutants if m.name in keep]
        if args.sample and args.sample < total:
            mutants = sorted(random.Random(args.seed).sample(mutants, args.sample), key=lambda p: p.name)
        original = PRODUCT[product_name].read_text(encoding="utf-8").splitlines()
        work_root = out / f"work-{label}"
        work = [(str(base), str(work_root), product_name, str(m), tests, ref_digests, flags, limits) for m in mutants]
        started = time.time()
        with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
            verdicts = list(pool.map(judge, work, chunksize=4))
        counts = {"KILLED": 0, "SURVIVED": 0, "NO_EFFECT": 0, "INVALID": 0}
        rows = []
        for m, (verdict, by) in zip(mutants, verdicts):
            counts[verdict] += 1
            line, before, after = describe(original, m)
            rows.append({"mutant": m.name, "verdict": verdict, "killed_by": by, "line": line,
                         "before": before, "after": after})
        judged = counts["KILLED"] + counts["SURVIVED"]
        score = 100.0 * counts["KILLED"] / judged if judged else 0.0
        summary[target] = {"file": product_name, "generated": total, "run": len(mutants), **counts,
                           "score": round(score, 1), "seconds": round(time.time() - started)}
        (out / f"{target}-{label}.json").write_text(json.dumps(rows, indent=1))
        with open(out / f"{target}-{label}-survivors.txt", "w", encoding="utf-8") as fh:
            for r in sorted((r for r in rows if r["verdict"] == "SURVIVED"), key=lambda r: r["line"]):
                fh.write(f"{product_name}:{r['line']}: {r['before']}  ==>  {r['after']}   [{r['mutant']}]\n")
        with open(out / f"{target}-{label}-no-effect.txt", "w", encoding="utf-8") as fh:
            for r in sorted((r for r in rows if r["verdict"] == "NO_EFFECT"), key=lambda r: r["line"]):
                fh.write(f"{product_name}:{r['line']}: {r['before']}  ==>  {r['after']}   [{r['mutant']}]\n")
        s = summary[target]
        print(f"{target:13} {product_name:22} generated {total:5}  run {s['run']:5}  killed {s['KILLED']:5}  "
              f"survived {s['SURVIVED']:4}  no-effect {s['NO_EFFECT']:5}  invalid {s['INVALID']:5}  "
              f"score {s['score']:5.1f}%  ({s['seconds']} s)", flush=True)
        shutil.rmtree(work_root, ignore_errors=True)

    summary_path = out / f"summary-{label}.json"
    if summary_path.exists():
        summary = {**json.loads(summary_path.read_text()), **summary}
    summary_path.write_text(json.dumps(summary, indent=1))
    print(f"results: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
