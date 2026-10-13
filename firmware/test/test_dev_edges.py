#!/usr/bin/env python3
"""dev.py at its edges: the command line, every refusal, the boards and the environment it runs in.

test_dev_release.py holds the rules of the happy path. This file feeds dev.py what a person gets
wrong: a missing value, a board that does not exist, a tool that is not installed. Each check
documents what dev.py does today.

Where today's behaviour is wrong the check is a KNOWN FAILURE: `known_failure()` passes while the
defect is there and fails the day it is fixed, so the marker cannot outlive the defect. Each one
names the defect and the line.

No hardware and no network: the esphome command is a small script in a temporary directory, and
nothing is written outside that directory. About 2 s.
"""

import base64
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

import edge_harness
from edge_harness import CLEAN_ENV, ROOT, SCRIPTS, check, died, environ, patched, quiet, raised, script
import dev

DEV = SCRIPTS / "dev.py"
NO_ENVF = "/nonexistent/ota-release.env"


def cli(*args, stdin="", **env):
    return subprocess.run([sys.executable, str(DEV), *args], input=stdin, capture_output=True, text=True,
                          env={**CLEAN_ENV, "ENVF": NO_ENVF, **env}, timeout=60)


def main(*args, typed=None):
    """dev.main() in this process, with no terminal. Returns (how, text):
    ("die", message), ("exit", code) or ("ran", stdout). The command-line checks that only need
    the refusal use this; the ones about exit codes and streams start a real process with cli()."""
    def no_terminal(q):
        if typed is None:
            raise EOFError
        return typed
    with patched(dev, input=no_terminal), quiet() as (out, err):
        try:
            dev.main(list(args))
        except dev.Die as e:
            return "die", str(e)
        except SystemExit as e:
            return "exit", e.code
        return "ran", out.getvalue()


tmp_holder = tempfile.TemporaryDirectory(prefix="dev-edges-")
TMP = pathlib.Path(tmp_holder.name)

# ================================================================================================
print("== dev.py edges: the command line ==")

r = cli()
check(r.returncode == 1 and "Commands:" in r.stdout, "no arguments prints the usage and exits 1")
for flag in ("-h", "--help", "help"):
    r = cli(flag)
    check(r.returncode == 0 and "Commands:" in r.stdout, f"{flag} prints the usage and exits 0")

# (arguments, text expected on stderr, what it shows)
REFUSALS = [
    (["frobnicate"], "invalid choice: 'frobnicate' (choose from 'walk', 'doctor'", "an unknown command is named, with the ones there are"),
    (["frobnicate", "esphome"], "invalid choice: 'frobnicate'", "the same with the word esphome after it"),
    (["build", "ESPHOME"], "argument target: invalid choice: 'ESPHOME'", "the word esphome is case-sensitive"),
    (["build", "esphome", "--bogus"], "unrecognized arguments: --bogus", "an unknown option"),
    (["build", "--bogus"], "unrecognized arguments: --bogus", "the same without the word esphome"),
    (["build", "esphome", "extra"], "unrecognized arguments: extra", "a stray positional argument"),
    (["build", "esphome", "--board", "nope"], "--board must be one of c3, c3-plus, classic", "a board that does not exist"),
    (["build", "esphome", "--board", "amebaz2"], "needs the unit's flash layout", "amebaz2 without a flash layout"),
    (["build", "esphome", "--board", "C3"], "(got 'C3')", "the board is case-sensitive"),
    (["build", "esphome", "--boa", "c3"], "unrecognized arguments: --boa c3", "an abbreviated option is not guessed at"),
    (["flash", "esphome"], "flash needs --port", "flash without a port"),
    (["flash"], "flash needs --port", "the same without the word esphome"),
    (["monitor", "esphome"], "monitor needs --port", "monitor without a port"),
    (["erase", "esphome"], "erase needs --port", "erase without a port"),
    (["bench", "esphome"], "bench needs --port", "bench without a port"),
    (["bench", "esphome", "--port", "/dev/null"], "bench needs --sim-port", "bench without a simulator port"),
    (["build", "esphome", "--serial", "5"], "unrecognized arguments: --serial 5", "--serial belongs to matter-convert.py"),
    (["build", "esphome", "--stage"], "unrecognized arguments: --stage", "--stage belongs to matter-convert.py"),
    (["convert", "esphome", "--board", "amebaz2-factory"], "invalid choice: 'convert'", "convert is matter-convert.py's"),
    (["matter-slots", "127.0.0.1"], "invalid choice: 'matter-slots'", "and so are the probes"),
    (["ota", "amebaz2", "build"], "invalid choice: 'ota'", "the retired ota steps are an unknown command like any other"),
    (["flash", "amebaz2", "--port", "/dev/null"], "argument target: invalid choice: 'amebaz2'", "the retired amebaz2 target is not a word dev.py knows"),
]
for args, needle, why in REFUSALS:
    how, text = main(*args)
    check(how == "die" and needle in text, f"refused: {why}")
