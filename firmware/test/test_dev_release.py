#!/usr/bin/env python3
"""dev.py release engine: the pure rules behind `dev.py ota amebaz2 lint|verint|tag` (#143).

No SDK, no env file, no network. While ota-release.sh still exists its `verint` must agree with
dev.py on every input here, so the port cannot drift from the script it replaces.
"""

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "firmware" / "scripts"
sys.path.insert(0, str(SCRIPTS))
import dev  # noqa: E402

ok = True


def check(cond, msg):
    global ok
    print(("  ok  " if cond else "  FAIL ") + msg)
    ok = ok and cond


def refused(fn, *args):
    try:
        fn(*args)
    except dev.Die:
        return True
    return False


# --- semver <-> softwareVersion int ---
check(dev.semver_to_int("1.3.44") == 10344, "1.3.44 -> 10344")
check(dev.semver_to_int("1.0.0") == 10000, "1.0.0 -> 10000 (clears the legacy sw34 fleet)")
check(dev.semver_to_int(" 1.2.9\n") == 10209, "whitespace around version.txt is ignored")
check(dev.semver_to_int("34") == 34, "a legacy raw-int version passes through")
check(dev.semver_to_int("0") == 0, "CI's base fallback 0 passes through")
check(refused(dev.semver_to_int, "1.100.0"), "minor 100 is refused (it would collide with the next major)")
check(refused(dev.semver_to_int, "1.0.100"), "patch 100 is refused")
check(refused(dev.semver_to_int, "1.2"), "two-part version is refused")
check(refused(dev.semver_to_int, "v1.2.3"), "a v prefix is refused")
check(refused(dev.semver_to_int, ""), "an empty version is refused")
for sem in ("1.3.44", "1.0.0", "2.99.99", "0.0.1"):
    check(dev.int_to_semver(dev.semver_to_int(sem)) == sem, f"{sem} round-trips through the int")
check(dev.semver_to_int("1.3.44") > dev.semver_to_int("1.3.43")
      and dev.semver_to_int("1.4.0") > dev.semver_to_int("1.3.99")
      and dev.semver_to_int("2.0.0") > dev.semver_to_int("1.99.99"),
      "the int is strictly monotonic across patch, minor and major")

# --- .zap endpoint contiguity ---
check(dev.endpoints_contiguous([0, 1, 2, 3]) == ([0, 1, 2, 3], True), "0..3 is contiguous")
check(dev.endpoints_contiguous([3, 0, 2, 1])[1], "order in the .zap does not matter")
check(not dev.endpoints_contiguous([0, 1, 3])[1], "a gap is refused (boot-crashes AmebaZ2)")
check(not dev.endpoints_contiguous([1, 2, 3])[1], "endpoints must start at 0")
check(not dev.endpoints_contiguous([0, 1, 1])[1], "a duplicate endpoint id is refused")

# --- release-runner check: which repo to ask about ---
check(dev.github_slug("git@github.com:owner/repo.git") == "owner/repo", "ssh remote -> owner/repo")
check(dev.github_slug("https://github.com/owner/repo") == "owner/repo", "https remote -> owner/repo")
check(dev.github_slug("https://github.com/owner/repo.git/") == "owner/repo.git",
      "only one suffix is stripped, .git first (same as the script)")
check(dev.github_slug("https://github.com/owner/repo/") == "owner/repo", "trailing slash is stripped")
check(dev.github_slug("ssh://git@git.example.com:2222/owner/repo.git") == "", "a non-GitHub remote gives no slug")
check(dev.github_slug("") == "", "no origin gives no slug")

# --- CLI, and parity with the script it replaces ---
devpy = [sys.executable, str(SCRIPTS / "dev.py"), "ota", "amebaz2", "verint"]
script = SCRIPTS / "ota-release.sh"
for arg in ("1.3.44", "1.0.0", "34", "0", "1.100.0", "1.2", "bogus"):
    d = subprocess.run(devpy + [arg], capture_output=True, text=True)
    want_rc = 1 if refused(dev.semver_to_int, arg) else 0
    check(d.returncode == want_rc and (want_rc or d.stdout.strip() == str(dev.semver_to_int(arg))),
          f"CLI verint {arg}: exit {d.returncode}, stdout '{d.stdout.strip()}'")
    if script.is_file():
        s = subprocess.run(["bash", str(script), "verint", arg], capture_output=True, text=True)
        check((s.returncode, s.stdout.strip()) == (d.returncode, d.stdout.strip()),
              f"verint {arg}: dev.py and ota-release.sh agree")
d = subprocess.run(devpy, capture_output=True, text=True)
check(d.returncode == 0 and d.stdout.strip() == str(dev.cur_version()), "CLI verint with no argument reads version.txt")
d = subprocess.run(devpy + [""], capture_output=True, text=True)
check(d.returncode == 0 and d.stdout.strip() == str(dev.cur_version()), "an empty argument also reads version.txt")

print("== DEV RELEASE ENGINE OK ==" if ok else "== DEV RELEASE ENGINE FAILED ==")
sys.exit(0 if ok else 1)
