#!/usr/bin/env python3
"""The tooling around dev.py at its edges: the image format library, version-lint.py, cpp-lint,
the docs linter, the shell scripts, the commit hook, and how the workflows, the test runner and
the lint lists agree with each other. The clip flasher has a test of its own, test_flasher.py.

Each check documents what the tool does today. Where that is wrong the check is a KNOWN FAILURE:
`known_failure()` passes while the defect is there and fails the day it is fixed, so the marker
cannot outlive the defect. Each one names the defect and the line.

No hardware and no network. Every tool is run against a tree made in a temporary directory (git
repositories with no user configuration, stand-ins for esphome and clang-format), so nothing in
this checkout is written to. About 5 s, most of it starting bash, git and python a few hundred
times.
"""

import contextlib
import importlib.util
import io
import os
import pathlib
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import types

# Under the pre-commit hook GIT_DIR and GIT_INDEX_FILE name this repository, and a git command in
# a temporary tree would act on it instead. Nothing below may see them.
for _k in [k for k in os.environ if k.startswith("GIT_")]:
    del os.environ[_k]

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "firmware" / "scripts"
sys.path.insert(0, str(SCRIPTS))
import amebaz2_image as az  # noqa: E402
from image_fixtures import chain, step  # noqa: E402

ok = True
known = 0


def check(cond, msg):
    global ok
    print(("  ok  " if cond else "  FAIL ") + msg)
    ok = ok and bool(cond)


def known_failure(fixed, msg, defect):
    """A check that is expected to fail today. `fixed` is the condition that holds once the
    defect is gone. While it is False this prints an xfail line and the suite stays green."""
    global ok, known
    if fixed:
        print(f"  FAIL {msg}: passes now, so the defect is fixed. Turn this into a plain check ({defect})")
        ok = False
    else:
        known += 1
        print(f"  xfail {msg}  [{defect}]")