# A refusal is one line on stderr and exit code 1, never a traceback.
for args in (["build", "esphome", "--board", "nope"], ["frobnicate"], ["build", "--bogus"], ["build", "--board"]):
    r = cli(*args)
    check(r.returncode == 1 and r.stderr.count("\n") == 1 and "[dev] ERROR:" in r.stderr and r.stdout == "",
          f"`dev.py {' '.join(args)}` is one `[dev] ERROR:` line on stderr, nothing on stdout, exit 1")

# A flag that is the last word has no value, and the message says so (it used to read
# "unknown option: --board", for an option that exists).
for flag in ("--board", "--port", "--sim-port", "--name", "--friendly-name"):
    check(main("build", "esphome", flag) == ("die", f"argument {flag}: expected one argument"),
          f"{flag} with no value is refused, and the message says a value is missing")

# The word esphome after the command is optional: there is no other firmware to name.
for args in (["next"], ["next", "esphome"], ["next", "--board", "classic"], ["next", "esphome", "--board", "classic"],
             ["next", "--board", "classic", "esphome"]):
    check(main(*args)[0] == "ran", f"`dev.py {' '.join(args)}` runs")
r = cli("build", "--help")
check(r.returncode == 0 and "--board" in r.stdout and "--friendly-name" in r.stdout, "a command's own --help lists its options")

# The node name is a hostname: `next` only prints, so it shows which names get through.
for name in ("a", "ac-2", "0a", "a" * 31):
    check(main("next", "esphome", "--name", name)[0] == "ran", f"--name '{name[:12]}' ({len(name)} chars) is accepted")
check(main("next", "esphome", "--name", "-lead") == ("die", "argument --name: expected one argument"),
      "--name '-lead' is refused: a word that starts with a hyphen is read as an option")
for name in ("Bad_Name", "UPPER", "-lead", "trail-", "a" * 32, "a.b", "a b", "ac\n"):
    how, text = main("next", "esphome", f"--name={name}")
    check(how == "die" and "is not a valid hostname" in text, f"--name {name[:12]!r} ({len(name)} chars) is refused")
check(main("next", "esphome", "--name", "")[0] == "ran", "an empty --name falls back to the default name")

# The stock module has no USB path: every command that would touch a port is refused before it runs.
for board in dev.ESPHOME_AMEBAZ2_BOARDS:
    for cmd in ("walk", "erase", "flash", "monitor", "bench", "next"):
        how, text = main(cmd, "esphome", "--board", board, "--port", "/dev/null", "--sim-port", "/dev/null")
        check(how == "die" and "has test and build" in text, f"{cmd} --board {board} is refused")

# erase asks for the word ERASE. Anything else, or no terminal, erases nothing.
for typed in (None, "", "erase", "y", "yes", "ERASE now", "E R A S E"):
    check(main("erase", "esphome", "--port", "/dev/null", typed=typed) == ("die", "not confirmed; nothing erased"),
          f"erase with {'no terminal' if typed is None else repr(typed)} typed erases nothing")
r = cli("erase", "esphome", "--port", "/dev/null", stdin="erase\n")
check(r.returncode == 1 and "not confirmed; nothing erased" in r.stderr and "BRAND-NEW board only" in r.stderr,
      "erase from a real process: the warning, the prompt, and exit 1 on the wrong word")

# walk with no terminal: every question is a no, the port prompt is skipped, the notes are printed.
r = cli("walk", "esphome")
check(r.returncode == 0 and "5/5 next steps" in r.stdout and "Staged bring-up" in r.stdout,
      "walk with no terminal skips every step and still prints the bring-up notes")

