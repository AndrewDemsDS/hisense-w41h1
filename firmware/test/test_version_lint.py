#!/usr/bin/env python3
"""version-lint.py against the real tree, and against small trees made to fail it.

No network. The made-up trees are git repositories in a temporary directory, so the merge-base
rule and the tag rule run for real.
"""

import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
LINT = ROOT / "firmware" / "scripts" / "version-lint.py"
VERSION_FILE = "firmware/esphome/packages/version.yaml"

ok = True


def check(cond, msg):
    global ok
    print(("  ok  " if cond else "  FAIL ") + msg)
    ok = ok and cond


CLEAN_ENV = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}


def lint(repo, *args):
    return subprocess.run([sys.executable, str(LINT), "--repo", str(repo), *args], capture_output=True, text=True,
                          env=CLEAN_ENV)


def git(repo, *args):
    # A throwaway tree, not a commit to this project: git runs without the user's configuration
    # (identity, hooks, signing), so the test behaves the same on a laptop and on a CI runner. It
    # also drops every GIT_* variable: under the pre-commit hook they name this repository.
    env = {**CLEAN_ENV, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_SYSTEM": os.devnull,
           "GIT_AUTHOR_NAME": "test", "GIT_AUTHOR_EMAIL": "test@example.com",
           "GIT_COMMITTER_NAME": "test", "GIT_COMMITTER_EMAIL": "test@example.com"}
    subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, env=env)


def write(repo, rel, text):
    path = repo / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def version_yaml(version):
    return f'esphome:\n  project:\n    name: someone.something\n    version: "{version}"\n'


def tree(tmp, name, version="2.0.0", changelog="# Changelog\n\n## Unreleased\n\n- a change\n"):
    repo = pathlib.Path(tmp) / name
    repo.mkdir()
    git(repo, "init", "-q", "-b", "main")
    # Nothing below may run unless the repository really is the new one in the temporary directory.
    if not (repo / ".git" / "HEAD").is_file():
        sys.exit(f"test_version_lint: git init did not create {repo}/.git, refusing to go on")
    write(repo, VERSION_FILE, version_yaml(version))
    write(repo, "CHANGELOG.md", changelog)
    write(repo, "firmware/esphome/components/hisense_ac/hub.cpp", "// v1\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "base")
    return repo


# --- the real tree ---
r = subprocess.run([sys.executable, str(LINT)], capture_output=True, text=True)
check(r.returncode == 0, f"this tree passes: {(r.stdout or r.stderr).strip()}")
real = subprocess.run([sys.executable, str(LINT), "--print"], capture_output=True, text=True).stdout.strip()
check(real.count(".") == 2, f"--print gives the version alone: {real}")
r = subprocess.run([sys.executable, str(LINT), "--tag", "v0.0.1"], capture_output=True, text=True)
check(r.returncode == 1 and "is not 'v" in r.stderr, "this tree refuses a tag that is not its version")

with tempfile.TemporaryDirectory() as tmp:
    # --- the version itself ---
    good = tree(tmp, "good")
    check(lint(good).returncode == 0, "a tree with a semver version and an Unreleased section passes")
    for bad in ("2.0", "2.0.0-rc1", "v2.0.0", "02.0.0", "2.0.0.1", "latest"):
        r = lint(tree(tmp, f"bad-{bad}", version=bad))
        check(r.returncode == 1 and "MAJOR.MINOR.PATCH" in r.stderr, f"version '{bad}' is refused")
    r = lint(tree(tmp, "no-section", changelog="# Changelog\n\n## 1.9.0 - 2026-01-01\n\n- old\n"))
    check(r.returncode == 1 and "neither" in r.stderr, "a changelog with no section for the version and no Unreleased is refused")
    released = tree(tmp, "released", changelog="# Changelog\n\n## 2.0.0 - 2026-10-12\n\n- the release\n\n## 1.0.0\n\n- old\n")
    check(lint(released).returncode == 0, "a changelog with the version's own section passes without Unreleased")

    # --- the tag ---
    check(lint(released, "--tag", "v2.0.0").returncode == 0, "tag v2.0.0 on version 2.0.0 with its section passes")
    r = lint(released, "--tag", "v2.0.1")
    check(r.returncode == 1 and "is not 'v2.0.0'" in r.stderr, "a tag that is not the version is refused")
    r = lint(released, "--tag", "2.0.0")
    check(r.returncode == 1, "a tag without the leading v is refused")
    r = lint(good, "--tag", "v2.0.0")
    check(r.returncode == 1 and "no `## 2.0.0` section" in r.stderr, "a tag while the changelog only has Unreleased is refused")
    git(released, "tag", "v2.1.0")
    r = lint(released)
    check(r.returncode == 1 and "below the newest release tag v2.1.0" in r.stderr, "a version below the newest release tag is refused")
    git(released, "tag", "esp32-v9.9.9")
    git(released, "tag", "-d", "v2.1.0")
    check(lint(released).returncode == 0, "the retired firmwares' tags (esp32-v*) are not releases of this one")

    # --- release notes ---
    r = lint(released, "--notes")
    check(r.returncode == 0 and r.stdout.strip() == "- the release", "--notes prints the version's section and stops at the next one")
    check(lint(good, "--notes").returncode == 1, "--notes fails when the version has no section")

    # --- a pull request: component or package change without a changelog entry ---
    git(good, "checkout", "-q", "-b", "feature")
    write(good, "firmware/esphome/components/hisense_ac/hub.cpp", "// v2\n")
    git(good, "commit", "-q", "-am", "component change")
    r = lint(good, "--base", "main")
    check(r.returncode == 1 and "CHANGELOG.md did not" in r.stderr, "a component change without a changelog entry is refused")
    write(good, "CHANGELOG.md", "# Changelog\n\n## Unreleased\n\n- a change\n- another\n")
    git(good, "commit", "-q", "-am", "changelog")
    check(lint(good, "--base", "main").returncode == 0, "the same change with a changelog entry passes, without a version bump")
    git(good, "checkout", "-q", "-b", "docs-only", "main")
    write(good, "README.md", "docs\n")
    git(good, "add", "-A")
    git(good, "commit", "-q", "-m", "docs")
    check(lint(good, "--base", "main").returncode == 0, "a change outside components/ and packages/ needs no entry")
    git(good, "checkout", "-q", "-b", "package", "main")
    write(good, "firmware/esphome/packages/node.yaml", "logger:\n")
    git(good, "add", "-A")
    git(good, "commit", "-q", "-m", "package change")
    check(lint(good, "--base", "main").returncode == 1, "a package change without a changelog entry is refused")

    # --- the ESPHome pin ---
    pins = tree(tmp, "pins")
    write(pins, ".github/workflows/qa.yaml", 'env:\n  ESPHOME_VERSION: "2026.7.4"\n')
    write(pins, "firmware/scripts/dev.py", 'ESPHOME_PIN = "2026.7.4"\n')
    check(lint(pins).returncode == 0, "the same ESPHome pin in two places passes")
    write(pins, "firmware/scripts/dev.py", 'ESPHOME_PIN = "2026.8.0"\n')
    r = lint(pins)
    check(r.returncode == 1 and "ESPHome pin differs" in r.stderr, "an ESPHome pin that differs between two files is refused")

print("== VERSION LINT OK ==" if ok else "== VERSION LINT FAILED ==")
sys.exit(0 if ok else 1)
