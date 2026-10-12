#!/usr/bin/env python3
"""dev.py at its edges: the command line, every refusal, the settings file, convert from end to
end, the provider staging commands and the break-glass probes.

test_dev_release.py holds the rules of the happy path. This file feeds dev.py what a person gets
wrong: a missing value, a board that does not exist, a settings file that does not parse, an image
cut short, a unit that answers with rubbish. Each check documents what dev.py does today.

Where today's behaviour is wrong the check is a KNOWN FAILURE: `known_failure()` passes while the
defect is there and fails the day it is fixed, so the marker cannot outlive the defect. Each one
names the defect and the line.

No hardware and no network: the units and the OTA provider host are stand-ins (a TCP listener on
127.0.0.1, recorded ssh and scp calls), the esphome and ota_image_tool.py commands are small
scripts in a temporary directory, and nothing is written outside that directory. About 2 s.
"""

import contextlib
import io
import json
import os
import pathlib
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "firmware" / "scripts"
DEV = SCRIPTS / "dev.py"
sys.path.insert(0, str(SCRIPTS))
import amebaz2_image as az  # noqa: E402
import dev  # noqa: E402

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


def died(fn, *args, **kwargs):
    """The message dev.py died with, or None when it did not die."""
    try:
        fn(*args, **kwargs)
    except dev.Die as e:
        return str(e)
    return None


def raised(fn, *args, **kwargs):
    """The exception class name a call ends in, or None."""
    try:
        fn(*args, **kwargs)
    except BaseException as e:  # noqa: BLE001 - the point is to see which one
        return type(e).__name__
    return None


@contextlib.contextmanager
def patched(obj, **attrs):
    """Set attributes for the length of a block. An attribute that did not exist is removed."""
    missing = object()
    old = {k: getattr(obj, k, missing) for k in attrs}
    # Only what the object itself holds is restored by assignment: a name that came from a base
    # (or, for a module, from builtins) is deleted again.
    own = {k: (k in vars(obj)) for k in attrs}
    for k, v in attrs.items():
        setattr(obj, k, v)
    try:
        yield
    finally:
        for k, v in old.items():
            if own[k]:
                setattr(obj, k, v)
            else:
                delattr(obj, k)


@contextlib.contextmanager
def environ(**kv):
    """Set (or with None, remove) environment variables for the length of a block."""
    old = {k: os.environ.get(k) for k in kv}
    for k, v in kv.items():
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v
    try:
        yield
    finally:
        for k, v in old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


@contextlib.contextmanager
def quiet():
    """Capture what dev.py prints. Yields the two buffers."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        yield out, err


def script(path, text):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


# A child process gets none of the caller's git state and none of the variables dev.py reads, so
# the result does not depend on the shell the suite was started from.
CLEAN_ENV = {k: v for k, v in os.environ.items()
             if not k.startswith("GIT_") and k not in ("ESPHOME", "ENVF", "PIPX_BIN_DIR", "PLATFORMIO_BUILD_CACHE_DIR")}
NO_ENVF = "/nonexistent/ota-release.env"


def cli(*args, stdin="", **env):
    return subprocess.run([sys.executable, str(DEV), *args], input=stdin, capture_output=True, text=True,
                          env={**CLEAN_ENV, "ENVF": NO_ENVF, **env}, timeout=60)


def main(*args, typed=None):
    """dev.main() in this process, with no settings file and no terminal. Returns (how, text):
    ("die", message), ("exit", code) or ("ran", stdout). The command-line checks that only need
    the refusal use this; the ones about exit codes and streams start a real process with cli()."""
    def no_terminal(q):
        if typed is None:
            raise EOFError
        return typed
    with patched(dev, ENVF=pathlib.Path(NO_ENVF), input=no_terminal), quiet() as (out, err):
        try:
            dev.main(list(args))
        except dev.Die as e:
            return "die", str(e)
        except SystemExit as e:
            return "exit", e.code
        return "ran", out.getvalue()


def fake_image(serial, body=0x400):
    """Three chained sub-images signed the way the bootloader checks them, plus the byte-sum.
    The layout (0xE0 manifest, 0x60 headers, 0x20 trailers) is amebaz2_image.py's own."""
    buf = bytearray(az.HDR0)
    for i in range(3):
        hdr = bytearray(az.HDR_LEN)
        struct.pack_into("<II", hdr, 0, body, az.LINK_END if i == 2 else az.HDR_LEN + body + az.MAC_LEN)
        buf += hdr + bytes([0x10 + i]) * body + bytes(az.MAC_LEN)
    signed = az.resign(bytes(buf), serial=serial)
    return signed + az.bytesum(signed)


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
    (["frobnicate"], "target must be esphome (got '')", "an unknown command with no target is told about the target"),
    (["build"], "target must be esphome (got '')", "a command with no target"),
    (["build", "ESPHOME"], "target must be esphome", "the target is case-sensitive"),
    (["build", "esphome", "--bogus"], "unknown option: --bogus", "an unknown option"),
    (["build", "esphome", "extra"], "unknown option: extra", "a stray positional argument"),
    (["build", "esphome", "--board", "nope"], "--board must be c3", "a board that does not exist"),
    (["build", "esphome", "--board", "amebaz2"], "needs the unit's flash layout", "amebaz2 without a flash layout"),
    (["build", "esphome", "--board", "C3"], "--board must be c3", "the board is case-sensitive"),
    (["flash", "esphome"], "flash needs --port", "flash without a port"),
    (["monitor", "esphome"], "monitor needs --port", "monitor without a port"),
    (["erase", "esphome"], "erase needs --port", "erase without a port"),
    (["bench", "esphome"], "bench needs --port", "bench without a port"),
    (["bench", "esphome", "--port", "/dev/null"], "bench needs --sim-port", "bench without a simulator port"),
    (["build", "esphome", "--serial", "5"], "unknown option: --serial", "--serial outside convert"),
    (["build", "esphome", "--stage"], "unknown option: --stage", "--stage outside convert"),
    (["convert", "esphome"], "convert is for esphome --board amebaz2-factory", "convert for the default ESP32 board"),
    (["convert", "esphome", "--board", "c3-plus"], "convert is for", "convert for the C3 Plus"),
    (["convert", "esphome", "--board", "classic"], "convert is for", "convert for the classic ESP32"),
    (["convert", "esphome", "--board", "amebaz2-native"], "amebaz2-native has none", "convert for the native layout"),
    (["convert", "esphome", "--board", "amebaz2-factory", "--serial", "0"], "positive integer (got '0')", "serial 0"),
    (["convert", "esphome", "--board", "amebaz2-factory", "--serial", "-1"], "positive integer (got '-1')", "a negative serial"),
    (["convert", "esphome", "--board", "amebaz2-factory", "--serial", "1.5"], "positive integer", "a fractional serial"),
    (["convert", "esphome", "--board", "amebaz2-factory", "--serial", "0x10"], "positive integer", "a hex serial"),
    (["convert", "esphome", "--board", "amebaz2-factory", "--software-version", "0"], "positive integer",
     "softwareVersion 0"),
    (["convert", "esphome", "--board", "amebaz2-factory"], f"missing {NO_ENVF}", "convert without a settings file"),
    (["matter-slots"], f"missing {NO_ENVF}", "matter-slots without a settings file"),
    (["matter-backup"], f"missing {NO_ENVF}", "matter-backup without a settings file"),
    (["ota"], "matter-final", "the retired ota command, with nothing after it"),
    (["flash", "amebaz2", "--port", "/dev/null"], "matter-final", "the retired amebaz2 target on a flashing command"),
]
for args, needle, why in REFUSALS:
    how, text = main(*args)
    check(how == "die" and needle in text, f"refused: {why}")