# ================================================================================================
print("== dev.py edges: boards, the esphome command, build and erase ==")

# The pins are read from the board file, so the wiring note of `bench` cannot drift from it.
for board, pins in (("c3", ("5", "6", "10")), ("c3-plus", ("5", "6", "10")), ("classic", ("19", "18", "4"))):
    c = dev.Ctx(board, None, None)
    check((c.pin("tx"), c.pin("rx"), c.pin("de")) == pins, f"--board {board}: tx, rx, de are {pins}, as its board file says")
check(died(dev.Ctx, "") is not None and died(dev.Ctx, "esp8266") is not None, "an empty board, or one dev.py has no file for, is refused")

# Which esphome is run: $ESPHOME, then PATH, then pipx's directory.
pipx_bin = TMP / "pipx-bin"
with environ(ESPHOME="/opt/x/esphome", PIPX_BIN_DIR=None):
    check(dev.esphome_argv() == ["/opt/x/esphome"], "$ESPHOME names the command")
with environ(ESPHOME=None, PIPX_BIN_DIR=str(pipx_bin)), patched(dev, have=lambda tool: False):
    check(dev.esphome_argv() == ["esphome"], "no esphome anywhere: the bare name, so the error names it")
    pipx_bin.mkdir()
    (pipx_bin / "esphome").write_text("#!/bin/sh\n")
    check(dev.esphome_argv() == ["esphome"], "a pipx esphome that is not executable is not used")
    script(pipx_bin / "esphome", "#!/bin/sh\n")
    check(dev.esphome_argv() == [str(pipx_bin / "esphome")], "an executable esphome in PIPX_BIN_DIR is used when PATH has none")
# $ESPHOME may be a command with arguments. It is split into words, never given to a shell.
spaced = script(TMP / "my tools" / "esphome", "#!/bin/sh\n")
for text, argv in (("uvx esphome", ["uvx", "esphome"]), ("python3 -m esphome", ["python3", "-m", "esphome"]),
                   ("'/opt/my tools/esphome' -v", ["/opt/my tools/esphome", "-v"]), (str(spaced), [str(spaced)]),
                   ("x; touch f", ["x;", "touch", "f"])):
    with environ(ESPHOME=text):
        check(dev.esphome_argv() == argv, f"ESPHOME={text.replace(str(TMP), '<tmp>')!r} is {len(argv)} word(s)")
for text, needle in (("", "set and empty"), ("   ", "set and empty"), ("'unclosed", "is not a command line")):
    with environ(ESPHOME=text):
        msg = died(dev.esphome_argv)
        check(msg and needle in msg, f"ESPHOME={text!r} is refused with a message")

# esphome_run: what it refuses, and the command line it builds.
calls = []


def record_run(args, env=None, cwd=None, check=True):  # noqa: ARG001 - dev.run's signature
    calls.append({"args": [str(a) for a in args], "env": env, "cwd": cwd})
    return 0


esphome_dir = TMP / "esphome"
esphome_dir.mkdir()
with patched(dev, run=record_run, ESPHOME_DIR=esphome_dir), environ(ESPHOME="esphome-stub", PLATFORMIO_BUILD_CACHE_DIR=None):
    with patched(dev, have=lambda tool: False):
        msg = died(dev.Ctx("c3", None, None).esphome_run, "config")
        check(msg and "'esphome-stub' not found" in msg, "esphome missing: refused, names the command and how to get it")
    with patched(dev, have=lambda tool: True):
        msg = died(dev.Ctx("c3", None, None).esphome_run, "config")
        check(msg and "secrets.yaml" in msg and not calls, "no secrets.yaml: refused before esphome runs")
        (esphome_dir / "secrets.yaml").write_text("")
        for sub in ("run", "logs", "upload", "clean"):
            msg = died(dev.Ctx("amebaz2-factory", None, None).esphome_run, sub)
            check(msg and "has test and build" in msg and not calls, f"esphome {sub} for the stock module is refused")
        dev.Ctx("classic", None, None, "ac-2", "Bedroom A/C").esphome_run("run", "--device", "/dev/ttyX")
        a = calls[-1]["args"]
        check(a == ["esphome-stub", "-s", "name", "ac-2", "-s", "friendly_name", "Bedroom A/C",
                    "run", "w41h1.yaml", "--device", "/dev/ttyX"],
              "classic: the node's name first, then the subcommand, the board file and the caller's arguments")
        check(calls[-1]["cwd"] == esphome_dir, "esphome runs in the ESPHome directory")
        check(calls[-1]["env"].get("PLATFORMIO_BUILD_CACHE_DIR", "").endswith("w41h1-dev/pio-build-cache"),
              "esphome gets the build cache directory")
        dev.Ctx("amebaz2-sdk", None, None, "ac-3").esphome_run("compile", cache=False)
        check(calls[-1]["args"] == ["esphome-stub", "-s", "name", "ac-3", "compile", "w41h1-amebaz2-sdk.yaml"],
              "stock module: the board file, no pin substitutions, the node name kept")
        check("PLATFORMIO_BUILD_CACHE_DIR" not in calls[-1]["env"], "cache=False builds without the build cache")
        dev.Ctx("c3-plus", None, None, None, "Plus").esphome_run("config")
        check(calls[-1]["args"] == ["esphome-stub", "-s", "friendly_name", "Plus", "config", "w41h1-esp32c3-plus.yaml"],
              "C3 Plus: the board file, and a friendly name alone is passed")