def raised(fn, *args, **kwargs):
    """The exception class name a call ends in, or None."""
    try:
        fn(*args, **kwargs)
    except Exception as e:  # noqa: BLE001 - the point is to see which one
        return type(e).__name__
    return None


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write(path, text):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def script(path, text):
    path = write(path, text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


tmp_holder = tempfile.TemporaryDirectory(prefix="scripts-edges-")
TMP = pathlib.Path(tmp_holder.name)

# Every git this process starts, directly or through a tool it tests, runs without the user's
# configuration (identity, hooks, signing, quoting rules), so the result is the same on a laptop
# and on a runner, and it never looks for a repository above the temporary directory.
os.environ.update({"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_SYSTEM": os.devnull,
                   "GIT_CEILING_DIRECTORIES": str(TMP.parent)})
GIT_ENV = {**os.environ, "GIT_AUTHOR_NAME": "test", "GIT_AUTHOR_EMAIL": "test@example.com",
           "GIT_COMMITTER_NAME": "test", "GIT_COMMITTER_EMAIL": "test@example.com"}


def in_process(fn, *args):
    """Call a tool's entry point here instead of starting python again. Returns its exit code
    and what it printed, shaped like subprocess.run's result."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = fn(*args)
        except SystemExit as e:
            code = e.code
    return types.SimpleNamespace(returncode=code or 0, stdout=out.getvalue(), stderr=err.getvalue())


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True, env=GIT_ENV).stdout


def new_repo(path):
    path = pathlib.Path(path)
    path.mkdir(parents=True)
    git(path, "init", "-q", "-b", "main")
    # Nothing below may run unless the repository is the new one in the temporary directory.
    if not (path / ".git" / "HEAD").is_file():
        sys.exit(f"test_scripts_edges: git init did not create {path}/.git, refusing to go on")
    return path


# ================================================================================================
print("== amebaz2_image.py: malformed, truncated and oversized images ==")


END = az.LINK_END
three = az.resign(chain((0x40, step(0x40)), (0x80, step(0x80)), (0x20, END)), serial=7)

check(raised(az.sub_images, b"") == "ImageFormatError", "an empty payload has no chain")
check(raised(az.sub_images, bytes(az.HDR0 + az.HDR_LEN - 1)) == "ImageFormatError", "a payload cut inside the first header is refused")
check(raised(az.sub_images, chain((0, END))[:-1]) == "ImageFormatError", "a payload cut inside the last trailer is refused")
check(az.sub_images(chain((0, END))) == [(0, 0xE0, 0, 0x140)], "the smallest image: one sub-image with an empty body")
huge = bytearray(chain((0x40, END)))
struct.pack_into("<I", huge, az.HDR0, 0xFFFFFFFF)
check(raised(az.sub_images, bytes(huge)) == "ImageFormatError", "a size of 0xFFFFFFFF is refused, not read")
check(raised(az.sub_images, chain((0x40, 0))) == "ImageFormatError", "a link of 0 is refused: it would never advance")
check(raised(az.sub_images, chain((0x40, 0x7FFFFFFF))) == "ImageFormatError", "a link far past the end is refused")
check(raised(az.sub_images, chain((0x40, step(0x40)))) == "ImageFormatError", "a link to a header that is not there is refused")
eight = chain(*[(8, step(8))] * 7, (8, END))
check(len(az.sub_images(eight)) == 8, "eight sub-images are walked")
check(raised(az.sub_images, chain(*[(8, step(8))] * 8, (8, END))) == "ImageFormatError", "a ninth is refused: the walk is capped")
for cut in (len(three) - 1, len(three) // 2, az.HDR0 + 4, 16, 0):
    good, lines = az.verify(three[:cut])
    check(not good and az.failures(lines), f"verify of an image cut to {cut:#x} bytes fails and says why")
check(az.verify(three)[0] and az.verify(three, az.bytesum(three))[0], "the whole image verifies, with and without the byte-sum")

# A link is only checked for being non-zero and inside the payload. One that points INTO the
# sub-image it belongs to is walked as a second sub-image. Behaviour today.
overlap = bytearray(chain((0x100, 0x20)))
struct.pack_into("<II", overlap, az.HDR0 + 0x20, 0x10, END)
subs = az.sub_images(bytes(overlap))
check(len(subs) == 2 and subs[1][1] == az.HDR0 + 0x20, "a link that points inside its own sub-image is accepted by the walk")
check(not az.verify(az.resign(bytes(overlap)))[0],
      "and re-signing such a chain gives an image that does not verify, so resign() alone proves nothing")

# Bytes after the last trailer are no part of any signature. Only the byte-sum covers them.
padded = three + b"\x01" * 64
check(az.verify(padded)[0], "bytes after the last trailer are not covered by any signature")
check(not az.verify(padded, az.bytesum(three))[0] and az.verify(padded, az.bytesum(padded))[0], "the byte-sum is what covers them")

good, lines = az.verify(three, az.bytesum(three)[:3])
check(not good and lines[-1] == "byte-sum = MISMATCH", "a byte-sum trailer of three bytes is a mismatch, not an error")
check(not az.manifest_ok(b"") and not az.manifest_ok(three[:az.HDR0 + az.HDR_LEN - 1]) and az.manifest_ok(three),
      "the manifest check takes any length and only passes on a signed manifest")
check(raised(az.read_serial, three[:az.SERIAL_OFF + 3]) == "ImageFormatError" and az.read_serial(three[:az.SERIAL_OFF + 4]) == 7,
      "the serial is read from 0xF4, and a payload that ends before it is refused")
check(az.bytesum(b"") == bytes(4) and az.bytesum(b"\x01\x02\xff") == b"\x02\x01\x00\x00",
      "the byte-sum of nothing is 0, and it is little-endian")

before = bytearray(three)
out = az.resign(before, serial=9)
check(bytes(before) == three and isinstance(out, bytes) and az.read_serial(out) == 9, "resign leaves its input alone and returns new bytes")
check(az.resign(three) == three, "resign with no serial keeps the serial, and is a no-op on a signed image")
check(raised(az.resign, bytes(64)) == "ImageFormatError", "resign refuses a payload too short to sign")
broken = bytearray(three)
struct.pack_into("<I", broken, az.HDR0 + 4, 0)
check(raised(az.resign, bytes(broken), 9) == "ImageFormatError", "resign refuses a malformed chain before it changes anything")
# A serial that does not fit 32 bits is struct's error, not this module's own. dev.py checks the
# range before it calls resign, so this is only reachable from another caller.
check(raised(az.resign, three, 2 ** 32) == "error" and raised(az.resign, three, -1) == "error",
      "resign with a serial outside 32 bits raises struct.error, not ImageFormatError")
check(az.read_serial(az.resign(three, serial=0xFFFFFFFF)) == 0xFFFFFFFF, "resign itself takes the erased-flash serial (dev.py refuses it)")

# split_payload: carving an image out of a slot or a whole-chip dump.
ERASED = b"\xff" * 4096
body = bytes(range(1, 250)) * 9                    # no 0xFF in it
check(az.split_payload(body) is None, "a region with no erased run holds no image")
check(az.split_payload(ERASED) is None and az.split_payload(b"abc" + ERASED) is None, "an erased run at the start leaves no room for the byte-sum")
check(az.split_payload(body + ERASED) is None, "an image below the minimum size (1 MiB) is not returned")
for n, wanted in ((0x100, True), (0xFF, False), (0x200, True), (0x201, False)):
    got = az.split_payload(bytes(n) + b"SUM!" + ERASED, min_len=0x100, max_len=0x200)
    check((got is not None) == wanted, f"a {n:#x}-byte image with limits [0x100, 0x200] is {'returned' if wanted else 'not returned'}")
payload, trailer = az.split_payload(bytes(0x180) + b"SUM!" + ERASED + b"later", min_len=0x100, max_len=0x200)
check(len(payload) == 0x180 and trailer == b"SUM!", "the four bytes before the erased run are the byte-sum trailer")
inner = bytes(0x100) + ERASED + bytes(0x100) + b"SUM!" + ERASED
payload, trailer = az.split_payload(inner, min_len=0x10, max_len=0x1000)
check(len(payload) == 0x100 - 4, "an image with 4 KB of 0xFF inside it is cut at that run (the caller's verify then fails)")

# ================================================================================================
print("== version-lint.py: odd changelogs, tags and command lines ==")

LINT = SCRIPTS / "version-lint.py"
VERSION_FILE = "firmware/esphome/packages/version.yaml"
vl = load("version_lint", LINT)


def lint(repo, *args, env=None):
    """version-lint.py on a tree. With `env` it is a real process, otherwise main() is called here."""
    if env is None:
        return in_process(vl.main, ["--repo", str(repo), *args])
    return subprocess.run([sys.executable, str(LINT), "--repo", str(repo), *args], capture_output=True, text=True, env=env)


def version_yaml(version):
    return f'esphome:\n  project:\n    name: someone.something\n    version: "{version}"\n'


def tree(name, version="2.0.0", changelog="# Changelog\n\n## Unreleased\n\n- a change\n", yaml=None, repo=False):
    """A tree for the linter. The checks of the version file and the changelog need no history,
    so it is a plain directory unless `repo` asks for a git repository with one commit."""
    path = TMP / "vl" / name
    if repo:
        new_repo(path)
    write(path / VERSION_FILE, yaml if yaml is not None else version_yaml(version))
    write(path / "CHANGELOG.md", changelog)
    write(path / "firmware/esphome/components/hisense_ac/hub.cpp", "// v1\n")
    if repo:
        git(path, "add", "-A")
        git(path, "commit", "-q", "-m", "base")
    return path


def sections(*titles):
    return [(t, 0, 0) for t in titles]


for title in ("2.0.0", "2.0.0 - 2026-10-12", "[2.0.0] - 2026-10-12", "[2.0.0]", "v2.0.0", "[v2.0.0] - x"):
    check(vl.version_section(sections(title), "2.0.0") is not None, f"`## {title}` is the section of 2.0.0")
for title in ("2.0.0 (2026-10-12)", "2.0.0-rc1", "12.0.0", "2.0.01", "2.0.0 2026-10-12", "Release 2.0.0", "2x0x0", "2.0", ""):
    check(vl.version_section(sections(title), "2.0.0") is None, f"`## {title}` is not the section of 2.0.0")
check(vl.version_section([("2.0.0", 5, 9), ("2.0.0", 20, 30)], "2.0.0") == (5, 9), "of two sections with the same version the first is taken")

good = tree("good", repo=True)
check(lint(good).stdout.strip() == "[version-lint] ok: firmware 2.0.0", "the ok line names the version")
for args, why in ((["--tag"], "--tag with no value"), (["--base"], "--base with no value"), (["--bogus"], "an unknown flag"),
                  (["extra"], "a stray word")):
    r = lint(good, *args)
    check(r.returncode == 2 and "version-lint.py" in r.stderr and r.stdout == "", f"{why} prints the usage and exits 2")
check(in_process(vl.main, ["--repo"]).returncode == 2, "--repo with no value exits 2")
r = lint(good, "--bogus", env=GIT_ENV)
check(r.returncode == 2 and r.stdout == "" and "Always checked:" in r.stderr, "from a real process too: usage on stderr, exit 2")
r = lint(good, env=GIT_ENV)
check(r.returncode == 0 and r.stderr == "", "and a pass prints its one line on stdout and exits 0")
r = lint(TMP / "vl" / "no-such-tree")
check(r.returncode == 1 and f"{VERSION_FILE} is missing" in r.stderr, "a tree without the version file is refused")
no_log = tree("no-changelog")
(no_log / "CHANGELOG.md").unlink()
r = lint(no_log)
check(r.returncode == 1 and "CHANGELOG.md is missing" in r.stderr, "a tree without CHANGELOG.md is refused")

# The version file.
for name, yaml, wanted in (
        ("unquoted", "esphome:\n  project:\n    version: 2.0.0\n", 0),
        ("comment", 'esphome:\n  project:\n    version: "2.0.0"   # the release\n', 0),
        ("twice", 'esphome:\n  project:\n    version: "2.0.0"\nother:\n  thing:\n    version: "2.0.0"\n', 1),
        ("indent2", 'esphome:\n  version: "2.0.0"\n', 1),
        ("empty", "", 1),
        ("tab", 'esphome:\n  project:\n\tversion: "2.0.0"\n', 1)):
    r = lint(tree(f"yaml-{name}", yaml=yaml))
    check(r.returncode == wanted and (wanted == 0 or "expected one `version:`" in r.stderr),
          f"version.yaml '{name}' is {'accepted' if wanted == 0 else 'refused'}")
bad = tree("bad-version", version="2.0.0-rc1")
r = lint(bad, "--print")
check(r.returncode == 0 and r.stdout.strip() == "2.0.0-rc1", "--print prints whatever the file holds, valid or not, and exits 0")

# The changelog.
for name, heading, wanted in (("lower", "## unreleased", 0), ("suffix", "## Unreleased changes", 0),
                              ("bracket", "## [Unreleased]", 0), ("bracket-date", "## [Unreleased] - soon", 0),
                              ("word", "## Unreleasedness", 1), ("other", "## [Released]", 1),
                              ("nospace", "##Unreleased", 1), ("h3", "### Unreleased", 1), ("h1", "# Unreleased", 1)):
    r = lint(tree(f"log-{name}", changelog=f"# Changelog\n\n{heading}\n\n- a change\n"))
    check(r.returncode == wanted, f"`{heading}` {'counts' if wanted == 0 else 'does not count'} as the Unreleased section")
released = tree("released", changelog="# Changelog\n\n## 2.0.0 - 2026-10-12\n\n- the release\n\n## 1.0.0\n\n- old\n- older\n", repo=True)
r = lint(released, "--notes", "1.0.0")
check(r.returncode == 0 and r.stdout == "- old\n- older\n", "--notes VERSION prints that section, and the last one runs to the end of the file")
r = lint(released, "--notes", "9.9.9")
check(r.returncode == 1 and "no `## 9.9.9` section" in r.stderr and r.stdout == "", "--notes for a version with no section fails and prints nothing")
r = lint(tree("empty-notes", changelog="# Changelog\n\n## 2.0.0\n\n## 1.0.0\n\n- old\n"), "--notes")
check(r.returncode == 1 and "section is empty" in r.stderr, "--notes for an empty section fails")
r = lint(released, "--tag", "v2.0.0", "--base", "main")
check(r.returncode == 0 and r.stdout.strip() == "[version-lint] ok: firmware 2.0.0, tag v2.0.0, base main", "--tag and --base together: both named in the ok line")
r = lint(tree("bracketed", changelog="# Changelog\n\n## [2.0.0] - 2026-10-12\n\n- the release\n"), "--tag", "v2.0.0")
check(r.returncode == 0, "a tag is accepted against a `## [2.0.0] - date` section")
fenced = tree("fenced", changelog="# Changelog\n\n## 2.0.0\n\n- how to build:\n\n```bash\n## not a heading, a shell comment\nmake\n```\n\n- last entry\n")
r = lint(fenced, "--notes")
check(r.returncode == 0 and "## not a heading, a shell comment" in r.stdout and r.stdout.rstrip().endswith("- last entry"),
      "--notes keeps the whole section when it holds a code block with a `## ` line")
tilde = tree("fenced-tilde", changelog="# Changelog\n\n## 2.0.0\n\n~~~\n## in a tilde block\n```\n## still inside: the other fence does not close it\n~~~\n\n## 1.0.0\n\n- old\n")
check(lint(tilde, "--notes").stdout.count("## ") == 2 and lint(tilde, "--notes", "1.0.0").stdout == "- old\n",
      "a fence closes only with its own kind, and the heading after it is a heading again")

# Tags.
tags = tree("tags", version="9.5.0", repo=True)
for t in ("v9.5.0", "v9.4.9", "v2.99.99", "v9.6.0-rc1", "v09.9.0", "vnext", "esp32-v99.0.0", "amebaz2-v99.0.0", "matter-final"):
    git(tags, "tag", t)
check(lint(tags).returncode == 0, "a version equal to the newest release tag passes; pre-release, zero-padded and other tags are not releases")
git(tags, "tag", "v10.0.0")
r = lint(tags)
check(r.returncode == 1 and "below the newest release tag v10.0.0" in r.stderr, "v10.0.0 is newer than 9.5.0: the comparison is by number, not by text")
loose = tree("not-a-repo")
check(lint(loose).returncode == 0, "a tree that is not a git repository passes the basic checks (no tags to compare with)")
r = lint(loose, "--base", "main")
check(r.returncode == 1 and "git merge-base main HEAD" in r.stderr, "--base outside a git repository fails with git's message")

# The changelog rule of a pull request.
r = lint(good, "--base", "no-such-ref")
check(r.returncode == 1 and "git merge-base no-such-ref HEAD" in r.stderr, "--base with a ref that does not exist fails with git's message")
git(good, "checkout", "-q", "-b", "delete")
git(good, "rm", "-q", "firmware/esphome/components/hisense_ac/hub.cpp")
git(good, "commit", "-q", "-m", "delete a component file")
check(lint(good, "--base", "main").returncode == 1, "deleting a component file needs a changelog entry too")
git(good, "checkout", "-q", "-b", "whitespace", "main")
write(good / "firmware/esphome/components/hisense_ac/hub.cpp", "// v2\n")
write(good / "CHANGELOG.md", "# Changelog\n\n## Unreleased\n\n- a change\n\n")
git(good, "commit", "-q", "-am", "component change, blank line in the changelog")
check(lint(good, "--base", "main").returncode == 0, "any difference in CHANGELOG.md satisfies the rule, a blank line included")
git(good, "checkout", "-q", "-b", "unicode", "main")
write(good / "firmware/esphome/components/hisense_ac/caf\u00e9.cpp", "// new\n")
git(good, "add", "-A")
git(good, "commit", "-q", "-m", "a component file with a non-ASCII name")
r = lint(good, "--base", "main")
check(r.returncode == 1 and "caf\u00e9.cpp" in r.stderr, "a new component file with a non-ASCII name needs a changelog entry, and is named as it is")
git(good, "checkout", "-q", "main")

# The ESPHome pin.
pins = tree("pins")
write(pins / ".github/workflows/qa.yaml", 'env:\n  ESPHOME_VERSION: "2026.7.4"\n')
write(pins / ".github/workflows/esphome-release.yaml", "env:\n  ESPHOME_VERSION: 2026.7.4\n")
write(pins / "firmware/scripts/dev.py", 'ESPHOME_PIN = "2026.7.4"   # the release CI builds with\n')
write(pins / "firmware/scripts/esphome-upstream-check.sh", 'ESPHOME_SPEC="esphome==2026.7.4"\n')
check(lint(pins).returncode == 0, "the same pin in all four files passes, quoted or not")
write(pins / "firmware/scripts/esphome-upstream-check.sh", 'ESPHOME_SPEC="esphome==2026.8.0"\n')
r = lint(pins)
check(r.returncode == 1 and "esphome-upstream-check.sh 2026.8.0" in r.stderr, "the upstream check's pin differing is refused and named")
write(pins / "firmware/scripts/esphome-upstream-check.sh", 'ESPHOME_SPEC="esphome==2026.7.4"\n')
write(pins / ".github/workflows/qa.yaml", 'env:\n  ESPHOME_VERSION: "2026.7.4"\njobs:\n  x:\n    env:\n      ESPHOME_VERSION: "2026.8.0"\n')
r = lint(pins)
check(r.returncode == 1 and "expected one ESPHome pin, found ['2026.7.4', '2026.8.0']" in r.stderr, "two different pins inside one file are refused")
write(pins / ".github/workflows/qa.yaml", "env:\n  OTHER: 1\n")
r = lint(pins)
check(r.returncode == 1 and "qa.yaml: expected one ESPHome pin, found none" in r.stderr, "a pin file that no longer holds a pin is refused")
(pins / ".github/workflows/qa.yaml").unlink()
check(lint(pins).returncode == 0, "a pin file that is absent is skipped, not refused")

# git missing from PATH.
no_git = {**GIT_ENV, "PATH": str(TMP)}
check(lint(good, env=no_git).returncode == 0, "with no git on PATH the basic checks still pass (no tags to compare with)")
r = lint(good, "--base", "main", env=no_git)
check(r.returncode == 1 and "Traceback" not in r.stderr and "cannot run git" in r.stderr, "--base with no git on PATH fails with a message")

# ================================================================================================
print("== cpp-lint.py: which files it takes, and its own rules ==")

cpp = load("cpp_lint", SCRIPTS / "cpp-lint.py")
real_root = cpp.ROOT
R = new_repo(TMP / "cpp")
HEAD40 = "// line\n" * 45
TRACKED = {
    "firmware/esphome/components/hisense_ac/a.cpp": "int a;\n",
    "firmware/esphome/components/hisense_ac/a.h": "#pragma once\n",
    "firmware/esphome/components/hisense_ac/notes.md": "notes\n",
    "firmware/esphome/components/hisense_ac/__init__.py": "\n",
    "firmware/esphome/components/w41h1_module/m.cpp": "int m;\n",
    "firmware/esphome/components/other/x.cpp": "int x;\n",
    "firmware/test/t.cpp": "int t;\n",
    "firmware/test/stubinc/s.h": "#include <x>\n",
    "firmware/test/build/gen.cpp": "int g;\n",
    "firmware/test/managed_components/v/v.c": "int v;\n",
    "firmware/test/vendor.cpp": "// This file is the Confidential property of someone else.\nint v;\n",
    "firmware/test/late.cpp": HEAD40 + "// confidential, but below the banner window\n",
    "firmware/test/gone.cpp": "int gone;\n",
    "firmware/esp32-recon/main/r.c": "int r;\n",
    "firmware/esp32-recon/main/r.tcc": "int r;\n",
    "firmware/scripts/tool.cpp": "int s;\n",
    "dumps/d.cpp": "int d;\n",
    "top.cpp": "int top;\n",
}
for rel, text in TRACKED.items():
    write(R / rel, text)
git(R, "add", "-A")
git(R, "commit", "-q", "-m", "tree")
write(R / "firmware/test/untracked.cpp", "int u;\n")
(R / "firmware/test/gone.cpp").unlink()
cpp.ROOT = R
IN_SCOPE = ["firmware/esp32-recon/main/r.c", "firmware/esp32-recon/main/r.tcc",
            "firmware/esphome/components/hisense_ac/a.cpp", "firmware/esphome/components/hisense_ac/a.h",
            "firmware/esphome/components/w41h1_module/m.cpp", "firmware/test/late.cpp",
            "firmware/test/stubinc/s.h", "firmware/test/t.cpp"]
check(cpp.select_files([]) == IN_SCOPE, "in scope: tracked C and C++ files of the four owned trees, subdirectories included")
for rel, why in (("firmware/esphome/components/other/x.cpp", "another component"), ("firmware/scripts/tool.cpp", "a tree that is not owned"),
                 ("top.cpp", "the repo root"), ("dumps/d.cpp", "dumps/"), ("firmware/test/build/gen.cpp", "a build directory"),
                 ("firmware/test/managed_components/v/v.c", "managed components"),
                 ("firmware/esphome/components/hisense_ac/notes.md", "not C or C++"),
                 ("firmware/test/vendor.cpp", "a file with a proprietary banner"),
                 ("firmware/test/untracked.cpp", "a file git does not track"),
                 ("firmware/test/gone.cpp", "a tracked file that is gone from the working tree")):
    check(rel not in cpp.select_files([]), f"out of scope: {why}")
check("firmware/test/late.cpp" in cpp.select_files([]), "the banner is only looked for in the first 40 lines")

here = os.getcwd()
os.chdir(R)
try:
    check(cpp.select_files(["firmware/test/t.cpp"]) == ["firmware/test/t.cpp"], "a path narrows the run to that file")
    check(cpp.select_files(["firmware/test"]) == cpp.select_files(["firmware/test/"]) == [f for f in IN_SCOPE if f.startswith("firmware/test/")],
          "a directory, with or without the trailing slash, takes everything in scope below it")
    check(cpp.select_files([str(R / "firmware/test/t.cpp")]) == ["firmware/test/t.cpp"], "an absolute path works")
    check(cpp.select_files(["firmware/test/../test/t.cpp"]) == ["firmware/test/t.cpp"], "a path with .. in it is normalised")
    check(cpp.select_files(["firmware/tes"]) == [], "a directory name is matched whole: firmware/tes does not take firmware/test")
    check(cpp.select_files(["firmware/test/vendor.cpp", "top.cpp", "README.md"]) == [], "out-of-scope paths are skipped without a word")
    check(cpp.select_files(["/etc/hostname", str(TMP)]) == [], "paths outside the repository are skipped, they do not widen the run to everything")
    check(cpp.select_files(["firmware/test/typo.cpp"]) == [], "a path that does not exist selects nothing")
    os.chdir(R / "firmware" / "test")
    check(cpp.select_files(["t.cpp"]) == ["firmware/test/t.cpp"], "a relative path is taken from the current directory, not the repo root")
finally:
    os.chdir(here)
    cpp.ROOT = real_root

# The shell wrapper and the command line, against the same tree.
shutil.copy(SCRIPTS / "cpp-lint.py", R / "firmware/scripts/cpp-lint.py")
shutil.copy(SCRIPTS / "cpp-lint.sh", R / "firmware/scripts/cpp-lint.sh")
BIN = TMP / "bin"
for tool in ("bash", "dirname", "grep", "sed", "git", "env"):
    found = shutil.which(tool)
    if found:
        (BIN / tool).parent.mkdir(exist_ok=True)
        (BIN / tool).symlink_to(found)
(BIN / "python3").symlink_to(sys.executable)
FMT = ('#!/bin/sh\nif [ "$1" = --version ]; then echo "clang-format version $FAKE_FMT_VERSION"; exit 0; fi\n'
       'echo "$@" >> "$FAKE_FMT_LOG"\nexit 0\n')
WRAP_ENV = {**GIT_ENV, "PATH": str(BIN), "HOME": str(TMP), "XDG_CACHE_HOME": str(TMP / "cache"), "CPP_LINT_NO_INSTALL": "1",
            "FAKE_FMT_LOG": str(TMP / "fmt.log")}
for k in ("CLANG_FORMAT", "CLANG_TIDY"):
    WRAP_ENV.pop(k, None)


def wrapper(*args, **env):
    return subprocess.run(["bash", str(R / "firmware/scripts/cpp-lint.sh"), *args], capture_output=True, text=True,
                          cwd=R, env={**WRAP_ENV, **env})


r = wrapper("bogus")
check(r.returncode == 2 and "cpp-lint.sh check" in r.stdout, "cpp-lint.sh with an unknown mode prints its usage and exits 2")
r = wrapper("files")
check(r.returncode == 0 and r.stdout.split() == IN_SCOPE, "cpp-lint.sh files lists the scope and needs no clang tool")
r = wrapper("check", "--no-tidy")
check(r.returncode == 3 and "clang-format 13.0.1 not found" in r.stderr, "no pinned clang-format and installing forbidden: exit 3, as the hook expects")
script(BIN / "clang-format", FMT)
r = wrapper("check", "--no-tidy", FAKE_FMT_VERSION="13.0.10")
check(r.returncode == 3, "clang-format 13.0.10 on PATH is not taken for the pinned 13.0.1")
r = wrapper("check", "--no-tidy", FAKE_FMT_VERSION="14.0.0")
check(r.returncode == 3, "another major version on PATH is not used")
r = wrapper("check", "--no-tidy", "firmware/test/t.cpp", "README.md", FAKE_FMT_VERSION="13.0.1")
check(r.returncode == 0 and "1 file(s) clean" in r.stderr, "the pinned version on PATH is used, and the hook's path form narrows the run")
r = wrapper("check", FAKE_FMT_VERSION="13.0.1")
check(r.returncode == 3 and "clang-tidy 22.1.8 not found" in r.stderr, "without --no-tidy the pinned clang-tidy is required too")
(BIN / "clang-format").unlink()
r = wrapper("check", "--no-tidy", "firmware/test/t.cpp", CLANG_FORMAT=str(script(TMP / "any-format", FMT)), FAKE_FMT_VERSION="99")
check(r.returncode == 0, "$CLANG_FORMAT is used as given: its version is not checked")
r = wrapper("check", "--no-tidy", "firmware/test/typo.cpp", CLANG_FORMAT=str(TMP / "any-format"))
check(r.returncode == 0 and "0 file(s) clean" in r.stderr, "a path that selects nothing passes as '0 file(s) clean'")
r = subprocess.run([sys.executable, str(R / "firmware/scripts/cpp-lint.py"), "nonsense"], capture_output=True, text=True, env=WRAP_ENV)
check(r.returncode == 2, "cpp-lint.py with an unknown mode exits 2")

# The custom rules.
def rules(rel, text):
    return [msg.split(";")[0].split("(")[0].strip() for _, _, msg in cpp.check_rules(rel, text)]


CLEAN = "#pragma once\nint f();\n"
check(rules("a.h", CLEAN) == [], "a clean header has no findings")
check(rules("a.h", "int f();\n") == ["header has no #pragma once"], "a header without #pragma once")
check(rules("firmware/test/stubinc/x.h", "int f();\n") == [], "the stub headers are exempt from #pragma once")
check(rules("a.cpp", "int a;\tint b;\n") == ["tab character"], "a tab")
check(rules("a.cpp", "int a;\r\n")[:1] == ["Windows newline"], "a CRLF line ending")
check(rules("a.cpp", "int a;") == ["file does not end with a newline"], "no newline at the end")
check(rules("a.cpp", "int a;  \n") == ["trailing whitespace"], "trailing spaces")
check(rules("a.cpp", "#define LIMIT 10\n") == ["#define for an integer constant"], "an integer #define in C++")
check(rules("a.cpp", "#define LIMIT 0x1F  // why\n") == ["#define for an integer constant"], "a hex #define with a comment")
check(rules("a.c", "#define LIMIT 10\n") == [], "an integer #define in C is allowed (no constexpr there)")
check(rules("a.cpp", "#define LIMIT 10  // NOLINT shared with C\n") == [], "NOLINT on the line exempts it")
check(rules("a.cpp", "#define NAME \"x\"\n#define F(x) (x)\n#define LIMIT (10)\n#define NEG -1\n") == [],
      "string, function-like, parenthesised and negative #defines are not flagged")
check(rules("firmware/test/hal_stub.h", "#pragma once\n#define PA_14 14\n") == [], "the exempt headers keep their integer #defines")
check(rules("a.cpp", "void f() { byte b = 1; }\n") == ["`byte` type"], "the byte type")
check(rules("a.cpp", "void f() { sprintf(buf, \"x\"); }\n") == ["sprintf"], "sprintf")
check(rules("a.cpp", "void f() { snprintf(buf, 4, \"x\"); }\n") == [], "snprintf is fine")
check(rules("a.cpp", "void f() { sscanf(s, \"%d\", &x); std::bind(f, 1); }\n") == ["sscanf", "std::bind"], "sscanf and std::bind")
check(rules("a.cpp", "// sprintf(buf) would overflow\nconst char *s = \"sprintf(x)\";  /* sscanf( */\n") == [],
      "the same words in a comment or a string are not code")
check(rules("a.cpp", "// the slave replies\n") == ["'slave'"], "the inclusive-language rule reads comments too")
check(rules("a.cpp", "void f() {\n  if (x)\n    ESP_LOGD(TAG, \"x\");\n}\n") == ["lone log call as an if/else/for/while body"],
      "a lone log call as an if body")
check(rules("a.cpp", "void f() {\n  if (x) {\n    ESP_LOGD(TAG, \"x\");\n  }\n}\n") == [], "the same in braces is fine")
for label, text in (("a string with an escaped quote", 'const char *s = "a\\"b // not a comment"; int sprintf_ok;\n'),
                    ("a digit separator", "int n = 1'000'000; // sprintf(\n"),
                    ("a char literal holding a quote", "char c = '\"'; char d = '\\''; // sprintf(\n"),
                    ("a raw string", 'auto s = R"x(sprintf( " )x"; // sprintf(\n'),
                    ("a comment that never closes", "int a; /* sprintf(\n")):
    masked = cpp.mask_comments_strings(text)
    check(len(masked) == len(text) and masked.count("\n") == text.count("\n") and "sprintf(" not in masked,
          f"masking keeps length and lines, and blanks {label}")
for text in ("int a;  \r\nint b;", "\n", "", "int a;\n"):
    once = cpp.fix_rules(text)
    check(cpp.fix_rules(once) == once and cpp.check_rules("a.cpp", once) == [], f"fix is stable on {text!r} and leaves nothing fixable")

# The clang-tidy unit list says it mirrors run_tests.sh.
run_tests = (ROOT / "firmware/test/run_tests.sh").read_text()
check(all((ROOT / f).is_file() for f, _ in cpp.TIDY_UNITS), "every clang-tidy unit names a file that exists")
compiled = sorted(set(re.findall(r"\b(test_\w+\.cpp)\b", run_tests)))
tidy = {f for f, _ in cpp.TIDY_UNITS}
missing = [t for t in compiled if f"firmware/test/{t}" not in tidy]
known_failure(not missing, "every host test run_tests.sh compiles is a clang-tidy unit",
              f"cpp-lint.py:284 TIDY_UNITS is a hand-kept copy of run_tests.sh's compile list and lacks {len(missing)} "
              f"of {len(compiled)} tests ({', '.join(missing[:4])}{', ...' if len(missing) > 4 else ''}), and nothing "
              "of w41h1_module: clang-tidy never sees them")

# ================================================================================================
print("== docs/guide/lint.py: broken links and the leak denylist ==")

WIKI = TMP / "wiki"
WIKI.mkdir()
shutil.copy(ROOT / "docs/guide/lint.py", WIKI / "lint.py")
PAGE = "# Title\n\nText.\n"


def wiki(pages):
    """Run the linter over a wiki made of `pages` (name -> text). Returns (exit code, output)."""
    for old in WIKI.glob("*.md"):
        old.unlink()
    shutil.rmtree(WIKI / "sub", ignore_errors=True)
    for name, text in pages.items():
        write(WIKI / name, text)
    # The linter reads its page list when it is loaded, so it is loaded again for each wiki.
    r = in_process(load("wiki_lint", WIKI / "lint.py").main)
    return r.returncode, r.stdout


def page_errors(body, extra=None):
    """The ERROR lines for one page with `body` under its title."""
    code, out = wiki({"Home.md": "# Home\n\n" + body + "\n", "Other-Page.md": PAGE, "_Sidebar.md": "[o](Other-Page)\n", **(extra or {})})
    return code, [ln.split("ERROR ", 1)[1] for ln in out.splitlines() if "ERROR " in ln]


code, out = wiki({"Home.md": PAGE, "Other-Page.md": PAGE, "_Sidebar.md": "[o](Other-Page)\n"})
check(code == 0 and "OK: 0 error(s), 0 warning(s) across 3 pages" in out, "a clean wiki passes")
code, out = wiki({})
check(code == 0 and "across 0 pages" in out, "a directory with no pages passes: there is nothing that says it should have any")

for body, needle, why in (
        ("[x](Missing-Page)", "inter-wiki link to missing page: Missing-Page", "a link to a page that does not exist"),
        ("[x](Missing-Page#part)", "missing page: Missing-Page", "the same with an anchor"),
        ("[x](other-page)", "missing page: other-page", "a link in the wrong case"),
        ("[x](Other-Page.md)", "missing page: Other-Page.md", "a link with the .md suffix"),
        ("[x](../README.md)", "has a path", "a link to a repo file"),
        ("![d](images/none.png)", "image not found: images/none.png", "an image that does not exist"),
        ("[d](diagram.svg)", "image not found: diagram.svg", "a link to an image file that does not exist"),
        ("### Deep", "heading jumps H1->H3", "a heading that skips a level"),
        ("```mermaid\ngraph TD", "unclosed ``` code/mermaid fence", "a code fence that never closes"),
        ("An em dash \u2014 here.", "stop-slop: em dash", "an em dash in prose"),
):
    code, errs = page_errors(body)
    check(code == 1 and any(needle in e for e in errs), f"refused: {why}")
for body, why in (("[x](Other-Page)", "a link to a page that exists"), ("[x](Other-Page#part)", "the same with an anchor"),
                  ("[x](#part)", "an anchor within the page itself"), ("[x](https://example.com/a/b)", "a web link"),
                  ("[x](mailto:hello@example.com)", "a mail link"),
                  ("```\n[x](Missing-Page) \u2014 ### not a heading\n```", "a broken link, an em dash and a heading inside a code block"),
                  ("An en dash \u2013 here.", "an en dash")):
    code, errs = page_errors(body)
    check(code == 0 and not errs, f"accepted: {why}")
code, out = wiki({"Home.md": "## Not H1\n", "_Sidebar.md": ""})
check(code == 1 and "first heading is H2, must be H1" in out and "no H1 title heading" in out, "a page that starts below H1 is refused")
code, out = wiki({"Home.md": "# One\n\n# Two\n", "Lonely.md": PAGE, "_Sidebar.md": ""})
check(code == 0 and "2 H1 headings" in out and "page not linked from sidebar: Lonely" in out, "two H1s and a page missing from the sidebar only warn")
code, out = wiki({"Home.md": PAGE})
check(code == 0 and "no _Sidebar.md" in out, "no sidebar at all only warns")

# The leak denylist.
for body, why in (("See /home/alice/projects/x/ for it.", "a home path"), ("On node 14 it works.", "a node number below 20"),
                  ("Reach it at 192.168.4.114.", "a private address outside the placeholder range"),
                  ("Reach it at 10.0.0.5.", "a 10.x address"), ("Open homeassistant.local.", "the Home Assistant host name"),
                  ("```\nssh you@192.168.10.149\n```", "an address inside a code block: code is not exempt from the leak rules")):
    code, errs = page_errors(body)
    check(code == 1 and any("public-ready leak" in e for e in errs), f"leak refused: {why}")
for body, why in (("See /home/you/projects/x/ and /home/user/x/.", "the placeholder home paths"),
                  ("Reach it at 192.168.1.50.", "the 192.168.1.x placeholder range"), ("Use your-ha-host.local.", "the placeholder host")):
    code, errs = page_errors(body)
    check(code == 0 and not errs, f"placeholder accepted: {why}")
code, errs = page_errors("On node 80 it works.")
known_failure(code == 1, "a node number of 20 or more is a leak",
              "docs/guide/lint.py:44 LEAKS: the pattern is `node (?:[0-9]|1[0-9])`, so `node 80` (a real node of this "
              "installation) passes")
code, errs = page_errors("Reach it at 172.16.4.20.")
known_failure(code == 1, "a 172.16.0.0/12 address is a private IP",
              "docs/guide/lint.py:46 LEAKS: only 192.168.x and 10.x are matched")
code, errs = page_errors("Reach it at fe80::1ff:fe23:4567 or mail someone@gmail.com.")
check(code == 0, "IPv6 addresses and mail addresses are not on the denylist (behaviour today)")
code, errs = page_errors("Firmware 10.3.50.1 is out.")
check(code == 1, "a four-part version number that starts with 10. is taken for a private address (a false alarm)")
code, errs = page_errors("Write a link as `[text](Page-Name)`.")
known_failure(code == 0, "link syntax shown in inline code is not checked as a link",
              "docs/guide/lint.py:177 check_file(): only fenced blocks are skipped, so a link example in backticks is "
              "reported as a broken link")

# What the linter covers. It reads the *.md files beside itself and nothing else.
code, out = wiki({"Home.md": PAGE, "sub/Deep.md": "### bad \u2014 /home/alice/x/\n"})
check(code == 0, "a page in a subdirectory is not linted")
r = subprocess.run([sys.executable, str(WIKI / "lint.py")], capture_output=True, text=True, cwd=TMP)
check(r.returncode == 0 and "OK: 0 error(s)" in r.stdout, "run as a script from another directory it lints the pages beside itself")
write(WIKI / "Home.md", "## Not H1\n")
r = subprocess.run([sys.executable, str(WIKI / "lint.py")], capture_output=True, text=True, cwd=TMP)
check(r.returncode == 1 and "FAIL: 2 error(s)" in r.stdout, "and exits 1 when it found an error")
workflows = "".join(p.read_text() for p in (ROOT / ".github/workflows").glob("*.yaml"))
hooks = "".join(p.read_text() for p in (ROOT / "firmware/.githooks").iterdir())
check("docs/guide/lint.py" in hooks, "the commit hook runs docs/guide/lint.py when a guide page is staged")
known_failure("guide/lint.py" in workflows + run_tests, "docs/guide/lint.py is run by a workflow or run_tests.sh",
              "no workflow calls docs/guide/lint.py: a pull request from a clone without the hook is not checked. "
              "The step for qa.yaml's host-qa job is `python3 docs/guide/lint.py`")

# ================================================================================================
print("== stop-slop.sh and the pre-commit hook ==")

EM, EN = "\u2014", "\u2013"
SLOP = SCRIPTS / "stop-slop.sh"
S = new_repo(TMP / "slop")
write(S / "old.md", f"# Old\n\nA line with an em dash {EM} from before the rule.\n" + "filler line\n" * 30)
git(S, "add", "-A")
git(S, "commit", "-q", "-m", "base")


def slop(files, moves=()):
    """Stage `files` (name -> text) and renames on a clean index, run stop-slop.sh, undo. Returns exit code and output."""
    for src, dst in moves:
        git(S, "mv", src, dst)
    for name, text in files.items():
        write(S / name, text)
    git(S, "add", "-A")
    r = subprocess.run(["bash", str(SLOP)], capture_output=True, text=True, cwd=S, env=GIT_ENV)
    git(S, "reset", "-q", "--hard")
    git(S, "clean", "-q", "-fd")
    return r.returncode, r.stdout


for name, text, wanted, why in (
        ("new.md", f"A new line {EM} with an em dash.\n", 1, "an em dash in a new markdown file"),
        ("docs/deep/NEW.MD", f"x {EM} y\n", 1, "the same in a subdirectory, in a file named .MD"),
        ("new.markdown", f"x {EM} y\n", 1, "the same in a .markdown file"),
        ("my notes.md", f"x {EM} y\n", 1, "the same in a file with a space in its name"),
        ("new.md", f"Pages 3{EN}5.\n", 0, "an en dash"),
        ("new.txt", f"x {EM} y\n", 0, "an em dash in a text file: only markdown is scanned"),
        ("new.py", f"# x {EM} y\n", 0, "an em dash in a code comment: only markdown is scanned"),
        ("new.mdx", f"x {EM} y\n", 1, "the same in an .mdx file (the upstream docs page is one)"),
        ("docs/NEW.Mdx", f"x {EM} y\n", 1, "the same in a file named .Mdx"),
        ("new.mdx.txt", f"x {EM} y\n", 0, "a file that only has .mdx inside its name"),
        ("old.md", f"# Old\n\nA line with an em dash {EM} from before the rule.\n" + "filler line\n" * 30 + "A clean new line.\n", 0,
         "a clean line added to a file that already has an em dash"),
):
    code, out = slop({name: text})
    check(code == wanted and (wanted == 0 or name in out), f"{'blocked' if wanted else 'passes'}: {why}")
code, out = slop({"new.md": "clean\n"})
check(code == 0 and out == "", "clean markdown passes without a word")

code, out = slop({"renamed.md": (S / "old.md").read_text() + f"An added line {EM} with an em dash.\n"}, moves=[("old.md", "renamed.md")])
check(code == 1 and "renamed.md" in out and "An added line" in out and "from before the rule" not in out,
      "an em dash added to a markdown file renamed in the same commit is blocked, and only the added line is shown")
code, out = slop({}, moves=[("old.md", "moved.md")])
check(code == 0 and out == "", "a file that is only renamed passes: the em dash it already had is not new prose")
code, out = slop({"notes.txt": (S / "old.md").read_text()}, moves=[("old.md", "notes.txt")])
check(code == 0, "a markdown file renamed to something that is not markdown is not scanned")
for name in ("caf\u00e9.md", "\u03b1\u03b2.md", 'a "quoted" name.md', "tab\there.md"):
    code, out = slop({name: f"x {EM} y\n"})
    check(code == 1 and name in out, f"an em dash in a markdown file named {name!r} is blocked")

# The hook, run in a small repository that has stand-ins where it calls the real tools: each
# stand-in leaves a file behind, which shows whether the hook called it.
HOOK = ROOT / "firmware/.githooks/pre-commit"
H = new_repo(TMP / "hook")
HOME = TMP / "hook-home"
HOME.mkdir()
script(H / "firmware/test/run_tests.sh", '#!/usr/bin/env bash\ntouch "$MARKS/tests"\nexit "${TESTS_RC:-0}"\n')
script(H / "firmware/scripts/version-lint.py", "import os, pathlib, sys\n"
       "pathlib.Path(os.environ['MARKS'], 'version').touch()\nsys.exit(int(os.environ.get('VERSION_RC', '0')))\n")
script(H / "firmware/scripts/cpp-lint.sh", '#!/usr/bin/env bash\necho "$@" > "$MARKS/cpp"\nexit "${CPP_RC:-0}"\n')
script(H / "docs/guide/lint.py", "import os, pathlib, sys\n"
       "pathlib.Path(os.environ['MARKS'], 'guide').touch()\nsys.exit(int(os.environ.get('GUIDE_RC', '0')))\n")
shutil.copy(SLOP, H / "firmware/scripts/stop-slop.sh")
write(H / "README.md", "# readme\n")
git(H, "add", "-A")
git(H, "commit", "-q", "-m", "base")
MARKS = TMP / "marks"


def hook(files, **env):
    """Stage `files` on a clean index and run the hook. Returns (exit code, output, the stand-ins that ran)."""
    shutil.rmtree(MARKS, ignore_errors=True)
    MARKS.mkdir()
    for name, text in files.items():
        write(H / name, text)
    git(H, "add", "-A")
    r = subprocess.run(["bash", str(HOOK)], capture_output=True, text=True, cwd=H,
                       env={**GIT_ENV, "HOME": str(HOME), "MARKS": str(MARKS), **env})
    git(H, "reset", "-q", "--hard")
    git(H, "clean", "-q", "-fd")
    return r.returncode, r.stdout + r.stderr, sorted(p.name for p in MARKS.iterdir())


for name, ran, why in (
        ("firmware/test/new_test.cpp", ["cpp", "tests"], "a host test: the tests and the C/C++ lint"),
        ("firmware/esphome/components/hisense_ac/x.py", ["tests"], "a component file: the tests"),
        ("firmware/scripts/dev.py", ["tests"], "dev.py: the tests"),
        ("firmware/scripts/matter-convert.py", ["tests"], "matter-convert.py: the tests"),
        ("firmware/scripts/cpp-lint.py", ["tests"], "the lint tool: the tests (test_scripts_edges.py covers it)"),
        ("firmware/scripts/esphome-compile-check.sh", ["tests"], "CI's compile gate script: the tests"),
        ("firmware/flasher/ch341common.py", ["tests"], "the clip flasher: the tests"),
        ("firmware/.githooks/pre-commit", ["tests"], "the hook itself: the tests"),
        ("reverse-engineering/tools/decode_ac_frames.py", ["tests"], "decode_ac_frames.py, which layer 2 and virtual_ac.py import: the tests"),
        ("reverse-engineering/tools/sniff.py", [], "another reverse-engineering tool: nothing"),
        ("firmware/esphome/packages/node.yaml", ["version"], "a package: the version lint, not the tests"),
        ("CHANGELOG.md", ["version"], "the changelog: the version lint"),
        (".github/workflows/qa.yaml", ["version"], "a workflow: the version lint (it holds a pin)"),
        ("README.md", [], "a document: nothing but the prose gate"),
        ("docs/guide/Home.md", ["guide"], "a guide page: the guide linter"),
        ("docs/guide/images/x.md", [], "a file below the guide's own directory: nothing, the linter does not read it"),
        ("firmware/esp32-recon/main/x.c", ["cpp"], "the diagnostic tool: the C/C++ lint only"),
):
    code, out, marks = hook({name: "x\n"})
    check(code == 0 and marks == ran, f"staged {why}")
code, out, marks = hook({"docs/guide/lint.py": (H / "docs/guide/lint.py").read_text() + "# changed\n"})
check(code == 0 and marks == ["guide", "tests"], "staged the guide linter: itself and the tests")
code, out, marks = hook({"docs/guide/Home.md": "x\n"}, GUIDE_RC="1")
check(code == 1 and "GUIDE LINT FAILED" in out, "a failing guide lint stops the commit")
code, out, marks = hook({"firmware/test/x.cpp": "x\n"}, TESTS_RC="1")
check(code == 1 and "HOST TESTS FAILED" in out and "cpp" not in marks, "failing host tests stop the commit before anything else runs")
code, out, marks = hook({"CHANGELOG.md": "x\n"}, VERSION_RC="1")
check(code == 1 and "VERSION LINT FAILED" in out, "a failing version lint stops the commit")
code, out, marks = hook({"firmware/esp32-recon/main/x.c": "x\n", "firmware/esp32-recon/main/y.h": "x\n", "a b.cpp": "x\n"})
check((MARKS / "cpp").read_text().split("\n")[0] == "check --no-tidy a b.cpp firmware/esp32-recon/main/x.c firmware/esp32-recon/main/y.h",
      "the C/C++ lint gets the staged sources, without clang-tidy")
code, out, marks = hook({"x.cpp": "x\n"}, CPP_LINT_TIDY="1")
check((MARKS / "cpp").read_text().strip() == "check x.cpp", "CPP_LINT_TIDY=1 adds clang-tidy")
code, out, marks = hook({"x.cpp": "x\n"}, CPP_RC="3")
check(code == 0 and "C/C++ lint skipped" in out, "exit 3 from the lint (pinned tools missing) warns and lets the commit through")
code, out, marks = hook({"x.cpp": "x\n"}, CPP_RC="1")
check(code == 1 and "C/C++ LINT FAILED" in out, "any other failure of the lint stops the commit")
code, out, marks = hook({"notes.md": f"x {EM} y\n"})
check(code == 1 and "em dash in new prose: notes.md" in out, "the prose gate runs on every commit")
chained = script(HOME / ".local/share/git/hooks/pre-commit", '#!/bin/sh\ntouch "$MARKS/global"\nexit "${GLOBAL_RC:-0}"\n')
code, out, marks = hook({"README.md": "x\n"})
check(code == 0 and marks == ["global"], "a global pre-commit hook in the user's home is chained to")
code, out, marks = hook({"README.md": "x\n"}, GLOBAL_RC="1")
check(code == 1, "and its failure stops the commit")
chained.unlink()

# A large commit. The hook used to decide with `git diff --cached --name-only | grep -qE ...` under
# `set -o pipefail`: grep -q exits at the first match, git is killed by SIGPIPE when the list is
# longer than the pipe holds, pipefail turns that into a failed pipeline and the host tests were
# skipped, silently. The list is read once now.
bulk = {"firmware/test/a.cpp": "x\n"}
bulk.update({f"zz/{'d' * 180}/{i:05d}.txt": "" for i in range(1500)})
code, out, marks = hook(bulk)
check(code == 0 and marks == ["cpp", "tests"], "a commit of 1500 files that includes a host test runs the host tests")
bulk = {f"aa/{'d' * 180}/{i:05d}.txt": "" for i in range(1500)}
bulk["zz/last.cpp"] = "x\n"
bulk["CHANGELOG.md"] = "x\n"
code, out, marks = hook(bulk)
check(code == 0 and marks == ["cpp", "version"] and (MARKS / "cpp").read_text().split() == ["check", "--no-tidy", "zz/last.cpp"],
      "and one whose only C++ file and changelog come after 1500 others still gets the lints")
code, out, marks = hook({"firmware/esphome/components/hisense_ac/caf\u00e9.cpp": "x\n"})
check(code == 0 and marks == ["cpp", "tests"] and "caf\u00e9.cpp" in (MARKS / "cpp").read_text(),
      "a staged source with a non-ASCII name reaches the lint under its real name")

# ================================================================================================
print("== esphome-compile-check.sh, with a stand-in for esphome ==")

C = TMP / "compile"
shutil.copytree(SCRIPTS, C / "firmware/scripts", ignore=shutil.ignore_patterns("__pycache__", "*.env"))
write(C / "firmware/esphome/secrets.yaml.example", "wifi_ssid: example\n")
write(C / VERSION_FILE, version_yaml("3.1.4"))
# esphome -s name NAME compile CONFIG: writes the generated defines.h the script looks at, the
# way ESPHome does, and prints what FAKE_OUTPUT says.
FAKE_ESPHOME = script(TMP / "fake-esphome", '''#!/usr/bin/env bash
name="$3"; config="$5"
echo "$name $config" >> "$FAKE_LOG"
[ -f secrets.yaml ] || { echo "no secrets.yaml"; exit 9; }
out="$(dirname "$config")/.esphome/build/$name/src/esphome/core"
mkdir -p "$out"
case "$config" in
  *tests/*) echo "#define ESPHOME_VERSION 1" > "$out/defines.h" ;;
  *) echo "#define ESPHOME_PROJECT_VERSION \\"${FAKE_VERSION:-3.1.4}\\"" > "$out/defines.h" ;;
esac
printf '%b\\n' "${FAKE_OUTPUT:-}"
echo "INFO Successfully compiled program."
exit "${FAKE_RC:-0}"
''')
CC = C / "firmware/scripts/esphome-compile-check.sh"


def compile_check(*configs, **env):
    (TMP / "fake.log").unlink(missing_ok=True)
    r = subprocess.run(["bash", str(CC), *configs], capture_output=True, text=True, cwd=TMP,
                       env={**GIT_ENV, "ESPHOME": str(FAKE_ESPHOME), "FAKE_LOG": str(TMP / "fake.log"), **env})
    log = (TMP / "fake.log").read_text().splitlines() if (TMP / "fake.log").exists() else []
    return r.returncode, r.stdout + r.stderr, log


code, out, log = compile_check()
check(code == 2 and "usage:" in out and not log, "no configuration given: usage, exit 2")
check(not (C / "firmware/esphome/secrets.yaml").exists(), "and nothing was touched")
code, out, log = compile_check("w41h1.yaml", "tests/build.hub-only.esp32-c3-idf.yaml")
check(code == 0 and "compiled clean: w41h1.yaml tests/build.hub-only.esp32-c3-idf.yaml" in out, "two configurations that compile clean pass")
check(log == ["ci-w41h1 w41h1.yaml", "ci-build-hub-only-esp32-c3-idf tests/build.hub-only.esp32-c3-idf.yaml"],
      "each gets a node name of its own made from the file name, dots turned to hyphens")
check((C / "firmware/esphome/secrets.yaml").read_text() == "wifi_ssid: example\n", "a missing secrets.yaml is copied from the example")
write(C / "firmware/esphome/secrets.yaml", "wifi_ssid: mine\n")
compile_check("w41h1.yaml")
check((C / "firmware/esphome/secrets.yaml").read_text() == "wifi_ssid: mine\n", "an existing secrets.yaml is left alone")
code, out, log = compile_check("w41h1.yaml", "w41h1-esp32c3.yaml", FAKE_RC="1", FAKE_OUTPUT="src/main.cpp:1:1: error: boom")
check(code == 1 and "::error::w41h1.yaml does not compile" in out and "error: boom" in out and len(log) == 1,
      "a configuration that does not compile fails, shows the end of the log and stops there")
WARN = "src/esphome/components/hisense_ac/hisense_bus.cpp:10:5: warning: unused variable 'x' [-Wunused-variable]"
code, out, log = compile_check("w41h1.yaml", FAKE_OUTPUT=WARN)
check(code == 1 and "does not compile cleanly" in out, "a compiler warning from hisense_ac fails the gate")
code, out, log = compile_check("w41h1.yaml", FAKE_OUTPUT=WARN.replace("hisense_ac/hisense_bus", "w41h1_module/w41h1_module"))
check(code == 1, "and one from w41h1_module")
code, out, log = compile_check("w41h1.yaml", FAKE_OUTPUT="src/esphome/components/wifi/wifi_component.cpp:1:1: warning: theirs")
check(code == 0, "a warning from another component passes: only this repo's two are held clean")
# The check is a text search for a component path and the word warning or error on one line.
code, out, log = compile_check("w41h1.yaml", FAKE_OUTPUT="Compiling .pioenvs/x/src/esphome/components/hisense_ac/error_codes.cpp.o")
check(code == 1, "a source FILE named error_* in a component trips the gate with no warning at all (behaviour today)")
code, out, log = compile_check("w41h1.yaml", FAKE_VERSION="3.1.3")
check(code == 1 and "was built without firmware version 3.1.4" in out, "an image whose project version is not the version file's fails")
code, out, log = compile_check("tests/build.esp32-idf.yaml")
check(code == 0, "a file under tests/ carries no version and is not asked for one")
code, out, log = compile_check("./tests/build.esp32-idf.yaml")
check(code == 1 and "was built without firmware version" in out,
      "the same file given as ./tests/... is asked for a version: the exemption is a prefix match on the argument")

# ================================================================================================
print("== how the lists agree: workflows, run_tests.sh, the lint scope, .gitignore ==")

release = (ROOT / ".github/workflows/esphome-release.yaml").read_text()
qa = (ROOT / ".github/workflows/qa.yaml").read_text()
boards = sorted(p.name for p in (ROOT / "firmware/esphome").glob("w41h1*.yaml"))
listed = sorted(set(re.findall(r"w41h1[\w-]*\.yaml", "".join(re.findall(r'^\s*all="(.*)"$', release, re.M)))))
matrix = sorted(set(re.findall(r"w41h1[\w-]*\.yaml", "".join(re.findall(r"^\s*configs: (.*)$", release, re.M)))))
check(listed == boards, "esphome-release.yaml: the hand-written list of board files is the set of board files")
check(matrix == boards, "esphome-release.yaml: the compile matrix names every board file (three copies of one list)")
qa_compiled = {b for b in boards if b in qa or (b.startswith("w41h1-amebaz2-") and f"{b[14:-5]}:0x" in qa)}
not_in_qa = sorted(set(boards) - qa_compiled)
known_failure(not not_in_qa, "every board file is compiled by qa.yaml",
              f"qa.yaml compiles the ESP32 and stock-module files only: {', '.join(not_in_qa)} get `esphome config` "
              "on a pull request and their first compile when a release tag is pushed, which is the worst moment "
              "to find out they do not build")

pin_sh = (SCRIPTS / "cpp-lint.sh").read_text()
fmt_pin = re.search(r"^CLANG_FORMAT_VERSION=(\S+)$", pin_sh, re.M).group(1)
tidy_pin = re.search(r"^CLANG_TIDY_VERSION=(\S+)$", pin_sh, re.M).group(1)
for path in (".github/workflows/qa.yaml", "CONTRIBUTING.md"):
    text = (ROOT / path).read_text()
    check(f"clang-format {fmt_pin}" in text and f"clang-tidy {tidy_pin}" in text, f"{path} quotes the clang pins of cpp-lint.sh ({fmt_pin}, {tidy_pin})")

tracked = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z"], capture_output=True, text=True, env=GIT_ENV).stdout.split("\0")
shell = sorted(f for f in tracked if f.endswith(".sh") or f.startswith("firmware/.githooks/"))
linted = [f for f in shell if re.match(r"firmware/(scripts|test)/[^/]+\.sh$", f) or f.startswith("firmware/.githooks/")]
known_failure(linted == shell, "CI's shellcheck step covers every tracked shell script",
              f"qa.yaml:40 names firmware/scripts/*.sh, firmware/test/*.sh and the hooks: "
              f"{', '.join(sorted(set(shell) - set(linted)))} is never linted")

outputs = sorted(set(re.findall(r"-o\s+([\w.]+)", run_tests)))
ignored = {o: subprocess.run(["git", "-C", str(ROOT), "check-ignore", "-q", f"firmware/test/{o}"], env=GIT_ENV).returncode == 0
           for o in outputs}
binaries = [o for o in outputs if not o.endswith(".o")]
check(all(ignored[o] for o in binaries), f"every test binary run_tests.sh builds is in .gitignore ({len(binaries)} of them)")
objects = [o for o in outputs if o.endswith(".o")]
known_failure(all(ignored[o] for o in objects), "the object files run_tests.sh builds are in .gitignore",
              f"run_tests.sh:26-31 builds {', '.join(objects)} in firmware/test/ and removes them after the link. "
              "When the link fails `set -e` stops before the rm and they stay as untracked files that nothing ignores")

for layer in ("test_image_chain.py", "test_dev_release.py", "test_matter_convert.py", "test_flasher.py", "test_version_lint.py"):
    check(layer in run_tests, f"run_tests.sh runs {layer}")

tmp_holder.cleanup()
print(f"== SCRIPTS EDGES OK ({known} known failures) ==" if ok else "== SCRIPTS EDGES FAILED ==")
sys.exit(0 if ok else 1)