# A refusal is one line on stderr and exit code 1, never a traceback.
r = cli("build", "esphome", "--board", "nope")
check(r.returncode == 1 and r.stderr.count("\n") == 1 and "[dev] ERROR:" in r.stderr and r.stdout == "",
      "a refusal is one `[dev] ERROR:` line on stderr, nothing on stdout, exit 1")

# A flag that is the last word has no value. dev.py reports the flag itself as unknown, which is
# the behaviour today and reads wrong ("unknown option: --board" for an option that exists).
for flag in ("--board", "--port", "--sim-port", "--name", "--friendly-name"):
    check(main("build", "esphome", flag) == ("die", f"unknown option: {flag}"),
          f"{flag} with no value is refused (as an 'unknown option', which is misleading)")
check(main("convert", "esphome", "--board", "amebaz2-factory", "--serial") == ("die", "unknown option: --serial"),
      "--serial with no value is refused the same way")

# An unknown command with a valid target prints the whole usage text and no line that says what
# was wrong: nothing on stderr.
r = cli("frobnicate", "esphome")
check(r.returncode == 1 and "Commands:" in r.stdout and r.stderr == "",
      "an unknown command prints the usage and exits 1, with no message naming the command")

# The node name is a hostname: `next` only prints, so it shows which names get through.
for name in ("a", "ac-2", "0a", "a" * 31):
    check(main("next", "esphome", "--name", name)[0] == "ran", f"--name '{name[:12]}' ({len(name)} chars) is accepted")
for name in ("Bad_Name", "UPPER", "-lead", "trail-", "a" * 32, "a.b", "a b", "ac\n"):
    how, text = main("next", "esphome", "--name", name)
    check(how == "die" and "is not a valid hostname" in text, f"--name {name[:12]!r} ({len(name)} chars) is refused")
check(main("next", "esphome", "--name", "")[0] == "ran", "an empty --name falls back to the default name")

# The stock module has no USB path: every command that would touch a port is refused before it runs.
for board in dev.ESPHOME_AMEBAZ2_BOARDS:
    for cmd in ("walk", "erase", "flash", "monitor", "bench", "next"):
        how, text = main(cmd, "esphome", "--board", board, "--port", "/dev/null", "--sim-port", "/dev/null")
        check(how == "die" and "has test, build and convert" in text, f"{cmd} --board {board} is refused")

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

for board, pins, esphome_board in (("c3", (5, 6, 10), "esp32-c3-devkitm-1"), ("c3-plus", (5, 6, 10), "esp32-c3-devkitm-1"),
                                   ("classic", (19, 18, 4), "esp32dev")):
    c = dev.Ctx("esphome", board, None, None)
    check(c.pins == pins and c.esphome_board == esphome_board, f"--board {board}: pins {pins}, {esphome_board}")
for board in dev.ESPHOME_AMEBAZ2_BOARDS:
    check(dev.Ctx("esphome", board, None, None).pins == ("PA14", "PA13", "PA17"), f"--board {board}: the module's own pins")
check(died(dev.Ctx, "esphome", "", None, None) is not None, "an empty board is refused")

# Which esphome is run: $ESPHOME, then PATH, then pipx's directory.
pipx_bin = TMP / "pipx-bin"
with environ(ESPHOME="/opt/x/esphome", PIPX_BIN_DIR=None):
    check(dev.Ctx("esphome", "c3", None, None).esphome_cmd() == "/opt/x/esphome", "$ESPHOME names the command")
with environ(ESPHOME=None, PIPX_BIN_DIR=str(pipx_bin)), patched(dev, have=lambda tool: False):
    c = dev.Ctx("esphome", "c3", None, None)
    check(c.esphome_cmd() == "esphome", "no esphome anywhere: the bare name, so the error names it")
    pipx_bin.mkdir()
    (pipx_bin / "esphome").write_text("#!/bin/sh\n")
    check(c.esphome_cmd() == "esphome", "a pipx esphome that is not executable is not used")
    script(pipx_bin / "esphome", "#!/bin/sh\n")
    check(c.esphome_cmd() == str(pipx_bin / "esphome"), "an executable esphome in PIPX_BIN_DIR is used when PATH has none")

# esphome_run: what it refuses, and the command line it builds.
calls = []


def record_run(args, env=None, cwd=None, check=True):  # noqa: ARG001 - dev.run's signature
    calls.append({"args": [str(a) for a in args], "env": env, "cwd": cwd})
    return 0