# --board c3 used to build w41h1.yaml with -s overrides while the repo also has w41h1-esp32c3.yaml,
# which adds the status LED: two different images for one board. It builds the board file now.
c3_file = (ROOT / "firmware/esphome/w41h1-esp32c3.yaml").read_text()
with patched(dev, run=record_run, have=lambda tool: True, ESPHOME_DIR=esphome_dir), environ(ESPHOME="esphome-stub"):
    dev.Ctx("c3", None, None).esphome_run("compile")
check(calls[-1]["args"] == ["esphome-stub", "compile", "w41h1-esp32c3.yaml"] and "status-led.yaml" in c3_file,
      "--board c3 builds w41h1-esp32c3.yaml, the file with the SuperMini's status LED")

# build: the stock-module slot image is a side effect of the link step, so a fully cached build
# has none and dev.py builds once more without the cache.
with patched(dev, ESPHOME_DIR=TMP / "esphome-build"):
    ctx = dev.Ctx("amebaz2-factory", None, None, "t")
    image = ctx.esphome_slot_image()
    image.parent.mkdir(parents=True)
    runs = []

    def fake_esphome_run(sub, *args, cache=True, make=()):
        runs.append((sub, cache))
        if len(runs) in make:
            image.write_bytes(b"x")

    ctx.esphome_run = lambda sub, *a, cache=True: fake_esphome_run(sub, *a, cache=cache, make=(1,))
    with quiet():
        dev.build(ctx)
    check(runs == [("compile", True)], "build: one compile when the slot image is there afterwards")

    image.unlink()
    runs.clear()
    (image.parent / "raw_firmware.elf").write_bytes(b"elf")
    (image.parent / "firmware.uf2").write_bytes(b"uf2")
    ctx.esphome_run = lambda sub, *a, cache=True: fake_esphome_run(sub, *a, cache=cache, make=(2,))
    with quiet():
        dev.build(ctx)
    check(runs == [("compile", True), ("compile", False)], "build: no slot image, so a second compile without the cache")
    check(not (image.parent / "raw_firmware.elf").exists() and not (image.parent / "firmware.uf2").exists(),
          "build: the linked outputs are removed first, so the link step runs again")

    image.unlink()
    runs.clear()
    ctx.esphome_run = lambda sub, *a, cache=True: fake_esphome_run(sub, *a, cache=cache, make=())
    with quiet():
        msg = died(dev.build, ctx)
    check(msg and "did not produce image_firmware_is.0x010000.bin" in msg and len(runs) == 2,
          "build: still no slot image after the second compile is an error, not a silent success")

    runs.clear()
    esp = dev.Ctx("c3", None, None)
    esp.esphome_run = lambda sub, *a, cache=True: fake_esphome_run(sub, *a, cache=cache, make=())
    dev.build(esp)
    check(runs == [("compile", True)], "build: an ESP32 board compiles once and looks for no slot image")

