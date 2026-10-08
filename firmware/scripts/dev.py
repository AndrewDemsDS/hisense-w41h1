#!/usr/bin/env python3
"""dev.py -- one portable entry point for the from-source build / flash / test / release flow
(issues #118, #143).

It is becoming the release engine too (#143). The AmebaZ2 steps that need no SDK and no env file
are implemented here: lint, verint, epoch and tag. Their decisions are plain functions with host
tests (firmware/test/test_dev_release.py, test_image_epoch.sh); ota_guards.py keeps the guard
verdicts both targets share. The steps not ported yet (build, package, stage, flash, release,
publish, revert, and every ESP32 step) still forward to ota-release.sh / esp32-release.sh, which
stay until nothing references them. The helpers dev.py calls (run_tests.sh, esp32-lint.sh,
firmware/setup.sh, scripts/setup.sh) are separate tools, not release logic. Every external command
is printed before it runs. Run it as
`python3 firmware/scripts/dev.py <cmd> <target> [opts]`.

Why Python and not bash: portability (no bashisms, runs the same on any box with python3), and the
env handling is explicit. ESP-IDF's export.sh can only be *sourced* into a shell, so we source it
once in a subprocess, capture the resulting environment with `env -0`, and hand that dict to every
later idf.py call -- the effect of sourcing export.sh, without carrying a mutated shell around.

Commands:
  walk    <target>                       guided: doctor -> test -> build -> flash -> next (y/N each)
  doctor  <target>                       check tools + SDK pins against versions.env (read-only)
  fetch   <target>                       fetch the pinned SDKs (asks first; several GB)
  test    <target>                       host QA (run_tests.sh) + the target's lint
  build   <target>                       build the app (esp32: set-target first if needed)
  erase   <target> --port P              erase-flash, BRAND-NEW boards only (typed confirmation)
  flash   <target> --port P              flash + monitor
  monitor <target> --port P              serial monitor only
  bench   <target> --port P --sim-port S  busmon/app vs virtual_ac.py over a USB adapter
  next    <target>                       print the staged bring-up and its safety warnings
  ota     <target> <step> [args]         Matter OTA release steps (amebaz2 | esp32), below

OTA steps, `dev.py ota amebaz2 <step>`:
  lint                                   host tests + .zap contiguity + softwareVersion (the git hook)
  verint [semver]                        semver -> Matter softwareVersion int (no SDK, no env file)
  epoch                                  print the build clock (SOURCE_DATE_EPOCH) build would use
  tag                                    signed local tag amebaz2-v<semver> + release-runner check
  preflight | verify                     tools + link pre-flight | read the node's live version
  build | package | stage | flash | release | publish | revert [...]   forwarded to ota-release.sh
OTA steps, `dev.py ota esp32 <step>`:
  preflight | verify, and build | package | stage | flash | release | publish | tag | verint,
  forwarded to esp32-release.sh

Targets: amebaz2 | esp32 | esphome. Board (esp32/esphome): --board c3 (ESP32-C3 SuperMini, default)
or --board classic (ESP32-D0WDQ6). Env: IDF_PATH / ESP_MATTER_PATH (esp32; default ~/esp/esp-idf and
~/esp/esp-matter), ESPHOME (esphome command, default `esphome`), ENVF (the release env file, default
firmware/scripts/ota-release.env; the self-hosted runner copies its secrets file to that exact path).
"""

import atexit
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import ctypes.util
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
ESP = REPO / "firmware/esp32-matter"
ESPHOME_DIR = REPO / "firmware/esphome"
TEST = REPO / "firmware/test"
# Overridable, as the release scripts always allowed (e.g. a copy with an SSH-tunnel MS_WS, or
# another unit's NODE_ID). The default name is load-bearing: the self-hosted release runner copies
# its secrets file to exactly firmware/scripts/ota-release.env.
ENVF = Path(os.environ.get("ENVF") or HERE / "ota-release.env")
sys.path.insert(0, str(HERE))
import ota_guards  # noqa: E402  (pure guard verdicts shared by both targets, host-tested)
ESPHOME_PIN = "2026.7.4"   # CI's `esphome config` pin (.github/workflows/qa.yaml); keep in step

# Line-buffer stdout so our own lines stay in order with the stderr warnings and with the output
# of the tools we run. Without it, piping or logging dev.py (tee, CI) shows "doctor found gaps
# (above)" before the gap list, and a `$ cmd` line after the command's own output.
sys.stdout.reconfigure(line_buffering=True)

C = {"cyan": "\033[1;36m", "yellow": "\033[1;33m", "red": "\033[1;31m",
     "grey": "\033[1;90m", "green": "\033[32m", "off": "\033[0m"}


def say(m, file=None): print(f"{C['cyan']}[dev]{C['off']} {m}", file=file or sys.stdout)
def warn(m): print(f"{C['yellow']}[dev] WARNING:{C['off']} {m}", file=sys.stderr)
def ok(m): print(f"  {C['green']}ok{C['off']}    {m}")


class Die(Exception):
    pass


def die(m):
    raise Die(m)


def ask(q):
    try:
        return input(f"{q} [y/N] ").strip().lower() == "y"
    except EOFError:
        return False


def run(args, env=None, cwd=None, check=True):
    """Print the command then execute it, streaming output."""
    shown = " ".join(str(a) for a in args)
    print(f"{C['grey']}  $ {shown}{C['off']}")
    r = subprocess.run([str(a) for a in args], env=env, cwd=str(cwd) if cwd else None)
    if check and r.returncode != 0:
        die(f"command failed ({r.returncode}): {shown}")
    return r.returncode