esphome_dir = TMP / "esphome"
esphome_dir.mkdir()
with patched(dev, run=record_run, ESPHOME_DIR=esphome_dir), environ(ESPHOME="esphome-stub", PLATFORMIO_BUILD_CACHE_DIR=None):
    with patched(dev, have=lambda tool: False):
        msg = died(dev.Ctx("esphome", "c3", None, None).esphome_run, "config")
        check(msg and "'esphome-stub' not found" in msg, "esphome missing: refused, names the command and how to get it")
    with patched(dev, have=lambda tool: True):
        msg = died(dev.Ctx("esphome", "c3", None, None).esphome_run, "config")
        check(msg and "secrets.yaml" in msg and not calls, "no secrets.yaml: refused before esphome runs")
        (esphome_dir / "secrets.yaml").write_text("")
        for sub in ("run", "logs", "upload", "clean"):
            msg = died(dev.Ctx("esphome", "amebaz2-factory", None, None).esphome_run, sub)
            check(msg and "has test, build and convert" in msg and not calls, f"esphome {sub} for the stock module is refused")
        dev.Ctx("esphome", "classic", None, None, "ac-2", "Bedroom A/C").esphome_run("run", "--device", "/dev/ttyX")
        a = calls[-1]["args"]
        check(a == ["esphome-stub", "-s", "board", "esp32dev", "-s", "tx_pin", "19", "-s", "rx_pin", "18",
                    "-s", "de_pin", "4", "-s", "name", "ac-2", "-s", "friendly_name", "Bedroom A/C",
                    "run", "w41h1.yaml", "--device", "/dev/ttyX"],
              "classic: substitutions first, then the subcommand, the file and the caller's arguments")
        check(calls[-1]["cwd"] == esphome_dir, "esphome runs in the ESPHome directory")
        check(calls[-1]["env"].get("PLATFORMIO_BUILD_CACHE_DIR", "").endswith("w41h1-dev/pio-build-cache"),
              "esphome gets the build cache directory")
        dev.Ctx("esphome", "amebaz2-sdk", None, None, "ac-3").esphome_run("compile", cache=False)
        check(calls[-1]["args"] == ["esphome-stub", "-s", "name", "ac-3", "compile", "w41h1-amebaz2-sdk.yaml"],
              "stock module: the board file, no pin substitutions, the node name kept")
        check("PLATFORMIO_BUILD_CACHE_DIR" not in calls[-1]["env"], "cache=False builds without the build cache")
        dev.Ctx("esphome", "c3-plus", None, None, None, "Plus").esphome_run("config")
        check(calls[-1]["args"] == ["esphome-stub", "-s", "friendly_name", "Plus", "config", "w41h1-esp32c3-plus.yaml"],
              "C3 Plus: the board file, and a friendly name alone is passed")

# --board c3 builds w41h1.yaml with -s overrides. The repo also has w41h1-esp32c3.yaml, which adds
# the status LED package, so `dev.py build --board c3` and `esphome compile w41h1-esp32c3.yaml`
# are two different images for the same board. Documented here, reported as a simplification.
c3_file = (ROOT / "firmware/esphome/w41h1-esp32c3.yaml").read_text()
with patched(dev, run=record_run, have=lambda tool: True, ESPHOME_DIR=esphome_dir), environ(ESPHOME="esphome-stub"):
    dev.Ctx("esphome", "c3", None, None).esphome_run("compile")
check("w41h1.yaml" in calls[-1]["args"] and "status-led.yaml" in c3_file,
      "--board c3 builds w41h1.yaml, not w41h1-esp32c3.yaml (which alone has the status LED)")

# build: the stock-module slot image is a side effect of the link step, so a fully cached build
# has none and dev.py builds once more without the cache.
with patched(dev, ESPHOME_DIR=TMP / "esphome-build"):
    ctx = dev.Ctx("esphome", "amebaz2-factory", None, None, "t")
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
    esp = dev.Ctx("esphome", "c3", None, None)
    esp.esphome_run = lambda sub, *a, cache=True: fake_esphome_run(sub, *a, cache=cache, make=())
    dev.build(esp)
    check(runs == [("compile", True)], "build: an ESP32 board compiles once and looks for no slot image")

# doctor: read-only, lists every gap, and its exit code says whether there was one.
doctor_dir = TMP / "esphome-doctor"
doctor_dir.mkdir()
fake_esphome = script(TMP / "fake-bin" / "esphome", '#!/bin/sh\necho "Version: ${FAKE_ESPHOME_VERSION}"\n')
with patched(dev, ESPHOME_DIR=doctor_dir), environ(ESPHOME=str(fake_esphome), FAKE_ESPHOME_VERSION=dev.ESPHOME_PIN):
    ctx = dev.Ctx("esphome", "c3", None, None)
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
    msg = died(dev.fetch, dev.Ctx("esphome", "c3", None, None))
    check(msg and "install pipx first" in msg, "fetch without pipx is refused and says how to get it")
calls.clear()
with patched(dev, have=lambda tool: True, run=record_run, input=lambda q: "n"):
    dev.fetch(dev.Ctx("esphome", "c3", None, None))
    check(not calls, "fetch answered with n installs nothing")

# erase: the typed word, then esptool from the same environment as esphome.
venv = TMP / "venv" / "bin"
script(venv / "esphome", "#!/bin/sh\n")
script(venv / "python", "#!/bin/sh\n")
calls.clear()
with patched(dev, run=record_run, input=lambda q: "ERASE"), environ(ESPHOME=str(venv / "esphome")):
    with quiet():
        dev.erase(dev.Ctx("esphome", "c3", "/dev/ttyFAKE", None))
check(calls and calls[-1]["args"] == [str(venv / "python"), "-m", "esptool", "--port", "/dev/ttyFAKE", "erase_flash"],
      "erase confirmed: esptool erase_flash on the port given, with the python beside esphome")
calls.clear()
with patched(dev, run=record_run, input=lambda q: " ERASE "), environ(ESPHOME="/nonexistent/esphome"):
    with quiet():
        dev.erase(dev.Ctx("esphome", "c3", "/dev/ttyFAKE", None))
check(calls and calls[-1]["args"][0] == "python3", "erase: no python beside esphome falls back to python3 on PATH")
check(calls and "--chip" not in calls[-1]["args"],
      "erase: no --chip is passed, so esptool erases whichever ESP chip answers on that port")
calls.clear()
with patched(dev, run=record_run, input=lambda q: "erase"), quiet():
    msg = died(dev.erase, dev.Ctx("esphome", "c3", "/dev/ttyFAKE", None))
