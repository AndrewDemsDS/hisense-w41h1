#!/usr/bin/env python3
"""pin-bump.py on fixture trees: the rewrite of the ESPHome and ESP-IDF pins, the choice of the
newest release, and the agreement with version-lint.py and the pin-bumps workflow.

No network: PyPI, Docker Hub and the registry are stand-ins inside this process. Every tree is
made in a temporary directory, so nothing in this checkout is written to.
"""

import contextlib
import importlib.util
import io
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import urllib.error

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "firmware" / "scripts"
sys.dont_write_bytecode = True


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pb = load("pin_bump", "pin-bump.py")
LINT = SCRIPTS / "version-lint.py"
CLEAN_ENV = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}

ok = True


def check(cond, msg):
    global ok
    print(("  ok  " if cond else "  FAIL ") + msg)
    ok = ok and bool(cond)


OLD_DIGEST = "sha256:" + "a" * 64
NEW_DIGEST = "sha256:" + "b" * 64

FIXTURE = {
    ".github/workflows/qa.yaml": 'env:\n  ESPHOME_VERSION: "2026.9.1"\n',
    ".github/workflows/esphome-release.yaml": 'env:\n  ESPHOME_VERSION: "2026.9.1"\n',
    "firmware/scripts/dev.py": 'ESPHOME_PIN = "2026.9.1"   # the release CI builds with\n',
    "firmware/scripts/esphome-upstream-check.sh": 'ESPHOME_SPEC="esphome==2026.9.1"\n',
    ".github/ci/esphome/requirements.txt": "# the pin\nesphome==2026.9.1\n",
    "firmware/esphome/packages/version.yaml": 'esphome:\n  project:\n    version: "2.0.2"\n',
    "README.md": "and ESPHome 2026.9.1 installed\n(`pipx install esphome==2026.9.1`):\n",
    "CONTRIBUTING.md": "the version the project pins (2026.9.1 at the time of\nwriting\n",
    "docs/guide/Build-Flash-Test.md": "```\npipx install esphome==2026.9.1      # if doctor says\n```\n",
    "docs/guide/ESP32-Build-Environment.md":
        "| `esphome` | PyPI | `2026.9.1`, the release CI builds with | x |\n\npipx install esphome==2026.9.1\n",
    "firmware/esphome/README.md": "for example `2.0.0 (ESPHome 2026.9.1)`\n",
    "CHANGELOG.md": "# Changelog\n\nIntro.\n\n## Unreleased\n\n- First thing.\n\n- Second thing,\n  two lines.\n\n"
                    "## 2.0.2 - 2026-10-27\n\n- Built with 2026.9.1.\n",
    ".github/workflows/nightly.yaml": (
        "jobs:\n"
        "  other:\n"
        "    steps:\n"
        "      - run: echo v5.5.5 is not ours\n"
        "\n"
        "  # firmware/esp32-recon/ builds with\n"
        "  # ESP-IDF v5.5.5, the newest v5.5 release. Not v5.5.50.\n"
        "  esp32-recon:\n"
        "    container:\n"
        f"      image: espressif/idf:v5.5.5@{OLD_DIGEST}\n"
        "    steps:\n"
        "      - run: echo \"$(cat v || echo v5.5.5)\"\n"
        "\n"
        "  # after\n"
        "  later:\n"
        "    steps:\n"
        "      - run: echo v5.5.5\n"),
    "firmware/esp32-recon/README.md": "CI builds it every night with v5.5.5 in Espressif's container (the\n",
}


def tree(overrides=None):
    d = pathlib.Path(tempfile.mkdtemp(dir=TMP))
    for rel, text in {**FIXTURE, **(overrides or {})}.items():
        if text is None:
            continue
        (d / rel).parent.mkdir(parents=True, exist_ok=True)
        (d / rel).write_text(text)
    return d


