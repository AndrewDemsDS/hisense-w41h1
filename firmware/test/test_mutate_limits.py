#!/usr/bin/env python3
"""The fence coverage/mutate.py puts around a mutated test binary.

A mutant is a program nobody wrote. Two kinds took a development machine down: one that allocates
without bound and one that prints without end (with a harness that read the output into memory).
This test feeds mutate.py's runner both, as tiny programs written here, not as real mutants, and
checks that each is stopped, that each counts as KILLED, and that this process did not grow while
it happened. It also holds the start-up rules: the default worker count, and the refusal of more
workers than a quarter of the machine's memory allows.

No mutant is generated and no test of the suite is mutated: uvx and universalmutator are not
needed. It compiles three programs of a few lines with g++. About 10 s, most of it waiting for the two timeouts.
"""

import importlib.util
import pathlib
import resource
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
MUTATE = HERE / "coverage" / "mutate.py"
spec = importlib.util.spec_from_file_location("mutate", MUTATE)
mutate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mutate)

ok = True


def check(cond, msg):
    global ok
    print(("  ok  " if cond else "  FAIL ") + msg)
    ok = ok and bool(cond)


def rss_mb():
    """This process's peak resident memory so far."""
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024


PROGRAMS = {
    # Allocates and touches 1 MB at a time, for ever. Exits 0 if it ever got 4 GB.
    "hog": """
#include <cstdlib>
#include <cstring>
int main() {
  for (long mb = 0; mb < 4096; mb++) {
    char *p = static_cast<char *>(std::malloc(1 << 20));
    if (p == nullptr) return 3;
    std::memset(p, 0x5A, 1 << 20);
  }
  return 0;
}
""",
    # Prints without end, to stdout and stderr.
    "chatter": """
#include <cstdio>
int main() {
  for (;;) {
    std::puts("FAIL: this line is printed for ever, and whoever reads it into memory is lost");
    std::fputs("and so is this one\\n", stderr);
  }
}
""",
    # Writes a file without end.
    "scribble": """
#include <cstdio>
int main() {
  std::FILE *f = std::fopen("scribble.out", "wb");
  static char block[1 << 16];
  for (;;) {
    if (std::fwrite(block, 1, sizeof block, f) != sizeof block) return 4;
  }
}
""",
    "fine": "int main() { return 0; }\n",
    "failing": "int main() { return 1; }\n",
}

print("== mutate.py: the fence around a mutated test binary ==")
with tempfile.TemporaryDirectory(prefix="mutate-limits-") as tmp:
    tmp = pathlib.Path(tmp)
    for name, source in PROGRAMS.items():
        (tmp / f"{name}.cpp").write_text(source)
        subprocess.run(["g++", "-std=c++17", "-O0", "-w", str(tmp / f"{name}.cpp"), "-o", str(tmp / name)], check=True, timeout=120)

    scope = mutate.systemd_scope_works()
    modes = (["scope"] if scope else []) + ["rlimit"]
    print(f"  --   systemd user scope: {'available' if scope else 'not available here, the RLIMIT_AS fallback is what runs'}")
    for mode in modes:
        limits = mutate.choose_limits(64, sanitize=False, mode=mode)
        limits["timeout_s"] = 2
        before = rss_mb()

        start = time.monotonic()
        code = mutate.run_fenced(tmp / "hog", tmp, limits)
        took = time.monotonic() - start
        check(code != 0 and code != mutate.TIMED_OUT and took < limits["timeout_s"],
              f"{mode}: a binary that allocates without bound is stopped by the 64 MB limit, not by the clock "
              f"(status {code}, {took:.1f} s)")
        check(mutate.kills(tmp / "hog", tmp, limits), f"{mode}: and it counts as KILLED")

        start = time.monotonic()
        code = mutate.run_fenced(tmp / "chatter", tmp, limits)
        took = time.monotonic() - start
        check(code == mutate.TIMED_OUT and took < limits["timeout_s"] + 5,
              f"{mode}: a binary that prints without end is killed at the timeout ({took:.1f} s)")
        check(mutate.kills(tmp / "chatter", tmp, limits), f"{mode}: and it counts as KILLED")
        leftover = subprocess.run(["pgrep", "-f", str(tmp / "chatter")], capture_output=True, text=True).stdout.split()
        check(not leftover, f"{mode}: nothing of it is left running")

        code = mutate.run_fenced(tmp / "scribble", tmp, limits)
        size = (tmp / "scribble.out").stat().st_size
        check(code != 0 and size <= mutate.MAX_FILE_BYTES, f"{mode}: a binary that writes a file without end stops at {size >> 20} MB")
        (tmp / "scribble.out").unlink()

        check(not mutate.kills(tmp / "fine", tmp, limits), f"{mode}: a binary that exits 0 is not killed: the mutant survives")
        check(mutate.kills(tmp / "failing", tmp, limits), f"{mode}: a binary that exits 1 kills the mutant, as before")

        grew = rss_mb() - before
        check(grew < 8, f"{mode}: this process did not grow while they ran ({grew:.1f} MB): no output was read into it")

    print("== mutate.py: the limits it starts with ==")
    GB = 1 << 30
    check(mutate.DEFAULT_JOBS <= 4, f"the default worker count is {mutate.DEFAULT_JOBS}: at most 4")
    check(mutate.max_jobs(512, 16 * GB) == 8 and mutate.max_jobs(512, 2 * GB) == 1 and mutate.max_jobs(512, 1 * GB) == 0,
          "workers times the per-mutant limit may claim a quarter of the machine: 8 on 16 GB, 1 on 2 GB, none on 1 GB")
    limits = mutate.choose_limits(512, sanitize=True, mode="rlimit")
    check(limits["mode"] == "none" and "WARNING" in (limits["warning"] or ""),
          "--sanitize without a systemd scope: RLIMIT_AS is skipped, with a warning")
    check(mutate.choose_limits(512, sanitize=True, mode="scope")["warning"] is None
          and mutate.choose_limits(512, sanitize=False, mode="rlimit")["mode"] == "rlimit",
          "no warning with a scope, and RLIMIT_AS without the sanitizers")
    text = mutate.describe_limits(mutate.choose_limits(512, sanitize=False, mode="rlimit"), 4)
    check("512 MB" in text and "20 s" in text and "/dev/null" in text and "Workers: 4" in text, "the line printed at the start names every limit")

    def cli(*args):
        return subprocess.run([sys.executable, str(MUTATE), *args, "--out", str(tmp / "out")], capture_output=True, text=True, timeout=60)

    r = cli("--jobs", "100000", "--target", "uart_h", "--sample", "1")
    check(r.returncode != 0 and "refused" in r.stderr and "quarter" in r.stderr and not (tmp / "out").exists(),
          "more workers than a quarter of the memory allows: refused before anything is generated or built")
    r = cli("--mem-limit", "100000000", "--target", "uart_h", "--sample", "1")
    check(r.returncode != 0 and "refused" in r.stderr and not (tmp / "out").exists(), "a per-mutant limit larger than that quarter: refused")
    r = cli("--jobs", "0")
    check(r.returncode != 0 and not (tmp / "out").exists(), "--jobs 0 is refused")
    source = MUTATE.read_text()
    check("capture_output" not in source and "stdout=subprocess.PIPE" not in source and "check_output" not in source,
          "mutate.py reads no child's output into memory anywhere")

print("== MUTATE LIMITS OK ==" if ok else "== MUTATE LIMITS FAILED ==")
sys.exit(0 if ok else 1)