check(msg == "not confirmed; nothing erased" and not calls, "erase: the word in lower case runs nothing")

# walk: the order of the steps, driven by scripted answers.
order = []
answers = iter(["y", "y", "y", "y", "/dev/ttyFAKE", "n", "y"])
steps = {name: (lambda ctx, name=name: order.append(name)) for name in ("fetch", "test_target", "build", "erase", "flash")}
with patched(dev, input=lambda q: next(answers), doctor=lambda ctx: order.append("doctor") or False, **steps), quiet():
    ctx = dev.Ctx("esphome", "c3", None, None)
    dev.walk(ctx)
check(order == ["doctor", "fetch", "test_target", "build", "flash"] and ctx.port == "/dev/ttyFAKE",
      "walk: doctor, fetch after gaps, test, build, then flash on the port typed in, erase skipped on n")

# ================================================================================================
print("== dev.py edges: the settings file (ota-release.env) ==")

envf = TMP / "ota-release.env"


def load(text, required=(), **env):
    envf.write_text(text)
    with patched(dev, ENVF=envf), environ(**env):
        return dev.load_env(required)


with patched(dev, ENVF=TMP / "absent.env"):
    msg = died(dev.load_env)
check(msg and "missing" in msg and "ota-release.env.example" in msg, "a missing settings file is refused and names the example")
with patched(dev, ENVF=TMP):
    check(died(dev.load_env) is not None, "a directory in place of the settings file is refused")

cfg = load('A="$HOME/x"\nB="$A/y"\nC="two words"\nD="a=b"\nE="line1\nline2"\nF=\'$HOME\'\n', HOME="/home/you")
check(cfg["A"] == "/home/you/x" and cfg["B"] == "/home/you/x/y", "$HOME and a reference to an earlier setting are expanded")
check(cfg["C"] == "two words" and cfg["D"] == "a=b", "a value with a space, and one with an equals sign, survive")
check(cfg["E"] == "line1\nline2", "a value with a newline survives (the listing is NUL-separated)")
check(cfg["F"] == "$HOME", "single quotes are bash's: no expansion")

cfg = load("echo noise on stdout\nOTA_TOOL=/t\n")
check(cfg["OTA_TOOL"] == "/t", "a settings file that prints to stdout still loads")

cfg = load("VID=0xFFF1\n", OTA_TOOL="/from/the/environment")
check(cfg["OTA_TOOL"] == "/from/the/environment", "a setting the file lacks is taken from the caller's environment")
cfg = load("OTA_TOOL=/from/the/file\n", OTA_TOOL="/from/the/environment")
check(cfg["OTA_TOOL"] == "/from/the/file", "the file wins over the caller's environment")

envf.write_text("VID=1\nPID=2\n")
with patched(dev, ENVF=envf), environ(OTA_TOOL=None):
    msg = died(dev.load_env, ("OTA_TOOL", "VID", "PID"))
check(msg and msg.startswith("OTA_TOOL is not set"), "a required setting that is missing is named")
envf.write_text('OTA_TOOL=""\n')
with patched(dev, ENVF=envf), environ(OTA_TOOL=None):
    check(died(dev.load_env, ("OTA_TOOL",)) is not None, "a required setting that is empty counts as missing")

envf.write_text("OTA_TOOL=/t\nif then\n")
r = cli("matter-slots", "127.0.0.1", ENVF=str(envf))
check(r.returncode == 1 and "could not source" in r.stderr and "syntax error" in r.stderr,
      "a settings file with a syntax error is refused, and bash's own message is shown")

# Two things a settings file can do that dev.py does not see. Both are behaviour today.
envf.write_text("OTA_TOOL=/t\nexit 0\n")
with patched(dev, ENVF=envf), environ(OTA_TOOL=None):
    cfg = dev.load_env()
check(cfg == {}, "a settings file that calls `exit 0` loads as NO settings at all, with no error")
envf.write_text('OTA_TOOL=/t\n[ -n "$NOT_SET_ANYWHERE" ] && X=1\n')
with patched(dev, ENVF=envf), environ(NOT_SET_ANYWHERE=None):
    msg = died(dev.load_env)
check(msg and "could not source" in msg,
      "a settings file whose last line is a false test is 'could not source', though every setting was read")

if os.geteuid() != 0:
    envf.write_text("OTA_TOOL=/t\n")
    envf.chmod(0)
    r = cli("matter-slots", "127.0.0.1", ENVF=str(envf))
    envf.chmod(0o600)
    check(r.returncode == 1 and "could not source" in r.stderr, "a settings file that cannot be read is refused")

msg = died(dev.need, {"A": "1", "B": ""}, "A", "B")
check(msg and msg.startswith("B is not set"), "need() names the first setting that is missing or empty")

# The slot-selection serial base.
check(dev.serial_base({}) == 1100 and dev.serial_base({"SERIAL_BASE": ""}) == 1100, "SERIAL_BASE defaults to 1100")
check(dev.serial_base({"SERIAL_BASE": " 2000 "}) == 2000, "SERIAL_BASE is read as a decimal integer, spaces ignored")
check(dev.serial_base({"SERIAL_BASE": "0"}) == 0, "SERIAL_BASE=0 is taken as 0 (the string is not empty)")
known_failure(raised(dev.serial_base, {"SERIAL_BASE": "eleven hundred"}) in (None, "Die"),
              "a SERIAL_BASE that is not a number is refused with a message",
              "dev.py:541 serial_base(): int() raises ValueError, which reaches the user as a traceback")
known_failure(raised(dev.serial_base, {"SERIAL_BASE": "0x44C"}) in (None, "Die"),
              "a hex SERIAL_BASE is read, or refused with a message",
              "dev.py:541 serial_base(): same ValueError, while VID and PID in the same file are hex")

# ================================================================================================
print("== dev.py edges: convert, from the built image to the .ota and its manifest ==")

# A stand-in for connectedhomeip's ota_image_tool.py: it records its arguments in front of the
# image, or fails when told to.
ota_tool = script(TMP / "ota_image_tool.py",
                  "import json, os, sys\n"
                  "if os.environ.get('FAKE_OTA_TOOL_FAILS'):\n    sys.exit(1)\n"
                  "src, dst = sys.argv[-2], sys.argv[-1]\n"
                  "open(dst, 'wb').write(json.dumps(sys.argv[1:-2]).encode() + b'\\n' + open(src, 'rb').read())\n")