def run(*args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = pb.main(list(args))
    return code, out.getvalue() + err.getvalue()


def snapshot(d):
    return {p.relative_to(d).as_posix(): p.read_text() for p in d.rglob("*") if p.is_file()}


def lint(d):
    return subprocess.run([sys.executable, str(LINT), "--repo", str(d)], capture_output=True, text=True, env=CLEAN_ENV)


tmp_holder = tempfile.TemporaryDirectory()
TMP = pathlib.Path(tmp_holder.name)

# ================================================================================================
print("== the table of pins is version-lint.py's ==")
vl = load("version_lint", "version-lint.py")
check([rel for rel, _ in pb.ESPHOME_PINS] == [rel for rel, _ in vl.ESPHOME_PINS],
      "pin-bump.py rewrites exactly the places version-lint.py checks")
check(all((ROOT / rel).is_file() for rel, _ in pb.ESPHOME_DOCS), "every doc it rewrites exists in this tree")
real_docs = {rel: (ROOT / rel).read_text() for rel, _ in pb.ESPHOME_DOCS}
real_pin = pb.esphome_pin(ROOT)
check(all(re.search(p, real_docs[rel]) for rel, p in pb.ESPHOME_DOCS), "and each of its patterns finds its line there")
check(pb.idf_pin(ROOT)[0].startswith("v5.5."), "the real nightly.yaml pins one espressif/idf image on v5.5")
check(re.search(pb.RECON_DOC, (ROOT / pb.RECON_README).read_text()) is not None, "and the esp32-recon README names it")
check(pb.RECON_JOB.search((ROOT / pb.NIGHTLY).read_text()) is not None, "the esp32-recon job is found in nightly.yaml")
check(real_pin, f"the real tree has one ESPHome pin ({real_pin})")

# ================================================================================================
print("== esphome --set ==")
d = tree()
code, out = run("esphome", "--set", "2026.10.0", "--repo", str(d))
check(code == 0 and "2026.9.1 -> 2026.10.0" in out, "a newer release is set (exit 0)")
after = snapshot(d)
for rel, pattern in pb.ESPHOME_PINS:
    check(re.findall(pattern, after[rel], re.M) == ["2026.10.0"], f"{rel} names 2026.10.0")
check("ESPHome 2026.10.0 installed" in after["README.md"] and "esphome==2026.10.0" in after["README.md"], "README.md: both mentions")
check("pins (2026.10.0 at the time of" in after["CONTRIBUTING.md"], "CONTRIBUTING.md")
check("pipx install esphome==2026.10.0      #" in after["docs/guide/Build-Flash-Test.md"], "Build-Flash-Test.md: the install line")
check(after["docs/guide/ESP32-Build-Environment.md"].count("2026.10.0") == 2, "ESP32-Build-Environment.md: the table and the install line")
check(after["firmware/esphome/README.md"] == FIXTURE["firmware/esphome/README.md"], "an example firmware string is history and stays")
check(after["CHANGELOG.md"] == ("# Changelog\n\nIntro.\n\n## Unreleased\n\n- First thing.\n\n- Second thing,\n  two lines.\n\n"
                                "- Builds with ESPHome 2026.10.0 (was 2026.9.1).\n\n## 2.0.2 - 2026-10-27\n\n- Built with 2026.9.1.\n"),
      "CHANGELOG.md: one line at the end of Unreleased, history untouched")
r = lint(d)
check(r.returncode == 0, "version-lint.py passes on the bumped tree")

code, out = run("esphome", "--set", "2026.10.0", "--repo", str(d))
check(code == 0 and "already pinned to 2026.10.0" in out and snapshot(d) == after, "the same release again: exit 0, nothing written")

d = tree({"CHANGELOG.md": "# Changelog\n\n## 2.0.2 - 2026-10-27\n\n- x\n"})
run("esphome", "--set", "2026.10.0", "--repo", str(d))
check((d / "CHANGELOG.md").read_text() == "# Changelog\n\n## Unreleased\n\n- Builds with ESPHome 2026.10.0 (was 2026.9.1).\n\n"
      "## 2.0.2 - 2026-10-27\n\n- x\n", "no Unreleased section: one is made above the newest release")
d = tree({"CHANGELOG.md": "# Changelog\n\n## Unreleased\n\n## 2.0.2 - 2026-10-27\n\n- x\n"})
run("esphome", "--set", "2026.10.0", "--repo", str(d))
check((d / "CHANGELOG.md").read_text() == "# Changelog\n\n## Unreleased\n\n- Builds with ESPHome 2026.10.0 (was 2026.9.1).\n\n"
      "## 2.0.2 - 2026-10-27\n\n- x\n", "an empty Unreleased section gets the line")

d = tree({"README.md": "and ESPHome 2026.8.0 installed\n"})
code, out = run("esphome", "--set", "2026.10.0", "--repo", str(d))
check(code == 0 and (d / "README.md").read_text() == "and ESPHome 2026.8.0 installed\n" and "check it by hand" in out,
      "a doc naming another release is left alone and reported")
d = tree({"docs/guide/Build-Flash-Test.md": None})
code, out = run("esphome", "--set", "2026.10.0", "--repo", str(d))
check(code == 0 and "Build-Flash-Test.md: missing, skipped" in out, "a missing doc is reported, not an error")

for bad, why in (("2026.11.0b1", "a beta"), ("2026.8.0", "a downgrade"), ("latest", "not a version")):
    d = tree()
    before = snapshot(d)
    code, out = run("esphome", "--set", bad, "--repo", str(d))
    check(code == 1 and "FAIL" in out and snapshot(d) == before, f"{why} is refused and nothing is written")
d = tree({"firmware/scripts/dev.py": 'ESPHOME_PIN = "2026.8.0"\n'})
before = snapshot(d)
code, out = run("esphome", "--set", "2026.10.0", "--repo", str(d))
check(code == 1 and "pins differ" in out and snapshot(d) == before, "pins that already differ are refused (version-lint first)")
d = tree({".github/ci/esphome/requirements.txt": None})
code, out = run("esphome", "--set", "2026.10.0", "--repo", str(d))
check(code == 1 and "requirements.txt is missing" in out, "a missing pin file is an error, not a skip")

# ================================================================================================
print("== idf --set ==")
d = tree()
code, out = run("idf", "--set", "v5.5.6", "--digest", NEW_DIGEST, "--repo", str(d))
nightly = (d / pb.NIGHTLY).read_text()
check(code == 0 and "v5.5.5 -> v5.5.6" in out, "a newer tag on the line is set (exit 0)")
check(f"image: espressif/idf:v5.5.6@{NEW_DIGEST}\n" in nightly and OLD_DIGEST not in nightly, "the image line: tag and digest")
check("# ESP-IDF v5.5.6, the newest v5.5 release. Not v5.5.50." in nightly, "the comment above the job, and v5.5.50 is not taken for v5.5.5")
check('echo "$(cat v || echo v5.5.6)"' in nightly, "the fallback in the job's step")
check(nightly.count("echo v5.5.5") == 2, "another job's v5.5.5, above or below, is not touched")
check("every night with v5.5.6 in Espressif's" in (d / pb.RECON_README).read_text(), "the esp32-recon README")
code, out = run("idf", "--set", "v5.5.6", "--digest", NEW_DIGEST, "--repo", str(d))
check(code == 0 and "already on v5.5.6" in out, "the same tag again: exit 0, nothing to change")

for args, why in ((["v6.0.0", "--digest", NEW_DIGEST], "another line (v6)"), (["v5.6.0", "--digest", NEW_DIGEST], "another line (v5.6)"),
                  (["v5.5.4", "--digest", NEW_DIGEST], "a downgrade"), (["v5.5.6", "--digest", "sha256:12"], "a short digest"),
                  (["5.5.6", "--digest", NEW_DIGEST], "a tag without its v")):
    d = tree()
    before = snapshot(d)
    code, out = run("idf", "--set", *args, "--repo", str(d))
    check(code == 1 and snapshot(d) == before, f"{why} is refused and nothing is written")
d = tree({pb.NIGHTLY: "jobs:\n  esp32-recon:\n    container:\n      image: espressif/idf:v5.5.5\n"})
code, out = run("idf", "--check", "--repo", str(d))
check(code == 1 and "expected one" in out, "an image without a digest is not a pin it understands")

# ================================================================================================
print("== the newest release, with the network stood in for ==")
responses = {}


def fake_fetch(url, headers=None):
    for prefix, value in responses.items():
        if url.startswith(prefix):
            if isinstance(value, Exception):
                raise value
            return value(url) if callable(value) else value
    raise AssertionError(f"unexpected URL {url}")


real_fetch = pb.fetch_json
pb.fetch_json = fake_fetch
files = [{"yanked": False}]
responses[pb.PYPI] = {"releases": {"2026.9.1": files, "2026.10.0": files, "2026.10.1": [{"yanked": True}],
                                   "2026.11.0b1": files, "2026.11.0.dev3": files, "2027.1.0": [], "2026.2.10": files}}
check(pb.newest_esphome() == "2026.10.0", "PyPI: 2026.10.0, compared as numbers, no beta, no dev, no yanked or empty release")

pages = {
    "1": {"results": [{"name": "v5.5.4"}, {"name": "v5.5.10"}, {"name": "v5.5"}], "next": pb.HUB_TAGS + "v5.5.&page=2"},
    "2": {"results": [{"name": "v5.5.9-rc1"}, {"name": "v5.5.6"}, {"name": "release-v5.5"}], "next": None},
}
responses[pb.HUB_TAGS] = lambda url: pages["2" if "page=2" in url else "1"]
check(pb.newest_idf((5, 5)) == "v5.5.10", "Docker Hub: v5.5.10 over v5.5.6, across two pages, no rc or line tag")
responses[pb.HUB_TAGS] = {"results": [{"name": "v6.0.0"}], "next": None}
try:
    pb.newest_idf((5, 5))
    check(False, "no tag on the line is an error")
except pb.Problem as e:
    check("no espressif/idf tag on v5.5" in str(e), "no tag on the line is an error, not a jump to v6")

responses[pb.REGISTRY_TOKEN] = {"token": "t"}
responses[pb.REGISTRY_MANIFEST] = {"manifests": [
    {"digest": "sha256:" + "1" * 64, "platform": {"os": "linux", "architecture": "arm64"}},
    {"digest": NEW_DIGEST, "platform": {"os": "linux", "architecture": "amd64"}},
    {"digest": "sha256:" + "2" * 64, "platform": {"os": "unknown", "architecture": "unknown"}}]}
check(pb.idf_digest("v5.5.6") == NEW_DIGEST, "the registry: the linux/amd64 entry of the manifest list")

d = tree()
out_file = d / "gh-output"
responses[pb.HUB_TAGS] = {"results": [{"name": "v5.5.6"}, {"name": "v5.5.5"}], "next": None}
code, out = run("idf", "--check", "--repo", str(d), "--output", str(out_file))
check(code == 0 and "a bump is due" in out, "idf --check: a bump is due (exit 0)")
check(out_file.read_text() == f"current=v5.5.5\nlatest=v5.5.6\ndue=true\ndigest={NEW_DIGEST}\n", "and --output has what the workflow reads")
code, out = run("esphome", "--check", "--repo", str(d), "--output", str(out_file))
check(code == 0 and "a bump is due" in out and out_file.read_text().endswith("current=2026.9.1\nlatest=2026.10.0\ndue=true\n"),
      "esphome --check: due, and the lines are appended")
responses[pb.PYPI] = {"releases": {"2026.9.1": files}}
code, out = run("esphome", "--check", "--repo", str(d))
check(code == 0 and "is current, nothing to do" in out, "already current: exit 0 with a clear message")

responses[pb.PYPI] = pb.Problem("cannot read https://pypi.org/pypi/esphome/json: timed out")
code, out = run("esphome", "--check", "--repo", str(d))
check(code == 1 and "cannot read" in out, "a network failure: exit 1 with a message")
pb.fetch_json = real_fetch


def refuse(*a, **k):
    raise urllib.error.URLError("no network in the test")


real_urlopen = pb.urllib.request.urlopen
pb.urllib.request.urlopen = refuse
code, out = run("idf", "--check", "--repo", str(d))
check(code == 1 and "FAIL: cannot read https://hub.docker.com" in out, "fetch_json turns a URL error into that message")
pb.urllib.request.urlopen = real_urlopen

for args, why in (([], "no arguments"), (["pip", "--check"], "an unknown target"), (["esphome"], "no mode"),
                  (["esphome", "--check", "--set", "1.2.3"], "two modes"), (["esphome", "--set", "1.2.3", "--digest", NEW_DIGEST], "--digest for ESPHome"),
                  (["idf", "--check", "--digest", NEW_DIGEST], "--digest with --check")):
    code, out = run(*args)
    check(code == 2, f"usage error: {why}")

# ================================================================================================
print("== the workflow ==")
wf = (ROOT / ".github/workflows/pin-bumps.yaml").read_text()
qa = (ROOT / ".github/workflows/qa.yaml").read_text()
check("github.server_url == 'https://github.com'" in wf, "pin-bumps.yaml runs on GitHub only")
uses = re.findall(r"uses:\s*(\S+)", wf)
check(uses and all(re.search(r"@[0-9a-f]{40}$", u) for u in uses), "every action is pinned by commit")
check(re.search(r"(?m)^permissions: \{\}$", wf) and "write-all" not in wf, "no permission at the top, the job states its own")
check("pin-bump.py \"$WHAT\" --check --output \"$GITHUB_OUTPUT\"" in wf and "version-lint.py" in wf, "it runs the check, and version-lint after the rewrite")
check("--state all" in wf, "a pull request for the same version, closed ones included, stops it")
check("gh workflow run qa.yaml" in wf and re.search(r"(?m)^  workflow_dispatch:$", qa), "it starts qa.yaml, which takes workflow_dispatch")
check("dry_run" in wf and "github.ref_name != github.event.repository.default_branch" in wf, "a dry run, forced off the default branch")
check(not re.search(r"(?i)claude|anthropic|generated with|co-authored", wf), "no assistant or vendor text in what the bot writes")
run_tests = (ROOT / "firmware/test/run_tests.sh").read_text()
check("test_pin_bump.py" in run_tests, "run_tests.sh runs this test")

tmp_holder.cleanup()
print("== PIN BUMP OK ==" if ok else "== PIN BUMP FAILED ==")
sys.exit(0 if ok else 1)