# doctor: read-only, lists every gap, and its exit code says whether there was one.
doctor_dir = TMP / "esphome-doctor"
doctor_dir.mkdir()
fake_esphome = script(TMP / "fake-bin" / "esphome", '#!/bin/sh\necho "Version: ${FAKE_ESPHOME_VERSION}"\n')
with patched(dev, ESPHOME_DIR=doctor_dir), environ(ESPHOME=str(fake_esphome), FAKE_ESPHOME_VERSION=dev.ESPHOME_PIN):
    ctx = dev.Ctx("c3", None, None)
    with quiet() as (out, err):
        res = dev.doctor(ctx)
    check(res is False and "secrets.yaml (cp secrets.yaml.example)" in out.getvalue(), "doctor: a missing secrets.yaml is a gap")
    (doctor_dir / "secrets.yaml").write_text("")
    with quiet() as (out, err):
        res = dev.doctor(ctx)
    check(res is True and "doctor: all good" in out.getvalue(), "doctor: every tool there and the pinned ESPHome is all good")
    with environ(FAKE_ESPHOME_VERSION="2020.1.0"), quiet() as (out, err):
        res = dev.doctor(ctx)
    check(res is False and f"esphome is 2020.1.0, CI pins {dev.ESPHOME_PIN}" in out.getvalue(),
          "doctor: another ESPHOME version than the pin is a gap")
    silent = script(TMP / "fake-bin" / "esphome-silent", "#!/bin/sh\nexit 0\n")
    with environ(ESPHOME=str(silent)), quiet() as (out, err):
        res = dev.doctor(ctx)
    check(res is False and "esphome is ?" in out.getvalue(), "doctor: an esphome that prints no version is a gap, not a crash")
    with patched(dev, have=lambda tool: tool in ("git", "python3")), quiet() as (out, err):
        res = dev.doctor(ctx)
    text = out.getvalue()
    check(res is False and "g++ not on PATH" in text and "esphome not installed" in text and "pipx not on PATH" in text,
          "doctor: g++, esphome and pipx missing are three gaps, all listed in one run")
r = cli("doctor", "esphome", ESPHOME="/nonexistent/esphome")
check(r.returncode == 1 and "esphome not installed" in r.stdout and "doctor found gaps" in r.stderr,
      "doctor exits 1 when it found a gap")

# fetch: needs pipx, and asks before it installs.
with patched(dev, have=lambda tool: False):
    msg = died(dev.fetch, dev.Ctx("c3", None, None))
    check(msg and "install pipx first" in msg, "fetch without pipx is refused and says how to get it")
calls.clear()
with patched(dev, have=lambda tool: True, run=record_run, input=lambda q: "n"):
    dev.fetch(dev.Ctx("c3", None, None))
    check(not calls, "fetch answered with n installs nothing")
# Its closing message names every value the build reads from secrets.yaml. One that is left out
# (wifi_ap_password was) is one the user learns about from a failed build.
calls.clear()
with patched(dev, have=lambda tool: True, run=record_run, input=lambda q: "y"), quiet() as (out, err):
    dev.fetch(dev.Ctx("c3", None, None))
secrets_read = set()
for yaml_file in sorted(dev.ESPHOME_DIR.glob("*.yaml")) + sorted((dev.ESPHOME_DIR / "packages").glob("*.yaml")):
    if yaml_file.name != "secrets.yaml":
        secrets_read |= set(re.findall(r"!secret\s+(\w+)", yaml_file.read_text()))
check(len(secrets_read) >= 4 and all(key in out.getvalue() for key in secrets_read),
      "fetch ends by naming every secret the board files and packages read: " + ", ".join(sorted(secrets_read)))
example = (dev.ESPHOME_DIR / "secrets.yaml.example").read_text()
check(all(re.search(rf"^{key}:", example, re.M) for key in secrets_read),
      "and secrets.yaml.example has a line for each of them")
# The example's API key is public. It has to look like a placeholder, so nobody takes it for a
# key, and still be 32 bytes of base64, so ESPHome accepts the file as it is.
example_key = re.search(r'^hisense_ac__encryption_key:\s*"([^"]+)"', example, re.M)
check(example_key is not None and base64.b64decode(example_key.group(1), validate=True) == bytes(32),
      "the example's API key is the placeholder everyone can recognise: 32 zero bytes")

# The guide pages the two scripts send the user to exist, are not "Moved" pointers, and have the
# heading the link names.
def heading_anchors(text):
    return {re.sub(r"[^a-z0-9 -]", "", h.lower()).replace(" ", "-") for h in re.findall(r"^#+\s+(.+)$", text, re.M)}