repo = TMP / "repo"
conv_esphome = TMP / "esphome-convert"
built = repo / "firmware" / "built-images"


def convert(board="amebaz2-factory", name=None, image=None, settings="", **opts):
    """Run dev.convert against a temporary tree. Returns (died-with, stdout, stderr)."""
    envf.write_text(f'OTA_TOOL="{ota_tool}"\nVID="0xFFF1"\nPID="0x8001"\n{settings}')
    with patched(dev, REPO=repo, ESPHOME_DIR=conv_esphome, ENVF=envf), environ(SERIAL_BASE=None, PI_HOST=None):
        ctx = dev.Ctx("esphome", board, None, None, name)
        ctx.convert_opts = dict(opts)
        src = ctx.esphome_slot_image()
        src.parent.mkdir(parents=True, exist_ok=True)
        if image is None:
            src.unlink(missing_ok=True)
        else:
            src.write_bytes(image)
        with quiet() as (out, err):
            msg = died(dev.convert, ctx)
        return msg, out.getvalue(), err.getvalue()


good = fake_image(1)
msg, out, err = convert(image=None)
check(msg and "run: dev.py build esphome --board amebaz2-factory" in msg and "--name" not in msg,
      "convert with no built image says which build to run")
msg, out, err = convert(image=None, name="ac-2")
check(msg and msg.endswith("--board amebaz2-factory --name ac-2"), "and repeats the node name the build needs")
check(not built.exists(), "nothing has been written to built-images so far")

msg, out, err = convert(image=good)
raw, ota, manifest = (built / f"rac-v10351-esphome.{ext}" for ext in ("bin", "ota", "json"))
check(msg is None and raw.is_file() and ota.is_file() and manifest.is_file(),
      "convert writes the re-signed image, the .ota and the manifest, named after softwareVersion 10351")
signed = raw.read_bytes()
check(az.verify(signed[:-4], signed[-4:])[0] and az.read_serial(signed) == 1100 + 10351,
      "the image verifies and carries serial SERIAL_BASE + softwareVersion (11451)")
tool_args = json.loads(ota.read_bytes().split(b"\n", 1)[0])
check(tool_args == ["create", "-v", "0xFFF1", "-p", "0x8001", "-vn", "10351", "-vs", "1.3.51", "-da", "sha256",
                    "-mi", "1", "-ma", "10350"],
      "ota_image_tool.py is given the vendor, product, version 10351 / 1.3.51 and the range 1 to 10350")
mv = json.loads(manifest.read_text())["modelVersion"]
check(mv["softwareVersion"] == 10351 and mv["softwareVersionString"] == "1.3.51"
      and mv["otaUrl"] == "file:///rac-v10351-esphome.ota" and mv["otaFileSize"] == ota.stat().st_size,
      "the manifest names the .ota, its size and the same version")
check("not staged" in out and "matter-slots" in out, "without --stage it says the files still have to be put on the provider")

msg, out, err = convert(image=good, settings='SERIAL_BASE="2000"\n')
check(az.read_serial(raw.read_bytes()) == 2000 + 10351, "SERIAL_BASE from the settings file moves the serial")
msg, out, err = convert(image=good, serial=77, software_version=10400)
signed = (built / "rac-v10400-esphome.bin").read_bytes()
check(msg is None and az.read_serial(signed) == 77, "--serial is taken as given, whatever SERIAL_BASE says")
mv = json.loads((built / "rac-v10400-esphome.json").read_text())["modelVersion"]
check(mv["softwareVersionString"] == "1.4.0" and mv["maxApplicableSoftwareVersion"] == 10399,
      "--software-version 10400 is labelled 1.4.0 and offered to every version below it")

# Two inputs convert takes without a word. Both are behaviour today, and both produce an update
# that does nothing on the unit: a serial below the running Matter image's is applied and then
# not booted, and a softwareVersion at or below the running one is never offered.
msg, out, err = convert(image=good, serial=5)
check(msg is None and "WARNING" not in err, "a --serial far below SERIAL_BASE is accepted with no warning")
msg, out, err = convert(image=good, software_version=10350)
check(msg is None and "WARNING" not in err,
      "a --software-version equal to the last Matter release (10350) is accepted with no warning")

msg, out, err = convert(board="amebaz2-sdk", image=good)
check(msg is None and "only the factory layout has been converted on hardware" in err, "the sdk layout converts with a warning")

# What it refuses, and what it leaves behind.
for f in built.iterdir():
    f.unlink()
for label, image in (("cut short by one byte", good[:-1]), ("with one body byte flipped", good[:0x200] + b"\xff" + good[0x201:]),
                     ("with its first header cut off", good[:az.HDR0 + 8] + bytes(len(good) - az.HDR0 - 8))):
    msg, out, err = convert(image=image)
    check(msg and "does not verify as built" in msg and not list(built.iterdir()), f"an image {label} is refused, nothing written")
big = fake_image(1, body=0x80000)   # 3 x 512 KB: larger than the factory slot (0x170000)
msg, out, err = convert(image=big)
check(msg and "the slot holds 0x170000" in msg and not list(built.iterdir()), "an image larger than the slot is refused, nothing written")
msg, out, err = convert(board="amebaz2-sdk", image=big)
check(msg is None, "the same image fits the sdk layout's larger slot (0x1ac000)")
for f in built.iterdir():
    f.unlink()

manifest.write_text("{}\n")
with environ(FAKE_OTA_TOOL_FAILS="1"):
    msg, out, err = convert(image=good)
check(msg == "ota_image_tool.py create failed" and not manifest.exists() and not ota.exists(),
      "ota_image_tool.py failing is an error, and the manifest of an earlier run is gone, not left stale")
check(raw.is_file(), "the re-signed .bin of the failed run stays behind (no .ota, no manifest)")

msg, out, err = convert(image=good, stage=True)
check(msg and msg.startswith("PI_HOST is not set"), "--stage without the provider host settings is refused before anything is built")

how = raised(convert, image=b"")
known_failure(how is None and convert(image=b"")[0] is not None,
              "an empty slot image file is refused with a message",
              "dev.py:593 convert(): read_serial() runs for the progress line before the length check in "
              "esphome_matter_image(), so a file under 0xF8 bytes ends in an ImageFormatError traceback")

