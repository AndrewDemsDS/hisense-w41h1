#!/usr/bin/env python3
"""Mutation testing of the pure ESPHome product code against the host tests.

    firmware/test/coverage/mutate.py [--target NAME ...] [--sample N] [--seed S]
                                     [--baseline] [--jobs J] [--out DIR]

Targets: protocol_cpp, protocol_h, map_h, uart_h (default: all four).

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
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import random
import re
import shutil
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
    "hisense_bus.cpp": COMP / "hisense_bus.cpp",
    "hisense_bus.h": COMP / "hisense_bus.h",
    "w41h1_uart_policy.h": MODULE / "w41h1_uart_policy.h",
}
TARGETS = {
    "protocol_cpp": "hisense_protocol.cpp",
    "protocol_h": "hisense_protocol.h",
    "map_h": "hisense_map.h",
    "uart_h": "w41h1_uart_policy.h",
}

P_H = "hisense_protocol.h"
M_H = "hisense_map.h"
B_H = "hisense_bus.h"
U_H = "w41h1_uart_policy.h"
REF_INC = [f"-I{TEST_DIR / 'stubinc'}", f"-I{TEST_DIR}", f"-I{REF}"]

# Object files: name -> (source, product files it reads, extra flags). A source given as a bare
# product name is taken from the work directory, so the mutant is what gets compiled.
UNITS = {
    "protocol.o": ("hisense_protocol.cpp", {"hisense_protocol.cpp", P_H}, []),
    "bus.o": ("hisense_bus.cpp", {"hisense_bus.cpp", B_H, P_H, M_H}, []),
    "t_parity.o": (TEST_DIR / "test_esphome_codec_parity.cpp", {P_H, M_H}, REF_INC),
    "t_bus.o": (TEST_DIR / "test_esphome_bus.cpp", {P_H, M_H, B_H}, []),
    "t_edge.o": (TEST_DIR / "test_esphome_edge_cases.cpp", {P_H, M_H}, []),
    "t_confirm.o": (TEST_DIR / "test_esphome_confirm.cpp", {P_H, M_H}, []),
    "t_setup.o": (TEST_DIR / "test_esphome_setup_request.cpp", {P_H, B_H}, []),
    "t_uart.o": (TEST_DIR / "test_esphome_uart_policy.cpp", {U_H}, []),
    "t_codec_gaps.o": (TEST_DIR / "test_esphome_codec_gaps.cpp", {P_H}, []),
    "t_map_gaps.o": (TEST_DIR / "test_esphome_map_gaps.cpp", {P_H, M_H, U_H}, []),
}
# Tests: name -> objects linked. Order is the order they run in: quick and broad first.
TESTS = {
    "edge": ["t_edge.o", "protocol.o"],
    "codec_gaps": ["t_codec_gaps.o", "protocol.o"],
    "map_gaps": ["t_map_gaps.o", "protocol.o"],
    "uart": ["t_uart.o"],
    "confirm": ["t_confirm.o", "protocol.o"],
    "parity": ["t_parity.o", "protocol.o", "driver.o"],
    "setup": ["t_setup.o", "bus.o", "protocol.o"],
    "bus": ["t_bus.o", "bus.o", "protocol.o"],
}
GAP_TESTS = {"codec_gaps", "map_gaps"}
# What stage 2 compares. probe.cpp includes every header, so each inline function is emitted.
EFFECT_UNITS = ["protocol.o", "bus.o", "probe.o"]
TEST_TIMEOUT_S = 20


def run(cmd, cwd, timeout=120):
    try:
        r = subprocess.run(cmd, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
        return r.returncode
    except subprocess.TimeoutExpired:
        return -999


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
        '#include "hisense_map.h"\n#include "hisense_bus.h"\n#include "w41h1_uart_policy.h"\n'
        "bool probe_reply_class_ok(unsigned char a, unsigned char b) "
        "{ return esphome::hisense_ac::reply_class_ok(a, b); }\n"
        "bool probe_uart(W41h1UartWatch *w, bool a, bool b, unsigned c) "
        "{ return w41h1_uart_restart_due(w, a, b, c); }\n"
        # Default member initialisers only show where a struct is constructed.
        "namespace H = esphome::hisense_ac;\n"
        "void probe_defaults(H::AcCommand *a, H::AcState *b, H::AcFeatures *c, H::AcFaults *d,\n"
        "    H::SetupRequestTracker *e, H::FrameAssembler *f, H::PowerIntent *g, H::SpecialState *h,\n"
        "    H::UnitView *i, H::CommandIntent *j, H::PendingCommand *k, H::OutdoorTempGate *l,\n"
        "    W41h1UartWatch *m) {\n"
        "  *a = H::AcCommand{}; *b = H::AcState{}; *c = H::AcFeatures{}; *d = H::AcFaults{};\n"
        "  *e = H::SetupRequestTracker{}; *f = H::FrameAssembler{}; *g = H::PowerIntent{};\n"
        "  *h = H::SpecialState{}; *i = H::UnitView{}; *j = H::CommandIntent{};\n"
        "  *k = H::PendingCommand{}; *l = H::OutdoorTempGate{}; *m = W41h1UartWatch{};\n"
        "}\n"
    )


def build_pristine(base: Path, tests: list, flags: list) -> dict:
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
        if run([str(exe)], base, TEST_TIMEOUT_S) != 0:
            sys.exit(f"unmutated test {t} fails: fix the suite before mutating")
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
    base, work_root, product_name, mutant_path, tests, ref_digests, flags = job
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
        if run([str(exe)], work, TEST_TIMEOUT_S) != 0:
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
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()

    out = args.out or Path(tempfile.mkdtemp(prefix="w41h1-mutation."))
    out.mkdir(parents=True, exist_ok=True)
    label = "baseline" if args.baseline else "with-gaps"
    if args.sanitize:
        label += "-san"
    if args.survivors_of:
        label += f"-of-{args.survivors_of}"
    flags = SANITIZE if args.sanitize else []
    tests = [t for t in TESTS if not (args.baseline and t in GAP_TESTS)]
    base = out / f"base-{label}"
    ref_digests = build_pristine(base, tests, flags)
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
        jobs = [(str(base), str(work_root), product_name, str(m), tests, ref_digests, flags) for m in mutants]
        started = time.time()
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
            verdicts = list(pool.map(judge, jobs, chunksize=4))
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
