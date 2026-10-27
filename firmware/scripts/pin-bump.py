#!/usr/bin/env python3
"""pin-bump.py -- find a newer ESPHome or ESP-IDF release and move the repository's pin to it.

The scheduled workflow .github/workflows/pin-bumps.yaml runs it and opens a pull request with
the result. It never merges, and nothing reaches a unit: the maintainer merges and flashes.

  pin-bump.py esphome --check            the pinned ESPHome release and the newest stable one on PyPI
  pin-bump.py esphome --set X.Y.Z        rewrite every pin (version-lint.py's list), the docs that
                                         name the release, and add a CHANGELOG.md line
  pin-bump.py idf --check                the ESP-IDF image of the nightly esp32-recon job and the
                                         newest tag on the same vMAJOR.MINOR line on Docker Hub
  pin-bump.py idf --set vX.Y.Z [--digest sha256:...]
                                         rewrite the image (tag and linux/amd64 digest), the job's
                                         comment and the esp32-recon README. Without --digest the
                                         digest is fetched from the registry

  --output FILE   (with --check) append current=, latest=, due=true|false (and digest= for idf)
                  to FILE, in the form of $GITHUB_OUTPUT
  --repo DIR      work on another tree (how firmware/test/test_pin_bump.py feeds it fixtures)

ESP-IDF stays on the line it is on: with v5.5.5 pinned, v5.5.6 is a bump and v6.0.0 is not. A
move to another MAJOR.MINOR is a decision for a person (a new toolchain and API changes), made
by hand. ESPHome takes the newest stable release: digits only, no beta (b) or release candidate.

Exit codes: 0 done (also when the pin is already current), 1 a network or repository problem
(with a message), 2 a usage error. Stdlib only.
"""

import importlib.util
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent
ROOT = SCRIPTS.parent.parent
TIMEOUT = 30

# The places that pin the ESPHome release are version-lint.py's table, so the check and the bump
# cannot list different files.
sys.dont_write_bytecode = True   # no __pycache__ beside the scripts
_spec = importlib.util.spec_from_file_location("version_lint", SCRIPTS / "version-lint.py")
version_lint = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(version_lint)
ESPHOME_PINS = version_lint.ESPHOME_PINS

# Prose that tells a reader which ESPHome to install. version-lint.py does not hold these, so a
# mention that is missing or names another release is reported and left alone, never an error.
# Examples of a firmware string ("2.0.0 (ESPHome 2026.9.1)") are history and not listed.
ESPHOME_DOCS = (
    ("README.md", r"ESPHome ([0-9.]+) installed"),
    ("README.md", r"esphome==([0-9.]+)"),
    ("CONTRIBUTING.md", r"pins \(([0-9.]+) at the time of"),
    ("docs/guide/Build-Flash-Test.md", r"esphome==([0-9.]+)"),
    ("docs/guide/ESP32-Build-Environment.md", r"esphome==([0-9.]+)"),
    ("docs/guide/ESP32-Build-Environment.md", r"`([0-9.]+)`, the release CI builds with"),
)

NIGHTLY = ".github/workflows/nightly.yaml"
RECON_README = "firmware/esp32-recon/README.md"
IDF_IMAGE = re.compile(r"^(\s*image:\s*espressif/idf:)(v\d+\.\d+\.\d+)@(sha256:[0-9a-f]{64})\s*$", re.M)
# The job and the comment block right above it: the only part of nightly.yaml that names IDF.
RECON_JOB = re.compile(r"(?ms)^(?:  #[^\n]*\n)*  esp32-recon:\n.*?(?=^(?:  #[^\n]*\n)*  [\w-]+:\n|\Z)")
RECON_DOC = r"every night with (v[0-9.]+) in Espressif's container"

PYPI = "https://pypi.org/pypi/esphome/json"
HUB_TAGS = "https://hub.docker.com/v2/repositories/espressif/idf/tags?page_size=100&name="
REGISTRY_TOKEN = "https://auth.docker.io/token?service=registry.docker.io&scope=repository:espressif/idf:pull"
REGISTRY_MANIFEST = "https://registry-1.docker.io/v2/espressif/idf/manifests/"
INDEX_TYPES = "application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json"

STABLE = re.compile(r"\d+\.\d+\.\d+")
IDF_TAG = re.compile(r"v(\d+)\.(\d+)\.(\d+)")


class Problem(Exception):
    pass


def as_tuple(version):
    return tuple(int(part) for part in version.lstrip("v").split("."))


# ---- network ----------------------------------------------------------------------------------

def fetch_json(url, headers=None):
    req = urllib.request.Request(url, headers={"User-Agent": "hisense-w41h1 pin-bump", **(headers or {})})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            return json.load(r)
    except (urllib.error.URLError, TimeoutError, OSError, ValueError) as e:
        raise Problem(f"cannot read {url.split('?')[0]}: {e}")