msg, out, err = convert(image=good, settings='VID="0xFFF2"\nPID="0x8002"\n')
mv = json.loads(manifest.read_text())["modelVersion"] if manifest.is_file() else {}
known_failure(mv.get("vid") == 0xFFF2 and mv.get("pid") == 0x8002,
              "the manifest carries the VID and PID of the settings file",
              "dev.py:607 convert(): ota_manifest() is called without vid/pid, so the manifest always says "
              "0xFFF1/0x8001 while the .ota header is built from the settings file's VID/PID")

# ================================================================================================
print("== dev.py edges: staging on the OTA provider host ==")

stage_cfg = {"PI_HOST": "root@provider", "PI_OTA_DIR": "/data/ota", "PI_SSH_KEY": "/keys/id"}
local_manifest = TMP / "rac-v10351-esphome.json"
local_manifest.write_text(json.dumps(dev.ota_manifest(b"x", 10351, "1.3.51", "file:///rac-v10351-esphome.ota")) + "\n")
local_ota = TMP / "rac-v10351-esphome.ota"
local_ota.write_bytes(b"x")


def man(pid, v):
    return json.dumps({"modelVersion": {"vid": 0xFFF1, "pid": pid, "softwareVersion": v}})


LISTING = (f"rac-v10350.json\t{man(0x8001, 10350)}\nrac-stock-v10332.json\t{man(0x8001, 10332)}\n"
           f"esp32-v10114.json\t{man(0x8000, 10114)}\n")


def stage(cfg=None, listing=LISTING, fail_at=None, scp_rc=0, listing_rc=0):
    """Run dev.pi_stage with ssh and scp recorded. Returns (died-with, ssh commands, scp calls)."""
    ssh, scp = [], []

    def fake_ssh(cfg, command, capture=False, quiet=False):  # noqa: ARG001 - dev.pi_ssh's signature
        ssh.append(command)
        if fail_at is not None and len(ssh) - 1 == fail_at:
            return 1, ""
        if capture:
            return listing_rc, listing
        return 0, ""

    def fake_scp(cfg, files, dest):  # noqa: ARG001
        scp.append(([os.path.basename(str(f)) for f in files], dest))
        return scp_rc

    with patched(dev, pi_ssh=fake_ssh, pi_scp=fake_scp), quiet():
        msg = died(dev.pi_stage, cfg or stage_cfg, local_manifest, local_ota)
    return msg, ssh, scp


msg, ssh, scp = stage()
tmpdir = f"/tmp/ota-stage.{os.getpid()}"
check(msg is None and ssh[0] == f"mkdir -p {tmpdir}" and scp == [(["rac-v10351-esphome.json", "rac-v10351-esphome.ota"], f"{tmpdir}/")],
      "staging copies the manifest and the .ota into a temporary directory on the host")
docker = next(c for c in ssh if c.startswith("docker run"))
check("-v /data/ota:/ota" in docker and "install -m 0644 -o root -g root /src/rac-v10351-esphome.json /src/rac-v10351-esphome.ota /ota/" in docker,
      "a root container installs both into the provider directory")
check("mv /ota/rac-v10350.json /ota/rac-v10350.json.archived" in docker, "the other manifest of the same product is archived")
check("rac-stock-v10332.json" not in docker and "esp32-v10114.json" not in docker,
      "the stock-revert manifest and the other product's manifest are left alone")
check(f"rm -rf {tmpdir}" in docker and "docker restart matter-server" in docker,
      "the temporary directory is removed and matter-server restarted in the same command")
check("rm -f /ota/chip_kvs_ota_provider_* /ota/ota_provider_*.log" in docker,
      "the provider's own state files and logs are deleted on every staging")
check("/dev/tcp/127.0.0.1/5580" in ssh[-1], "it waits for matter-server's port to come back")

msg, ssh, scp = stage(cfg={**stage_cfg, "OTA_KEEP_MANIFESTS": "1"})
check(msg is None and not any("for f in *.json" in c for c in ssh) and ".archived" not in "".join(ssh),
      "OTA_KEEP_MANIFESTS=1 lists and archives nothing")
msg, ssh, scp = stage(fail_at=0)
check(msg == "cannot reach root@provider" and not scp, "a host that cannot be reached is an error before anything is copied")
msg, ssh, scp = stage(scp_rc=1)
check(msg and "scp to root@provider" in msg and not any(c.startswith("docker") for c in ssh), "a failed copy stops before the install")
msg, ssh, scp = stage(fail_at=2)
check(msg == "root-container install on root@provider failed", "a failed install is an error")
msg, ssh, scp = stage(fail_at=3)
check(msg and "did not reopen :5580" in msg, "matter-server not coming back is an error")

# The listing command's exit code is not looked at. When it fails (the directory cannot be read,
# the connection drops) the plan is empty and the new manifest is installed BESIDE the old ones,
# with no warning. Behaviour today.
msg, ssh, scp = stage(listing="", listing_rc=255)
check(msg is None and ".archived" not in "".join(ssh),
      "a failed manifest listing archives nothing and is not reported")

msg, ssh, scp = stage(cfg={**stage_cfg, "PI_OTA_DIR": "/data/my ota"})
docker = next(c for c in ssh if c.startswith("docker run"))
known_failure("-v /data/my ota:/ota" not in docker,
              "a provider directory with a space in it is quoted for the remote shell",
              "dev.py:504-522 pi_stage(): PI_OTA_DIR and the temporary path go into the remote command unquoted")
msg, ssh, scp = stage(listing=LISTING + f"x;touch pwned.json\t{man(0x8001, 10001)}\n")
docker = next(c for c in ssh if c.startswith("docker run"))
known_failure("mv /ota/x;touch pwned.json" not in docker,
              "a manifest file name from the host is quoted before it goes into the root container's shell",
              "dev.py:519 pi_stage(): names read from the provider directory are pasted into `sh -c` unquoted")

# ================================================================================================
print("== dev.py edges: the break-glass probes (matter-slots, matter-backup) ==")


