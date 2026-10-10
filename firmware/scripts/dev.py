#!/usr/bin/env python3
"""dev.py -- one portable entry point for the from-source build / flash / test / release flow
(issues #118, #143).

It is becoming the release engine too (#143). The AmebaZ2 steps that need no SDK and no env file
and every other AmebaZ2 step (build, package, stage, flash, release, publish, revert) are
implemented here. Their decisions are plain functions with host tests
(firmware/test/test_dev_release.py, test_image_epoch.sh); ota_guards.py keeps the guard verdicts
both targets share. The ESP32 release steps (delta OTA against the archived deployed base, #82)
are implemented here as well. It replaced ota-release.sh, esp32-release.sh and ota-guards.sh.
The helpers dev.py calls (run_tests.sh, esp32-lint.sh,
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
  build [--bump[-patch|-minor|-major]] [--debug]   sync mirror -> SDK, FULL clean, build, verify
  package                                pad clip image + .ota + manifest (HISENSE_FLAVOUR=debug: debug)
  release [--bump[-minor|-major]] [--tag] [--flash] [--debug]   build + package + stage (+ tag, flash)
  stage | flash | publish               Pi staging | OTA + verify | upload the deployed files
  revert --backup <ip> [out.bin] | --flip <ip> [--force] | --slots <ip> |
         --repackage <stock-dump.bin> | --apply [--ip <ip>] [--yes]   back to stock firmware (#19)
OTA steps, `dev.py ota esp32 <step>` (the IDF + esp-matter env is sourced when idf.py is not on PATH):
  build                                  refuse unless the deployed base is archived, build, archive
  package [--full]                       delta patch vs the archived base -> .ota + manifest
  stage | flash | tag | publish | verint | release [--flash] | preflight | verify
ESP32 env switches: ESP32_FLAVOUR=release|debug (default debug), ESP32_TARGET, ESP32_NODE_FLAVOUR,
ESP32_ALLOW_IDF_MISMATCH=1, ESP32_ALLOW_NO_RECOVERY=1.

Targets: amebaz2 | esp32 | esphome. Board (esp32/esphome): --board c3 (ESP32-C3 SuperMini, default)
or --board classic (ESP32-D0WDQ6). esphome also takes --board amebaz2-factory, amebaz2-sdk or
amebaz2-native (the stock module through LibreTiny, by the flash layout the unit has: test and build
only, hardware test pending). ESPHome node identity: --name <hostname> and --friendly-name <text>
(default hisense-ac / "Air Conditioner"; give every node after the first its own). Env: IDF_PATH / ESP_MATTER_PATH (esp32; default ~/esp/esp-idf and
~/esp/esp-matter), ESPHOME (esphome command, default `esphome`), ENVF (the release env file, default
firmware/scripts/ota-release.env; the self-hosted runner copies its secrets file to that exact path).
"""

import asyncio
import base64
import hashlib
import hmac
import json
import os
import re
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
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
ESPHOME_AMEBAZ2_BOARDS = ("amebaz2-factory", "amebaz2-sdk", "amebaz2-native")
ESPHOME_AMEBAZ2_ONLY = ("esphome --board amebaz2-* only has test and build: the image has not run on a module yet, and "
                        "a first flash is a clip or UART download-mode job (firmware/docs/15-esphome-path.md)")
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
    def __init__(self, target, board, port, sim_port, name=None, friendly_name=None):
        self.target = target
        self.board = board
        self.port = port
        self.sim_port = sim_port
        self.name = name
        self.friendly_name = friendly_name
        self._versions = None
        if board == "c3":
            self.idf_tgt, self.pins, self.esphome_board = "esp32c3", (5, 6, 10), "esp32-c3-devkitm-1"
        elif board == "classic":
            self.idf_tgt, self.pins, self.esphome_board = "esp32", (19, 18, 4), "esp32dev"
        elif board in ESPHOME_AMEBAZ2_BOARDS and target == "esphome":
            # The stock module's own MCU through LibreTiny. Its pins are fixed by the module and
            # live in the board file, so nothing is passed as a substitution.
            self.idf_tgt, self.pins, self.esphome_board = None, ("PA14", "PA13", "PA17"), "cr3l"
        elif board == "amebaz2" and target == "esphome":
            # No default: an image built for the wrong layout bricks the unit at its first update.
            die("--board amebaz2 needs the unit's flash layout: amebaz2-factory, amebaz2-sdk or "
                "amebaz2-native (firmware/docs/15-esphome-path.md, 'Which flash layout a unit has')")
        elif board.startswith("amebaz2"):
            die(f"--board {board} is esphome-only (the Matter build for the module is the amebaz2 target)")
        else:
            die("--board must be c3 or classic (esphome also takes amebaz2-factory, amebaz2-sdk, amebaz2-native)")

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
        config = "w41h1.yaml"
        subs = ["-s", "board", self.esphome_board, "-s", "tx_pin", str(self.pins[0]),
                "-s", "rx_pin", str(self.pins[1]), "-s", "de_pin", str(self.pins[2])]
        if self.board in ESPHOME_AMEBAZ2_BOARDS:
            if sub not in ("config", "compile"):
                die(ESPHOME_AMEBAZ2_ONLY)
            config, subs = f"w41h1-{self.board}.yaml", []
        # A second node needs its own hostname: two boards both called hisense-ac fight over the
        # same mDNS name and Home Assistant device.
        if self.name:
            subs += ["-s", "name", self.name]
        if self.friendly_name:
            subs += ["-s", "friendly_name", self.friendly_name]
        run([cmd, *subs, sub, config, *args], cwd=ESPHOME_DIR)


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
            if v.get("AMEBA_MATTER_PIN"):
                git_head_is(f"{sdk}/ameba-rtos-z2/component/common/application/matter",
                            v["AMEBA_MATTER_PIN"], "ameba-rtos-matter", gaps)
            # scripts/setup.sh (our patches, the Matter-overlay edits) is the second half of fetch.
            # Checkouts at the right pins without it build an unpatched SDK, which fails in codegen
            # with "Unhandled server cluster: HISENSE_AIRCON_CLUSTER". Only patches/connectedhomeip.patch
            # registers that cluster in zap_cluster_list.json; the cluster XML and ClusterId.h are no
            # use as a marker, because `ota amebaz2 build` copies those too.
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
            # Not recursing: an on-demand submodule fetch dies on refs the submodule remotes no
            # longer serve ("not our ref"), and the submodule update below fetches what the tag needs.
            run(["git", "-C", idf_path, "fetch", "--tags", "--no-recurse-submodules", "origin"])
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
        build_amebaz2([])   # full clean, FWHS serial, verify: docs/10
    elif ctx.target == "esp32":
        env = esp_env()
        ensure_target(ESP, ctx.idf_tgt, env)
        run(["idf.py", "build"], env=env, cwd=ESP)
        say("dev build only. A shippable OTA goes through `dev.py ota esp32 build` (delta base archive, #82).")
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
        say("  OTA:  dev.py ota amebaz2 package, then stage, then flash")
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
  ! OTA: FWHS serial and version must bump (dev.py ota amebaz2 build does it) or the update reverts.
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