for tool in ("dev.py", "matter-convert.py"):
    pointers = re.findall(r"docs/guide/[\w-]+\.md(?:#[\w-]+)?", (SCRIPTS / tool).read_text())
    bad = []
    for pointer in sorted(set(pointers)):
        page, _, anchor = pointer.partition("#")
        path = ROOT / page
        if not path.is_file():
            bad.append(pointer + " (no such page)")
            continue
        text = path.read_text()
        if "**Moved on" in text:
            bad.append(pointer + " (a Moved pointer)")
        elif anchor and anchor not in heading_anchors(text):
            bad.append(pointer + " (no such heading)")
    check(pointers and not bad, f"{tool}: every guide page it names is there, current, with its heading"
          + (": " + "; ".join(bad) if bad else ""))

# erase: the typed word, then esptool from the same environment as esphome.
venv = TMP / "venv" / "bin"
script(venv / "esphome", "#!/bin/sh\n")
script(venv / "python", "#!/bin/sh\n")
calls.clear()
with patched(dev, run=record_run, input=lambda q: "ERASE"), environ(ESPHOME=str(venv / "esphome")):
    with quiet():
        dev.erase(dev.Ctx("c3", "/dev/ttyFAKE", None))
check(calls and calls[-1]["args"] == [str(venv / "python"), "-m", "esptool", "--chip", "esp32c3", "--port", "/dev/ttyFAKE", "erase_flash"],
      "erase confirmed: esptool erase_flash on the port given, for the board's chip, with the python beside esphome")
calls.clear()
with patched(dev, run=record_run, input=lambda q: " ERASE "), environ(ESPHOME="/nonexistent/esphome"):
    with quiet():
        dev.erase(dev.Ctx("c3", "/dev/ttyFAKE", None))
check(calls and calls[-1]["args"][0] == "python3", "erase: no python beside esphome falls back to python3 on PATH")
for board, chip in (("c3", "esp32c3"), ("c3-plus", "esp32c3"), ("classic", "esp32")):
    calls.clear()
    prompts = []
    with patched(dev, run=record_run, input=lambda q: prompts.append(q) or "ERASE"), environ(ESPHOME="/nonexistent/esphome"), quiet():
        dev.erase(dev.Ctx(board, "/dev/ttyFAKE"))
    check(calls[-1]["args"][3:5] == ["--chip", chip] and f"the {chip} on /dev/ttyFAKE" in prompts[0],
          f"erase --board {board}: esptool is told to expect an {chip}, and the prompt names the chip and the port")
for board in dev.ESPHOME_AMEBAZ2_BOARDS:
    calls.clear()
    with patched(dev, run=record_run, input=lambda q: "ERASE"), quiet():
        msg = died(dev.erase, dev.Ctx(board, "/dev/ttyFAKE"))
    check(msg and "has test and build" in msg and not calls, f"erase called for {board} runs nothing, whatever is typed")
calls.clear()
with patched(dev, run=record_run, input=lambda q: "erase"), quiet():
    msg = died(dev.erase, dev.Ctx("c3", "/dev/ttyFAKE", None))
check(msg == "not confirmed; nothing erased" and not calls, "erase: the word in lower case runs nothing")

# walk: the order of the steps, driven by scripted answers.
order = []
answers = iter(["y", "y", "y", "y", "/dev/ttyFAKE", "n", "y"])
steps = {name: (lambda ctx, name=name: order.append(name)) for name in ("fetch", "test_target", "build", "erase", "flash")}
with patched(dev, input=lambda q: next(answers), doctor=lambda ctx: order.append("doctor") or False, **steps), quiet():
    ctx = dev.Ctx("c3", None, None)
    dev.walk(ctx)
check(order == ["doctor", "fetch", "test_target", "build", "flash"] and ctx.port == "/dev/ttyFAKE",
      "walk: doctor, fetch after gaps, test, build, then flash on the port typed in, erase skipped on n")

# ================================================================================================
print("== dev.py edges: the environment it runs in ==")

# $ESPHOME used to go into `bash -c "command -v $ESPHOME"` as text, so anything after a `;` ran.
marker = TMP / "ran-from-ESPHOME"
for cmd in ("doctor", "build", "test"):
    r = cli(cmd, "esphome", ESPHOME=f"nonexistent-tool; touch {marker}")