class FakeUnit:
    """A unit's break-glass listener on 127.0.0.1: serves one connection with `handler(conn)`."""

    def __init__(self, handler):
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.got = b""
        self.thread = threading.Thread(target=self._serve, args=(handler,), daemon=True)
        self.thread.start()

    def _serve(self, handler):
        try:
            conn, _ = self.sock.accept()
        except OSError:
            return
        with conn:
            conn.settimeout(5)
            try:
                self.got = conn.recv(256)
                handler(conn)
            except OSError:
                pass

    def close(self):
        self.sock.close()
        self.thread.join(timeout=5)


def closed_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


unit = FakeUnit(lambda c: c.sendall(b"ok: fw1_sn=11450 fw2_sn=42 cur=1\n"))
reply = dev.breakglass_slots("127.0.0.1", "tok", unit.port, timeout=2)
unit.close()
check(reply == "ok: fw1_sn=11450 fw2_sn=42 cur=1" and unit.got == b"tok:slots", "slots: `<token>:slots` is sent and the ok: line returned")
unit = FakeUnit(lambda c: c.sendall(b"err: bad token\n"))
check(dev.breakglass_slots("127.0.0.1", "tok", unit.port, timeout=2) is None, "slots: a reply that is not ok: counts as no answer")
unit.close()
unit = FakeUnit(lambda c: c.recv(1))
check(dev.breakglass_slots("127.0.0.1", "tok", unit.port, timeout=0.2) is None, "slots: a unit that accepts and says nothing counts as no answer")
unit.close()
check(dev.breakglass_slots("127.0.0.1", "tok", closed_port(), timeout=1) is None, "slots: a refused connection counts as no answer")
check(dev.breakglass_slots("", "tok", 1) is None and dev.breakglass_slots("127.0.0.1", "", 1) is None
      and dev.breakglass_slots("127.0.0.1", "tok", "") is None, "slots: no address, no token or no port asks nothing")
unit = FakeUnit(lambda c: c.sendall(b"ok: " + b"x" * 700))
reply = dev.breakglass_query("127.0.0.1", unit.port, "tok:slots", 2)
unit.close()
check(512 <= len(reply) <= 768, "slots: a reply with no newline is cut off after 512 bytes, it does not read for ever")

slots_env = TMP / "slots.env"
slots_env.write_text(f'BREAKGLASS_TOKEN="tok"\nBREAKGLASS_PORT={closed_port()}\n')
r = cli("matter-slots", "127.0.0.1", ENVF=str(slots_env))
check(r.returncode == 1 and "did not answer" in r.stdout and "NOT a verdict" in r.stdout,
      "matter-slots: silence exits 1 and says it proves nothing")
r = cli("matter-slots", ENVF=str(slots_env))
check(r.returncode == 1 and "usage: dev.py matter-slots <unit-ip>" in r.stderr, "matter-slots with no address prints its usage")
r = cli("matter-backup", ENVF=str(slots_env))
check(r.returncode == 1 and "usage: dev.py matter-backup" in r.stderr, "matter-backup with no address prints its usage")
r = cli("matter-backup", "-x", ENVF=str(slots_env))
check(r.returncode == 1 and "unknown flag for matter-backup: -x" in r.stderr, "matter-backup refuses a flag")
r = cli("matter-backup", "127.0.0.1", "a.bin", "b.bin", ENVF=str(slots_env))
check(r.returncode == 1 and "unexpected argument for matter-backup: b.bin" in r.stderr, "matter-backup refuses a third argument")
slots_env.write_text('BREAKGLASS_TOKEN=""\nBREAKGLASS_PORT=2324\n')
r = cli("matter-slots", "127.0.0.1", ENVF=str(slots_env))
check(r.returncode == 1 and "BREAKGLASS_TOKEN is not set" in r.stderr, "the probes refuse to run with an empty token")
slots_env.write_text('BREAKGLASS_TOKEN="tok"\nBREAKGLASS_PORT=abc\n')
for cmd in ("matter-slots", "matter-backup"):
    r = cli(cmd, "127.0.0.1", ENVF=str(slots_env))
    known_failure(r.returncode == 1 and "Traceback" not in r.stderr,
                  f"{cmd}: a BREAKGLASS_PORT that is not a number is refused with a message",
                  "dev.py:645 and dev.py:675: int() on the setting raises ValueError, which reaches the user as a traceback")

# matter-backup against a stand-in unit. The image is 1 MiB and more because the carve only
# accepts [0x100000, 0x180000], the range of a real application image.
stock = fake_image(5, body=0x56000)                       # serial 5: below SERIAL_BASE, so "stock"
SLOT = stock + b"\xff" * 0x2000                           # as read from a slot: image, byte-sum, erased flash


def backup(reply, out_name="backup.bin", base="1100"):
    """Run dev.matter_backup against a unit that sends `reply`. Returns (exit code, stdout, file)."""
    unit = FakeUnit(lambda c: c.sendall(reply))
    out_file = TMP / out_name
    out_file.unlink(missing_ok=True)
    cfg = {"BREAKGLASS_TOKEN": "tok", "BREAKGLASS_PORT": str(unit.port), "SERIAL_BASE": base}
    code = None
    with patched(dev, load_env=lambda required=(): cfg), quiet() as (out, err):
        try:
            dev.matter_backup(["127.0.0.1", str(out_file)])
        except SystemExit as e:
            code = e.code
    unit.close()
    return code, out.getvalue(), out_file, unit.got


def header(data, length=None, addr="0x8010000"):
    return f"ok: backup {addr} {len(data) if length is None else length}\r\n".encode() + data


code, out, f, got = backup(header(SLOT))
check(code is None and f.read_bytes() == SLOT and got == b"tok:backup",
      "backup: a stock image that verifies is saved whole (image, byte-sum and the erased tail)")