def lint_version(purpose, allow_same=None):
    """Tree version vs the version last CONFIRMED booted on the device. `commit` (lint, the git
    hook) lets equal pass, so the tree that was just flashed can be committed (#136). `flash` needs
    strictly greater. ota_guards.py owns the rule. .released-version is one mark per repo, not per
    node: OTA_ALLOW_SAME_VERSION=1 lets flash roll the SAME version to another unit."""
    if allow_same is None:
        allow_same = os.environ.get("OTA_ALLOW_SAME_VERSION", "0") == "1"
    good, why = ota_guards.version_verdict(cur_version(), released_version(), purpose, allow_same)
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
# version.txt, the sync list, this script (it injects defines and SDK edits at build time), and
# the SDK setup (pins, patches, overlay edits). Markdown under firmware/src never compiles in, the
# same exclusion the CI version gate uses. ota-release.env feeds the image too (break-glass
# host/token) but is untracked by design.
IMAGE_INPUTS = [
    "firmware/src", ":(exclude)firmware/src/*.md",
    "firmware/scripts/dev.py",
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


def make_tag(tag, message, done):
    """Create a signed, path-prefixed semver tag locally (#77). Never pushed here."""
    if git_out("rev-parse", "-q", "--verify", f"refs/tags/{tag}")[0] == 0:
        say(f"tag {tag} already exists -- leaving it")
    else:
        run(["git", "-C", REPO, "tag", "-s", tag, "-m", message])
        say(done)
    guard_runner()
    say(f"push with: git push origin {tag}")


def tag_release():
    semver, v = cur_semver(), cur_version()
    make_tag(f"amebaz2-v{semver}", f"AmebaZ2 firmware {semver} (softwareVersion {v})",
             f"tagged amebaz2-v{semver} (softwareVersion {v})")


# ---- release engine: AmebaZ2 build + package, ported from ota-release.sh (#143) ----------------
def _env0(args, env=None, cwd=None):
    """(returncode, env dict) from a command whose stdout is `env -0`."""
    p = subprocess.run(args, stdout=subprocess.PIPE, env=env, cwd=str(cwd) if cwd else None)
    out = {}
    for chunk in p.stdout.split(b"\0"):
        if b"=" in chunk:
            k, v = chunk.split(b"=", 1)
            out[k.decode("utf-8", "replace")] = v.decode("utf-8", "replace")
    return p.returncode, out


BUILD_ENV_KEYS = ("SDK_ROOT", "GCC_RELEASE", "CHIP_CONFIG_H", "EXAMPLE_DIR", "OTA_TOOL", "VID", "PID")


def load_env(required=BUILD_ENV_KEYS):
    """The release settings: this process's environment with ota-release.env sourced over it (the
    file is bash, with $HOME and cross-references). Returned as a lookup dict only. It is NOT handed
    to child processes: the script never exported the file's variables either, so make and the SDK
    tools see the caller's environment plus the few variables build() exports by name."""
    if not ENVF.is_file():
        die(f"missing {ENVF} -- copy ota-release.env.example and fill it in")
    rc, cfg = _env0(["bash", "-c", 'set -a; . "$1" >/dev/null || exit 97; set +a; env -0', "bash", str(ENVF)])
    if rc != 0:
        die(f"could not source {ENVF}")
    for k in required:
        if not cfg.get(k):
            die(f"{k} is not set (in {ENVF})")
    return cfg


def sync_file_lists():
    """(required, optional) repo-relative paths from sync-files.sh, the single definition of what
    is copied into the SDK example dir. scripts/setup.sh sources the same file, so the two cannot
    drift (matter_aircon_map.h once went missing from one of them)."""
    text = (HERE / "sync-files.sh").read_text()
    out = []
    for name in ("SYNC_FILES_REQUIRED", "SYNC_FILES_OPTIONAL"):
        m = re.search(rf"^{name}=\(([^)]*)\)", text, re.M)
        if not m:
            die(f"{name} not found in sync-files.sh")
        out.append([ln.split("#")[0].strip() for ln in m.group(1).splitlines() if ln.split("#")[0].strip()])
    return out[0], out[1]


def sed_first(text, old, new):
    """Literal `sed 's/old/new/'`: the first occurrence on every line, nothing else."""
    return "\n".join(ln.replace(old, new, 1) for ln in text.split("\n"))


# The SDK edits build() applies. Each is a pure text transform returning the new text (unchanged
# when already applied), so a full clean or an SDK reinstall heals itself and the host tests can
# hold every one against a golden. The reasons live with the callers in apply_sdk_edits().
def edit_ota_hardening(hdr, block):
    return hdr if "HISENSE_OTA_HARDENING" in hdr else hdr + block


def edit_example_task_stack(text):
    return sed_first(text, 'example_matter_room_air_conditioner_task"), 2048',
                     'example_matter_room_air_conditioner_task"), 8192')


def edit_downlink_stack(text):
    return sed_first(text, 'xTaskCreate(DownlinkTask, "Downlink", 1024', 'xTaskCreate(DownlinkTask, "Downlink", 4096')


def edit_mode_select_span_guard(text):
    if "mSpan.data() == nullptr" in text:
        return text
    probe = "        if (endpointSpanPair.mEndpointId == endpointId)"
    guard = ("        if (endpointSpanPair.mSpan.data() == nullptr) { continue; }"
             "  // orphaned endpoint type pads this array\n")
    return sed_first(text, probe, guard + probe)


def edit_build_info_determinism(text):
    if "SOURCE_DATE_EPOCH" in text:
        return text
    text = text.replace("`date +", "`date -u -d @$${SOURCE_DATE_EPOCH:-0} +")
    text = text.replace("`id -u -n`", "builder")
    return text.replace("`$(HOSTNAME_APP)`", "")


def edit_build_info_order(text):
    return "\n".join("prerequirement: build_info" if ln == "prerequirement:" else ln
                     for ln in text.split("\n"))


PREFIX_MAP = "-ffile-prefix-map=$(HOME)=/build"   # $(HOME) stays literal: make expands it


def edit_prefix_map(text):
    if "ffile-prefix-map" in text:
        return text
    head = "CHIP_CXXFLAGS += $(INCLUDES)"
    add = f"\nCHIP_CFLAGS += {PREFIX_MAP}\nCHIP_CXXFLAGS += {PREFIX_MAP}"
    return "\n".join(head + add + ln[len(head):] if ln.startswith(head) else ln for ln in text.split("\n"))


def edit_ccache_launcher(text):
    if "pw_command_launcher" in text:
        return text
    probe = 'echo ameba_cpu = \\"ameba\\" >> $(OUTPUT_DIR)/args.gn && \\'
    return sed_first(text, probe, probe + '\n\techo pw_command_launcher = \\"ccache\\" >> $(OUTPUT_DIR)/args.gn && \\')


def _read_raw(path):
    """SDK text with its line endings untouched (parts of the Realtek tree are CRLF)."""
    with open(path, newline="") as f:
        return f.read()


def _write_raw(path, text):
    with open(path, "w", newline="") as f:
        f.write(text)


# The sleep profiles of the ep6 ModeSelect, in firmware order: the mode value is the Hisense sleep
# profile number (matter_drivers.cpp writes CurrentMode = sleep_raw / 2). Same five labels the ESP32
# build creates in code, so one name set reaches Home Assistant from either Matter target.
SLEEP_MODE_LABELS = ("Off", "General", "Old", "Young", "Kids")


def edit_mode_select_sleep_profiles(text):
    """Replace the Realtek example's coffee options on endpoint 1 with our sleep profiles on ep6.

    The manager file is Realtek's and is not vendored, so a fresh or restored SDK brings back the
    stock table. With it, ep6 has no entry: SupportedModes reads back as an empty list and Home
    Assistant shows the sleep select as unavailable, although CurrentMode still works."""
    if '"General"' in text:
        return text
    body = "".join(f'    buildModeOptionStruct("{label}", {mode}, List<const SemanticTag>()),\n'
                   for mode, label in enumerate(SLEEP_MODE_LABELS))
    text, n = re.subn(r"(coffeeOptions\[\] = \{\n).*?(\};)", lambda m: m.group(1) + body.rstrip(",\n") + "\n" + m.group(2),
                      text, count=1, flags=re.S)
    if n != 1:
        return text
    return re.sub(r"EndpointSpanPair\(1, (.*?\))\s*// Options for Endpoint 1",
                  r"EndpointSpanPair(6, \1 // Sleep profiles on endpoint 6", text, count=1)


def _edit_file(path, fn, done, applied, marker=None, missing_ok=False):
    """Apply one transform in place. `marker` must be in the result, or the SDK changed under us."""
    path = Path(path)
    if not path.is_file():
        if missing_ok:
            return
        die(f"SDK file not found: {path}")
    old = _read_raw(path)
    new = fn(old)
    if marker and marker not in new:
        die(f"failed to apply an SDK edit to {path} (expected '{marker}' afterwards)")
    if new == old:
        if done:
            say(f"  {done}")
        return
    _write_raw(path, new)
    if applied:
        say(f"  {applied}")


def apply_sdk_edits(cfg):
    sdk, ex = cfg["SDK_ROOT"], cfg["EXAMPLE_DIR"]
    # #76: AmebaZ2 has no Kconfig, so CHIP uses the weak upstream MRP defaults (RETRANS=4, active
    # 300, idle 500) that drop the long BDX OTA transfer on marginal Wi-Fi. Append our overrides
    # (firmware/src/sdk-edits/chip-ameba-ota-hardening.h) to the Ameba CHIP platform config, which
    # CHIPConfig.h includes before ReliableMessageProtocolConfig.h applies its #ifndef defaults.
    block = REPO / "firmware/src/sdk-edits/chip-ameba-ota-hardening.h"
    if not block.is_file():
        die(f"OTA-hardening block not found: {block}")
    _edit_file(f"{sdk}/connectedhomeip/src/platform/Ameba/CHIPPlatformConfig.h",
               lambda t: edit_ota_hardening(t, block.read_text()),
               "MRP OTA-hardening already present in Ameba CHIPPlatformConfig.h (#76)",
               "injected MRP OTA-hardening into Ameba CHIPPlatformConfig.h (#76): RETRANS 4->8, "
               "active 300->500, idle 500->800ms", "HISENSE_OTA_HARDENING")
    # The SDK creates DownlinkTask with a 1024-WORD (4 KB) stack, sized for stock examples whose
    # handlers set one or two attributes. Ours writes dozens of ember attributes, drives the EPM
    # delegate and logs. An overflow kills the task silently: PostDownlinkEvent keeps "succeeding"
    # until the 10-slot queue fills, then every status-derived attribute sits frozen at its .zap
    # default while the bus and the diag console look healthy. Seen on node 14 (docs/10 §17).
    _edit_file(f"{sdk}/ameba-rtos-z2/component/common/application/matter/core/matter_interaction.cpp",
               edit_downlink_stack, "DownlinkTask stack already raised to 4096 words",
               "raised DownlinkTask stack 1024 -> 4096 words (our downlink handler is far heavier "
               "than the stock examples')", 'xTaskCreate(DownlinkTask, "Downlink", 4096')
    # The example init task gets 2048 WORDS (8 KB), copied from the stock light example. Ours does
    # nine UserLabel writes, an ember write, the EPM delegate + Instance init and the ModeSelect
    # manager on it. Overflowing kills the task part-way, so matter_interaction_start_downlink()
    # never runs and there is no downlink queue at all (docs/10 §17).
    _edit_file(f"{ex}/example_matter_room_air_conditioner.cpp", edit_example_task_stack,
               "example init task stack already raised to 8192 words",
               "raised example init task stack 2048 -> 8192 words (our init is far heavier than the "
               "stock example's)", 'example_matter_room_air_conditioner_task"), 8192')
    # ZAP sizes supportedOptionsByEndpoints[] by endpoint TYPES, not endpoints. Our .zap carries an
    # orphaned endpoint type with ModeSelect enabled, so the array has a zero-filled second entry
    # (endpoint 0, null Span) that getModeOptionsProvider iterates. Defence in depth: skip it.
    _edit_file(f"{sdk}/ameba-rtos-z2/component/common/application/matter/drivers/matter_drivers/"
               "mode_select/ameba_mode_select_manager.cpp", edit_mode_select_span_guard,
               "ModeSelect span guard already applied",
               "applied ModeSelect null-span guard (orphaned endpoint type inflates the generated count)",
               "mSpan.data() == nullptr")
    # The same file's option table: Realtek's coffee example on endpoint 1 -> our sleep profiles
    # on endpoint 6. Without it the ep6 SupportedModes list is empty.
    _edit_file(f"{sdk}/ameba-rtos-z2/component/common/application/matter/drivers/matter_drivers/"
               "mode_select/ameba_mode_select_manager.cpp", edit_mode_select_sleep_profiles,
               "ModeSelect sleep profiles already on endpoint 6",
               "replaced the stock ModeSelect options with the sleep profiles on endpoint 6",
               'EndpointSpanPair(6, ')
    gcc_rel = f"{sdk}/ameba-rtos-z2/project/realtek_amebaz2_v0_example/GCC-RELEASE"
    for mk in ("application.is.matter.mk", "application.is.mk"):
        # The SDK's build_info target regenerates .ver on EVERY build by shelling out to `date`, so
        # the image carries the wall-clock second the build started, and `id -u -n` leaks the
        # builder's username into a public image. Pin the clock to SOURCE_DATE_EPOCH and the
        # identity to a constant.
        _edit_file(f"{gcc_rel}/{mk}", edit_build_info_determinism,
                   f"build_info determinism already applied ({mk})",
                   f"pinned build_info clock+identity in {mk}", "SOURCE_DATE_EPOCH", missing_ok=True)
        # `all: build_info application_is ...` makes build_info.h a SIBLING of the object compiles,
        # so under make -j the first build of a fresh SDK fails with "build_info.h: No such file".
        # Every object rule waits on `| prerequirement`, so hang build_info off that.
        _edit_file(f"{gcc_rel}/{mk}", edit_build_info_order,
                   f"build_info ordering already applied ({mk})",
                   f"ordered build_info before the object compiles in {mk}",
                   "\nprerequirement: build_info", missing_ok=True)


def sync_mirror(cfg, flavour):
    """Copy the mirror (firmware/src) into the SDK example dir and write the generated headers."""
    say("sync mirror -> SDK example dir")
    ex = Path(cfg["EXAMPLE_DIR"])
    required, optional = sync_file_lists()
    for f in required:   # a missing required file hard-fails the build: a stale copy would ship
        shutil.copy(REPO / f, ex)
    # #22/#23 build flavour. Release is the DEFAULT: the debug header is generated only for
    # `build --debug` and removed otherwise, so the unauthenticated :2323 console cannot ship by
    # forgetting a flag. Only logging/console/diagnostics may differ between flavours.
    flav_h = ex / "hisense_flavour.h"
    if flavour == "debug":
        flav_h.write_text("// GENERATED by dev.py build --debug -- do not commit.\n"
                          "#define HISENSE_DEBUG_BUILD 1\n")
        say("  flavour: DEBUG (:2323 console compiled in -- bench only, do not deploy)")
    else:
        flav_h.unlink(missing_ok=True)
        say("  flavour: release (no diagnostic console)")
    # #78 break-glass OTA target. Generated, never committed: the repo is public, so the real
    # server address lives in ota-release.env. Without OTA_HTTP_HOST the header is omitted and
    # matter_drivers.cpp keeps its inert placeholder.
    ota_h = ex / "hisense_ota_config.h"
    host = cfg.get("OTA_HTTP_HOST", "")
    if host:
        port = cfg.get("OTA_HTTP_PORT") or "8070"
        res = cfg.get("OTA_HTTP_RESOURCE") or "/rac-ota.bin"
        lines = ["// GENERATED by dev.py from ota-release.env -- do not commit.",
                 f'#define HISENSE_OTA_HOST     "{host}"',
                 f"#define HISENSE_OTA_PORT     {port}",
                 f'#define HISENSE_OTA_RESOURCE "{res}"']
        # #61: break-glass TRIGGER token. Ships in both flavours, so it is authenticated and fails
        # closed: no token means the listener is never opened. There is deliberately no default.
        token = cfg.get("BREAKGLASS_TOKEN", "")
        bg_port = cfg.get("BREAKGLASS_PORT") or "2324"
        if token:
            lines += [f'#define HISENSE_BREAKGLASS_TOKEN "{token}"',
                      f"#define HISENSE_BREAKGLASS_PORT  {bg_port}"]
        ota_h.write_text("\n".join(lines) + "\n")
        say(f"  break-glass OTA target: {host}:{port}{res}")
        if token:
            say(f"  break-glass trigger: listening on :{bg_port} (token set, both flavours)")
        else:
            say("  break-glass trigger: DISABLED (BREAKGLASS_TOKEN unset -- recovery needs a healthy Matter layer)")
    else:
        ota_h.unlink(missing_ok=True)
        say("  OTA_HTTP_HOST unset -- break-glass OTA keeps the inert placeholder host")
    for f in optional:
        if (REPO / f).is_file():
            shutil.copy(REPO / f, ex)
    # The mfg-cluster id header lives in connectedhomeip's zzz_generated tree, not the example dir.
    # scripts/setup.sh places it once, but a plain rebuild must re-sync it or a header change never
    # reaches the driver.
    cid = Path(cfg["SDK_ROOT"]) / "connectedhomeip/zzz_generated/app-common/clusters/HisenseAircon"
    cid.mkdir(parents=True, exist_ok=True)
    shutil.copy(REPO / "firmware/src/sdk-edits/HisenseAircon-ClusterId.h", cid / "ClusterId.h")
    # The cluster's ZCL definition is what the ZAP GUI reads for the available attributes.
    zcl = Path(cfg["SDK_ROOT"]) / "connectedhomeip/src/app/zap-templates/zcl/data-model/chip"
    if zcl.is_dir():
        shutil.copy(REPO / "firmware/src/sdk-edits/hisense-aircon-cluster.xml", zcl / "hisense-aircon-cluster.xml")


def header_with_version(text, vint, semver):
    """CHIPDeviceConfig.h with the softwareVersion int and string set (the header is derived)."""
    text = re.sub(r"(#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION )[0-9]+", rf"\g<1>{vint}", text)
    return re.sub(r'(DEVICE_SOFTWARE_VERSION_STRING ")[^"\n]*(")', rf"\g<1>{semver}\g<2>", text)


def set_header_version(cfg):
    h = cfg["CHIP_CONFIG_H"]
    _write_raw(h, header_with_version(_read_raw(h), cur_version(), cur_semver()))


def bumped(semver, level):
    m = re.fullmatch(r"([0-9]+)\.([0-9]+)\.([0-9]+)", semver)
    if not m:
        die(f"cannot bump non-semver version.txt '{semver}' (issue #77)")
    major, minor, patch = (int(x) for x in m.groups())
    if level == "major":
        return f"{major + 1}.0.0"
    if level == "minor":
        return f"{major}.{minor + 1}.0"
    return f"{major}.{minor}.{patch + 1}"


def bump_version(cfg, level):
    old = cur_semver()
    new = bumped(old, level)
    VERSION_FILE.write_text(new + "\n")
    set_header_version(cfg)
    say(f"version bumped {old} -> {new} (softwareVersion int {semver_to_int(new)}); "
        "firmware/src/version.txt + CHIPDeviceConfig.h -- commit version.txt")


def fwhs_serial(cfg):
    """The bootloader boots the signature-valid slot with the HIGHER FWHS serial. The Matter
    softwareVersion is irrelevant to it, so the serial must rise with every version or the OTA
    applies and then 'rolls back' (docs/10 §11)."""
    return int(cfg.get("SERIAL_BASE") or 1100) + cur_version()


DET_TIME_SHIM_C = """#include <time.h>
#include <stdlib.h>
time_t time(time_t *t) {
    const char *e = getenv("SOURCE_DATE_EPOCH");
    time_t v = e ? (time_t) strtoll(e, 0, 10) : 0;
    if (t) *t = v;
    return v;
}
"""


def det_time_shim():
    """Realtek's elf2bin.linux seeds srand(time(NULL)) and derives part of the image header from it,
    so packaging the SAME .axf twice gives two images. It is a closed prebuilt binary, so the only
    lever is the clock it seeds from: an LD_PRELOAD time() reading SOURCE_DATE_EPOCH. Built fresh
    into a private dir every run, since a predictable /tmp path would preload whatever sits there."""
    if not shutil.which("gcc"):
        die("host gcc needed to build the deterministic-clock shim")
    d = tempfile.mkdtemp(prefix="ota-det-time.")
    so = os.path.join(d, "shim.so")
    r = subprocess.run(["gcc", "-shared", "-fPIC", "-O2", "-x", "c", "-o", so, "-"],
                       input=DET_TIME_SHIM_C, text=True)
    if r.returncode != 0:
        shutil.rmtree(d, True)
        die("failed to build the deterministic-clock shim")
    return so


def chip_env(cfg, base, cwd):
    """`base` after sourcing connectedhomeip's activate.sh, checked for the one import the build
    needs. activate.sh can print "Error during activate" and still return 0: after a host Python
    upgrade the pigweed venv is dead and codegen dies on "No module named 'matter'" far from the
    cause (#121)."""
    act = f"{cfg['SDK_ROOT']}/connectedhomeip/scripts/activate.sh"
    rc, env = _env0(["bash", "-c", 'source "$1" >/dev/null 2>&1 || exit 97; env -0', "bash", act],
                    env=base, cwd=cwd)
    if rc != 0 or "PATH" not in env:
        die("activate.sh failed")
    if subprocess.run(["python3", "-c", "import matter.idl"], env=env,
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
        die("CHIP python env is broken (python3 cannot import matter.idl). Usual cause: the host "
            "python was upgraded under the pigweed venv. Rebuild it: cd $SDK_ROOT/connectedhomeip && "
            "rm -rf .environment && source scripts/bootstrap.sh (with a python3 the SDK supports "
            "first on PATH)")
    return env


def _make_tail(args, env, cwd, keep):
    """Run make with stdout+stderr captured, print the last `keep` lines, return the whole log."""
    print(f"{C['grey']}  $ {' '.join(args)}{C['off']}")
    r = subprocess.run(args, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, errors="replace")
    lines = r.stdout.splitlines()
    print("\n".join(lines[-(keep if r.returncode == 0 else 40):]))
    if r.returncode != 0:
        die(f"command failed ({r.returncode}): {' '.join(args)}")
    return r.stdout


def build_amebaz2(args):
    cfg = load_env()
    flavour = cfg.get("HISENSE_FLAVOUR") or "release"
    for a in args:
        # --debug selects the bench flavour (#22). Release is the default, so omitting the flag can
        # never ship the console.
        if a in ("--bump", "--bump-patch"):
            bump_version(cfg, "patch")
        elif a == "--bump-minor":
            bump_version(cfg, "minor")
        elif a == "--bump-major":
            bump_version(cfg, "major")
        elif a == "--debug":
            flavour = "debug"
        else:
            die(f"unknown flag for build: {a}")
    # Reproducible-build clock. The Realtek SDK bakes __DATE__/__TIME__ into the image, and the
    # image header carries hashes over that content, so a 5-byte timestamp becomes ~574 differing
    # bytes. GCC honours SOURCE_DATE_EPOCH for both macros: pin it to the image inputs' own date
    # (image_epoch), deterministic per source state and unchanged by a merge or cherry-pick that
    # leaves the inputs alone (#137). Override it only to reproduce an old image.
    epoch = cfg.get("SOURCE_DATE_EPOCH") or str(image_epoch())
    stamp = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(int(epoch)))
    say(f"SOURCE_DATE_EPOCH={epoch} ({stamp}) -- __DATE__/__TIME__ pinned to the image inputs")
    set_header_version(cfg)
    lint_zap()
    sync_mirror(cfg, flavour)
    apply_sdk_edits(cfg)
    # Path scrub: -ffile-prefix-map rewrites the absolute build path baked into __FILE__ and debug
    # info, so the image carries /build/... instead of the developer's $HOME. The GN CHIP core gets
    # it through CHIP_CFLAGS/CHIP_CXXFLAGS (injected here), the Ameba make app/main-lib through
    # CC/CXX below.
    mproj = f"{cfg['SDK_ROOT']}/ameba-rtos-z2/component/common/application/matter/project"
    for variant in ("amebaz2", "amebaz2plus"):
        _edit_file(f"{mproj}/{variant}/make/chip_core_sources.mk", edit_prefix_map, None, None,
                   "ffile-prefix-map", missing_ok=True)
    env = dict(os.environ, SOURCE_DATE_EPOCH=epoch)
    cc, cxx = f"CC=$(CROSS_COMPILE)gcc {PREFIX_MAP}", f"CXX=$(CROSS_COMPILE)g++ {PREFIX_MAP}"
    have_ccache = bool(shutil.which("ccache"))
    if have_ccache:
        # The GN core honours pw_command_launcher="ccache" (injected into args.gn through
        # chip_core_rules.mk, both variants: the build reads the non-"plus" one); make uses a CC
        # prefix. base_dir makes the hash stable across clean rebuilds, compiler_check=content
        # survives toolchain mtime noise. `time_macros` is deliberately NOT in the sloppiness list:
        # with it ccache replays a TU with a STALE __DATE__/__TIME__, which made two builds of one
        # commit match only sometimes.
        home = os.environ.get("HOME", "")
        env.update(CCACHE_DIR=cfg.get("CCACHE_DIR") or f"{home}/.ccache", CCACHE_BASEDIR=home,
                   CCACHE_COMPILERCHECK="content",
                   CCACHE_SLOPPINESS="include_file_mtime,include_file_ctime,pch_defines,locale,system_headers")
        subprocess.run(["ccache", "-M", cfg.get("CCACHE_MAXSIZE") or "25G"], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["ccache", "-z"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for variant in ("amebaz2", "amebaz2plus"):
            _edit_file(f"{mproj}/{variant}/make/chip_core_rules.mk", edit_ccache_launcher, None, None,
                       None, missing_ok=True)
        cc, cxx = f"CC=ccache $(CROSS_COMPILE)gcc {PREFIX_MAP}", f"CXX=ccache $(CROSS_COMPILE)g++ {PREFIX_MAP}"
        say(f"ccache ON (dir={env['CCACHE_DIR']}, base_dir={home}, check=content): GN core via "
            "pw_command_launcher, make via CC prefix")
    else:
        say("ccache not installed (sudo apt install ccache, or sudo pacman -S ccache) -- building "
            "without it (path-scrub still on via CC)")
    gcc_rel, ex = cfg["GCC_RELEASE"], cfg["EXAMPLE_DIR"]
    bsp = f"{cfg['SDK_ROOT']}/ameba-rtos-z2/component/soc/realtek/8710c/misc/bsp/lib/common/GCC"
    env = chip_env(cfg, env, gcc_rel)
    # MANDATORY full clean before EVERY build. The SDK's cache otherwise reuses a stale core
    # (libCHIP.a) + main lib: a "~77-second" fake build that ships an INCONSISTENT image (rolled
    # back on-device 3x). clean_matter_libs leaves the *copied* bsp libs and the gn out dir, so
    # remove those too (docs/10 §4).
    say("FULL CLEAN (mandatory -- defeats the stale-core cache)")
    _make_tail(["make", "clean_matter_libs"], env, gcc_rel, 1)
    _make_tail(["make", "clean_matter"], env, gcc_rel, 1)
    for lib in ("libCHIP.a", "lib_main.a"):
        Path(bsp, lib).unlink(missing_ok=True)
    shutil.rmtree(Path(ex) / "build/chip", ignore_errors=True)
    jobs = cfg.get("BUILD_JOBS") or str(len(os.sched_getaffinity(0)))
    shim = det_time_shim()
    try:
        env["DET_SHIM"] = shim
        say(f"BUILD (genuine recompile; ameba make -j{jobs}; verify by ninja [N/353] + fresh "
            "libCHIP.a, NOT wall-clock)")
        run(["make", "room_air_conditioner_port", f"-j{jobs}", cc, cxx], env=env, cwd=gcc_rel)
        want = fwhs_serial(cfg)
        fwjson = Path(gcc_rel) / "amebaz2_firmware_is.json"
        doc = json.loads(fwjson.read_text())
        doc["FWHS"]["header"]["serial"] = want
        fwjson.write_text(json.dumps(doc, indent=2))
        print(f"OTA FWHS serial set -> {want}")
        # LD_PRELOAD pins the clock elf2bin seeds its RNG from (see det_time_shim).
        log = _make_tail(["make", "is_matter", f"-j{jobs}", cc, cxx], dict(env, LD_PRELOAD=shim), gcc_rel, 2)
    finally:
        shutil.rmtree(os.path.dirname(shim), True)
    if have_ccache:
        stats = subprocess.run(["ccache", "-s"], env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True).stdout
        hits = re.search(r"Hits:[^(\n]*\(([^)]*)\)", stats)
        misses = re.search(r"Misses:[^(\n]*\(([^)]*)\)", stats)
        say(f"ccache: {hits.group(1) if hits else '?'} hits / {misses.group(1) if misses else '?'} misses")
    # The built image must carry the bumped serial (guard against a silent miss).
    if f"header-serial {want}" not in log:
        die(f"built image serial != {want} -- bootloader would roll back the OTA (docs/10 §11)")
    say(f"OTA image serial verified: {want} (> on-device -> bootloader will keep the new slot)")
    ec = Path(ex) / "build/chip/codegen/zap-generated/endpoint_config.h"
    arr = " ".join(m for ln in ec.read_text().splitlines() if "FIXED_ENDPOINT_ARRAY" in ln
                   for m in re.findall(r"\{[^}]*\}", ln))
    say(f"built: FIXED_ENDPOINT_ARRAY = {arr}")
    if "0x0000, 0x0001, 0x0002" not in arr:
        die("endpoints not contiguous in build output -- refusing (boot-crash risk)")
    say(f"build OK (v{cur_version()})")


def guard_functional_delta(glob, *paths):
    """Note (never refuse) when nothing that compiles into the image changed since the last tag:
    version-only releases are legitimate, but they should be deliberate."""
    rc, tag = git_out("describe", "--tags", "--abbrev=0", "--match", glob, "HEAD^")
    if rc != 0 or not tag:
        return
    n = len(git_out("log", "--oneline", f"{tag}..HEAD", "--", *paths)[1].splitlines())
    if n == 0:
        say(f"  NOTE: no commits under {' '.join(paths)} since {tag} -- this release is version-only")
    else:
        say(f"  {n} commit(s) touch the image since {tag}")


AMEBA_CONSOLE_MARK = b"diag console listening"   # the :2323 console's own log string
CLIP_SIZE = 4194304                               # the module's 4 MB SPI flash


def ota_manifest(ota_bytes, v, semver, otaurl, vid=0xFFF1, pid=0x8001, min_version=1):
    """The python-matter-server provider manifest for one .ota (key order is the wire order)."""
    return {"modelVersion": {
        "vid": vid, "pid": pid, "softwareVersion": v, "softwareVersionString": semver,
        "cdVersionNumber": 1, "firmwareInformation": "", "softwareVersionValid": True,
        "otaUrl": otaurl, "otaFileSize": len(ota_bytes),
        "otaChecksum": base64.b64encode(hashlib.sha256(ota_bytes).digest()).decode(),
        "otaChecksumType": 1, "minApplicableSoftwareVersion": min_version,
        "maxApplicableSoftwareVersion": v - 1, "releaseNotesUrl": ""}}


def package_amebaz2(flavour=None):
    cfg = load_env()
    flavour = flavour or cfg.get("HISENSE_FLAVOUR") or "release"
    v, semver = cur_version(), cur_semver()
    # Both flavours ship publicly, so their artifacts must be distinguishable. Same version int on
    # purpose (#77): the FLAVOUR lives in the filename, never in the version. A debug and a release
    # image at one version are DIFFERENT binaries, never each other's delta base or recovery image.
    sfx = "-debug" if flavour == "debug" else ""
    bindir = Path(cfg["GCC_RELEASE"]) / "application_is/Debug/bin"
    fw, flash_img = bindir / "firmware_is.bin", bindir / "flash_is.bin"
    out = REPO / "firmware/built-images"
    fwarch, clip = out / f"firmware_is-v{v}{sfx}.bin", out / f"flash_rac-integrated-v{v}{sfx}.bin"
    ota, manifest = out / f"rac-v{v}{sfx}.ota", out / f"rac-v{v}{sfx}.json"
    if not fw.is_file():
        die("no firmware_is.bin -- build first")
    # The flavour is claimed by the caller but the CONTENT is whatever build/ holds, so a `package`
    # after the wrong `build` would mislabel an image, and a debug image under a release name is
    # exactly the mistake #22 exists to prevent. Check the bytes for the console's own log string.
    fw_bytes = fw.read_bytes()
    if AMEBA_CONSOLE_MARK in fw_bytes:
        if flavour != "debug":
            die("built image CONTAINS the :2323 console but flavour is release -- rebuild without "
                "--debug, or package with HISENSE_FLAVOUR=debug")
    elif flavour == "debug":
        die("flavour is debug but the built image has NO console -- rebuild with 'build --debug' first")
    guard_functional_delta("amebaz2-v*", "firmware/src")
    # Clear this version's old outputs first: a package that dies half way must not leave an older
    # build's .ota/.json for stage to ship.
    out.mkdir(parents=True, exist_ok=True)
    for f in (ota, manifest, clip, fwarch):
        f.unlink(missing_ok=True)
    # otaUrl (#79): a LOCAL file:// by default (staged into --ota-provider-dir). With
    # OTA_RELEASE_BASE set it points at the GitHub release asset: python-matter-server downloads an
    # http(s):// otaUrl (checksum-verified) and re-serves it over BDX.
    otaurl = f"file:///rac-v{v}{sfx}.ota"
    if cfg.get("OTA_RELEASE_BASE"):
        otaurl = f"{cfg['OTA_RELEASE_BASE'].rstrip('/')}/amebaz2-v{semver}/rac-v{v}{sfx}.ota"
    say(f"package v{semver} (softwareVersion {v}, {flavour} flavour): raw image + clip image + .ota + manifest")
    flash_bytes = flash_img.read_bytes()
    clip.write_bytes(flash_bytes + b"\xff" * (CLIP_SIZE - len(flash_bytes)))
    r = subprocess.run(["python3", cfg["OTA_TOOL"], "create", "-v", cfg["VID"], "-p", cfg["PID"],
                        "-vn", str(v), "-vs", semver, "-da", "sha256", "-mi", "1", "-ma", str(v - 1),
                        str(fw), str(ota)], stdout=subprocess.DEVNULL)
    if r.returncode != 0:
        die("ota_image_tool.py create failed")
    # Archive the RAW firmware_is.bin too: publish uploads it as the byte-exact deployed payload
    # (it is what the break-glass HTTP OTA streams).
    fwarch.write_bytes(fw_bytes)
    manifest.write_text(json.dumps(ota_manifest(ota.read_bytes(), v, semver, otaurl)) + "\n")
    say(f"  raw:      {fwarch}  (byte-exact deployed payload, what publish uploads)")
    say(f"  clip:     {clip}")
    say(f"  ota:      {ota}  (+ .json manifest, otaUrl={otaurl})")


# ---- release engine: guards, Pi staging, OTA flash, publish (ported from ota-guards.sh and
# ---- ota-release.sh, #143). Target-neutral where both targets do the same thing. --------------
def need(cfg, *keys):
    for k in keys:
        if not cfg.get(k):
            die(f"{k} is not set (in {ENVF})")


def guard_tools(cfg, delta=False):
    """The flash helper venv and (ESP32 delta) detools. A missing venv used to surface as
    "rollback/boot crash", pointing at the device instead of this box."""
    py = cfg.get("OTAENV_PY") or die("set OTAENV_PY to the venv python that has aiohttp")
    if subprocess.run([py, "-c", "import aiohttp"], stderr=subprocess.DEVNULL).returncode != 0:
        die(f"OTAENV_PY ({py}) cannot import aiohttp -- python3 -m venv <dir> && <dir>/bin/pip install aiohttp")
    if delta:
        idf_py = cfg.get("IDF_PYTHON") or die("set IDF_PYTHON to the IDF python env")
        if subprocess.run([idf_py, "-c", "import detools"], stderr=subprocess.DEVNULL).returncode != 0:
            die(f"IDF_PYTHON ({idf_py}) cannot import detools -- {idf_py} -m pip install detools")


def read_link(cfg, node, min_rssi):
    """(ok, text) from the link guard CLI, run under OTAENV_PY (it needs aiohttp)."""
    r = subprocess.run([cfg["OTAENV_PY"], str(HERE / "ota_guards.py"), "link", cfg["MS_WS"], str(node),
                        str(min_rssi)], stdout=subprocess.PIPE, text=True)
    return r.returncode == 0, r.stdout.strip()


def wait_for_node(cfg, node, timeout_s=300, pause_s=10, sleep=time.sleep):
    """Block until the node answers a read through matter-server. `stage` restarts matter-server,
    which reopens its port long before it can serve a node: a flash started in that window failed
    its link check with "Server disconnected" (node 14, 1.3.45) or began update_node against a
    server that was not ready (node 80, 1.1.17). Returns the last link text, or dies."""
    waited, text = 0, ""
    while True:
        _, text = read_link(cfg, node, -200)   # any reading counts here, the guard judges it next
        if ota_guards.NO_READING not in text and "link read failed" not in text:
            if waited:
                say(f"  node {node} answered after ~{waited} s")
            return text
        if waited >= timeout_s:
            die(f"node {node} did not answer through matter-server within {timeout_s} s: {text}")
        if not waited:
            say(f"  waiting for node {node} to answer through matter-server (up to {timeout_s} s)")
        sleep(pause_s)
        waited += pause_s


def guard_link(cfg, node):
    """RSSI (0/54/4) and read latency through matter-server, before any update_node."""
    wait_for_node(cfg, node)
    good, out = read_link(cfg, node, cfg.get("OTA_MIN_RSSI") or "-70")
    if good:
        say(f"  link ok: {out}")
    elif ota_guards.NO_READING in out:
        # The override accepts a WEAK link. No reading at all is an unreachable node, and
        # update_node against it only burns the retries.
        die(f"link pre-flight failed for node {node}: {out}\n"
            "     OTA_ALLOW_WEAK_LINK=1 covers a weak signal, not a node that does not answer.")
    elif cfg.get("OTA_ALLOW_WEAK_LINK") == "1":
        say(f"  WARNING: OTA_ALLOW_WEAK_LINK=1 -- {out}")
    else:
        die(f"link pre-flight failed for node {node}: {out}\n"
            "     Move the node/AP closer or fix Wi-Fi first, or pass OTA_ALLOW_WEAK_LINK=1.")


def guard_fresh(source, *outputs):
    """Every staged file must be newer than the image it was derived from. Catches a half-failed
    package leaving an older build's .ota/.json behind."""
    src = os.path.getmtime(source)
    bad = ota_guards.stale_outputs(src, {str(o): (os.path.getmtime(o) if os.path.exists(o) else None)
                                         for o in outputs})
    if bad:
        die("refusing to stage stale outputs, re-run package:\n" + "\n".join(
            f"stale or missing (older than {os.path.basename(str(source))}): {b}" for b in bad))


def pi_ssh(cfg, command, capture=False, quiet=False):
    """(returncode, stdout) of one command on the Pi. BatchMode: never hang on a password prompt."""
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-i", cfg["PI_SSH_KEY"],
                        cfg["PI_HOST"], command],
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.DEVNULL if quiet else None, text=True)
    return r.returncode, (r.stdout or "") if capture else ""


def pi_scp(cfg, files, dest):
    return subprocess.run(["scp", "-o", "BatchMode=yes", "-i", cfg["PI_SSH_KEY"], *[str(f) for f in files],
                           f"{cfg['PI_HOST']}:{dest}"], stdout=subprocess.DEVNULL).returncode


def pi_now(cfg):
    """The Pi's clock (epoch string, '' when unreachable), so log filtering is immune to skew and TZ."""
    if not cfg.get("PI_HOST") or not cfg.get("PI_SSH_KEY"):
        return ""
    rc, out = pi_ssh(cfg, "date +%s", capture=True, quiet=True)
    return out.strip() if rc == 0 else ""


def pi_stage(cfg, manifest, *others):
    """Install files into the root-owned provider dir through a throwaway root container (the user
    is in the docker group, sudo needs a password), archive every other manifest for the same
    product, and restart matter-server (manifests are read once at init)."""
    files = [manifest, *others]
    keep = os.path.basename(str(manifest))
    host, ota_dir = cfg["PI_HOST"], cfg["PI_OTA_DIR"]
    tmp = f"/tmp/ota-stage.{os.getpid()}"
    if pi_ssh(cfg, f"mkdir -p {tmp}")[0] != 0:
        die(f"cannot reach {host}")
    if pi_scp(cfg, files, f"{tmp}/") != 0:
        die(f"scp to {host}:{tmp} failed")
    names = "".join(f" /src/{os.path.basename(str(f))}" for f in files)
    plan = []
    if cfg.get("OTA_KEEP_MANIFESTS") != "1":
        # listing = name<TAB>json for every manifest on the Pi, plus the local one being shipped
        _, listing = pi_ssh(cfg, f'cd {ota_dir} && for f in *.json; do [ -r "$f" ] || continue; '
                                 "printf '%s\\t' \"$f\"; tr -d '\\n' < \"$f\"; echo; done", capture=True)
        listing += f"{keep}\t{Path(manifest).read_text().replace(chr(10), '')}\n"
        try:
            plan = ota_guards.archive_plan(keep, ota_guards.parse_listing(listing))
        except ValueError:
            die("could not plan manifest archiving")
    mv = "".join(f" && mv /ota/{f} /ota/{f}.archived" for f in plan)
    rc, _ = pi_ssh(cfg, f"docker run --rm -v {ota_dir}:/ota -v {tmp}:/src alpine sh -c "
                        f"'install -m 0644 -o root -g root{names} /ota/ && rm -f /ota/chip_kvs_ota_provider_* "
                        f"/ota/ota_provider_*.log{mv}' && rm -rf {tmp} && docker restart matter-server >/dev/null")
    if rc != 0:
        die(f"root-container install on {host} failed")
    if plan:
        say(f"  archived {len(plan)} other manifest(s) for this product (renamed *.json.archived, reversible)")
    say("  staged via root container, provider junk pruned; waiting for matter-server to reload (~100 s)")
    rc, _ = pi_ssh(cfg, 'for i in $(seq 60); do bash -c "echo > /dev/tcp/127.0.0.1/5580" 2>/dev/null '
                        "&& exit 0; sleep 3; done; exit 1")
    if rc != 0:
        die("matter-server did not reopen :5580 within 3 min of the restart")
    say("  matter-server is serving again")


def pi_http_publish(cfg, image, current, archive, marker):
    """Persistent HTTP OTA mirror on the Pi. The break-glass path fetches a FULL image over plain
    HTTP from a compile-time URL, and losing the deployed image once already stranded node 80. So
    every stage copies the raw .bin into the user-owned docroot (PI_HTTP_DIR, served by the
    `ota-http` container): a per-version archive, plus the stable current-image path deployed nodes
    GET. That path is flavour-blind and shared by the whole fleet, so only a release image may
    repoint it: the flavour is read from the bytes, and a debug image is archived only."""
    http_dir = cfg.get("PI_HTTP_DIR")
    if not http_dir:
        say("  PI_HTTP_DIR unset -- skipping the HTTP OTA mirror")
        return
    image = Path(image)
    if not image.is_file():
        die(f"pi_http_publish: no {image}")
    repoint, why = ota_guards.repoint_verdict(ota_guards.image_flavour(image.read_bytes(), marker),
                                              cfg.get("OTA_HTTP_REPOINT_DEBUG") == "1")
    tmp = f"/tmp/ota-http.{os.getpid()}"
    if pi_ssh(cfg, f"mkdir -p {tmp} {http_dir}/archive")[0] != 0:
        die(f"cannot reach {cfg['PI_HOST']}")
    if pi_scp(cfg, [image], f"{tmp}/{archive}") != 0:
        die(f"scp of {archive} failed")
    install = f"install -m0644 {tmp}/{archive} {http_dir}/archive/{archive}"
    if repoint:
        install += f" && cp {http_dir}/archive/{archive} {http_dir}/{current}"
    if pi_ssh(cfg, f"{install} && rm -rf {tmp}")[0] != 0:
        die(f"HTTP OTA mirror install failed on {cfg['PI_HOST']}")
    say(f"  HTTP OTA mirror: archived {archive} (served by ota-http on the Pi); {why}")


def stage_amebaz2(flavour=None):
    cfg = load_env()
    need(cfg, "PI_HOST", "PI_OTA_DIR", "PI_SSH_KEY")
    flavour = flavour or cfg.get("HISENSE_FLAVOUR") or "release"
    v = cur_version()
    # Honour the flavour suffix package writes. Without it a debug build would silently deploy the
    # RELEASE image: both flavours share one version int by design (#77), so the flash verifies and
    # the only symptom is the missing console.
    sfx = "-debug" if flavour == "debug" else ""
    out = REPO / "firmware/built-images"
    src_ota, src_json = out / f"rac-v{v}{sfx}.ota", out / f"rac-v{v}{sfx}.json"
    fw = Path(cfg["GCC_RELEASE"]) / "application_is/Debug/bin/firmware_is.bin"
    if not src_ota.is_file():
        die(f"no {src_ota} -- run 'package' for this flavour first")
    say(f"stage v{v}{f' ({flavour})' if sfx else ''} on {cfg['PI_HOST']}:{cfg['PI_OTA_DIR']} + restart matter-server")
    guard_fresh(fw, src_ota, src_json)
    # Upload under the manifest's OWN names: its otaUrl already references rac-v<int><sfx>.ota.
    # pi_stage archives every other manifest for this pid, which covers the OTHER flavour at this
    # version int too: two candidates at one version is a coin toss for the provider.
    pi_stage(cfg, src_json, src_ota)
    # Mirror the RAW firmware_is.bin for the #78 break-glass path (rac-ota.bin is the compile-time
    # HTTP-OTA resource), which keeps the deployed image retrievable.
    pi_http_publish(cfg, fw, "rac-ota.bin", f"rac-v{v}{sfx}.bin", AMEBA_CONSOLE_MARK)


def check_subscription_log(cfg, node, since):
    """#64: after the flash gate confirmed the version AND a working subscription, look for the
    '(Re-)Subscription succeeded' line in the matter-server log when it is readable from here. The
    line is matched per node (a bare grep once 'confirmed' node 14 with node 35's line), ANSI
    stripped (matter-server colourises), both forms accepted (after an OTA the device reboots, so
    the healthy signal is usually the Re-Subscription), newest taken, and polled because it can
    land a few seconds late. Only lines after the flash started count (Pi clock)."""
    window = since or "15m"
    if not cfg.get("PI_HOST") or not cfg.get("PI_SSH_KEY"):
        say("  PI_HOST/PI_SSH_KEY unset -- cannot read the matter-server log; node availability stands "
            "as the subscription assertion (#64)")
        return
    line = ""
    for _ in range(6):
        rc, out = pi_ssh(cfg, f"docker logs --since {window} matter-server 2>&1 | "
                              r"sed -E 's/\x1b\[[0-9;]*m//g' | "
                              f"grep -E '<Node:{node}> (Re-)?Subscription succeeded' | tail -1 || true",
                         capture=True, quiet=True)
        if rc != 0:
            say(f"  could not read the matter-server log on {cfg['PI_HOST']} -- node availability stands "
                "as the subscription assertion (#64)")
            return
        line = out.strip()
        if line:
            break
        time.sleep(10)
    if line:
        say(f"  matter-server log confirms: {line[:120]}")
    else:
        # Availability after the re-interview already asserted the subscription. matter-server
        # sometimes RESUMES one without logging a fresh line, so a missing line is not a break:
        # warn, never die, since a false die aborts a healthy flash mid-run.
        say(f"  no fresh '(Re-)Subscription succeeded' for node {node} since {since or '15m ago'} -- "
            "availability after re-interview already asserted the subscription (#64); matter-server "
            "likely resumed it without a new line. OK.")


def ws_program(cfg, name, *args, capture=False):
    """Run one of the matter-server websocket programs below under OTAENV_PY (the venv that has
    aiohttp; dev.py itself runs on any python3). Returns (returncode, stdout)."""
    r = subprocess.run([cfg["OTAENV_PY"], str(Path(__file__).resolve()), "_ws", name, *[str(a) for a in args]],
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.DEVNULL if capture else None, text=True)
    return r.returncode, (r.stdout or "").strip() if capture else ""


def flash_amebaz2():
    cfg = load_env()
    need(cfg, "OTAENV_PY", "MS_WS", "NODE_ID")
    node, v = cfg["NODE_ID"], cur_version()
    # flash is what records .released-version (only after the new version verifies), so it is the
    # one place an unbumped version must still be refused. A retry after a failed flash is
    # unaffected: the mark only moves on success.
    say(f"pre-flight: version, tools + link to node {node}")
    lint_version("flash", cfg.get("OTA_ALLOW_SAME_VERSION") == "1")
    guard_tools(cfg)
    guard_link(cfg, node)
    since = pi_now(cfg)
    say(f"flash v{v} to node {node} (retries; then verify the reported version changed)")
    if ws_program(cfg, "flash", cfg["MS_WS"], node, v, "amebaz2")[0] != 0:
        die(f"flash verification failed for v{v} -- version not sustained (rollback/boot crash, docs/10 "
            "§7,§11) or the subscription gate failed (#64, docs/10 §16); see the [flash] lines above")
    RELEASED_MARK.parent.mkdir(parents=True, exist_ok=True)
    RELEASED_MARK.write_text(f"{v}\n")
    say(f"recorded on-device version {v}")
    check_subscription_log(cfg, node, since)


def gh_release_upload(tag, files):
    """Upload the files that exist to the release, clobbering. Returns how many went up."""
    n = 0
    for f in files:
        if Path(f).is_file() and subprocess.run(["gh", "release", "upload", tag, str(f), "--clobber"],
                                                 stdout=subprocess.DEVNULL).returncode == 0:
            say(f"  uploaded {Path(f).name}")
            n += 1
    return n


def publish_amebaz2():
    """Upload the DEPLOYED artifacts to the GitHub release (#89): the bytes a device booted, under
    the canonical names. Only ever what THIS box confirmed booted: flash writes the marker after
    the device sustained the new version across three fresh reads."""
    if not shutil.which("gh"):
        die("gh not on PATH -- needed to upload release assets")
    v = released_version()
    if v == 0:
        die("no on-device version recorded -- run 'flash' first")
    tag = f"amebaz2-v{int_to_semver(v)}"
    if subprocess.run(["gh", "release", "view", tag], cwd=str(REPO), stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode != 0:
        die(f"no release {tag} -- push the tag first ('dev.py ota amebaz2 tag')")
    # firmware_is-v<N>*.bin FIRST: the raw image is what the break-glass HTTP OTA streams, so the
    # archived copy is the byte-exact deployed payload. The clip/.ota are derived and are published
    # only when they exist, never regenerated.
    out = REPO / "firmware/built-images"
    n = gh_release_upload(tag, [out / f for f in (
        f"firmware_is-v{v}.bin", f"firmware_is-v{v}-debug.bin",
        f"flash_rac-integrated-v{v}.bin", f"rac-v{v}.ota", f"rac-v{v}.json",
        f"flash_rac-integrated-v{v}-debug.bin", f"rac-v{v}-debug.ota", f"rac-v{v}-debug.json")])
    if n == 0:
        die(f"no artifacts for v{v} in built-images/ -- build + package first")
    say(f"published {n} deployed artifact(s) to {tag} (on-device version {v})")


# ---- matter-server websocket programs. They need aiohttp, so dev.py re-runs itself under
# ---- OTAENV_PY as `dev.py _ws <name> ...` (see ws_program). Never import aiohttp at module level.
async def _ws_call(ws, command, args, mid, timeout=600):
    await ws.send_json({"message_id": mid, "command": command, "args": args})
    while True:
        d = json.loads((await ws.receive(timeout=timeout)).data)
        if d.get("message_id") == mid:
            return d


async def _ws_read(ws, node, path, mid="g", timeout=30):
    """FRESH read (read_attribute), NOT get_node: get_node returns matter-server's cached
    attributes, which can lie (stale version) after a reboot or rollback."""
    r = await _ws_call(ws, "read_attribute", {"node_id": node, "attribute_path": path}, mid, timeout)
    res = r.get("result")
    return res.get(path) if isinstance(res, dict) else res


async def _ws_update_node(url, node, v, tag, label):
    import aiohttp
    for n in range(1, 8):
        print(f"[{tag}] update_node attempt {n} -> {label}", flush=True)
        try:
            async with aiohttp.ClientSession() as s:
                async with s.ws_connect(url, heartbeat=30) as ws:
                    await ws.receive(timeout=10)
                    r = await _ws_call(ws, "update_node", {"node_id": node, "software_version": v}, str(n), 600)
                    if r.get("error_code") is None:
                        print(f"[{tag}] provider reports finished")
                        break
                    print(f"[{tag}] declined:", r.get("error_code"), r.get("details", ""))
        except Exception as e:
            print(f"[{tag}] exc", repr(e)[:120])
        await asyncio.sleep(15)


async def ws_flash(url, node, v, target, vs=""):
    """update_node with retries, then verify the DEVICE booted the new version, then the
    subscription gate. AmebaZ2 verifies the softwareVersion int (0/40/9). ESP32 verifies the
    STRING (0/40/10): that firmware leaves the int unwired (reads 0). Exit 2: version never
    sustained. Exit 3: the subscription gate failed."""
    import aiohttp
    if target == "esp32":
        attr, want, shown, label = "0/40/10", vs, (lambda x: str(x)), f"v{v} ({vs})"
        iv_note, ref = "", ""
        never = "delta base mismatch (safe), full image rejected by delta target, or boot crash"
    else:
        attr, want, shown, label = "0/40/9", v, (lambda x: f"v{x}"), f"v{v}"
        iv_note, ref = " (docs/10 §9)", ", docs/10 §16"
        never = "OTA serial not bumped (rollback) or boot crash (docs/10 §7,§11)"
    await _ws_update_node(url, node, v, "flash", label)
    # Require the version SUSTAINED across 3 consecutive fresh reads: a single read right after a
    # matter-server restart can return a stale cached value (this false-positived the flash).
    good = 0
    for _ in range(30):
        try:
            async with aiohttp.ClientSession() as s:
                async with s.ws_connect(url, heartbeat=30) as ws:
                    await ws.receive(timeout=8)
                    got = await _ws_read(ws, node, attr)
                    if got == want:
                        good += 1
                        print(f"[flash] device reports {shown(want)} ({good}/3)")
                        if good >= 3:
                            # #64: a sustained version read is NOT enough. The 2026-07-19
                            # regression passed every read gate while wildcard subscriptions failed
                            # with 'Invalid TLV tag'. The re-interview is FATAL, and the node must
                            # then come back available: matter-server only marks a node available
                            # once its subscription is up, so that transition is the assertion.
                            r = await _ws_call(ws, "interview_node", {"node_id": node}, "iv", 120)
                            if not r or r.get("error_code") is not None:
                                print(f"[flash] FAILED: re-interview rejected: "
                                      f"{r.get('error_code') if r else 'no reply'} -- data-model/subscription "
                                      f"break? (#64{ref})")
                                sys.exit(3)
                            print(f"[flash] re-interviewed node for HA{iv_note}")
                            available = False
                            for _ in range(25):   # ~75 s
                                try:
                                    g = await _ws_call(ws, "get_node", {"node_id": node}, "gn", 15)
                                    n = g.get("result") if g else None
                                    if isinstance(n, dict) and n.get("available") is True:
                                        available = True
                                        break
                                except Exception:
                                    pass
                                await asyncio.sleep(3)
                            if not available:
                                print("[flash] FAILED: node never became available after re-interview "
                                      f"(~75 s) -- subscription broken (#64{ref})")
                                sys.exit(3)
                            print(f"[flash] SUCCESS: device booted {shown(want)} and is subscribable "
                                  "(available after re-interview)")
                            return
                    else:
                        good = 0
                        print(f"[flash] device reports {shown(got)} (want {want}) ...")
        except Exception:
            pass
        await asyncio.sleep(12)
    print(f"[flash] FAILED: device never sustained {shown(want)} -- {never}")
    sys.exit(2)


async def ws_read_attr(url, node, path):
    """Print one fresh attribute read (the `verify` step)."""
    import aiohttp
    async with aiohttp.ClientSession() as s:
        async with s.ws_connect(url) as ws:
            await ws.receive(timeout=10)
            print(await _ws_read(ws, node, path, "v", 40))


async def ws_identity(url, node):
    """One-shot Basic Information read for the pre-apply summary: proves the node id resolves to a
    unit that is reachable NOW, and gives the operator something to match against the appliance."""
    import aiohttp
    out = {}
    async with aiohttp.ClientSession() as s:
        async with s.ws_connect(url, heartbeat=30) as ws:
            await ws.receive(timeout=10)
            for k, p in (("sw", "0/40/9"), ("vendor", "0/40/1"), ("product", "0/40/2"), ("label", "0/40/5")):
                try:
                    out[k] = await _ws_read(ws, node, p, k, 20)
                except Exception:
                    out[k] = "?"
    print(f"softwareVersion={out.get('sw')} vendor={out.get('vendor')} product={out.get('product')} "
          f"label={out.get('label')!r}")


def ws_main(argv):
    name, a = argv[0], argv[1:]
    if name == "flash":
        asyncio.run(ws_flash(a[0], int(a[1]), int(a[2]), a[3], a[4] if len(a) > 4 else ""))
    elif name == "read":
        asyncio.run(ws_read_attr(a[0], int(a[1]), a[2]))
    elif name == "identity":
        asyncio.run(ws_identity(a[0], int(a[1])))
    elif name == "revert-apply":
        asyncio.run(ws_revert_apply(a[0], int(a[1]), int(a[2]), a[3], a[4], int(a[5]), int(a[6])))
    else:
        die(f"unknown _ws program: {name}")


# ---- revert to stock (issue #19), ported from ota-release.sh ---------------------------------
# Ways back to the stock ConnectLife firmware without opening the case:
#   --flip <ip>: 1.3.8+ carries a break-glass TCP listener on BREAKGLASS_PORT. `<token>:slots`
#     reports both slots' FWHS serials; `<token>:revert` invalidates the running image's signature
#     and resets. The bootloader boots the signature-valid slot with the HIGHEST serial, so
#     invalidating the custom slot lets the stock slot (serial 100) win. A bare `<token>` with no
#     colon still triggers the HTTPS OTA, so only the colon commands are used here.
#   --repackage <dump>: carve the stock app out of a stock flash dump and re-sign it (serial patch
#     + HMAC-SHA256 + bytesum trailer), then wrap it as a Matter .ota for the update_node channel.
#   --apply: stage the repackaged image + drive update_node, then CLASSIFY the outcome (reverted /
#     not reverted / ambiguous). Never infers success from silence: stock leaves this fabric and a
#     bootloader-rejected module does too (#75).
#   --slots <ip>: read-only :slots probe for triage (changes nothing).
#   --backup <ip>: 1.3.9+ answers `<token>:backup` with `ok: backup <addr_hex> <len_dec>\r\n` +
#     exactly <len> raw bytes of the INACTIVE slot. Fetch it once right after the first conversion
#     and the unit keeps a way back even after later OTAs overwrite the stock slot.
SLOTS_RE = r"ok:\s*fw1_sn=(\d+)\s+fw2_sn=(\d+)\s+cur=(\d+)"
SLOT_MAX = 0x1AC000
IMAGE_KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e5f")


def serial_base(cfg):
    return int(cfg.get("SERIAL_BASE") or 1100)


def revert_version():
    """The int the revert image must carry: strictly above BOTH version markers."""
    return max(cur_version(), released_version()) + 1


def breakglass_query(ip, port, message, timeout):
    """One short break-glass exchange: send, read up to a newline (or 512 bytes). Raises OSError
    when the unit cannot be reached. create_connection handles IPv6 link-local (fe80::..%if)."""
    data = b""
    with socket.create_connection((ip, port), timeout=timeout) as s:
        s.sendall(message.encode())
        s.settimeout(timeout)
        try:
            while b"\n" not in data and len(data) < 512:
                b = s.recv(256)
                if not b:
                    break
                data += b
        except socket.timeout:
            pass
    return data.decode(errors="replace").strip()


def breakglass_slots(ip, token, port, timeout=8):
    """The raw `ok: ...` reply to `<token>:slots`, or None. Asymmetric on purpose: an answer is
    decisive (only the custom firmware serves :slots), silence proves nothing. Stock has no
    listener, so a healthy stock unit and a bootloader-rejected module are both silent here."""
    if not ip or not token or not port:
        return None
    try:
        r = breakglass_query(ip, int(port), f"{token}:slots", timeout)
    except OSError:
        return None
    return r if r.startswith("ok:") else None


def backup_file_tag(ip):
    """The unit's part of a backup file name: last octet for IPv4, a sanitised address otherwise."""
    tag = ip.split("%")[0]                         # drop an IPv6 zone id
    if re.fullmatch(r"(\d{1,3}\.){3}\d{1,3}", tag):
        return tag.rsplit(".", 1)[1]
    return re.sub(r"[^0-9A-Za-z]+", "-", tag).strip("-")


def revert_backup(args):
    import amebaz2_image
    cfg = load_env()
    need(cfg, "BREAKGLASS_TOKEN", "BREAKGLASS_PORT")
    ip = out = ""
    for a in args:
        if a.startswith("-"):
            die(f"unknown flag for revert --backup: {a}")
        elif not ip:
            ip = a
        elif not out:
            out = a
        else:
            die(f"unexpected argument for revert --backup: {a}")
    if not ip:
        die("usage: dev.py ota amebaz2 revert --backup <unit-ip> [out.bin]")
    token, port, base = cfg["BREAKGLASS_TOKEN"], int(cfg["BREAKGLASS_PORT"]), serial_base(cfg)
    say(f"revert --backup {ip}:{port} (fetch + validate the inactive-slot stock image)")

    def fail(msg):   # die loudly, save nothing
        print(f"[revert] FAILED: {msg}")
        sys.exit(1)

    try:
        s = socket.create_connection((ip, port), timeout=15)
    except OSError as e:
        fail(f"cannot reach {ip}:{port}: {e} -- if the unit is otherwise up, it likely runs pre-1.3.9 "
             "firmware with no :backup support")
    s.settimeout(60)
    try:
        s.sendall(f"{token}:backup".encode())
        buf = b""
        while b"\r\n" not in buf:
            b = s.recv(256)
            if not b:
                fail("connection closed before the backup header")
            buf += b
            if len(buf) > 4096:
                fail("no backup header in the first 4 KB")
        hdr, buf = buf.split(b"\r\n", 1)
        hdr = hdr.decode(errors="replace").strip()
        if hdr.startswith("err:"):
            fail(f"unit refused: {hdr}")
        m = re.fullmatch(r"ok:\s*backup\s+(\S+)\s+(\d+)", hdr)
        if not m:
            fail(f"unexpected :backup reply: {hdr!r} -- pre-1.3.9 firmware has no :backup support")
        addr, length = m.group(1), int(m.group(2))
        if not 0 < length <= SLOT_MAX:
            fail(f"implausible backup length {length:#x} (slot is {SLOT_MAX:#x} max)")
        while len(buf) < length:
            b = s.recv(min(65536, length - len(buf)))
            if not b:
                fail(f"connection closed at {len(buf):#x} of {length:#x} bytes")
            buf += b
        buf = buf[:length]
    finally:
        s.close()
    print(f"[revert] backup received: addr={addr} len={length:#x}")
    # Process like the repackage carve: image end = first 4096-byte 0xFF run, then a 4-byte trailer.
    i = buf.find(b"\xff" * 4096)
    if i < 0:
        fail("no 4096-byte 0xFF run in the backup -- slot empty or not a stock image?")
    imglen = i - 4
    if not 0x100000 <= imglen <= 0x180000:
        fail(f"carved length {imglen:#x} outside [0x100000,0x180000]")
    image, trailer = buf[:imglen], buf[imglen:imglen + 4]
    serial = amebaz2_image.read_serial(image)
    if serial >= base:
        fail(f"serial@0xF4={serial} >= {base} -- the inactive slot holds a CUSTOM image, not stock "
             "(nothing to back up; a second custom OTA already overwrote it)")
    good, lines = amebaz2_image.verify(image, trailer)   # manifest sig + EVERY sub-image trailer + byte-sum (#75)
    for ln in lines:
        print(f"[revert]   {ln}")
    if not good:
        fail("the backup does not verify: " + "; ".join(amebaz2_image.failures(lines)))
    if not out:
        out = str(REPO / "firmware/built-images" / f"stock-backup-{backup_file_tag(ip)}-sn{serial}.bin")
    with open(out, "wb") as f:
        f.write(buf)                               # raw slot bytes: image + trailer + 0xFF pad
    print(f"[revert] saved {out} ({len(buf):#x} bytes, stock serial {serial})")
    print(f"[revert] all checks passed: serial {serial} < {base}, manifest sig + every sub-image trailer + byte-sum OK")
    print(f"[revert] next (when needed): dev.py ota amebaz2 revert --repackage {out}")


def revert_flip(args):
    cfg = load_env()
    need(cfg, "BREAKGLASS_TOKEN", "BREAKGLASS_PORT")
    ip, force = "", False
    for a in args:
        if a == "--force":
            force = True
        elif a.startswith("-"):
            die(f"unknown flag for revert --flip: {a}")
        elif not ip:
            ip = a
        else:
            die(f"unexpected argument for revert --flip: {a}")
    if not ip:
        die("usage: dev.py ota amebaz2 revert --flip <unit-ip> [--force]")
    token, port, base = cfg["BREAKGLASS_TOKEN"], int(cfg["BREAKGLASS_PORT"]), serial_base(cfg)
    say(f"revert --flip {ip}:{port} (query slots, then invalidate the running slot)")
    try:
        r = breakglass_query(ip, port, f"{token}:slots", 10)
    except OSError as e:
        print(f"[revert] cannot reach {ip}:{port}: {e}")
        print("[revert] if the unit is otherwise up, it likely runs pre-1.3.8 firmware with NO break-glass "
              "listener -- use 'revert --repackage' + 'revert --apply' instead")
        sys.exit(2)
    m = re.match(SLOTS_RE, r)
    if not m:
        print(f"[revert] unexpected :slots reply: {r!r}")
        print("[revert] pre-1.3.8 firmware has no :slots support (there a bare <token> with no colon triggers "
              "HTTPS OTA) -- use 'revert --repackage' + 'revert --apply' instead")
        sys.exit(2)
    fw1, fw2, cur = (int(x) for x in m.groups())
    other = fw2 if cur == 1 else fw1
    print(f"[revert] slots: fw1_sn={fw1} fw2_sn={fw2} cur=fw{cur} -> other slot serial {other}")
    if other >= base and not force:
        print(f"[revert] REFUSED: other slot serial {other} >= {base} is a custom image, not stock (stock serial is 100)")
        print("[revert] re-run with --force to flip to that older custom image anyway")
        sys.exit(3)
    r = breakglass_query(ip, port, f"{token}:revert", 10)
    print(f"[revert] revert reply: {r!r}")
    print("[revert] running image invalidated + reset issued; the bootloader now boots the other slot. "
          "Give the unit ~30 s.")


def revert_slots(args):
    """Read-only break-glass probe: prints the slot map and changes NOTHING. An answer PROVES the
    custom firmware is alive, silence proves nothing."""
    cfg = load_env()
    need(cfg, "BREAKGLASS_TOKEN", "BREAKGLASS_PORT")
    ip = args[0] if args else ""
    if not ip:
        die("usage: dev.py ota amebaz2 revert --slots <unit-ip>")
    port = cfg["BREAKGLASS_PORT"]
    r = breakglass_slots(ip, cfg["BREAKGLASS_TOKEN"], port)
    if r:
        say(f"{ip}:{port} answered: {r}")
        say("  ANSWERED = the CUSTOM firmware is running (only it serves :slots). The module is alive.")
        return
    say(f"{ip}:{port} did not answer.")
    say("  This is NOT a verdict. Stock carries no break-glass listener, and stock also joins its")
    say("  OWN factory-provisioned network, so a healthy reverted unit is not even at this address.")
    say("  A bootloader-rejected module is equally silent. Discriminate with the flash QE bit:")
    say(f"  python3 {REPO}/firmware/flasher/ch341_sr.py  (QE cleared = the bootloader rejected it).")
    sys.exit(1)


def _hmac(data):
    return hmac.new(IMAGE_KEY, data, hashlib.sha256).digest()


def image_chain(payload):
    """Walk the sub-image chain: [(header, end, trailer_matches)] for EVERY trailer, or None when it
    cannot be walked. For sub-image i at header H: S=u32le(img[H]) is the segment SIZE, the trailer
    sits at END=H+0x60+S and covers img[START:END] with START=0 for i==0, else H. The next header is
    H + u32le(img[H+4]) (RELATIVE; 0xFFFFFFFF terminates). Never hardcode the layout (stock ships two)
    and never require the last trailer to end at EOF: elf2bin pads 0 or 0x20 trailing bytes. The walk
    is slot-agnostic by construction, so never gate any of this on a slot index. (#75)"""
    h, i, out = 0xE0, 0, []
    while True:
        if h + 0x60 > len(payload):
            return None
        size, nxt = struct.unpack_from("<I", payload, h)[0], struct.unpack_from("<I", payload, h + 4)[0]
        end, start = h + 0x60 + size, (0 if i == 0 else h)
        if end + 0x20 > len(payload):
            return None
        out.append((h, end, _hmac(payload[start:end]) == payload[end:end + 0x20]))
        if nxt == 0xFFFFFFFF:
            return out
        if nxt == 0 or i > 15:
            return None
        h += nxt
        i += 1


def image_check(payload, trailer, name):
    """Print one line for an image and return whether the bootloader would accept it: manifest
    signature, every sub-image trailer, and the transport byte-sum."""
    mac_ok = _hmac(payload[0xE0:0x140]) == payload[0:32]
    c = image_chain(payload)
    in_ok = c is not None and all(m for _, _, m in c)
    sum_ok = struct.pack("<I", sum(payload) & 0xFFFFFFFF) == trailer
    serial = struct.unpack_from("<I", payload, 0xF4)[0]
    sub = "UNWALKABLE" if c is None else " ".join("%#x=%s" % (e, "OK" if m else "MISMATCH") for _, e, m in c)
    print(f"  {name}: len={len(payload):#x} serial@0xF4={serial} hmac={'OK' if mac_ok else 'MISMATCH'} "
          f"trailers[{sub}] bytesum={'OK' if sum_ok else 'MISMATCH'}")
    return mac_ok and in_ok and sum_ok


def carve_stock(dump):
    """(image, length, offset) of the HMAC-valid image in a full flash dump (stock fw1 app at
    0x10000) or a raw slot image from revert --backup (app at 0x0), else None. The image ends at the
    first 4 KB run of erased flash, minus the 4-byte bytesum trailer."""
    for off in (0x10000, 0):
        c = dump[off:]
        i = c.find(b"\xff" * 4096)
        if i < 0:
            continue
        length = i - 4
        if not 0x100000 <= length <= 0x180000:
            continue
        if _hmac(c[0xE0:0x140]) != c[0:32]:
            continue
        return c, length, off
    return None


def resign_stock(image, new_serial):
    """The revert payload: the stock image with a serial the bootloader prefers, re-signed. Order
    matters, each step's input includes the previous step's output. Sub-image 0's trailer (at
    u32le(img[0xE0]) + 0x140) covers [0, END), so it reaches the serial and the manifest signature
    and must be recomputed last. Leaving it stale is what bricked the office unit on 2026-07-21:
    boot_load prints "Hash Result Incorrect!" and hangs with the flash QE bit cleared (#75)."""
    payload = bytearray(image)
    struct.pack_into("<I", payload, 0xF4, new_serial)
    payload[0:32] = _hmac(bytes(payload[0xE0:0x140]))                    # manifest sig
    end0 = struct.unpack_from("<I", payload, 0xE0)[0] + 0x140           # inner image HMAC (#75)
    payload[end0:end0 + 0x20] = _hmac(bytes(payload[0:end0]))
    payload += struct.pack("<I", sum(payload) & 0xFFFFFFFF)              # transport byte-sum
    return bytes(payload)


def revert_repackage(args):
    cfg = load_env()
    dump = args[0] if args else ""
    if not dump:
        die("usage: dev.py ota amebaz2 revert --repackage <stock-dump.bin>")
    if not Path(dump).is_file():
        die(f"stock dump not found: {dump}")
    v = revert_version()
    semver = f"{int_to_semver(v)}-stock"
    base = serial_base(cfg)
    new_serial = base + v
    bi = REPO / "firmware/built-images"
    payload_path, ota = bi / f"rac-stock-v{v}-payload.bin", bi / f"rac-stock-v{v}.ota"
    # Self-check the recipe against known-good bytes BEFORE trusting the carve: every archived
    # custom firmware_is-v*.bin and the dump's unpatched image must verify. A dump from a different
    # build or a wrong key fails loudly here instead of on the device.
    say(f"revert --repackage v{v} (serial {new_serial}): verify the signing recipe first")
    refs = sorted(bi.glob("firmware_is-v*.bin"))
    if not refs:
        print("  no firmware_is-v*.bin in built-images/ -- cannot self-check the recipe")
        sys.exit(1)
    good = True
    for f in refs:
        d = f.read_bytes()
        good &= image_check(d[:-4], d[-4:], f.name)
    carved = carve_stock(Path(dump).read_bytes())
    if carved is None:
        print("  no HMAC-valid stock image at 0x10000 (dump) or 0x0 (backup) -- refusing")
        sys.exit(1)
    img, imglen, off = carved
    # The carve proves "a signed image is here", NOT "that image is stock". A dump from an
    # already-converted unit carves just as cleanly, and shipping that as a "revert to stock" .ota
    # would push CUSTOM firmware under a stock label. Stock carries serial 100, custom carries
    # SERIAL_BASE+versionInt. A SERIAL test, never a slot test: stock has booted fine from FW2.
    carved_serial = struct.unpack_from("<I", img, 0xF4)[0]
    if carved_serial >= base:
        print(f"  carved image serial@0xF4={carved_serial} >= {base}: this is a CUSTOM image, not stock -- refusing")
        sys.exit(1)
    good &= image_check(img[:imglen], img[imglen:imglen + 4], f"input image @0x{off:x} (unpatched)")
    if not good:
        print("recipe self-check FAILED -- not building a revert image from unverified bytes")
        sys.exit(1)
    payload = resign_stock(img[:imglen], new_serial)
    payload_path.write_bytes(payload)
    print(f"  signed payload: {payload_path} ({len(payload):#x} bytes, serial {new_serial})")
    r = subprocess.run(["python3", cfg["OTA_TOOL"], "create", "-v", cfg["VID"], "-p", cfg["PID"],
                        "-vn", str(v), "-vs", semver, "-da", "sha256", "-mi", "1", "-ma", str(v - 1),
                        str(payload_path), str(ota)], stdout=subprocess.DEVNULL)
    if r.returncode != 0:
        die("ota_image_tool.py create failed")
    # Same manifest shape as package, plus payloadName so the carved stock payload behind the .ota
    # stays identifiable. otaUrl is the staged file:// name (--apply uploads both under these names).
    doc = ota_manifest(ota.read_bytes(), v, semver, f"file:///rac-stock-v{v}.ota")
    doc["modelVersion"]["payloadName"] = payload_path.name
    (bi / f"rac-stock-v{v}.json").write_text(json.dumps(doc) + "\n")
    say(f"  ota:      {ota}  (+ .json manifest, payloadName={payload_path.name})")
    say("  not staged. next: dev.py ota amebaz2 revert --apply")
    say("  NOTE: the 2026-07-21 brick (docs/10 §17 'Path 2') is root-caused (#75): the old recipe left")
    say("  sub-image 0's inner HMAC (at size@0xE0 + 0x140) stale. This payload recomputes it and")
    say("  self-checks EVERY sub-image trailer, on this payload and on every archived image.")
    say("  CONFIRMED ON HARDWARE 2026-07-27: a repackaged stock image booted (VID 5004 / PID 13825 / sw 2).")
    say("  Recovery if it ever does fail is still the CH341A clip; a rejected image hangs with QE CLEARED.")
    # Version-consumption guard: the revert image carries serial SERIAL_BASE+v, so once it is applied
    # the next custom OTA must EXCEED v or the bootloader ties and the stock slot wins. Keep
    # version.txt ahead of every revert int ever handed out here.
    if cur_version() <= v:
        nv = int_to_semver(v + 1)
        VERSION_FILE.write_text(nv + "\n")
        set_header_version(cfg)
        say(f"WARNING: the revert consumed fleet version {v} -- version.txt bumped to {nv} (int {v + 1})")
        say(f"         so the next custom build beats serial {new_serial}. COMMIT firmware/src/version.txt.")


def revert_triage(cfg, ip):
    """Printed whenever --apply cannot tell "stock is running" from "the bootloader rejected the
    image". Network silence is NOT a verdict: stock leaves this fabric, joins its own network and
    has no break-glass listener, so a healthy revert and a #75-class rejection look identical from
    here. Two healthy units were declared bricked on 2026-07-26 for exactly this reason."""
    for ln in f"""----------------------------------------------------------------------
TRIAGE: the unit is silent. Silence alone means NOTHING. Run these, in order.

1. VENDOR APP (no tools). A reverted unit IS stock: it rejoins ConnectLife by itself
   and reappears in the ConnectLife app as the same appliance. If the app sees it, the
   revert WORKED. Note the A/C keeps working from the IR remote either way, so 'the
   A/C responds' says nothing about the Wi-Fi module.

2. BREAK-GLASS (no tools, read-only). Stock does not carry the listener, so:
     dev.py ota amebaz2 revert --slots {ip}
   ANSWERS -> the CUSTOM firmware is still running: the revert did not take, the unit
              is healthy, nothing is bricked. Retry --apply, or flip if stock is in the
              other slot. SILENT -> consistent with stock AND with a rejected image;
              not a verdict.

3. FLASH QE BIT (CH341A clip; the definitive test). boot_load routes every image
   rejection through its shared failure sink (hal_flash_return_spi), which CLEARS the
   GD25Q32 QE bit; a healthy boot leaves it SET (docs/10 §17 Path 2, issue #75).
     python3 {REPO}/firmware/flasher/ch341_sr.py
   QE=0 -> the bootloader REJECTED the image (bricked, and it will NOT fall back to the
           other slot). QE=1 -> it did not reject anything; chase the network instead.

4. READ THE SLOTS OUT OF A DUMP (CH341A clip; settles it definitively).
     python3 {REPO}/firmware/flasher/ch341dump.py /tmp/unit.bin
   fw1 app @0x10000, fw2 app @0x190000; serial = u32le at app+0xF4 (stock 100, custom
   {serial_base(cfg)}+versionInt). 'revert --repackage /tmp/unit.bin' re-verifies the
   FULL acceptance recipe (manifest sig + #75 inner HMAC + bytesum) on fw1; for fw2,
   slice it out first: dd if=/tmp/unit.bin of=/tmp/fw2.bin bs=4096 skip=400.

5. RECOVERY, only if 3/4 say rejected: clip-write the unit's own dump with fw1 replaced
   by ORIGINAL stock slot bytes (factory signature, from 'revert --backup') and fw2
   erased to 0xFF, via ch341flash-full.py (it re-sets QE). docs/10 §17.
----------------------------------------------------------------------""".splitlines():
        say(ln)


def newest_stock_pair(bi):
    """The highest int N with both rac-stock-vN.ota and rac-stock-vN.json on disk, or None. --apply
    must NOT recompute revert_version(): --repackage consumes int v and bumps version.txt to v+1,
    so a separately-invoked apply would compute v+2 and look for a pair that was never built."""
    best = None
    for f in Path(bi).glob("rac-stock-v*.ota"):
        n = f.name[len("rac-stock-v"):-len(".ota")]
        if re.fullmatch(r"[0-9]+", n) and (Path(bi) / f"rac-stock-v{n}.json").is_file():
            best = int(n) if best is None else max(best, int(n))
    return best


def verify_stock_payload(bi, manifest):
    """Re-verify the payload behind a revert .ota BEFORE it goes near a device. --apply picks the
    newest pair on disk, which may come from an older revision: built-images/ still holds pre-#75
    payloads that bricked a unit, and a version accident is all it takes for one to be "newest".
    Returns (ok, message)."""
    m = json.loads(Path(manifest).read_text())["modelVersion"]
    pn = m.get("payloadName")
    if not pn:
        return False, f"{Path(manifest).name} has no payloadName -- built by a pre-#75 script revision. REFUSING."
    p = Path(bi) / pn
    if not p.exists():
        return False, f"payload {pn} missing -- cannot verify what this .ota carries. REFUSING."
    d = p.read_bytes()
    img, trailer = d[:-4], d[-4:]
    if _hmac(img[0xE0:0x140]) != img[0:32]:
        return False, "manifest signature MISMATCH -- REFUSING"
    if struct.pack("<I", sum(img) & 0xFFFFFFFF) != trailer:
        return False, "bytesum trailer MISMATCH -- REFUSING"
    h, i = 0xE0, 0
    while True:
        if h + 0x60 > len(img):
            return False, "chain UNWALKABLE -- REFUSING"
        size, nxt = struct.unpack_from("<I", img, h)[0], struct.unpack_from("<I", img, h + 4)[0]
        end, start = h + 0x60 + size, (0 if i == 0 else h)
        if end + 0x20 > len(img):
            return False, "chain trailer past EOF -- REFUSING"
        if _hmac(img[start:end]) != img[end:end + 0x20]:
            return False, f"sub-image {i} trailer @{end:#x} MISMATCH (issue #75 signature) -- REFUSING to stage a brick"
        if nxt == 0xFFFFFFFF:
            break
        if nxt == 0 or i > 15:
            return False, "malformed chain -- REFUSING"
        h += nxt
        i += 1
    return True, (f"{pn}: manifest sig OK, {i + 1} sub-image trailers OK, bytesum OK, "
                  f"serial@0xF4={struct.unpack_from('<I', img, 0xF4)[0]}")


def revert_apply(args):
    cfg = load_env()
    need(cfg, "OTAENV_PY", "MS_WS", "NODE_ID", "PI_HOST", "PI_OTA_DIR", "PI_SSH_KEY")
    # --ip is optional but strongly recommended: the break-glass listener is the ONLY no-clip signal
    # that separates "stock is running" from "the bootloader rejected the image", so without it the
    # verdict can never be better than AMBIGUOUS. --yes skips the prompt (scripted runs only).
    ip, assume_yes, i = "", False, 0
    usage_hint = "(usage: revert --apply [--ip <unit-ip>] [--yes])"
    while i < len(args):
        a = args[i]
        if a == "--ip":
            ip = args[i + 1] if i + 1 < len(args) else ""
            if not ip:
                die("--ip needs the unit's IP")
            i += 2
        elif a.startswith("--ip="):
            ip = a[len("--ip="):]
            i += 1
        elif a in ("--yes", "-y"):
            assume_yes = True
            i += 1
        else:
            die(f"unknown argument for revert --apply: {a} {usage_hint}")
    bi = REPO / "firmware/built-images"
    v = newest_stock_pair(bi)
    if v is None:
        die(f"no rac-stock-v*.{{ota,json}} pair in {bi} -- run 'revert --repackage <stock-dump.bin>' first")
    src_ota, src_json = bi / f"rac-stock-v{v}.ota", bi / f"rac-stock-v{v}.json"
    node, base = cfg["NODE_ID"], serial_base(cfg)
    token, port = cfg.get("BREAKGLASS_TOKEN", ""), cfg.get("BREAKGLASS_PORT", "")

    # Pre-apply confirmation. State, before anything is written: which node, which physical unit,
    # which slot the image lands in, and that the unit leaves this fabric.
    slots = fw1 = fw2 = cur = tgt = other = None
    if ip:
        slots = breakglass_slots(ip, token, port)
        m = re.search(r"fw1_sn=(\d+)", slots or ""), re.search(r"fw2_sn=(\d+)", slots or ""), \
            re.search(r"cur=(\d+)", slots or "")
        if slots:
            fw1, fw2, cur = (x.group(1) if x else "" for x in m)
            tgt, other = (2, fw2) if cur == "1" else (1, fw1)
        else:
            say(f"WARNING: {ip}:{port or '?'} did not answer :slots. Wrong IP, BREAKGLASS_TOKEN/PORT")
            say(f"         unset in {ENVF}, or the unit is already not running custom firmware. Fix it")
            say("         BEFORE applying: with no working break-glass probe the post-apply verdict")
            say("         can only ever be AMBIGUOUS.")
    pre = ws_program(cfg, "identity", cfg["MS_WS"], node, capture=True)[1]
    no_answer = f"NO ANSWER (node {node} unreachable through matter-server; update_node will likely fail)"
    say("----------------------------------------------------------------------")
    say("revert --apply: about to push STOCK firmware onto a LIVE unit.")
    say(f"  env file       : {ENVF}")
    say(f"  target node    : {node}  (read from {ENVF} -- an exported NODE_ID is IGNORED, the env file")
    say("                   is sourced AFTER the environment. To target another unit,")
    say("                   copy the env file and run with ENVF=<copy>.)")
    say(f"  node reads now : {pre or no_answer}")
    say(f"  matter-server  : {cfg['MS_WS']}  (staging on {cfg['PI_HOST']}:{cfg['PI_OTA_DIR']})")
    say(f"  image          : {src_ota.name}  softwareVersion int {v}, FWHS serial {base + v}")
    say(f"  unit ip        : {ip or 'NOT GIVEN -- no break-glass probe, verdicts will be weaker'}")
    if slots:
        say(f"  slots now      : fw1_sn={fw1} fw2_sn={fw2} running=fw{cur}")
        say(f"  OTA lands in   : fw{tgt} (the INACTIVE slot; whatever it holds now is overwritten)")
        if other and int(other) < base:
            say(f"  NOTE: fw{tgt} ALREADY holds a stock image (serial {other} < {base}). Prefer")
            say(f"        'dev.py ota amebaz2 revert --flip {ip}': it boots that slot with NO flash write and")
            say("        NO re-signed payload, so it carries none of the #75 image-rejection risk. It")
            say("        also keeps the factory-signed stock bytes instead of overwriting them.")
    say("  after this     : the unit LEAVES this Matter fabric, rejoins ConnectLife on its own")
    say("                   network, and loses the break-glass listener and the :2323 console.")
    say("                   Silence afterwards is EXPECTED and is NOT proof of success.")
    say("----------------------------------------------------------------------")
    if assume_yes:
        say("--yes given: proceeding without confirmation")
    else:
        # Fails CLOSED: no usable /dev/tty aborts here rather than pushing firmware at a unit
        # nobody is watching.
        try:
            with open("/dev/tty", "r+") as tty:
                tty.write(f"[dev] type 'revert node {node}' to proceed: ")
                tty.flush()
                reply = tty.readline().rstrip("\n")
        except OSError:
            die("no interactive terminal for the confirmation prompt -- re-run with --yes if you are sure")
        if reply != f"revert node {node}":
            die(f"aborted (got '{reply}')")

    say(f"applying newest repackaged revert image on disk: rac-stock-v{v}")
    say("  re-verifying the payload (manifest sig + EVERY sub-image HMAC + bytesum)")
    good, msg = verify_stock_payload(bi, src_json)
    print(f"  {msg}")
    if not good:
        sys.exit(1)
    # stage is keyed to the cur_version rac-v* names, so this does its own scp + junk prune +
    # matter-server restart for the rac-stock-* pair.
    say(f"stage rac-stock-v{v} on {cfg['PI_HOST']}:{cfg['PI_OTA_DIR']} + restart matter-server")
    if pi_scp(cfg, [src_ota, src_json], f"{cfg['PI_OTA_DIR']}/") != 0:
        die(f"scp to {cfg['PI_HOST']}:{cfg['PI_OTA_DIR']} failed")
    rc = subprocess.run(["ssh", "-o", "BatchMode=yes", "-i", cfg["PI_SSH_KEY"], cfg["PI_HOST"],
                         f"rm -f {cfg['PI_OTA_DIR']}/chip_kvs_ota_provider_* {cfg['PI_OTA_DIR']}/ota_provider_*.log "
                         "2>/dev/null;      docker restart matter-server >/dev/null 2>&1"]).returncode
    if rc != 0:
        die(f"matter-server restart on {cfg['PI_HOST']} failed")
    say("  staged + provider junk pruned + matter-server restarted (manifest cache reloaded)")
    say(f"apply rac-stock-v{v} to node {node} (update_node with retries, then classify the outcome)")
    rc = ws_program(cfg, "revert-apply", cfg["MS_WS"], node, v, ip, token, port or 0, base)[0]
    if rc == 0:
        say(f"revert applied: the unit runs STOCK firmware now (it will NOT report v{v} -- expected).")
        say("next steps: the unit speaks ConnectLife again. To put it back on custom firmware,")
        say("re-flash the custom image over CH341 (firmware/docs/10-firmware-ota-procedure.md).")
        say("if you re-commission afterwards and the stack takes the 'already commissioned' branch")
        say("and never advertises BLE, run the break-glass ':wipekv' first: the Matter DCT")
        say("(0x3E0000/0x3ED000) survives the stock round trip, so a stale fabric can linger.")
        # Deliberately NOT writing .released-version: it tracks the CUSTOM line, and a later custom
        # OTA must still be strictly greater than the version that was rolled back.
    elif rc == 3:
        die("revert did NOT take -- the unit is still running the custom firmware (healthy, not "
            "bricked); see the [revert] lines above")
    elif rc == 4:
        revert_triage(cfg, ip or "<unit-ip>")
        print(f"{C['red']}[dev] AMBIGUOUS:{C['off']} revert outcome UNKNOWN -- silence is neither success "
              "nor a brick; run the triage checks above", file=sys.stderr)
        sys.exit(4)
    else:
        die(f"revert --apply failed unexpectedly (exit {rc}) -- see the [revert] lines above")


async def ws_revert_apply(url, node, v, ip, token, port, base):
    """update_node with the stock image, then classify. Exit 0 = CONFIRMED on stock, 3 = CONFIRMED
    still on custom (revert did not take, unit healthy), 4 = AMBIGUOUS. Never reports success from
    silence: a healthy stock unit and a bootloader-rejected module both vanish from this fabric."""
    import aiohttp

    def slots_note(r):
        m = re.match(SLOTS_RE, r)
        if not m:
            return f"break-glass answered {r!r}"
        f1, f2, c = (int(x) for x in m.groups())
        running = f1 if c == 1 else f2
        return (f"break-glass answered: fw1_sn={f1} fw2_sn={f2} running=fw{c} "
                f"(serial {running} = {'CUSTOM' if running >= base else 'stock'})")

    def not_reverted(why):
        print(f"[revert] VERDICT: NOT REVERTED -- {why}")
        print("[revert] the module is HEALTHY and still running the custom firmware; nothing is bricked.")
        print("[revert] retry 'revert --apply', or use 'revert --flip <ip>' if the other slot holds stock.")
        sys.exit(3)

    async def read_ident():
        """Fresh (softwareVersion, vendorId) read, or (None, None) when the node is silent."""
        try:
            async with aiohttp.ClientSession() as s:
                async with s.ws_connect(url, heartbeat=30) as ws:
                    await ws.receive(timeout=8)
                    return await _ws_read(ws, node, "0/40/9", "g", 30), await _ws_read(ws, node, "0/40/1", "gv", 30)
        except Exception:
            return None, None

    async def node_unavailable():
        try:
            async with aiohttp.ClientSession() as s:
                async with s.ws_connect(url, heartbeat=30) as ws:
                    await ws.receive(timeout=8)
                    g = await _ws_call(ws, "get_node", {"node_id": node}, "gn", 15)
                    n = g.get("result") if g else None
                    return isinstance(n, dict) and n.get("available") is False
        except Exception:
            return False

    await _ws_update_node(url, node, v, "revert", f"v{v}")
    # Classify. Stock reports softwareVersion 4 / vendor 5004 (0xFFF1 is the custom line), so a
    # SUSTAINED 4 is a real confirmation. Nothing else is: this loop only gathers evidence and never
    # concludes from silence. The decision happens after it.
    good = cust = silent = 0
    for _ in range(30):
        got, vend = await read_ident()
        if got == 4:
            good, cust, silent = good + 1, 0, 0
            print(f"[revert] unit reports stock softwareVersion 4 (vendor {vend}) ({good}/3)", flush=True)
            if good >= 3:
                print("[revert] VERDICT: REVERTED -- unit is on stock firmware (softwareVersion 4 sustained)")
                return
        elif got is not None:
            good, silent, cust = 0, 0, cust + 1
            print(f"[revert] unit still answers on softwareVersion {got} (vendor {vend}) ({cust} in a row) ...",
                  flush=True)
        else:
            good, cust, silent = 0, 0, silent + 1
            print(f"[revert] no answer from node {node} ({silent} in a row) -- expected on stock, "
                  "equally expected on a dead module. Not a verdict.", flush=True)
        await asyncio.sleep(12)
    # No sustained stock version. Use the one signal that discriminates without a clip.
    print("[revert] ~6 min without a sustained stock version; probing the break-glass listener", flush=True)
    r = breakglass_slots(ip, token, port)
    if r:
        not_reverted(slots_note(r) + " -- so the custom firmware is still running and the OTA never took")
    # A unit whose OTA failed can take a while to rejoin the fabric, and that comeback is a decisive
    # negative. Give it one more window before declaring ambiguity.
    print("[revert] silent. Waiting 180 s for a late comeback, then re-probing", flush=True)
    await asyncio.sleep(180)
    got, vend = await read_ident()
    if got == 4:
        print(f"[revert] VERDICT: REVERTED -- unit answered on stock softwareVersion 4 (vendor {vend}) after the wait")
        return
    if got is not None:
        not_reverted(f"node {node} came back on softwareVersion {got} (vendor {vend}) -- the OTA never took")
    r = breakglass_slots(ip, token, port)
    if r:
        not_reverted(slots_note(r) + " (after the wait) -- so the custom firmware is still running")
    unavail = await node_unavailable()
    print("[revert] VERDICT: AMBIGUOUS -- the unit is silent on every channel we own.")
    print(f"[revert]   matter-server reports node {node} available=False: {unavail}. That is expected")
    print("[revert]   for BOTH a healthy stock unit and a dead module, so it is NOT a verdict.")
    if not ip or not token:
        print("[revert]   no --ip (or no BREAKGLASS_TOKEN), so the one no-clip discriminator never ran.")
    else:
        print(f"[revert]   break-glass at {ip}:{port} did not answer: consistent with stock (which has")
        print("[revert]   no listener) AND with a bootloader rejection.")
    print("[revert] Stock also joins its OWN factory-provisioned network, so it may be invisible to")
    print("[revert] us while perfectly healthy: absence from our VLAN is NOT evidence either.")
    print("[revert] Do not record this as success and do not record it as a brick. Triage below.")
    sys.exit(4)


def revert(args):
    sub = {"--backup": revert_backup, "--flip": revert_flip, "--repackage": revert_repackage,
           "--apply": revert_apply, "--slots": revert_slots}.get(args[0] if args else "")
    if not sub:
        die("usage: dev.py ota amebaz2 revert {--backup <unit-ip> [out.bin]|--flip <unit-ip> [--force]|"
            "--slots <unit-ip>|--repackage <stock-dump.bin>|--apply [--ip <unit-ip>] [--yes]}")
    sub(args[1:])


def release_amebaz2(args):
    """build + package + stage (+ tag, + flash)."""
    bump, debug, flash_after, tag_after = None, False, False, False
    for a in args:
        if a in ("--bump", "--bump-patch", "--bump-minor", "--bump-major"):
            bump = a
        elif a == "--debug":
            debug = True
        elif a == "--flash":
            flash_after = True
        elif a == "--tag":
            tag_after = True
        else:
            # Almost certainly a typo. Silently ignoring it is how a build ends up the wrong
            # flavour: `release --debug` once dropped the flag and shipped a console-less image.
            die(f"unknown flag for release: {a}")
    flavour = "debug" if debug else None
    build_amebaz2(([bump] if bump else []) + (["--debug"] if debug else []))
    package_amebaz2(flavour)
    stage_amebaz2(flavour)
    if tag_after:
        tag_release()
    if flash_after:
        flash_amebaz2()
    else:
        say("staged, not flashed. run: dev.py ota amebaz2 flash")


# ---- ESP32 (esp-matter) release engine, ported from esp32-release.sh (#143) -------------------
# It mechanises the two things that have gone wrong on this target:
#   * #82: ESP-IDF builds are NOT byte-reproducible and `idf.py build` overwrites build/, so the
#     exact DEPLOYED image (the delta base) is easily lost. `build` REFUSES to run unless the
#     currently-released base is archived in built-images/, then archives the fresh image itself.
#     Losing the 1.0.3 base once stranded the node on USB-only flashing.
#   * delta-only OTA: CONFIG_ENABLE_DELTA_OTA=y makes the device REJECT a full image, so `package`
#     builds a delta patch against the archived base and wraps THAT as the .ota.
# The version comes from CMakeLists.txt PROJECT_VER, same int scheme as AmebaZ2 (#77);
# esp32-lint.sh enforces PROJECT_VER <-> sdkconfig sync.
ESP_IMG = REPO / "firmware/built-images"
ESP_NEW_BIN = ESP / "build/hisense_ac_matter.bin"
ESP_CONSOLE_MARK = b"diagnostic console listening"


def esp_semver():
    m = re.search(r'^set\(PROJECT_VER "(.*)"\)', (ESP / "CMakeLists.txt").read_text(), re.M)
    if not m or not m.group(1):
        die(f"could not parse PROJECT_VER from {ESP / 'CMakeLists.txt'}")
    return m.group(1)


def esp_semver_to_int(s):
    s = "".join(str(s).split())
    m = re.fullmatch(r"([0-9]+)\.([0-9]+)\.([0-9]+)", s)
    if not m:
        die(f"PROJECT_VER '{s}' is not semver MAJOR.MINOR.PATCH")
    major, minor, patch = (int(x) for x in m.groups())
    if minor >= 100 or patch >= 100:
        die(f"minor/patch must be < 100 for the int mapping: '{s}'")
    return major * 10000 + minor * 100 + patch


def esp_int():
    return esp_semver_to_int(esp_semver())


def esp_target():
    """The archive namespace is PER-TARGET: an esp32 (Xtensa) and an esp32c3 (RISC-V) build can
    carry the same PROJECT_VER, and a delta against the wrong architecture's base is meaningless.
    Taken from the generated sdkconfig so the base lookup, the archive name and the patch
    generator's --chip all agree. esp32 when sdkconfig is absent (fresh checkout)."""
    return sdkconfig_target(ESP) or "esp32"


def esp_released_mark():
    """softwareVersion int last CONFIRMED booted, PER TARGET. One shared file was wrong the moment
    two architectures existed: releasing to the C3 wrote its version into the mark the esp32 path
    reads. A function, not a constant: the target is only known once sdkconfig exists."""
    return ESP_IMG / f".released-version-{esp_target()}"


def esp_released_int():
    mark = esp_released_mark()
    return int(mark.read_text().strip()) if mark.is_file() else 0


def lock_idf_version(text):
    """The `version:` under dependencies.lock's top-level `idf:` block, '' when absent."""
    inside = False
    for ln in text.splitlines():
        if ln == "  idf:":
            inside = True
        elif inside and ln.startswith("    version:"):
            return re.sub(r"""[\r"']""", "", ln.split("version:", 1)[1]).strip()
    return ""


def live_idf_version(env):
    """'ESP-IDF v5.5.4' / 'ESP-IDF v5.5.4-dirty' -> 5.5.4"""
    out = subprocess.run(["idf.py", "--version"], env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True).stdout
    found = re.findall(r"ESP-IDF v([0-9][0-9.]*)", out)
    return found[-1] if found else ""


def assert_idf_matches_lock(env):
    """dependencies.lock records the IDF that produced the last committed build. Another IDF still
    boots, but the whole binary shifts: the delta patch balloons (45 KB became 854 KB) and the lock
    is silently rewritten. Checked BEFORE the build, because the build itself rewrites the lock."""
    lock = ESP / "dependencies.lock"
    want = lock_idf_version(lock.read_text()) if lock.is_file() else ""
    live = live_idf_version(env)
    if not want:
        say("no IDF version in dependencies.lock -- skipping toolchain check")
        return
    if not live:
        die("could not determine the live IDF version from 'idf.py --version'")
    if want != live:
        die(f"IDF MISMATCH: dependencies.lock expects v{want} but the sourced IDF is v{live} "
            f"(IDF_PATH={env.get('IDF_PATH', 'unset')}).\n"
            f"     Source the matching export.sh and rebuild, e.g.  source ~/esp/esp-idf-v{want}/export.sh\n"
            "     Building on the wrong IDF still boots but shifts the whole binary: the delta-OTA patch\n"
            "     balloons and dependencies.lock is silently rewritten. If the bump is INTENTIONAL, re-run with\n"
            "     ESP32_ALLOW_IDF_MISMATCH=1 and commit the resulting dependencies.lock change deliberately.")
    say(f"IDF v{live} matches dependencies.lock")


def esp_archived_image(want):
    """The archived full image for a version int in this target's namespace, or None."""
    for f in sorted(ESP_IMG.glob(f"{esp_target()}-hisense_ac_matter-v*.bin")):
        m = re.search(r"-v([0-9]*\.[0-9]*\.[0-9]*)(-DELTA-BASE)?\.bin$", str(f))
        try:
            if m and esp_semver_to_int(m.group(1)) == want:
                return f
        except Die:
            continue
    return None


def esp_release_env():
    """The IDF + esp-matter environment: the caller's when it already has idf.py (CI sources
    export.sh first), else sourced here."""
    return dict(os.environ) if have("idf.py") else esp_env()


def build_esp32():
    # The env file is loaded here too: the recovery-credential guard below must see BREAKGLASS_TOKEN.
    cfg = load_env(("VID", "PID"))
    env = esp_release_env()
    if not shutil.which("idf.py", path=env.get("PATH")):
        die("idf.py not on PATH -- source the IDF + esp-matter env first")
    if cfg.get("ESP32_ALLOW_IDF_MISMATCH") == "1":
        lock = ESP / "dependencies.lock"
        say("WARNING: ESP32_ALLOW_IDF_MISMATCH=1 -- toolchain check skipped (lock "
            f"v{lock_idf_version(lock.read_text()) if lock.is_file() else ''} vs live v{live_idf_version(env)})")
    else:
        assert_idf_matches_lock(env)
    semver, v, rel = esp_semver(), esp_int(), esp_released_int()
    run(["bash", HERE / "esp32-lint.sh"])   # PROJECT_VER <-> sdkconfig sync + semver bounds
    if v <= rel:
        die(f"PROJECT_VER {semver} (int {v}) is not > last released ({rel}) -- bump PROJECT_VER + sdkconfig (#77)")
    # #82 gate: the currently-DEPLOYED image is the delta base for the NEXT release. It must be
    # archived before this build overwrites build/.
    if rel > 0:
        if esp_archived_image(rel) is None:
            die(f"released image (int {rel}) not archived in built-images/ -- recover the exact deployed "
                ".bin FIRST (#82); refusing to overwrite build/")
        say(f"#82 ok: released base (int {rel}) is archived")
    else:
        say("#82: no prior release recorded -- first build, nothing to preserve")
    # Recovery credentials. The Identify=88 OTA fetch target and the :2324 break-glass listener are
    # baked at BUILD time and are the only two remote ways back into this node. Without them the
    # image looks healthy and is quietly USB-only (node 35, 2026-07-20). ota-release.env defines
    # BREAKGLASS_TOKEN / BREAKGLASS_PORT but CMakeLists.txt consumes the HISENSE_* names, so accept
    # either spelling and export the names the build reads. Export only the ones that carry a
    # value: CMake gates on DEFINED ENV{...}, and an exported-but-EMPTY port becomes htons() with
    # no argument and kills the build.
    creds = {"HISENSE_OTA_URL": cfg.get("HISENSE_OTA_URL", ""),
             "HISENSE_BREAKGLASS_TOKEN": cfg.get("HISENSE_BREAKGLASS_TOKEN") or cfg.get("BREAKGLASS_TOKEN", ""),
             "HISENSE_BREAKGLASS_PORT": cfg.get("HISENSE_BREAKGLASS_PORT") or cfg.get("BREAKGLASS_PORT", "")}
    for k, val in creds.items():
        if val:
            env[k] = val
        else:
            env.pop(k, None)
    if cfg.get("ESP32_ALLOW_NO_RECOVERY") != "1":
        if not creds["HISENSE_OTA_URL"]:
            die("HISENSE_OTA_URL is unset -- the Identify=88 OTA fetch would bake a placeholder URL and\n"
                "     the image would be USB-only. Set it (ota-release.env or the environment), or pass\n"
                "     ESP32_ALLOW_NO_RECOVERY=1 if you really want a bench image with no remote recovery.")
        if not creds["HISENSE_BREAKGLASS_TOKEN"]:
            die("no break-glass token (set BREAKGLASS_TOKEN or HISENSE_BREAKGLASS_TOKEN) -- the :2324\n"
                "     listener fails closed and never opens, so the image would be USB-only. Set it, or pass\n"
                "     ESP32_ALLOW_NO_RECOVERY=1.")
        say("recovery credentials present (OTA URL + break-glass token exported to the build)")
    else:
        say("WARNING: ESP32_ALLOW_NO_RECOVERY=1 -- image will have NO remote recovery path (USB only)")
    # Flavour. The dev node runs DEBUG on purpose, and the :2323 console is gated on
    # CONFIG_HISENSE_DEBUG_BUILD, which lives only in sdkconfig.debug. Debug is the default here;
    # opt out with ESP32_FLAVOUR=release (an env var, there is no --release flag).
    flavour = cfg.get("ESP32_FLAVOUR") or "debug"
    sdkdef = "sdkconfig.defaults"
    if flavour == "debug":
        sdkdef = "sdkconfig.defaults;sdkconfig.debug"
        say("flavour: DEBUG (:2323 console + tx probe)")
    else:
        say("flavour: RELEASE (no console) -- node 28 normally wants debug")
    # The target comes from the existing sdkconfig (ESP32_TARGET overrides). A hardcoded esp32 once
    # flipped an esp32c3 tree back to Xtensa and wiped its build/ with nothing in the log saying so.
    target = cfg.get("ESP32_TARGET") or esp_target()
    say(f"idf.py build ({semver}, int {v}, target {target})")
    run(["idf.py", f"-DSDKCONFIG_DEFAULTS={sdkdef}", "set-target", target], env=env, cwd=ESP)
    run(["idf.py", f"-DSDKCONFIG_DEFAULTS={sdkdef}", "build"], env=env, cwd=ESP)
    if not ESP_NEW_BIN.is_file():
        die(f"build produced no {ESP_NEW_BIN}")
    if flavour == "debug":   # fail loudly rather than ship a consoleless image by accident
        if not re.search(r"^CONFIG_HISENSE_DEBUG_BUILD=y", (ESP / "sdkconfig").read_text(), re.M):
            die("debug flavour requested but CONFIG_HISENSE_DEBUG_BUILD is not set in the generated "
                "sdkconfig -- the :2323 console would be MISSING from this image")
        say("verified: CONFIG_HISENSE_DEBUG_BUILD=y (console present)")
    ESP_IMG.mkdir(parents=True, exist_ok=True)
    archive = ESP_IMG / f"{esp_target()}-hisense_ac_matter-v{semver}.bin"
    shutil.copyfile(ESP_NEW_BIN, archive)
    say(f"archived fresh image -> {archive}")


def package_esp32(args=()):
    cfg = load_env(("VID", "PID"))
    full = bool(args) and args[0] == "--full"
    semver, v, rel = esp_semver(), esp_int(), esp_released_int()
    # The ESP32 commissions with esp-matter's TEST PID 0x8000, DISTINCT from the AmebaZ2's 0x8001.
    # matter-server only serves an OTA whose manifest pid matches the device's.
    pid = cfg.get("ESP32_PID") or "0x8000"
    if not ESP_NEW_BIN.is_file():
        die(f"no {ESP_NEW_BIN} -- run build first")
    tool = cfg.get("OTA_IMAGE_TOOL") or die("set OTA_IMAGE_TOOL to the connectedhomeip src/app/ota_image_tool.py path")
    ota, manifest, patch = ESP_IMG / f"esp32-v{v}.ota", ESP_IMG / f"esp32-v{v}.json", ESP_IMG / f"esp32-v{v}.patch"
    otaurl = f"file:///esp32-v{v}.ota"
    # The flavour is read from the image bytes, the only place it can be read: nothing on the wire
    # reports a running node's flavour, so ESP32_NODE_FLAVOUR records what the target node runs.
    want = cfg.get("ESP32_FLAVOUR") or "debug"
    node_flavour = cfg.get("ESP32_NODE_FLAVOUR", "")
    if node_flavour and node_flavour != want:
        die(f"node {cfg.get('ESP32_NODE_ID', '')} runs the {node_flavour} flavour but ESP32_FLAVOUR is {want} "
            f"-- rebuild with ESP32_FLAVOUR={node_flavour}")
    have_flavour = ota_guards.image_flavour(ESP_NEW_BIN.read_bytes(), ESP_CONSOLE_MARK)
    why = ota_guards.flavour_mismatch(have_flavour, want)
    if why:
        die(f"{why} ({ESP_NEW_BIN})")
    say(f"  flavour ok: {have_flavour}")
    guard_functional_delta("esp32-v*", "firmware/esp32-matter/main", "firmware/esp32-matter/components",
                           "firmware/src/rs485-driver")
    # Clear this int's old outputs, so a package that dies half way cannot leave an older build's
    # .ota/.json for stage to ship.
    ESP_IMG.mkdir(parents=True, exist_ok=True)
    for f in (ota, manifest, patch):
        f.unlink(missing_ok=True)
    if cfg.get("OTA_RELEASE_BASE"):
        otaurl = f"{cfg['OTA_RELEASE_BASE'].rstrip('/')}/esp32-v{semver}/esp32-v{v}.ota"
    if full or rel == 0:
        say("packaging FULL image (--full)" if full else "packaging FULL image (no prior release to delta against)")
        say("NOTE: a delta-OTA-enabled device REJECTS a full image -- only use --full for the first flash / "
            "a base recovery")
        payload = ESP_NEW_BIN
    else:
        gen = cfg.get("DELTA_PATCH_GEN") or die("set DELTA_PATCH_GEN to esp_delta_ota_patch_gen.py")
        idf_py = cfg.get("IDF_PYTHON") or die("set IDF_PYTHON to the IDF python env (has detools+esptool)")
        if subprocess.run([idf_py, "-c", "import detools"], stderr=subprocess.DEVNULL).returncode != 0:
            die(f"IDF_PYTHON ({idf_py}) cannot import detools -- {idf_py} -m pip install detools")
        base = esp_archived_image(rel) or die(f"delta base for int {rel} not in built-images/ (#82)")
        payload = patch
        say(f"delta patch vs base {base.name} -> {payload.name}")
        run([idf_py, gen, "create_patch", "--chip", esp_target(), "--base_binary", base,
             "--new_binary", ESP_NEW_BIN, "--patch_file_name", payload])
    # minApplicableSoftwareVersion=0 (NOT 1 like AmebaZ2): this firmware leaves the softwareVersion
    # INT unwired (reports 0), so an OTA with min=1 is never offered to it.
    say(f"wrap {payload.name} as {ota.name} (vn={v} vs={semver} vid={cfg['VID']} pid={pid})")
    r = subprocess.run(["python3", tool, "create", "-v", cfg["VID"], "-p", pid, "-vn", str(v), "-vs", semver,
                        "-da", "sha256", "-mi", "0", "-ma", str(v - 1), str(payload), str(ota)],
                       stdout=subprocess.DEVNULL)
    if r.returncode != 0:
        die("ota_image_tool.py create failed")
    manifest.write_text(json.dumps(ota_manifest(ota.read_bytes(), v, semver, otaurl, int(cfg["VID"], 0),
                                                int(pid, 0), 0)) + "\n")
    say(f"  ota:  {ota}")
    say(f"  json: {manifest}  (otaUrl={otaurl})")


def stage_esp32():
    cfg = load_env(("VID", "PID"))
    need(cfg, "PI_HOST", "PI_OTA_DIR", "PI_SSH_KEY")
    v = esp_int()
    ota, manifest = ESP_IMG / f"esp32-v{v}.ota", ESP_IMG / f"esp32-v{v}.json"
    say(f"stage esp32-v{v} on {cfg['PI_HOST']}:{cfg['PI_OTA_DIR']} + restart matter-server")
    guard_fresh(ESP_NEW_BIN, ota, manifest)
    pi_stage(cfg, manifest, ota)
    # Mirror the RAW image too, so the break-glass full-image path always has the deployed bin
    # (losing it stranded node 80). esp32-ota.bin is the compile-time URL C3 nodes fetch.
    flavour = cfg.get("ESP32_FLAVOUR") or "debug"
    pi_http_publish(cfg, ESP_NEW_BIN, "esp32-ota.bin", f"{esp_target()}-v{v}-{flavour}.bin", ESP_CONSOLE_MARK)


def flash_esp32():
    cfg = load_env(("VID", "PID"))
    need(cfg, "OTAENV_PY", "MS_WS")
    node = cfg.get("ESP32_NODE_ID") or die("set ESP32_NODE_ID in ota-release.env (the ESP32 node, e.g. 28)")
    v, semver = esp_int(), esp_semver()
    say(f"pre-flight: tools + link to node {node}")
    guard_tools(cfg)
    guard_link(cfg, node)
    since = pi_now(cfg)
    say(f"flash esp32-v{v} ({semver}) to node {node} (retries; verify the reported version changed)")
    # update_node selects the OTA by the INT, but the result is verified by the STRING (0/40/10):
    # this firmware leaves the int (0/40/9) unwired, so checking it would never confirm success.
    if ws_program(cfg, "flash", cfg["MS_WS"], node, v, "esp32", semver)[0] != 0:
        die(f"flash verification failed for v{v} -- version string not sustained or the subscription "
            "gate failed (#64); see the [flash] lines above")
    esp_released_mark().write_text(f"{v}\n")
    say(f"recorded on-device version {v} ({esp_target()})")
    check_subscription_log(cfg, node, since)


def tag_esp32():
    semver = esp_semver()
    make_tag(f"esp32-v{semver}", f"ESP32 firmware {semver} (softwareVersion {esp_int()})", f"tagged esp32-v{semver}")


def publish_esp32():
    """Upload the DEPLOYED artifacts (#89). Most acute here: delta OTA embeds the BASE image's hash
    and the device verifies it against its running partition, so a patch built against a CI rebuild
    is rejected. The release asset has to be the archived deployed .bin."""
    if not shutil.which("gh"):
        die("gh not on PATH -- needed to upload release assets")
    rel = esp_released_int()
    if rel == 0:
        die("no on-device version recorded -- run 'flash' first")
    tag = f"esp32-v{int_to_semver(rel)}"
    if subprocess.run(["gh", "release", "view", tag], cwd=str(REPO), stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode != 0:
        die(f"no release {tag} -- push the tag first")
    base = esp_archived_image(rel) or die(f"deployed image (int {rel}) not archived in built-images/ (#82) "
                                          "-- nothing trustworthy to publish")
    n = gh_release_upload(tag, [base, ESP_IMG / f"esp32-v{rel}.ota", ESP_IMG / f"esp32-v{rel}.json"])
    say(f"published {n} deployed artifact(s) to {tag} -- THIS is the valid delta base for the next release")


def release_esp32(args):
    for a in args:
        if a != "--flash":
            die(f"unknown flag for release: {a}")
    build_esp32()
    package_esp32()
    stage_esp32()
    if "--flash" in args:
        flash_esp32()
    else:
        say("staged, not flashed. run: dev.py ota esp32 flash")


# ---- ota: every release step of both Matter targets ----------------------------------------
def ota_preflight(node_key):
    cfg = load_env(())
    need(cfg, node_key, "MS_WS")
    run(["bash", TEST / "run_tests.sh"], check=False)
    ok("host QA incl. OTA guard tests")
    guard_tools(cfg)
    guard_link(cfg, cfg[node_key])
    say(f"Pi clock reachable: {pi_now(cfg)}")


def ota_verify(node_key, attr, want, also):
    """Read the node's live version and compare it with the tree. `also` is a second accepted
    spelling (the int form of the same version)."""
    cfg = load_env(())
    need(cfg, node_key, "MS_WS", "OTAENV_PY")
    node = cfg[node_key]
    got = ws_program(cfg, "read", cfg["MS_WS"], node, attr, capture=True)[1]
    link = subprocess.run([cfg["OTAENV_PY"], str(HERE / "ota_guards.py"), "link", cfg["MS_WS"], str(node), "-200"],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout.strip()
    say(f"node {node} live version: {got}   (tree: {want})")
    say(f"link: {link}")
    if got not in (want, also):
        die(f"node {node} reports {got}, expected {want}")
    ok(f"node {node} is running {got}")


def ota(ctx, step, rest):
    if ctx.target == "esphome":
        die("ota is Matter-only (amebaz2 or esp32); ESPHome updates go through esphome run")
    if ctx.target == "amebaz2":
        steps = {
            "lint": lint,
            "verint": lambda: print(semver_to_int(rest[0] if rest and rest[0] else cur_semver())),
            "epoch": lambda: print(image_epoch()),
            "tag": tag_release,
            "build": lambda: build_amebaz2(rest),
            "package": package_amebaz2,
            "stage": stage_amebaz2,
            "flash": flash_amebaz2,
            "release": lambda: release_amebaz2(rest),
            "publish": publish_amebaz2,
            "revert": lambda: revert(rest),
            "preflight": lambda: ota_preflight("NODE_ID"),
            "verify": lambda: ota_verify("NODE_ID", "0/40/9", cur_semver(), str(cur_version())),
        }
        if step not in steps:
            die("ota amebaz2 step must be one of: " + ", ".join(steps))
        steps[step]()
        return
    steps = {
        "verint": lambda: print(esp_int()),
        "tag": tag_esp32,
        "build": build_esp32,
        "package": lambda: package_esp32(rest),
        "stage": stage_esp32,
        "flash": flash_esp32,
        "release": lambda: release_esp32(rest),
        "publish": publish_esp32,
        "preflight": lambda: ota_preflight("ESP32_NODE_ID"),
        "verify": lambda: ota_verify("ESP32_NODE_ID", "0/40/10", esp_semver(), str(esp_int())),
    }
    if step not in steps:
        die("ota esp32 step must be one of: " + ", ".join(steps))
    steps[step]()


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
    # A PERMANENT directory, never a temp one: ESP-IDF's install.sh creates its Python env from
    # the first python3 on PATH, and a venv's interpreter is a symlink to exactly that path. A shim
    # removed at exit leaves every env built through it with a dangling python3, and the next
    # install.sh then fails to recreate it.
    shim = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"),
                        "w41h1-dev", "esp-python")
    os.makedirs(shim, exist_ok=True)
    for name in ("python3", "python"):
        link = os.path.join(shim, name)
        if os.path.realpath(link) != os.path.realpath(py) or not os.path.islink(link):
            if os.path.lexists(link):
                os.remove(link)
            os.symlink(py, link)
    os.environ["PATH"] = f"{shim}{os.pathsep}{os.environ.get('PATH', '')}"
    say(f"ESP python: {py_minor(py) or '?'} ({py})")


def main(argv):
    if not argv:
        usage(1)
    if argv[0] in ("-h", "--help", "help"):
        usage(0)
    cmd = argv[0]
    if cmd == "_ws":   # internal: a websocket program, re-run under OTAENV_PY (see ws_program)
        ws_main(argv[1:])
        return
    target = argv[1] if len(argv) > 1 else ""
    if target not in ("amebaz2", "esp32", "esphome"):
        die(f"target must be amebaz2, esp32 or esphome (got '{target}')")
    rest = argv[2:]
    # Only where a build can follow: the note it prints must not land in `ota esp32 verint` output.
    if target == "esp32" and (cmd != "ota" or (rest[:1] or [""])[0] in ("build", "release")):
        use_esp_python()

    # `ota` forwards everything after the step to the release script, so parse it before the
    # option loop (release flags like --flash are not dev.py options).
    if cmd == "ota":
        step = rest[0] if rest else ""
        ota(Ctx(target, "c3", None, None), step, rest[1:])
        return

    board, port, sim_port, name, friendly_name, i = "c3", None, None, None, None, 0
    while i < len(rest):
        a = rest[i]
        if a == "--board" and i + 1 < len(rest):
            board = rest[i + 1]; i += 2
        elif a == "--port" and i + 1 < len(rest):
            port = rest[i + 1]; i += 2
        elif a == "--sim-port" and i + 1 < len(rest):
            sim_port = rest[i + 1]; i += 2
        elif a == "--name" and i + 1 < len(rest):
            name = rest[i + 1]; i += 2
        elif a == "--friendly-name" and i + 1 < len(rest):
            friendly_name = rest[i + 1]; i += 2
        else:
            die(f"unknown option: {a}")
    if (name or friendly_name) and target != "esphome":
        die("--name / --friendly-name are esphome-only (they set the node's hostname and HA name)")
    if name and not re.fullmatch(r"[a-z0-9]([a-z0-9-]{0,29}[a-z0-9])?", name):
        die(f"--name '{name}' is not a valid hostname (lowercase letters, digits, hyphens; max 31)")
    ctx = Ctx(target, board, port, sim_port, name, friendly_name)
    if board in ESPHOME_AMEBAZ2_BOARDS and cmd not in ("doctor", "fetch", "test", "build"):
        die(ESPHOME_AMEBAZ2_ONLY)

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