check(not marker.exists(), "$ESPHOME is looked up as a command name, not run as shell text (doctor, build, test)")
r = cli("erase", "esphome", "--port", "/dev/null", stdin="ERASE\n", ESPHOME=f"nonexistent-tool; touch {marker}", PATH=str(TMP / "empty-path"))
check(not marker.exists() and r.returncode == 1 and "Traceback" not in r.stderr, "nor by erase, which looks for the python beside it")

# A command with arguments in $ESPHOME ("uvx esphome", "python3 -m esphome") runs as that command.
(esphome_dir / "secrets.yaml").write_text("")
seen = TMP / "esphome-args"
launcher = script(TMP / "fake-bin" / "launcher", f'#!/bin/sh\nprintf "%s\\n" "$@" > {seen}\n')
with patched(dev, ESPHOME_DIR=esphome_dir), environ(ESPHOME=f"{launcher} esphome --quiet"), quiet():
    how = raised(dev.Ctx("classic", None, None).esphome_run, "config")
check(how is None and seen.read_text().split("\n")[:2] == ["esphome", "--quiet"] and "config" in seen.read_text(),
      "an $ESPHOME with arguments in it runs: the words after the command come first, then dev.py's own")
with patched(dev, ESPHOME_DIR=esphome_dir), environ(ESPHOME="no-such-launcher esphome"), quiet():
    msg = died(dev.Ctx("c3", None, None).esphome_run, "config")
check(msg and "'no-such-launcher' not found" in msg, "and one whose command does not exist is refused by name")
unrunnable = TMP / "fake-bin" / "not-a-program"
unrunnable.write_text("this is not a program\n")
unrunnable.chmod(0o755)
with patched(dev, ESPHOME_DIR=esphome_dir), environ(ESPHOME=str(unrunnable)), quiet():
    msg = died(dev.Ctx("c3", None, None).esphome_run, "config")
check(msg and msg.startswith("cannot run"), "a command that is there and cannot be executed is a message, not a traceback")

# dev.py says it needs only python3. Its tool lookup used to need bash.
only_python = TMP / "only-python"
only_python.mkdir()
(only_python / "python3").symlink_to(sys.executable)
r = subprocess.run([sys.executable, str(DEV), "doctor", "esphome"], capture_output=True, text=True,
                   env={"PATH": str(only_python), "HOME": str(TMP), "ENVF": NO_ENVF})
check(r.returncode == 1 and "Traceback" not in r.stderr and "git not on PATH" in r.stdout,
      "doctor on a PATH without bash lists what is missing")
r = subprocess.run([sys.executable, str(DEV), "test", "esphome"], capture_output=True, text=True,
                   env={"PATH": str(only_python), "HOME": str(TMP), "ENVF": NO_ENVF})
check(r.returncode == 1 and "Traceback" not in r.stderr and "cannot run bash" in r.stderr,
      "test on a PATH without bash says it cannot run bash (the host tests are a bash script)")

# A PATH with python3 and bash and nothing else: doctor lists every gap and exits 1.
(only_python / "bash").symlink_to(shutil.which("bash"))
r = subprocess.run([sys.executable, str(DEV), "doctor", "esphome"], capture_output=True, text=True,
                   env={"PATH": str(only_python), "HOME": str(TMP), "ENVF": NO_ENVF})
check(r.returncode == 1 and all(gap in r.stdout for gap in ("git not on PATH", "g++ not on PATH", "esphome not installed",
                                                             "pipx not on PATH")),
      "doctor on a PATH with only python3 and bash lists git, g++, esphome and pipx as gaps and exits 1")
r = subprocess.run([sys.executable, str(DEV), "build", "esphome"], capture_output=True, text=True,
                   env={"PATH": str(only_python), "HOME": str(TMP), "ENVF": NO_ENVF})
check(r.returncode == 1 and "'esphome' not found (dev.py fetch esphome, or set ESPHOME=)" in r.stderr,
      "build with no esphome on PATH is refused with the way to get it")

check(not any(hasattr(dev, name) for name in ("convert", "load_env", "pi_stage", "matter_slots", "matter_backup", "ENVF")),
      "dev.py holds nothing of the Matter conversion: that is matter-convert.py")

tmp_holder.cleanup()
sys.exit(edge_harness.finish("DEV.PY EDGES"))