def newest_esphome():
    """The newest release on PyPI with digits only, at least one file, not every file yanked."""
    data = fetch_json(PYPI)
    stable = [v for v, files in data.get("releases", {}).items()
              if STABLE.fullmatch(v) and files and not all(f.get("yanked") for f in files)]
    if not stable:
        raise Problem("PyPI lists no stable esphome release")
    return max(stable, key=as_tuple)


def newest_idf(line):
    """The newest vMAJOR.MINOR.PATCH tag of espressif/idf on the line (MAJOR, MINOR)."""
    url, tags = HUB_TAGS + f"v{line[0]}.{line[1]}.", []
    while url:
        page = fetch_json(url)
        tags += [r.get("name", "") for r in page.get("results", [])]
        url = page.get("next")
    found = [t for t in tags if IDF_TAG.fullmatch(t) and as_tuple(t)[:2] == line]
    if not found:
        raise Problem(f"Docker Hub lists no espressif/idf tag on v{line[0]}.{line[1]}")
    return max(found, key=as_tuple)


def idf_digest(tag):
    """The digest of the linux/amd64 image of the tag (from the manifest list, not the list's own)."""
    token = fetch_json(REGISTRY_TOKEN).get("token")
    if not token:
        raise Problem("the Docker registry gave no pull token")
    index = fetch_json(REGISTRY_MANIFEST + tag, {"Authorization": f"Bearer {token}", "Accept": INDEX_TYPES})
    for m in index.get("manifests", []):
        p = m.get("platform", {})
        if p.get("os") == "linux" and p.get("architecture") == "amd64" and re.fullmatch(r"sha256:[0-9a-f]{64}", m.get("digest", "")):
            return m["digest"]
    raise Problem(f"espressif/idf:{tag} has no linux/amd64 image in its manifest list")


# ---- the tree ---------------------------------------------------------------------------------

def read(repo, rel):
    path = repo / rel
    if not path.is_file():
        raise Problem(f"{rel} is missing")
    return path.read_text(encoding="utf-8")


def write(repo, rel, text):
    (repo / rel).write_text(text, encoding="utf-8")


def replace_group(text, pattern, old, new, flags=re.M):
    """Replace capture group 1 of every match of pattern whose group is `old`. Returns (text, n)."""
    count = 0

    def sub(m):
        nonlocal count
        if m.group(1) != old:
            return m.group(0)
        count += 1
        start, end = m.start(1) - m.start(0), m.end(1) - m.start(0)
        return m.group(0)[:start] + new + m.group(0)[end:]

    return re.sub(pattern, sub, text, flags=flags), count


def esphome_pin(repo):
    found = {}
    for rel, pattern in ESPHOME_PINS:
        values = set(re.findall(pattern, read(repo, rel), re.M))
        if len(values) != 1:
            raise Problem(f"{rel}: expected one ESPHome pin, found {sorted(values) or 'none'}")
        found[rel] = values.pop()
    if len(set(found.values())) != 1:
        raise Problem("the ESPHome pins differ (run version-lint.py): "
                      + ", ".join(f"{rel} {v}" for rel, v in found.items()))
    return next(iter(found.values()))


def idf_pin(repo):
    found = IDF_IMAGE.findall(read(repo, NIGHTLY))
    if len(found) != 1:
        raise Problem(f"{NIGHTLY}: expected one `image: espressif/idf:vX.Y.Z@sha256:...` line, found {len(found)}")
    return found[0][1], found[0][2]


def add_changelog_line(repo, line):
    """Append a bullet to `## Unreleased`, creating the section above the first release if needed."""
    lines = read(repo, version_lint.CHANGELOG).split("\n")
    _, sections = version_lint.changelog_sections(repo)
    unreleased = [(first, end) for title, first, end in sections if version_lint.is_unreleased(title)]
    if unreleased:
        first, end = unreleased[0]
        last = end
        while last > first and not lines[last - 1].strip():
            last -= 1
        # A blank line between bullets, as the file writes them, and one before the next heading.
        lines[last:last] = ["", line] + ([""] if last == end and end < len(lines) else [])
    else:
        at = sections[0][1] - 1 if sections else len(lines)
        lines[at:at] = ["## Unreleased", "", line, ""]
    write(repo, version_lint.CHANGELOG, "\n".join(lines))