check("stock serial 5" in out and "addr=0x8010000" in out, "backup: it reports the serial and the slot address the unit named")
for label, reply, needle in (
        ("the unit refusing", b"err: busy\r\n", "unit refused: err: busy"),
        ("a reply that is not a backup header", b"hello\r\n", "unexpected :backup reply"),
        ("a length of 0", header(b"", length=0), "implausible backup length"),
        ("a length beyond the largest slot", header(b"", length=dev.SLOT_MAX + 1), "implausible backup length"),
        ("the stream ending early", header(SLOT[:0x1000], length=len(SLOT)), "connection closed at 0x1000"),
        ("no header in the first 4 KB", b"x" * 5000, "no backup header in the first 4 KB"),
        ("the connection closing at once", b"", "connection closed before the backup header"),
        ("a slot with no erased tail", header(stock), "no 4096-byte 0xFF run"),
        ("an erased slot", header(b"\xff" * 0x100000), "outside [0x100000,0x180000]"),
        ("an image too small to be one", header(fake_image(5) + b"\xff" * 0x2000), "outside [0x100000,0x180000]"),
        ("one flipped byte", header(SLOT[:0x400] + bytes([SLOT[0x400] ^ 1]) + SLOT[0x401:]), "does not verify"),
        ("a stale byte-sum", header(stock[:-1] + bytes([stock[-1] ^ 1]) + b"\xff" * 0x2000), "byte-sum = MISMATCH"),
):
    code, out, f, got = backup(reply)
    check(code == 1 and needle in out and not f.exists(), f"backup: {label} fails and saves nothing")
code, out, f, got = backup(header(SLOT), base="5")
check(code == 1 and "CUSTOM image, not stock" in out and not f.exists(),
      "backup: a serial at or above SERIAL_BASE is this project's image, not the vendor's, and is not saved")

# With no file name the backup goes to firmware/built-images/, which only `convert` creates.
unit = FakeUnit(lambda c: c.sendall(header(SLOT)))
fresh = TMP / "fresh-clone"
cfg = {"BREAKGLASS_TOKEN": "tok", "BREAKGLASS_PORT": str(unit.port)}
with patched(dev, load_env=lambda required=(): cfg, REPO=fresh), quiet():
    how = raised(dev.matter_backup, ["127.0.0.1"])
unit.close()
known_failure(how is None and (fresh / "firmware/built-images/stock-backup-1-sn5.bin").is_file(),
              "backup: the default file is written on a tree that has no built-images directory yet",
              "dev.py:735 matter_backup(): nothing creates firmware/built-images/, so after the whole slot has "
              "been fetched and verified the save ends in a FileNotFoundError traceback and the backup is lost")

for ip, tag in (("10.0.0.7", "7"), ("1.2.3", "1-2-3"), ("unit.local", "unit-local"), ("::1", "1"), ("", "")):
    check(dev.backup_file_tag(ip) == tag, f"backup file tag of {ip!r} is {tag!r}")

# ================================================================================================
print("== dev.py edges: the environment it runs in ==")

# $ESPHOME goes into `bash -c "command -v $ESPHOME"` as text.
marker = TMP / "ran-from-ESPHOME"
cli("doctor", "esphome", ESPHOME=f"nonexistent-tool; touch {marker}")
known_failure(not marker.exists(),
              "$ESPHOME is looked up as a command name, not run as shell text",
              "dev.py:123 have(): the tool name is interpolated into `bash -c`, so anything after a `;` runs "
              "(same pattern at dev.py:305). shutil.which() does the lookup without a shell")

# A command with arguments in $ESPHOME ("uvx esphome", "python3 -m esphome") passes the lookup
# (bash finds the first word) and then fails as one file name.
with patched(dev, ESPHOME_DIR=esphome_dir), environ(ESPHOME="true --with-an-argument"), quiet():
    how = raised(dev.Ctx("esphome", "c3", None, None).esphome_run, "config")
known_failure(how in (None, "Die"),
              "an $ESPHOME with arguments in it runs, or is refused with a message",
              "dev.py:204 esphome_run(): the string is used as one argv[0], so subprocess raises FileNotFoundError "
              "after have() said the command exists")

# dev.py says it needs only python3. Its tool lookup needs bash.
only_python = TMP / "only-python"
only_python.mkdir()
(only_python / "python3").symlink_to(sys.executable)
r = subprocess.run([sys.executable, str(DEV), "doctor", "esphome"], capture_output=True, text=True,
                   env={"PATH": str(only_python), "HOME": str(TMP), "ENVF": NO_ENVF})
known_failure(r.returncode == 1 and "Traceback" not in r.stderr,
              "doctor on a PATH without bash lists what is missing",
              "dev.py:123 have(): `bash -c command -v` raises FileNotFoundError when bash itself is absent, "
              "so doctor ends in a traceback instead of a gap list")

# A PATH with python3 and bash and nothing else: doctor lists every gap and exits 1.
bash_path = subprocess.run(["bash", "-c", "command -v bash"], capture_output=True, text=True).stdout.strip()
(only_python / "bash").symlink_to(bash_path)
r = subprocess.run([sys.executable, str(DEV), "doctor", "esphome"], capture_output=True, text=True,
                   env={"PATH": str(only_python), "HOME": str(TMP), "ENVF": NO_ENVF})
check(r.returncode == 1 and all(gap in r.stdout for gap in ("git not on PATH", "g++ not on PATH", "esphome not installed",
                                                             "pipx not on PATH")),
      "doctor on a PATH with only python3 and bash lists git, g++, esphome and pipx as gaps and exits 1")
r = subprocess.run([sys.executable, str(DEV), "build", "esphome"], capture_output=True, text=True,
                   env={"PATH": str(only_python), "HOME": str(TMP), "ENVF": NO_ENVF})
check(r.returncode == 1 and "'esphome' not found (dev.py fetch esphome, or set ESPHOME=)" in r.stderr,
      "build with no esphome on PATH is refused with the way to get it")

# Constants the Matter removal left in dev.py. Each is checked only while it is still there, so
# deleting one does not need an edit here.
if hasattr(dev, "IMAGE_KEY"):
    check(dev.IMAGE_KEY == az.KEY, "dev.IMAGE_KEY (unused) still equals the key amebaz2_image.py signs with")
check(dev.SLOT_MAX == max(length for _, length in dev.ESPHOME_AMEBAZ2_SLOTS.values()),
      "SLOT_MAX, the backup length limit, is the largest slot of any layout")
check(dev.int_to_semver(dev.MATTER_FINAL_VERSION + 1) == "1.3.51", "the default conversion version is labelled 1.3.51")

tmp_holder.cleanup()
print(f"== DEV.PY EDGES OK ({known} known failures) ==" if ok else "== DEV.PY EDGES FAILED ==")
sys.exit(0 if ok else 1)
