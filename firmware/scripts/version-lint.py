#!/usr/bin/env python3
"""version-lint.py -- the firmware version, the changelog and the release tag, held together.

The version is written once, in firmware/esphome/packages/version.yaml. A release is the git tag
`v<version>`. This script is what run_tests.sh, the pre-commit hook, the QA workflow and the
release workflow all call, so they cannot disagree.

  version-lint.py                    the checks that need no git history (below)
  version-lint.py --tag v2.0.0       also: the tag is `v` + the version, and CHANGELOG.md has
                                     that version's own section (Unreleased is not enough)
  version-lint.py --base origin/main also: if anything under firmware/esphome/components/ or
                                     firmware/esphome/packages/ differs from the merge base with
                                     that ref, CHANGELOG.md has to differ too
  version-lint.py --print            print the version and nothing else
  version-lint.py --notes [VERSION]  print that version's CHANGELOG.md section (default: the
                                     version in version.yaml), for the release text

Always checked:
  * the version is MAJOR.MINOR.PATCH, digits only, no leading zeros;
  * CHANGELOG.md has a `## <version>` section or a `## Unreleased` one;
  * the version is not below the newest `v*` tag, when the clone has tags;
  * every place that pins the ESPHome release names the same one.

A version bump is not required for a change: the number moves when a release is cut
(CONTRIBUTING.md, "Releasing"). --repo <dir> checks another tree, which is how the host test
(firmware/test/test_version_lint.py) feeds it bad input.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

VERSION_FILE = "firmware/esphome/packages/version.yaml"
CHANGELOG = "CHANGELOG.md"
# A change under these needs a changelog entry in the same pull request.
WATCHED = ("firmware/esphome/components/", "firmware/esphome/packages/")
SEMVER = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)")
# Where the ESPHome release is pinned, and how each file spells it.
ESPHOME_PINS = (
    (".github/workflows/qa.yaml", r'^\s*ESPHOME_VERSION:\s*"?([0-9.]+)"?\s*$'),
    (".github/workflows/esphome-release.yaml", r'^\s*ESPHOME_VERSION:\s*"?([0-9.]+)"?\s*$'),
    ("firmware/scripts/dev.py", r'^ESPHOME_PIN = "([0-9.]+)"'),
    ("firmware/scripts/esphome-upstream-check.sh", r'^ESPHOME_SPEC="esphome==([0-9.]+)"'),
)


class Problem(Exception):
    pass


def read_version(repo):
    path = repo / VERSION_FILE
    if not path.is_file():
        raise Problem(f"{VERSION_FILE} is missing")
    found = re.findall(r'^    version:\s*"?([^"\s#]+)"?\s*(?:#.*)?$', path.read_text(encoding="utf-8"), re.M)
    if len(found) != 1:
        raise Problem(f"{VERSION_FILE}: expected one `version:` under `esphome: project:`, found {len(found)}")
    return found[0]


def as_tuple(version):
    return tuple(int(part) for part in version.split("."))


def changelog_sections(repo):
    """Second-level headings of CHANGELOG.md, in order: [(title, first line, line after last)]."""
    path = repo / CHANGELOG
    if not path.is_file():
        raise Problem(f"{CHANGELOG} is missing")
    lines = path.read_text(encoding="utf-8").splitlines()
    starts = [i for i, line in enumerate(lines) if line.startswith("## ")]
    sections = []
    for n, i in enumerate(starts):
        end = starts[n + 1] if n + 1 < len(starts) else len(lines)
        sections.append((lines[i][3:].strip(), i + 1, end))
    return lines, sections


def version_section(sections, version):
    """The section of one release: `## 2.0.0`, `## 2.0.0 - 2026-10-12` or `## [2.0.0] - ...`."""
    want = re.compile(r"\[?v?" + re.escape(version) + r"\]?(\s+-\s+.*)?")
    for title, first, end in sections:
        if want.fullmatch(title):
            return first, end
    return None


def git(repo, *args):
    # Under a git hook, GIT_DIR and GIT_INDEX_FILE name the repository being committed to and
    # would win over -C. The tree asked about is `repo`, so they are dropped.
    env = {k: v for k, v in os.environ.items()
           if k not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_PREFIX", "GIT_COMMON_DIR")}
    r = subprocess.run(["git", "-C", str(repo), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                       env=env)
    if r.returncode != 0:
        raise Problem(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


def check_basic(repo, problems):
    version = read_version(repo)
    if not SEMVER.fullmatch(version):
        problems.append(f"{VERSION_FILE}: '{version}' is not MAJOR.MINOR.PATCH (digits only, no leading zeros, "
                        "no suffix)")
        return version
    _, sections = changelog_sections(repo)
    titles = [title for title, _, _ in sections]
    if version_section(sections, version) is None and not any(t.lower().startswith("unreleased") for t in titles):
        problems.append(f"{CHANGELOG} has neither a `## {version}` section nor a `## Unreleased` one")

    # Not below the newest release. A shallow CI checkout has no tags, and then there is nothing
    # to compare with: the tag check of the release workflow covers the release itself.
    try:
        tags = git(repo, "tag", "--list", "v[0-9]*").split()
    except (Problem, FileNotFoundError):
        tags = []
    released = [as_tuple(t[1:]) for t in tags if SEMVER.fullmatch(t[1:])]
    if released and as_tuple(version) < max(released):
        newest = ".".join(str(n) for n in max(released))
        problems.append(f"{VERSION_FILE}: {version} is below the newest release tag v{newest}")

    pins = {}
    for rel, pattern in ESPHOME_PINS:
        path = repo / rel
        if not path.is_file():
            continue
        found = set(re.findall(pattern, path.read_text(encoding="utf-8"), re.M))
        if len(found) != 1:
            problems.append(f"{rel}: expected one ESPHome pin, found {sorted(found) or 'none'}")
        else:
            pins[rel] = found.pop()
    if len(set(pins.values())) > 1:
        problems.append("the ESPHome pin differs: " + ", ".join(f"{rel} {pin}" for rel, pin in pins.items()))
    return version


def check_tag(repo, version, tag, problems):
    if tag != f"v{version}":
        problems.append(f"tag '{tag}' is not 'v{version}', the version in {VERSION_FILE}")
    _, sections = changelog_sections(repo)
    if version_section(sections, version) is None:
        problems.append(f"{CHANGELOG} has no `## {version}` section: rename `## Unreleased` to "
                        f"`## {version} - <date>` before tagging")


def check_base(repo, base, problems):
    merge_base = git(repo, "merge-base", base, "HEAD").strip()
    changed = git(repo, "diff", "--name-only", merge_base, "HEAD").split("\n")
    watched = [f for f in changed if f.startswith(WATCHED)]
    if watched and CHANGELOG not in changed:
        shown = ", ".join(watched[:5]) + (f" and {len(watched) - 5} more" if len(watched) > 5 else "")
        problems.append(f"{shown} changed against {base} and {CHANGELOG} did not: add an entry under "
                        "`## Unreleased`")


def notes(repo, version):
    lines, sections = changelog_sections(repo)
    span = version_section(sections, version)
    if span is None:
        raise Problem(f"{CHANGELOG} has no `## {version}` section")
    body = "\n".join(lines[span[0]:span[1]]).strip()
    if not body:
        raise Problem(f"{CHANGELOG}: the `## {version}` section is empty")
    return body


def main(argv):
    repo = Path(__file__).resolve().parent.parent.parent
    tag = base = None
    mode = "check"
    notes_version = None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("--repo", "--tag", "--base") and i + 1 < len(argv):
            if a == "--repo":
                repo = Path(argv[i + 1])
            elif a == "--tag":
                tag = argv[i + 1]
            else:
                base = argv[i + 1]
            i += 2
        elif a == "--print":
            mode = "print"; i += 1
        elif a == "--notes":
            mode = "notes"; i += 1
            if i < len(argv) and not argv[i].startswith("--"):
                notes_version = argv[i]; i += 1
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2

    try:
        if mode == "print":
            print(read_version(repo))
            return 0
        if mode == "notes":
            print(notes(repo, notes_version or read_version(repo)))
            return 0
        problems = []
        version = check_basic(repo, problems)
        if tag is not None:
            check_tag(repo, version, tag, problems)
        if base is not None:
            check_base(repo, base, problems)
    except Problem as e:
        problems = [str(e)]
        version = "?"
    for p in problems:
        print(f"[version-lint] FAIL: {p}", file=sys.stderr)
    if problems:
        return 1
    extra = "".join(f", {what}" for what in ((f"tag {tag}" if tag else ""), (f"base {base}" if base else "")) if what)
    print(f"[version-lint] ok: firmware {version}{extra}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