def have(tool):
    return subprocess.run(["bash", "-c", f"command -v {tool}"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def load_versions():
    """versions.env is KEY=value (single source of truth for SDK pins)."""
    out = {}
    for line in (REPO / "versions.env").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip().strip('"').strip("'")
    return out


def _env0_after(source_cmd):
    """Run `source_cmd` in bash, then capture the resulting environment via env -0 into a dict."""
    p = subprocess.run(["bash", "-c", f"{source_cmd} >/dev/null 2>&1; env -0"],
                       stdout=subprocess.PIPE, check=False)
    env = {}
    for chunk in p.stdout.split(b"\0"):
        if b"=" in chunk:
            k, v = chunk.split(b"=", 1)
            env[k.decode("utf-8", "replace")] = v.decode("utf-8", "replace")
    return env


_esp_env_cache = {}


def esp_env(idf_only=False):
    """The environment after sourcing ESP-IDF (+ esp-matter unless idf_only). Cached per mode.

    If idf.py is already on PATH and idf_only, keep the current env.
    """
    key = "idf" if idf_only else "full"
    if key in _esp_env_cache:
        return _esp_env_cache[key]
    idf_path = os.environ.get("IDF_PATH", str(Path.home() / "esp/esp-idf"))
    matter_path = os.environ.get("ESP_MATTER_PATH", str(Path.home() / "esp/esp-matter"))
    if have("idf.py") and idf_only:
        _esp_env_cache[key] = dict(os.environ)
        return _esp_env_cache[key]
    if not Path(idf_path, "export.sh").is_file():
        die(f"no ESP-IDF at {idf_path} (set IDF_PATH, or: dev.py fetch esp32)")
    say(f"sourcing ESP-IDF ({idf_path})")
    src = f'. "{idf_path}/export.sh"'
    if not idf_only:
        if not Path(matter_path, "export.sh").is_file():
            die(f"no esp-matter at {matter_path} (set ESP_MATTER_PATH, or: dev.py fetch esp32)")
        say(f"sourcing esp-matter ({matter_path})")
        src += f' && . "{matter_path}/export.sh"'
    env = _env0_after(src)
    if "PATH" not in env:
        die("sourcing the ESP env produced no PATH -- check IDF_PATH/ESP_MATTER_PATH")
    _esp_env_cache[key] = env
    return env


def sdkconfig_target(project):
    m = re.search(r'^CONFIG_IDF_TARGET="(.*)"', (Path(project) / "sdkconfig").read_text(), re.M) \
        if (Path(project) / "sdkconfig").is_file() else None
    return m.group(1) if m else ""


def ensure_target(project, idf_tgt, env):
    """idf.py set-target wipes sdkconfig + build/, so only run it when the target changes."""
    cur = sdkconfig_target(project)
    if cur != idf_tgt:
        if cur:
            warn(f"{project} is configured for {cur}; switching to {idf_tgt} wipes its sdkconfig and build/")
        run(["idf.py", "set-target", idf_tgt], env=env, cwd=project)


class Ctx:
    """Parsed invocation: target, board-derived pins, and options."""
    def __init__(self, target, board, port, sim_port):
        self.target = target
        self.board = board
        self.port = port
        self.sim_port = sim_port
        self._versions = None
        if board == "c3":
            self.idf_tgt, self.pins, self.esphome_board = "esp32c3", (5, 6, 10), "esp32-c3-devkitm-1"
        elif board == "classic":
            self.idf_tgt, self.pins, self.esphome_board = "esp32", (19, 18, 4), "esp32dev"
        else:
            die("--board must be c3 or classic")

    @property
    def versions(self):
        # Read on first use: the SDK-free ota steps (lint, verint, epoch) never need the pins.
        if self._versions is None:
            self._versions = load_versions()
        return self._versions

    def need_port(self, cmd):
        if not self.port:
            die(f"{cmd} needs --port <serial device> (e.g. /dev/ttyACM0)")

    def esphome_cmd(self):
        if "ESPHOME" in os.environ:
            return os.environ["ESPHOME"]
        if have("esphome"):
            return "esphome"
        # `pipx install` puts the app in ~/.local/bin (or PIPX_BIN_DIR), which a fresh Ubuntu shell
        # does not have on PATH until the next login. Use it there instead of reporting it missing.
        pipx_bin = Path(os.environ.get("PIPX_BIN_DIR", str(Path.home() / ".local/bin"))) / "esphome"
        return str(pipx_bin) if pipx_bin.is_file() and os.access(pipx_bin, os.X_OK) else "esphome"

    def esphome_run(self, sub, *args):
        cmd = self.esphome_cmd()
        if not have(cmd):
            die(f"'{cmd}' not found (dev.py fetch esphome, or set ESPHOME=)")
        if not (ESPHOME_DIR / "secrets.yaml").is_file():
            die(f"no {ESPHOME_DIR}/secrets.yaml -- cp secrets.yaml.example secrets.yaml and fill it in")
        subs = ["-s", "board", self.esphome_board, "-s", "tx_pin", str(self.pins[0]),
                "-s", "rx_pin", str(self.pins[1]), "-s", "de_pin", str(self.pins[2])]
        run([cmd, *subs, sub, "w41h1.yaml", *args], cwd=ESPHOME_DIR)


# ---- doctor ----------------------------------------------------------------------------------
# ESP-IDF v5.5 Linux prerequisites, verbatim from its get-started/linux-macos-setup guide.
ESP_HOST_PKGS = ("git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build "
                 "ccache libffi-dev libssl-dev dfu-util libusb-1.0-0")
# connectedhomeip's Linux prerequisites (docs/guides/BUILDING.md). esp-matter's install.sh sources
# connectedhomeip's bootstrap.sh -p all,esp32, so the ESP32 Matter fetch needs these as well.
# default-jre from that list is left out: a clean ubuntu:24.04 without Java fetched and built fine.
CHIP_HOST_PKGS = ("git gcc g++ pkg-config cmake curl libssl-dev libdbus-1-dev libglib2.0-dev "
                  "libavahi-client-dev ninja-build python3-venv python3-dev python3-pip unzip "
                  "libgirepository1.0-dev libcairo2-dev libreadline-dev libevent-dev")
ESP32_HOST_PKGS = " ".join(dict.fromkeys(f"{ESP_HOST_PKGS} {CHIP_HOST_PKGS}".split()))


def esp_host_gaps():
    """The ESP-IDF host prerequisites that break a fresh machine, each checked the way it fails.

    Found on a clean ubuntu:24.04: install.sh aborts after downloading every toolchain because
    openocd-esp32 cannot load libusb-1.0.so.0. cmake and ninja are install=on_request on Linux in
    ESP-IDF's tools.json, so idf.py expects system ones. The venv module is a separate Debian package.
    """
    # curl: pigweed's pw_env_setup (run by esp-matter's install.sh) shells out to it.
    gaps = [f"{t} not on PATH" for t in ("cmake", "ninja", "curl") if not have(t)]
    if not ctypes.util.find_library("usb-1.0"):
        gaps.append("libusb-1.0 shared library not found")
    # pgi, from connectedhomeip's requirements.all.txt, loads glib while pip builds it; without the
    # library the whole esp-matter install.sh fails at "Installing pip requirements for all".
    if not ctypes.util.find_library("glib-2.0"):
        gaps.append("glib-2.0 shared library not found")
    with tempfile.TemporaryDirectory() as d:
        if subprocess.run([sys.executable, "-m", "venv", d],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
            gaps.append("python3 cannot create a venv (python3-venv)")
    return gaps


def py_minor(exe):
    """'3.12' for a runnable interpreter, '' when it is missing or dead (e.g. a venv whose base
    Python was upgraded away)."""
    try:
        return subprocess.run([str(exe), "-c", "import sys; print('%d.%d' % sys.version_info[:2])"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                              timeout=20).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


def git_head_is(dirpath, want, label, gaps):
    d = Path(dirpath)
    if not (d / ".git").exists():
        gaps.append(f"{label}: not found at {dirpath}"); return
    have_sha = subprocess.run(["git", "-C", str(d), "rev-parse", "HEAD"],
                              stdout=subprocess.PIPE, text=True).stdout.strip()
    want_sha = subprocess.run(["git", "-C", str(d), "rev-parse", f"{want}^{{commit}}"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout.strip()
    if have_sha and have_sha == want_sha:
        ok(f"{label} @ {want}")
    else:
        gaps.append(f"{label} is at {have_sha[:12]}, versions.env pins {want}")


def have_tool(tool, gaps, note=""):
    if have(tool):
        ok(tool)
    else:
        gaps.append(f"{tool} not on PATH{f' ({note})' if note else ''}")


def doctor(ctx):
    gaps = []
    say(f"doctor: {ctx.target}")
    have_tool("git", gaps); have_tool("python3", gaps)
    have_tool("g++", gaps, "the host tests compile C++: sudo apt install g++, or pacman -S gcc")
    v = ctx.versions
    if ctx.target == "amebaz2":
        sdk = os.path.realpath(REPO / "sdk") if (REPO / "sdk").exists() else ""
        if sdk and Path(sdk).is_dir():
            ok(f"sdk symlink -> {sdk}")
            git_head_is(f"{sdk}/ameba-rtos-z2", v.get("AMEBA_Z2_PIN", ""), "ameba-rtos-z2", gaps)
            git_head_is(f"{sdk}/connectedhomeip", v.get("CHIP_PIN", ""), "connectedhomeip", gaps)
            # scripts/setup.sh (our patches, the Matter-overlay edits) is the second half of fetch.
            # Checkouts at the right pins without it build an unpatched SDK, which fails in codegen
            # with "Unhandled server cluster: HISENSE_AIRCON_CLUSTER". Only patches/connectedhomeip.patch
            # registers that cluster in zap_cluster_list.json; the cluster XML and ClusterId.h are no
            # use as a marker, because ota-release.sh build copies those too.
            reg = Path(f"{sdk}/connectedhomeip/src/app/zap_cluster_list.json")
            if reg.is_file() and "HISENSE_AIRCON_CLUSTER" in reg.read_text(errors="replace"):
                ok("scripts/setup.sh applied (Hisense cluster registered in connectedhomeip)")
            else:
                gaps.append("scripts/setup.sh has not run on this SDK (no patches, no Hisense cluster): "
                            f"AMEBA_SDK={sdk}/ameba-rtos-z2 CHIP_SDK={sdk}/connectedhomeip "
                            "bash scripts/setup.sh")
        else:
            gaps.append("no ./sdk symlink (dev.py fetch amebaz2)")
        ok("ota-release.env") if ENVF.is_file() else gaps.append("ota-release.env (cp ota-release.env.example)")
    elif ctx.target == "esp32":
        idf_path = os.environ.get("IDF_PATH", str(Path.home() / "esp/esp-idf"))
        matter_path = os.environ.get("ESP_MATTER_PATH", str(Path.home() / "esp/esp-matter"))
        host = esp_host_gaps()
        gaps += host
        if host:
            gaps.append(f"ESP-IDF + Matter host packages (Debian/Ubuntu): sudo apt install {ESP32_HOST_PKGS}")
        git_head_is(idf_path, v.get("IDF_PIN", ""), "ESP-IDF", gaps)
        # The checkout alone is not an install: a failed install.sh leaves the right commit with no
        # Python env, which then fails at the first build. export.sh prints an ERROR in that state
        # but still returns 0 when sourced, so check for what a working export provides: idf.py.
        # One probe: after export.sh, is idf.py there, and which Python env did ESP-IDF pick.
        idf_env = ""
        if Path(idf_path, "export.sh").is_file():
            probe = subprocess.run(
                ["bash", "-c", f'. "{idf_path}/export.sh" >/dev/null 2>&1; '
                               'command -v idf.py >/dev/null && printf %s "$IDF_PYTHON_ENV_PATH"'],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            idf_env = probe.stdout.strip() if probe.returncode == 0 else ""
            if not idf_env:
                gaps.append(f"ESP-IDF tools are not installed: {idf_path}/export.sh fails "
                            "(fix the host packages, then run dev.py fetch esp32 again)")
        git_head_is(matter_path, v.get("ESP_MATTER_PIN", ""), "esp-matter", gaps)
        # ESP-IDF's env and esp-matter's pigweed venv must come from one interpreter. A venv left on
        # another Python (a host upgrade, or a fetch under a different python3) breaks the next
        # install or build far from the cause. Ported from the dev.sh doctor in #121.
        pw_py = Path(matter_path, "connectedhomeip/connectedhomeip/.environment/pigweed-venv/bin/python3")
        # is_symlink too: the dead-venv case is a python3 symlink to a removed interpreter, and
        # Path.exists() follows the link and reports False for exactly that.
        if idf_env and (pw_py.exists() or pw_py.is_symlink()):
            idf_ver, pw_ver = py_minor(f"{idf_env}/bin/python"), py_minor(pw_py)
            if not pw_ver:
                gaps.append(f"esp-matter's pigweed venv is dead ({pw_py} does not run): move its "
                            ".environment aside, then run dev.py fetch esp32")
            elif idf_ver and pw_ver != idf_ver:
                gaps.append(f"esp-matter's venv is Python {pw_ver} but ESP-IDF's env is {idf_ver}; they "
                            "must match: move .environment aside, then run dev.py fetch esp32")
            elif idf_ver:
                ok(f"ESP-IDF env and esp-matter venv share Python {pw_ver}")
    elif ctx.target == "esphome":
        cmd = ctx.esphome_cmd()
        if have(cmd):
            ver = subprocess.run([cmd, "version"], stdout=subprocess.PIPE, text=True).stdout.split()
            ver = ver[-1] if ver else "?"
            ok(f"esphome {ver}") if ver == ESPHOME_PIN else gaps.append(f"esphome is {ver}, CI pins {ESPHOME_PIN}")
        else:
            gaps.append("esphome not installed (dev.py fetch esphome)")
            if not have("pipx"):
                gaps.append("pipx not on PATH (fetch esphome needs it: sudo apt install pipx)")
        ok("secrets.yaml") if (ESPHOME_DIR / "secrets.yaml").is_file() else gaps.append("secrets.yaml (cp secrets.yaml.example)")
    for g in gaps:
        print(f"  {C['red']}MISS{C['off']}  {g}")
    if gaps:
        warn("doctor found gaps (above)")
        return False
    say("doctor: all good")
    return True


# ---- fetch -----------------------------------------------------------------------------------
def fetch(ctx):
    v = ctx.versions
    if ctx.target == "amebaz2":
        say("AmebaZ2: firmware/setup.sh fetches + pins the Realtek SDK and connectedhomeip (~15 GB,")
        say("needs sudo for host packages), then scripts/setup.sh applies the patches and overlays.")
        root = os.environ.get("SDK_ROOT", str(Path.home() / "ameba-dev"))
        if not ask(f"fetch into {root}?"):
            return
        run(["bash", REPO / "firmware/setup.sh", root])
        if not (REPO / "sdk").exists():
            run(["ln", "-s", root, REPO / "sdk"])
        # scripts/setup.sh refuses to run without these two (`: "${AMEBA_SDK:?}"`), so a bare call
        # stopped the fetch at its last step, after the multi-GB clone had finished.
        run(["bash", REPO / "scripts/setup.sh"],
            env=dict(os.environ, AMEBA_SDK=f"{root}/ameba-rtos-z2", CHIP_SDK=f"{root}/connectedhomeip"))
    elif ctx.target == "esp32":
        idf_path = os.environ.get("IDF_PATH", str(Path.home() / "esp/esp-idf"))
        matter_path = os.environ.get("ESP_MATTER_PATH", str(Path.home() / "esp/esp-matter"))
        say(f"ESP32: ESP-IDF {v.get('IDF_PIN')} -> {idf_path}, esp-matter {v.get('ESP_MATTER_PIN','')[:12]} -> {matter_path} (several GB)")
        host = esp_host_gaps()
        if host:
            die("fix these before the multi-GB fetch (install.sh fails on them only at the very end): "
                + "; ".join(host) + f". Debian/Ubuntu: sudo apt install {ESP32_HOST_PKGS}")
        if not ask("fetch?"):
            return
        if not Path(idf_path).is_dir():
            run(["git", "clone", "-b", v["IDF_PIN"], "--recursive",
                 "https://github.com/espressif/esp-idf.git", idf_path])
        else:
            say(f"{idf_path} exists; checking out {v['IDF_PIN']}")
            run(["git", "-C", idf_path, "fetch", "--tags", "origin"])
            run(["git", "-C", idf_path, "checkout", v["IDF_PIN"]])
            run(["git", "-C", idf_path, "submodule", "update", "--init", "--recursive"])
        run(["./install.sh", "esp32,esp32c3"], cwd=idf_path)
        env = esp_env(idf_only=True)
        if not Path(matter_path).is_dir():
            run(["git", "init", "-q", matter_path])
            run(["git", "-C", matter_path, "remote", "add", "origin",
                 "https://github.com/espressif/esp-matter.git"])
        run(["git", "-C", matter_path, "fetch", "--depth", "1", "origin", v["ESP_MATTER_PIN"]])
        run(["git", "-C", matter_path, "checkout", "-q", "FETCH_HEAD"])
        run(["git", "-C", matter_path, "submodule", "update", "--init", "--depth", "1"])
        run(["./scripts/checkout_submodules.py", "--platform", "esp32", "linux", "--shallow"],
            env=env, cwd=f"{matter_path}/connectedhomeip/connectedhomeip")
        # With the ESP-IDF env: install.sh ends in `python3 -m pip install -r requirements.txt`, which
        # a distro Python (PEP 668, e.g. Ubuntu 24.04) refuses as externally-managed. esp-matter's
        # docs source ESP-IDF's export.sh first so python3 is the IDF venv. --no-host-tool skips
        # building chip-tool/chip-cert, which the firmware build does not use.
        flags = ["--no-host-tool"]
        # Re-running with connectedhomeip's environment already bootstrapped fails inside pigweed's
        # activate.sh ("pw: command not found", exit 127), even after a clean first install.
        # install.sh's --no-bootstrap exists for exactly that case and still installs esp-matter's
        # Python requirements.
        chip_env = Path(matter_path, "connectedhomeip/connectedhomeip/.environment")
        if (chip_env / "activate.sh").is_file() and (chip_env / "cipd/packages/pigweed/gn").is_file():
            say("connectedhomeip's environment is already bootstrapped; reusing it (--no-bootstrap)")
            flags.append("--no-bootstrap")
        run(["./install.sh", *flags], env=env, cwd=matter_path)
    elif ctx.target == "esphome":
        if not have("pipx"):
            die(f"install pipx first (Debian/Ubuntu: sudo apt install pipx; Arch: sudo pacman -S "
                f"python-pipx), or pip install esphome=={ESPHOME_PIN} in a venv and set ESPHOME=")
        if not ask(f"pipx install esphome=={ESPHOME_PIN}?"):
            return
        run(["pipx", "install", "--force", f"esphome=={ESPHOME_PIN}"])
        if not have("esphome"):
            say("pipx put esphome in ~/.local/bin, which is not on this shell's PATH. dev.py finds it")
            say("there anyway; to run esphome by hand, run `pipx ensurepath` and open a new terminal.")
        if not (ESPHOME_DIR / "secrets.yaml").is_file():
            run(["cp", ESPHOME_DIR / "secrets.yaml.example", ESPHOME_DIR / "secrets.yaml"])
        say(f"now fill in {ESPHOME_DIR}/secrets.yaml (it is gitignored): your Wi-Fi, and a new API key")


# ---- test / build / flash / monitor ----------------------------------------------------------
def test_target(ctx):
    run(["bash", TEST / "run_tests.sh"])
    if ctx.target == "amebaz2":
        lint_zap()                 # run_tests.sh just ran, so only the rest of `ota amebaz2 lint`
        lint_version("commit")
    elif ctx.target == "esp32":
        run(["bash", HERE / "esp32-lint.sh"])
    elif ctx.target == "esphome":
        ctx.esphome_run("config")


def build(ctx):
    if ctx.target == "amebaz2":
        run(["bash", HERE / "ota-release.sh", "build"])   # full clean, FWHS serial, verify: docs/10
    elif ctx.target == "esp32":
        env = esp_env()
        ensure_target(ESP, ctx.idf_tgt, env)
        run(["idf.py", "build"], env=env, cwd=ESP)
        say("dev build only. A shippable OTA goes through esp32-release.sh (delta base archive, #82).")
    elif ctx.target == "esphome":
        ctx.esphome_run("compile")


def erase(ctx):
    ctx.need_port("erase")
    if ctx.target == "amebaz2":
        die("AmebaZ2 has no erase step here: the clip flasher writes regions (see Installing-Custom-Firmware)")
    warn("erase-flash is for a BRAND-NEW board only: it wipes NVS, which on a working node holds the")
    warn("Matter fabric / Wi-Fi config. A factory board needs it (stale vendor NVS breaks commissioning).")
    try:
        confirm = input(f"type ERASE to erase {ctx.port}: ").strip()
    except EOFError:
        confirm = ""
    if confirm != "ERASE":
        die("not confirmed; nothing erased")
    if ctx.target == "esp32":
        run(["idf.py", "-p", ctx.port, "erase-flash"], env=esp_env(idf_only=True), cwd=ESP)
    elif ctx.target == "esphome":
        cmd = ctx.esphome_cmd()
        py = Path(os.path.realpath(subprocess.run(["bash", "-c", f"command -v {cmd}"],
                  stdout=subprocess.PIPE, text=True).stdout.strip() or cmd)).parent / "python"
        py = str(py) if py.is_file() and os.access(py, os.X_OK) else "python3"
        run([py, "-m", "esptool", "--port", ctx.port, "erase_flash"])


def flash(ctx):
    if ctx.target == "amebaz2":
        say("AmebaZ2 first install is a SOIC-8 clip write; a commissioned node takes OTA instead:")
        say("  clip: python3 firmware/flasher/ch341flash.py firmware/built-images/flash_rac-integrated-v<ver>.bin")
        say("  OTA:  firmware/scripts/ota-release.sh package && ota-release.sh stage && ota-release.sh flash")
        say("Read docs/guide/Installing-Custom-Firmware.md first: dump the chip before every clip write.")
    elif ctx.target == "esp32":
        ctx.need_port("flash")
        env = esp_env()
        ensure_target(ESP, ctx.idf_tgt, env)
        run(["idf.py", "-p", ctx.port, "flash", "monitor"], env=env, cwd=ESP)
    elif ctx.target == "esphome":
        ctx.need_port("flash")
        ctx.esphome_run("run", "--device", ctx.port)


def monitor(ctx):
    ctx.need_port("monitor")
    if ctx.target == "amebaz2":
        die("AmebaZ2 has no USB console; use the debug build's :2323 console or the UART pads")
    elif ctx.target == "esp32":
        run(["idf.py", "-p", ctx.port, "monitor"], env=esp_env(idf_only=True), cwd=ESP)
    elif ctx.target == "esphome":
        ctx.esphome_run("logs", "--device", ctx.port)


def bench(ctx):
    if ctx.target == "amebaz2":
        die("bench is for esp32/esphome; for AmebaZ2 run virtual_ac.py --port on the DI/RO tap")
    ctx.need_port("bench")
    if not ctx.sim_port:
        die("bench needs --sim-port <USB-TTL or USB-RS485 adapter>")
    if subprocess.run([sys.executable, "-c", "import serial"],
                      stderr=subprocess.DEVNULL).returncode != 0:
        die(f"virtual_ac.py needs pyserial for {sys.executable} (Debian/Ubuntu: sudo apt install "
            "python3-serial; Arch: sudo pacman -S python-pyserial)")
    say("bench wiring (no A/C, no mains):")
    say(f"  USB-TTL:    board TX GPIO{ctx.pins[0]} -> adapter RX, board RX GPIO{ctx.pins[1]} <- adapter TX, GND-GND (3.3 V adapter)")
    say("  USB-RS485:  transceiver A-A, B-B, GND-GND (board DI/RO/DE wired as for the A/C)")
    if not ask("wired like that?"):
        return
    proj = None
    if ctx.target == "esp32":
        proj = ESP / "smoketest"
        env = esp_env(idf_only=True)
        ensure_target(proj, ctx.idf_tgt, env)
        run(["idf.py", "-p", ctx.port, "build", "flash"], env=env, cwd=proj)
    else:
        ctx.esphome_run("run", "--no-logs", "--device", ctx.port)
    say(f"starting virtual_ac.py on {ctx.sim_port} (Ctrl-C stops both)")
    sim = subprocess.Popen([sys.executable, str(TEST / "virtual_ac.py"), "--port", ctx.sim_port])
    try:
        say("PASS looks like: sim prints '[0x0A] handshake poll -> echoed slave reply', then busmon logs")
        say("'A/C #N: power=1 mode=... set=24C indoor=25C' about once a second with RX climbing.")
        if ctx.target == "esp32":
            run(["idf.py", "-p", ctx.port, "monitor"], env=esp_env(idf_only=True), cwd=proj, check=False)
        else:
            ctx.esphome_run("logs", "--device", ctx.port)
    finally:
        sim.terminate()


def next_steps(ctx):
    print(f"""Staged bring-up for {ctx.target} (never leave the A/C in an unknown state):

  1. Bench, no A/C     dev.py test {ctx.target}; then dev.py bench {ctx.target} --port P --sim-port S
  2. Real bus, USB     module out, tap A/B ONLY, watch decoded status (read), then one control (write)
  3. Integration       power from the connector's 5 V, no laptop attached, close it up

Safety, before stage 2:
  ! Ground loop: while USB-powered connect ONLY A/B. Joining the mains-earthed A/C GND to a
    laptop-earthed board browns it out (RTCWDT resets, flash-read errors). GND/5V join at stage 3.
  ! 3.3 V transceiver only (MAX3485 / SP3485 / SN65HVD75). A 5 V MAX485 module's RO kills the RX pin.""")
    if ctx.target in ("esp32", "esphome"):
        print("""  ! ESP32-C3: fit a ~10k pulldown on DE (GPIO10). DE floats until gpio_init() and a high DE parks
    a second driver on the A/C bus. Never put the UART on GPIO18/19 (the C3's only USB).
  ! Classic ESP32: never GPIO16/17 on WROVER/D0WDQ6 (PSRAM-bonded, dead as I/O).
  ! Brand-new board: dev.py erase {t} --port P before the first flash (stale vendor NVS).
Full detail: firmware/esp32-matter/README.md, docs/guide/Build-Flash-Test.md""".replace("{t}", ctx.target))
    elif ctx.target == "amebaz2":
        print("""  ! Dump the whole chip (firmware/flasher/ch341dump.py) before every clip write; never flashrom.
  ! OTA: FWHS serial and version must bump (ota-release.sh build does it) or the update reverts.
Full detail: firmware/docs/10-firmware-ota-procedure.md, docs/guide/Installing-Custom-Firmware.md""")


def walk(ctx):
    say(f"guided flow for {ctx.target} (board: {ctx.board}). Each step asks first; N skips it.")
    if ask("1/5 doctor: check tools and SDK pins?"):
        if not doctor(ctx) and ask("gaps found. run fetch?"):
            fetch(ctx)
    if ask("2/5 host QA + lint?"):
        test_target(ctx)
    if ask("3/5 build?"):
        build(ctx)
    if ctx.target != "amebaz2":
        if not ctx.port:
            try:
                ctx.port = input("serial port for flashing (blank to skip flash): ").strip()
            except EOFError:
                ctx.port = ""
        if ctx.port:
            if ask("   brand-new board that needs erase-flash first?"):
                erase(ctx)
            if ask(f"4/5 flash + monitor {ctx.port}?"):
                flash(ctx)
    else:
        if ask("4/5 show the AmebaZ2 flash options?"):
            flash(ctx)
    say("5/5 next steps")
    next_steps(ctx)


# ---- release engine: the SDK-free AmebaZ2 steps, ported from ota-release.sh (#143) -------------
# Version source of truth is the git-tracked firmware/src/version.txt, a semver (#77). The Matter
# softwareVersion int is derived from it (MAJOR*10000 + MINOR*100 + PATCH): readable, strictly
# monotonic, and CI can gate it without the SDK. The OTA provider only serves a strictly greater
# int. Never hand-edit the int or the SDK header, edit version.txt and commit it.
VERSION_FILE = REPO / "firmware/src/version.txt"
RELEASED_MARK = REPO / "firmware/built-images/.released-version"   # int last CONFIRMED booted on-device
ZAP = REPO / "firmware/src/sdk-edits/room-air-conditioner-app.zap"


def semver_to_int(s):
    """'MAJOR.MINOR.PATCH' -> the Matter softwareVersion int."""
    s = "".join(str(s).split())
    if re.fullmatch(r"[0-9]+", s):   # legacy raw-int version.txt (pre-#77 branches / CI base)
        return int(s)
    m = re.fullmatch(r"([0-9]+)\.([0-9]+)\.([0-9]+)", s)
    if not m:
        die(f"version '{s}' is not semver MAJOR.MINOR.PATCH (issue #77)")
    major, minor, patch = (int(x) for x in m.groups())
    if minor >= 100 or patch >= 100:
        die(f"minor/patch must be < 100 for the *10000+*100 int mapping: '{s}'")
    return major * 10000 + minor * 100 + patch


def int_to_semver(v):
    return f"{v // 10000}.{(v // 100) % 100}.{v % 100}"


def cur_semver():
    if not VERSION_FILE.is_file():
        die(f"missing {VERSION_FILE}")
    return "".join(VERSION_FILE.read_text().split())


def cur_version():
    return semver_to_int(cur_semver())


def released_version():
    if not RELEASED_MARK.is_file():
        return 0
    raw = RELEASED_MARK.read_text().strip()
    if not re.fullmatch(r"[0-9]+", raw):
        die(f"{RELEASED_MARK} does not hold a softwareVersion int: '{raw}'")
    return int(raw)


def endpoints_contiguous(ids):
    """(sorted ids, ok). Endpoints must be exactly {0,1,2,...}: a gap boot-crashes AmebaZ2."""
    ids = sorted(ids)
    return ids, ids == list(range(len(ids)))


def lint_zap():
    """Checks the committed mirror .zap, so it runs in CI and the git hook without the SDK."""
    if not ZAP.is_file():
        die(f"mirror .zap not found: {ZAP}")
    ids, good = endpoints_contiguous(e["endpointId"] for e in json.loads(ZAP.read_text())["endpoints"])
    if not good:
        print(f"  .zap endpoints NOT contiguous: {ids} -- an endpoint gap boot-crashes AmebaZ2 (docs/10 §3)")
        die(".zap endpoint lint failed")
    print(f"  .zap endpoints contiguous: {ids}")


def lint_version(purpose):
    """Tree version vs the version last CONFIRMED booted on the device. `commit` (lint, the git
    hook) lets equal pass, so the tree that was just flashed can be committed (#136). `flash` needs
    strictly greater. ota_guards.py owns the rule. .released-version is one mark per repo, not per
    node: OTA_ALLOW_SAME_VERSION=1 lets flash roll the SAME version to another unit."""
    good, why = ota_guards.version_verdict(cur_version(), released_version(), purpose,
                                           os.environ.get("OTA_ALLOW_SAME_VERSION", "0") == "1")
    if not good:
        die(why)
    say(why)


def lint():
    say("lint: host codec/map tests")
    r = subprocess.run(["bash", str(TEST / "run_tests.sh")], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, errors="replace")
    if r.returncode != 0:
        print("\n".join(r.stdout.splitlines()[-20:]))
        die("host tests FAILED")
    say("  host tests passed")
    say("lint: .zap endpoint contiguity")
    lint_zap()
    say("lint: softwareVersion")
    lint_version("commit")
    say("lint OK")


def git_out(*args):
    """(returncode, stripped stdout) of `git -C REPO <args>`, stderr discarded."""
    r = subprocess.run(["git", "-C", str(REPO), *args], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, text=True)
    return r.returncode, r.stdout.strip()


# Every tracked path whose content can reach the AmebaZ2 image: the mirrored sources and
# version.txt, the sync list, the build script (it injects defines and SDK edits at build time), and
# the SDK setup (pins, patches, overlay edits). Markdown under firmware/src never compiles in, the
# same exclusion the CI version gate uses. ota-release.env feeds the image too (break-glass
# host/token) but is untracked by design.
# ota-release.sh is still the builder, so this list must stay equal to its IMAGE_INPUTS
# (test_image_epoch.sh compares the two). It names dev.py instead once `build` is ported.
IMAGE_INPUTS = [
    "firmware/src", ":(exclude)firmware/src/*.md",
    "firmware/scripts/ota-release.sh",
    "firmware/scripts/sync-files.sh",
    "firmware/setup.sh",
    "scripts/setup.sh",
    "scripts/apply-matter-edits.sh",
    "patches",
    "versions.env",
]


def image_epoch():
    """The build clock (#137): the AUTHOR date of the newest commit touching IMAGE_INPUTS, not the
    HEAD commit time. Author dates survive merge, rebase and cherry-pick, and git's default history
    simplification walks through a merge that did not change these paths, so a branch build and the
    tag build of its merge commit get the same epoch and therefore the same bytes. A merge that
    combines input changes from both sides is its own new source state and gets the merge's own
    date; a squash merge also rewrites the author date. Returns the epoch, notes go to stderr."""
    if git_out("rev-parse", "--git-dir")[0] == 0:
        # A depth-1 clone makes HEAD a graft root: every path looks touched by it, so HEAD's date
        # wins and the tag rebuild silently stops matching. Refuse rather than guess.
        if git_out("rev-parse", "--is-shallow-repository")[1] != "false":
            die("shallow clone: the build clock needs the history of the image inputs (#137). "
                "Run 'git fetch --unshallow', or set SOURCE_DATE_EPOCH explicitly")
        epoch = git_out("log", "-1", "--format=%at", "--", *IMAGE_INPUTS)[1]
        if not epoch:
            die("no commit touches the image inputs -- cannot derive SOURCE_DATE_EPOCH")
        commit = git_out("log", "-1", "--format=%h", "--", *IMAGE_INPUTS)[1]
        say(f"  build clock: author date of {commit}, the newest commit touching the image inputs",
            file=sys.stderr)
        if git_out("status", "--porcelain", "--", *IMAGE_INPUTS)[1]:
            say("  WARNING: uncommitted changes to the image inputs. This image will NOT match any "
                "rebuild of a future commit; commit first (including a --bump) if it is going to be "
                "flashed", file=sys.stderr)
        return int(epoch)
    # Not a git checkout (a source tarball): git archive and GitHub tarballs stamp every file with
    # the commit time, so the newest input mtime is still deterministic for one release.
    newest = None
    for p in IMAGE_INPUTS:
        root = REPO / p
        if p.startswith(":") or not root.exists():
            continue
        files = [root] if not root.is_dir() else (Path(d) / f for d, _, fs in os.walk(root) for f in fs)
        for f in files:
            st = f.lstat()
            if stat.S_ISREG(st.st_mode) and not f.name.endswith(".md"):
                newest = st.st_mtime_ns if newest is None else max(newest, st.st_mtime_ns)
    if newest is None:
        die("not a git checkout and no image inputs found -- set SOURCE_DATE_EPOCH")
    say("  build clock: not a git checkout, newest image-input mtime", file=sys.stderr)
    return newest // 10**9


def github_slug(url):
    """owner/repo from a github.com remote URL (ssh or https), '' for anything else."""
    m = re.search(r"github\.com[:/](.*)", url or "")
    if not m:
        return ""
    slug = m.group(1)
    if slug.endswith(".git"):
        slug = slug[:-4]
    return slug[:-1] if slug.endswith("/") else slug


def guard_runner(slug=""):
    """Release tags build ONLY on the self-hosted sdk-builder runner (it holds the SDK + OTA env).
    If it is offline a pushed tag sits in `queued` with no error, which is how 1.3.43 stalled
    (#138). Asked before the "push with" hint so the operator knows first. Warn, never refuse: the
    tag is local and harmless, the push is a separate manual step, and the job runs as soon as the
    runner comes up. A check that cannot run (no gh, not logged in, no admin scope, API error) says
    so and moves on."""
    label = os.environ.get("RUNNER_LABEL") or "sdk-builder"
    if not shutil.which("gh"):
        say(f"  WARNING: gh not on PATH -- runner check skipped; make sure a '{label}' runner is "
            "online before pushing")
        return
    if not slug:
        slug = github_slug(git_out("remote", "get-url", "origin")[1])
    if not slug:
        r = subprocess.run(["gh", "repo", "view", "--json", "nameWithOwner", "-q", ".nameWithOwner"],
                           cwd=str(REPO), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        slug = r.stdout.strip() if r.returncode == 0 else ""
    if not slug:
        say("  WARNING: cannot tell which GitHub repo origin is -- runner check skipped")
        return
    # stderr kept apart: on an HTTP error gh prints the JSON body to stdout, the reason to stderr.
    r = subprocess.run(["gh", "api", f"repos/{slug}/actions/runners?per_page=100", "--jq",
                        '.runners[] | select(.status == "online") | .labels[].name'],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        reason = (r.stderr.strip().splitlines() or [""])[-1]
        say(f"  WARNING: runner check skipped (gh api repos/{slug}/actions/runners failed: {reason})")
        say(f"           make sure a '{label}' runner is online before pushing the tag")
        return
    if label in r.stdout.splitlines():
        say(f"  runner check: a '{label}' runner is online for {slug}")
        return
    say(f"  WARNING: no '{label}' runner is online for {slug}.")
    say("           The release workflow runs only there, so a pushed tag will sit in 'queued' (no error)")
    say("           until the runner is started. Start the self-hosted runner (run.sh in its install dir),")
    say(f"           then push. Check: gh api repos/{slug}/actions/runners --jq '.runners[] | {{name, status}}'")


def tag_release():
    """Create the path-prefixed semver tag amebaz2-vX.Y.Z locally (#77). Never pushed here."""
    semver = cur_semver()
    tag = f"amebaz2-v{semver}"
    if git_out("rev-parse", "-q", "--verify", f"refs/tags/{tag}")[0] == 0:
        say(f"tag {tag} already exists -- leaving it")
    else:
        run(["git", "-C", REPO, "tag", "-s", tag, "-m",
             f"AmebaZ2 firmware {semver} (softwareVersion {cur_version()})"])
        say(f"tagged {tag} (softwareVersion {cur_version()})")
    guard_runner()
    say(f"push with: git push origin {tag}")


# ---- ota: preflight/verify here; unported steps forward to the release scripts ---------------
def _guards_bash(target, snippet):
    """Run a snippet with ota-release.env sourced and ota-guards.sh available (its say/die/pi_now/
    guard_* helpers). The guard logic stays in bash on purpose -- that is the engine. ota-guards.sh
    reads HERE/REPO/ESP and calls say/die, so define them exactly as the release scripts do."""
    script = (f'HERE="{HERE}"; REPO="{REPO}"; ESP="{ESP}"; '
              f'set -a; . "{ENVF}"; set +a; '
              f'''say() {{ printf '\\033[1;36m[dev]\\033[0m %s\\n' "$*"; }}; '''
              f'''die() {{ printf '\\033[1;31m[dev] ERROR:\\033[0m %s\\n' "$*" >&2; exit 1; }}; '''
              f'. "{HERE}/ota-guards.sh"; {snippet}')
    return subprocess.run(["bash", "-c", script]).returncode


def ota(ctx, step, rest):
    rel = HERE / ("ota-release.sh" if ctx.target == "amebaz2" else "esp32-release.sh")
    if ctx.target == "esphome":
        die("ota is Matter-only (amebaz2 or esp32); ESPHome updates go through esphome run")
    node_var = "ESP32_NODE_ID" if ctx.target == "esp32" else "NODE_ID"
    if ctx.target == "amebaz2" and step in ("lint", "verint", "epoch", "tag"):
        if step == "lint":
            lint()
        elif step == "verint":   # an empty argument falls back to version.txt, as `${1:-...}` did
            print(semver_to_int(rest[0] if rest and rest[0] else cur_semver()))
        elif step == "epoch":
            print(image_epoch())
        else:
            tag_release()
    elif step == "preflight":
        run(["bash", TEST / "run_tests.sh"], check=False)
        ok("host QA incl. OTA guard tests")
        rc = _guards_bash(ctx.target,
                          f'node="${{{node_var}:?}}"; guard_tools; guard_link "$node"; '
                          'say "Pi clock reachable: $(pi_now)"')
        if rc != 0:
            die("preflight failed (see above)")
    elif step == "verify":
        if ctx.target == "esp32":
            attr = "0/40/10"
            m = re.search(r'^set\(PROJECT_VER "(.*)"\)', (ESP / "CMakeLists.txt").read_text(), re.M)
            want = m.group(1) if m else ""
        else:
            attr = "0/40/9"
            want = (REPO / "firmware/src/version.txt").read_text().strip()
        read_ver = (
            'import asyncio,json,sys,aiohttp\n'
            'U,N,A=sys.argv[1],int(sys.argv[2]),sys.argv[3]\n'
            'async def m():\n'
            ' async with aiohttp.ClientSession() as s:\n'
            '  async with s.ws_connect(U) as ws:\n'
            '   await ws.receive(timeout=10)\n'
            '   await ws.send_json({"message_id":"v","command":"read_attribute","args":{"node_id":N,"attribute_path":A}})\n'
            '   while True:\n'
            '    d=json.loads((await ws.receive(timeout=40)).data)\n'
            '    if d.get("message_id")=="v":\n'
            '     r=d.get("result"); print(r.get(A) if isinstance(r,dict) else r); return\n'
            'asyncio.run(m())\n')
        # verint gives the same value in the ameba int form; accept either the semver or the int.
        if ctx.target == "amebaz2":
            verint = str(cur_version())
        else:
            verint = subprocess.run(["bash", str(rel), "verint"], stdout=subprocess.PIPE,
                                    stderr=subprocess.DEVNULL, text=True).stdout.strip()
        snippet = (
            f'node="${{{node_var}:?}}"; '
            f'got="$("$OTAENV_PY" -c \'{read_ver}\' "$MS_WS" "$node" "{attr}")"; '
            f'rssi="$("$OTAENV_PY" "{HERE}/ota_guards.py" link "$MS_WS" "$node" -200 2>/dev/null || true)"; '
            f'say "node $node live version: $got   (tree: {want})"; say "link: $rssi"; '
            f'if [ "$got" = "{want}" ] || [ "$got" = "{verint}" ]; then '
            f'printf \'  \\033[32mok\\033[0m    node %s is running %s\\n\' "$node" "$got"; '
            f'else echo "node $node reports $got, expected {want}" >&2; exit 1; fi')
        if _guards_bash(ctx.target, snippet) != 0:
            die(f"node reports an unexpected version (expected {want})")
    elif step in ("package", "stage", "flash", "release", "build", "publish", "tag", "verint", "revert"):
        if step == "revert" and ctx.target != "amebaz2":
            die("revert is AmebaZ2-only (back to the stock firmware, #19)")
        rc = subprocess.run(["bash", str(rel), step, *rest]).returncode
        sys.exit(rc)
    else:
        die("ota step must be lint, verint, epoch, tag, preflight, verify, build, package, stage, "
            "flash, release, publish or revert (lint and epoch are amebaz2-only)")


# ---- argument parsing + dispatch -------------------------------------------------------------
def usage(code):
    print(__doc__.strip())
    sys.exit(code)


def use_esp_python():
    """Put one Python first on PATH for the ESP-IDF and esp-matter environments (ported from dev.sh, #121).

    ESP-IDF picks its Python env from the first python3 on PATH, and esp-matter's pigweed venv is
    built from it too, so both must come from one interpreter. esp-matter's CI uses 3.12. On a
    3.14 host ESP-IDF otherwise picks a py3.14 env that lacks esp-matter's codegen modules (seen:
    `import lark` fails) while the pigweed venv is 3.12. Uses ESP_PYTHON when set, else a
    uv-managed 3.12 on a 3.14+ host, and changes nothing otherwise.
    """
    py = os.environ.get("ESP_PYTHON", "")
    if not py:
        host = py_minor("python3")
        if not host or tuple(int(x) for x in host.split(".")) < (3, 14):
            return
        found = subprocess.run(["uv", "python", "find", "3.12"], stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True) if have("uv") else None
        py = found.stdout.strip() if found and found.returncode == 0 else ""
        if not py:
            warn(f"host python3 is {host}; ESP-IDF and esp-matter need one interpreter esp-matter "
                 "supports: uv python install 3.12, or set ESP_PYTHON=/path/to/python3.12")
            return
    if not (os.path.isfile(py) and os.access(py, os.X_OK)):
        die(f"ESP_PYTHON={py} is not an executable file")
    shim = tempfile.mkdtemp(prefix="dev-esp-python.")
    atexit.register(shutil.rmtree, shim, True)
    for name in ("python3", "python"):
        os.symlink(py, os.path.join(shim, name))
    os.environ["PATH"] = f"{shim}{os.pathsep}{os.environ.get('PATH', '')}"
    say(f"ESP python: {py_minor(py) or '?'} ({py})")


def main(argv):
    if not argv:
        usage(1)
    if argv[0] in ("-h", "--help", "help"):
        usage(0)
    cmd = argv[0]
    target = argv[1] if len(argv) > 1 else ""
    if target not in ("amebaz2", "esp32", "esphome"):
        die(f"target must be amebaz2, esp32 or esphome (got '{target}')")
    rest = argv[2:]
    if target == "esp32":
        use_esp_python()

    # `ota` forwards everything after the step to the release script, so parse it before the
    # option loop (release flags like --flash are not dev.py options).
    if cmd == "ota":
        step = rest[0] if rest else ""
        ota(Ctx(target, "c3", None, None), step, rest[1:])
        return

    board, port, sim_port, i = "c3", None, None, 0
    while i < len(rest):
        a = rest[i]
        if a == "--board" and i + 1 < len(rest):
            board = rest[i + 1]; i += 2
        elif a == "--port" and i + 1 < len(rest):
            port = rest[i + 1]; i += 2
        elif a == "--sim-port" and i + 1 < len(rest):
            sim_port = rest[i + 1]; i += 2
        else:
            die(f"unknown option: {a}")
    ctx = Ctx(target, board, port, sim_port)

    dispatch = {"walk": walk, "doctor": doctor, "fetch": fetch, "test": test_target,
                "build": build, "erase": erase, "flash": flash, "monitor": monitor,
                "bench": bench, "next": next_steps}
    fn = dispatch.get(cmd)
    if not fn:
        usage(1)
    result = fn(ctx)
    # doctor returns False on gaps -> non-zero exit.
    if cmd == "doctor" and result is False:
        sys.exit(1)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except Die as e:
        print(f"{C['red']}[dev] ERROR:{C['off']} {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