def set_esphome(repo, new, notes):
    if not STABLE.fullmatch(new):
        raise Problem(f"'{new}' is not a stable ESPHome release (X.Y.Z, digits only)")
    old = esphome_pin(repo)
    if new == old:
        notes.append(f"esphome: already pinned to {old}, nothing to change")
        return False
    if as_tuple(new) < as_tuple(old):
        raise Problem(f"{new} is older than the pinned {old}: a downgrade is done by hand")
    for rel, pattern in ESPHOME_PINS:
        text, n = replace_group(read(repo, rel), pattern, old, new)
        write(repo, rel, text)
        notes.append(f"  {rel}: {n} pin(s)")
    for rel, pattern in ESPHOME_DOCS:
        path = repo / rel
        if not path.is_file():
            notes.append(f"  {rel}: missing, skipped")
            continue
        text, n = replace_group(path.read_text(encoding="utf-8"), pattern, old, new)
        write(repo, rel, text)
        notes.append(f"  {rel}: {n} mention(s)" + ("" if n else f" (no `{pattern}` naming {old}: check it by hand)"))
    add_changelog_line(repo, f"- Builds with ESPHome {new} (was {old}).")
    notes.append(f"  {version_lint.CHANGELOG}: a line under ## Unreleased")
    notes.append(f"esphome: {old} -> {new}")
    return True


def set_idf(repo, new, digest, notes):
    m = IDF_TAG.fullmatch(new)
    if not m:
        raise Problem(f"'{new}' is not an ESP-IDF release tag (vX.Y.Z)")
    if digest is not None and not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        raise Problem(f"'{digest}' is not a sha256 digest")
    old, old_digest = idf_pin(repo)
    if new == old:
        notes.append(f"idf: already on {old}, nothing to change")
        return False
    if as_tuple(new)[:2] != as_tuple(old)[:2]:
        raise Problem(f"{new} leaves the {old.rsplit('.', 1)[0]} line: a move to another ESP-IDF line is made by hand")
    if as_tuple(new) < as_tuple(old):
        raise Problem(f"{new} is older than the pinned {old}: a downgrade is done by hand")
    digest = digest or idf_digest(new)
    text = read(repo, NIGHTLY)
    job = RECON_JOB.search(text)
    if job is None:
        raise Problem(f"{NIGHTLY}: no esp32-recon job")
    block = re.sub(rf"\b{re.escape(old)}(?![\d.]*\d)", new, job.group(0)).replace(old_digest, digest)
    n = len(re.findall(rf"\b{re.escape(old)}(?![\d.]*\d)", job.group(0)))
    write(repo, NIGHTLY, text[:job.start()] + block + text[job.end():])
    notes.append(f"  {NIGHTLY}: the image (tag and digest) and {n - 1} other mention(s) in the esp32-recon job")
    path = repo / RECON_README
    if path.is_file():
        doc, k = replace_group(path.read_text(encoding="utf-8"), RECON_DOC, old, new)
        write(repo, RECON_README, doc)
        notes.append(f"  {RECON_README}: {k} mention(s)" + ("" if k else " (check it by hand)"))
    notes.append(f"idf: {old} -> {new} ({digest})")
    return True


# ---- commands ---------------------------------------------------------------------------------

def check(what, repo, output):
    if what == "esphome":
        current = esphome_pin(repo)
        latest = newest_esphome()
        extra = {}
        name = "ESPHome"
    else:
        current, _ = idf_pin(repo)
        latest = newest_idf(as_tuple(current)[:2])
        extra = {"digest": idf_digest(latest)}
        name = "ESP-IDF (esp32-recon)"
    due = as_tuple(latest) > as_tuple(current)
    if due:
        print(f"{what}: pinned {current}, newest {latest}: a bump is due")
    else:
        print(f"{what}: pinned {current}, newest {latest}: {name} is current, nothing to do")
    for k, v in extra.items():
        print(f"{what}: {latest} {k} {v}")
    if output:
        with open(output, "a", encoding="utf-8") as f:
            for k, v in {"current": current, "latest": latest, "due": "true" if due else "false", **extra}.items():
                f.write(f"{k}={v}\n")
    return 0


def usage():
    print(__doc__.strip(), file=sys.stderr)
    return 2


def main(argv):
    repo, output, digest, mode, value = ROOT, None, None, None, None
    if not argv or argv[0] not in ("esphome", "idf"):
        return usage()
    what, rest, i = argv[0], argv[1:], 0
    while i < len(rest):
        a = rest[i]
        if a == "--check" and mode is None:
            mode = "check"; i += 1
        elif a == "--set" and mode is None and i + 1 < len(rest):
            mode, value = "set", rest[i + 1]; i += 2
        elif a in ("--repo", "--output", "--digest") and i + 1 < len(rest):
            if a == "--repo":
                repo = Path(rest[i + 1])
            elif a == "--output":
                output = rest[i + 1]
            else:
                digest = rest[i + 1]
            i += 2
        else:
            return usage()
    if mode is None or (digest is not None and (what != "idf" or mode != "set")):
        return usage()
    try:
        if mode == "check":
            return check(what, repo, output)
        notes = []
        if what == "esphome":
            set_esphome(repo, value, notes)
        else:
            set_idf(repo, value, digest, notes)
        print("\n".join(notes))
        return 0
    except Problem as e:
        print(f"[pin-bump] FAIL: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
