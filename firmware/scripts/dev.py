#!/usr/bin/env python3
"""dev.py -- one portable entry point for the ESPHome firmware: check the tools, test, build, flash
and bench it, and move a stock module off the retired Matter firmware (issues #118, #143, #182).

Every external command is printed before it runs. Run it as
`python3 firmware/scripts/dev.py <cmd> esphome [opts]`. It needs only python3.

Commands:
  walk    esphome                        guided: doctor -> test -> build -> flash -> next (y/N each)
  doctor  esphome                        check the tools and the ESPHome version (read-only)
  fetch   esphome                        install the pinned ESPHome with pipx (asks first)
  test    esphome                        host QA (run_tests.sh) + `esphome config`
  build   esphome                        compile the firmware
  erase   esphome --port P               erase-flash, BRAND-NEW ESP32 boards only (typed confirmation)
  flash   esphome --port P               flash over USB + logs
  monitor esphome --port P               serial logs only
  bench   esphome --port P --sim-port S  the node against virtual_ac.py over a USB adapter, no A/C
  next    esphome                        print the staged bring-up and its safety warnings
  convert esphome --board amebaz2-factory [--name N] [--software-version V] [--serial S] [--stage]
                                         wrap the built ESPHome image as a Matter .ota, so the
                                         retired Matter firmware on a stock module installs it (below)
  matter-slots  <unit-ip>                read both firmware slots' serials from a unit that still
                                         runs the retired Matter firmware (read-only)
  matter-backup <unit-ip> [out.bin]      save that unit's inactive slot, and print its address
                                         (which tells the flash layout; read-only)

Board: --board c3 (ESP32-C3 SuperMini, default), --board c3-plus (ESP32-C3 SuperMini Plus, the
one with an RGB pixel on GPIO8) or --board classic (ESP32-D0WDQ6), the ESP32 replacement board.
--board amebaz2-factory, amebaz2-sdk or amebaz2-native is the stock module
through LibreTiny, by the flash layout the unit has: test, build and convert (factory has run on a
module, sdk and native have not). Node identity: --name <hostname> and --friendly-name <text>
(default hisense-ac / "Air Conditioner"; give every node after the first its own). Env: ESPHOME
(esphome command, default `esphome`), ENVF (the settings file for convert and the matter-*
commands, default firmware/scripts/ota-release.env). ESPHome builds reuse compiled objects through
PLATFORMIO_BUILD_CACHE_DIR (default ~/.cache/w41h1-dev/pio-build-cache; set it yourself to move it,
or to an empty string to switch it off).

convert (docs/guide/Converting-a-Stock-Module-to-ESPHome.md): run `dev.py build esphome --board
amebaz2-factory` first. It re-signs that image with an FWHS serial above the running Matter image
(default SERIAL_BASE + version, the Matter build's own rule) and wraps it as a Matter .ota at a
softwareVersion one above the last Matter release (default), with its provider manifest, in
firmware/built-images/. --stage also puts both on the OTA provider host and restarts matter-server.
It needs connectedhomeip's ota_image_tool.py (OTA_TOOL in the settings file).

The Matter firmwares themselves (AmebaZ2 and esp-matter builds, their release steps, the way back
to the vendor firmware over the air) are retired. The last tree that has them is the tag
`matter-final`.
"""

import base64
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
ESPHOME_DIR = REPO / "firmware/esphome"
TEST = REPO / "firmware/test"
# The settings file for `convert` and the matter-* commands (see ota-release.env.example).
ENVF = Path(os.environ.get("ENVF") or HERE / "ota-release.env")

sys.path.insert(0, str(HERE))   # amebaz2_image, imported where it is used

RETIRED = ("the Matter firmwares are retired (issue #182). dev.py builds ESPHome only; the last tree "
           "with the amebaz2 and esp32 targets and the `ota` steps is the tag matter-final")
ESPHOME_AMEBAZ2_BOARDS = ("amebaz2-factory", "amebaz2-sdk", "amebaz2-native")
ESPHOME_AMEBAZ2_ONLY = ("esphome --board amebaz2-* has test, build and convert. A first flash is `convert` (over the "
                        "air from the retired Matter firmware), the clip or UART download mode, and later updates are "
                        "`esphome upload` (firmware/docs/15-esphome-path.md)")
# First slot address (LibreTiny names the app image after it) and slot length, per layout. The
# host test holds this equal to firmware/esphome/components/w41h1_module/layout-*.json.
ESPHOME_AMEBAZ2_SLOTS = {"amebaz2-factory": (0x010000, 0x170000), "amebaz2-sdk": (0x00C000, 0x1AC000),
                         "amebaz2-native": (0x010000, 0x1AC000)}
# Layouts a unit running the Matter firmware can have. native is absent: it has LibreTiny's
# bootloader and no Matter firmware to convert from.
ESPHOME_CONVERT_BOARDS = ("amebaz2-factory", "amebaz2-sdk")
ESPHOME_PIN = "2026.7.4"   # the release CI builds with; version-lint.py fails when the pins differ

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



class Ctx:
    """Parsed invocation: board-derived pins and options."""
    def __init__(self, target, board, port, sim_port, name=None, friendly_name=None):
        self.convert_opts = {}
        self.target = target
        self.board = board
        self.port = port
        self.sim_port = sim_port
        self.name = name
        self.friendly_name = friendly_name
        if board == "c3":
            self.pins, self.esphome_board = (5, 6, 10), "esp32-c3-devkitm-1"
        elif board == "c3-plus":
            # Same chip and pins as c3. It has a board file of its own for the RGB pixel on GPIO8.
            self.pins, self.esphome_board = (5, 6, 10), "esp32-c3-devkitm-1"
        elif board == "classic":
            self.pins, self.esphome_board = (19, 18, 4), "esp32dev"
        elif board in ESPHOME_AMEBAZ2_BOARDS:
            # The stock module's own MCU through LibreTiny. Its pins are fixed by the module and
            # live in the board file, so nothing is passed as a substitution.
            self.pins, self.esphome_board = ("PA14", "PA13", "PA17"), "cr3l"
        elif board == "amebaz2":
            # No default: an image built for the wrong layout bricks the unit at its first update.
            die("--board amebaz2 needs the unit's flash layout: amebaz2-factory, amebaz2-sdk or "
                "amebaz2-native (firmware/docs/15-esphome-path.md, 'Which flash layout a unit has')")
        else:
            die("--board must be c3, c3-plus, classic, amebaz2-factory, amebaz2-sdk or amebaz2-native")

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

    def esphome_build_dir(self):
        name = self.name or "hisense-ac"
        return ESPHOME_DIR / ".esphome/build" / name / ".pioenvs" / name

    def esphome_slot_image(self):
        """The application image LibreTiny builds for a firmware slot (stock module boards only).
        `convert` re-signs it, and its name carries the first slot's address."""
        return self.esphome_build_dir() / f"image_firmware_is.0x{ESPHOME_AMEBAZ2_SLOTS[self.board][0]:06X}.bin"

    def esphome_run(self, sub, *args, cache=True):
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
        elif self.board == "c3-plus":
            # The board file holds the pins and the status pixel, so nothing is passed.
            config, subs = "w41h1-esp32c3-plus.yaml", []
        # A second node needs its own hostname: two boards both called hisense-ac fight over the
        # same mDNS name and Home Assistant device.
        if self.name:
            subs += ["-s", "name", self.name]
        if self.friendly_name:
            subs += ["-s", "friendly_name", self.friendly_name]
        env = esphome_env(os.environ) if cache else {k: v for k, v in os.environ.items()
                                                      if k != "PLATFORMIO_BUILD_CACHE_DIR"}
        run([cmd, *subs, sub, config, *args], cwd=ESPHOME_DIR, env=env)


def esphome_env(base):
    """The environment for an esphome call: the caller's, plus a PlatformIO build cache unless one
    is already chosen. PlatformIO then keeps every compiled object by content, so a clean build, a
    second node name (its own build directory) or a fresh worktree reuses them. Measured on an
    AmebaZ2 image: 29 s cold, 18 s into a new build directory, 6 s after a clean with nothing
    changed, 77 MB in the cache per layout. It does nothing for the ESP32 boards, which ESPHome
    builds with ESP-IDF's own tools and ccache. An empty PLATFORMIO_BUILD_CACHE_DIR in the
    caller's environment is respected and switches the cache off."""
    env = dict(base)
    if "PLATFORMIO_BUILD_CACHE_DIR" not in env:
        cache = env.get("XDG_CACHE_HOME") or os.path.join(env.get("HOME") or os.path.expanduser("~"), ".cache")
        env["PLATFORMIO_BUILD_CACHE_DIR"] = os.path.join(cache, "w41h1-dev", "pio-build-cache")
    elif not env["PLATFORMIO_BUILD_CACHE_DIR"]:
        del env["PLATFORMIO_BUILD_CACHE_DIR"]
    return env



# ---- doctor ----------------------------------------------------------------------------------
def have_tool(tool, gaps, note=""):
    if have(tool):
        ok(tool)
    else:
        gaps.append(f"{tool} not on PATH{f' ({note})' if note else ''}")


def doctor(ctx):
    gaps = []
    say("doctor: esphome")
    have_tool("git", gaps); have_tool("python3", gaps)
    have_tool("g++", gaps, "the host tests compile C++: sudo apt install g++, or pacman -S gcc")
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
    ctx.esphome_run("config")


def build(ctx):
    ctx.esphome_run("compile")
    if ctx.board in ESPHOME_AMEBAZ2_BOARDS and not ctx.esphome_slot_image().is_file():
        # When nothing changed since a clean, the build cache hands back the linked firmware
        # and the .uf2 whole. LibreTiny writes the slot images as a side effect of the link
        # step, which then never runs, so they are missing. One build without the cache
        # links again and writes them.
        say("the build came whole from the build cache, without the slot images: building once without the cache")
        for f in ("raw_firmware.elf", "firmware.uf2"):
            (ctx.esphome_build_dir() / f).unlink(missing_ok=True)
        ctx.esphome_run("compile", cache=False)
        if not ctx.esphome_slot_image().is_file():
            die(f"the build did not produce {ctx.esphome_slot_image().name}: was the layout file applied?")


def erase(ctx):
    ctx.need_port("erase")
    warn("erase-flash is for a BRAND-NEW board only: it wipes NVS, which on a working node holds the")
    warn("Wi-Fi config. A factory board needs it (the vendor's test firmware leaves a stale one behind).")
    try:
        confirm = input(f"type ERASE to erase {ctx.port}: ").strip()
    except EOFError:
        confirm = ""
    if confirm != "ERASE":
        die("not confirmed; nothing erased")
    cmd = ctx.esphome_cmd()
    py = Path(os.path.realpath(subprocess.run(["bash", "-c", f"command -v {cmd}"],
              stdout=subprocess.PIPE, text=True).stdout.strip() or cmd)).parent / "python"
    py = str(py) if py.is_file() and os.access(py, os.X_OK) else "python3"
    run([py, "-m", "esptool", "--port", ctx.port, "erase_flash"])


def flash(ctx):
    ctx.need_port("flash")
    ctx.esphome_run("run", "--device", ctx.port)


def monitor(ctx):
    ctx.need_port("monitor")
    ctx.esphome_run("logs", "--device", ctx.port)


def bench(ctx):
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
    ctx.esphome_run("run", "--no-logs", "--device", ctx.port)
    say(f"starting virtual_ac.py on {ctx.sim_port} (Ctrl-C stops both)")
    sim = subprocess.Popen([sys.executable, str(TEST / "virtual_ac.py"), "--port", ctx.sim_port])
    try:
        say("PASS looks like: sim prints '[0x0A] handshake poll -> echoed slave reply', then the node's")
        say("log shows the bus link up and status frames decoding about once a second.")
        ctx.esphome_run("logs", "--device", ctx.port)
    finally:
        sim.terminate()


def next_steps(ctx):
    print("""Staged bring-up for the ESP32 replacement board (never leave the A/C in an unknown state):

  1. Bench, no A/C     dev.py test esphome; then dev.py bench esphome --port P --sim-port S
  2. Real bus, USB     module out, tap A/B ONLY, watch decoded status (read), then one control (write)
  3. Integration       power from the connector's 5 V, no laptop attached, close it up

Safety, before stage 2:
  ! Ground loop: while USB-powered connect ONLY A/B. Joining the mains-earthed A/C GND to a
    laptop-earthed board browns it out (RTCWDT resets, flash-read errors). GND/5V join at stage 3.
  ! 3.3 V transceiver only (MAX3485 / SP3485 / SN65HVD75). A 5 V MAX485 module's RO kills the RX pin.
  ! ESP32-C3: fit a ~10k pulldown on DE (GPIO10). DE floats until the firmware configures it and a
    high DE parks a second driver on the A/C bus. Never put the UART on GPIO18/19 (the C3's only USB).
  ! Classic ESP32: never GPIO16/17 on WROVER/D0WDQ6 (PSRAM-bonded, dead as I/O).
  ! Brand-new board: dev.py erase esphome --port P before the first flash (stale vendor NVS).
Full detail: docs/guide/ESP32-Replacement-Build.md, docs/guide/Build-Flash-Test.md
The stock module (--board amebaz2-*) is a different procedure: firmware/docs/15-esphome-path.md""")


def walk(ctx):
    say(f"guided flow for esphome (board: {ctx.board}). Each step asks first; N skips it.")
    if ask("1/5 doctor: check the tools and the ESPHome version?"):
        if not doctor(ctx) and ask("gaps found. run fetch?"):
            fetch(ctx)
    if ask("2/5 host QA + config check?"):
        test_target(ctx)
    if ask("3/5 build?"):
        build(ctx)
    if not ctx.port:
        try:
            ctx.port = input("serial port for flashing (blank to skip flash): ").strip()
        except EOFError:
            ctx.port = ""
    if ctx.port:
        if ask("   brand-new board that needs erase-flash first?"):
            erase(ctx)
        if ask(f"4/5 flash + logs {ctx.port}?"):
            flash(ctx)
    say("5/5 next steps")
    next_steps(ctx)


# ---- leaving the retired Matter firmware: convert, matter-slots, matter-backup -----------------
# A stock module that still runs this project's Matter firmware (tag matter-final) installs
# whatever its OTA provider sends into the slot it is not running from, and its bootloader boots
# the valid slot with the higher FWHS serial. `convert` re-signs an ESPHome image with a serial in
# the Matter numbering and wraps it as a Matter .ota. The two matter-* commands read a unit's slot
# state over that firmware's break-glass listener, which is how the flash layout is found.
#
# The last Matter release of the AmebaZ2 build (1.3.50), as its softwareVersion int
# (MAJOR*10000 + MINOR*100 + PATCH). A conversion image has to be offered as a newer version.
MATTER_FINAL_VERSION = 10350


def int_to_semver(v):
    return f"{v // 10000}.{(v // 100) % 100}.{v % 100}"


def _env0(args, env=None, cwd=None):
    """(returncode, env dict) from a command whose stdout is `env -0`."""
    p = subprocess.run(args, stdout=subprocess.PIPE, env=env, cwd=str(cwd) if cwd else None)
    out = {}
    for chunk in p.stdout.split(b"\0"):
        if b"=" in chunk:
            k, v = chunk.split(b"=", 1)
            out[k.decode("utf-8", "replace")] = v.decode("utf-8", "replace")
    return p.returncode, out


def load_env(required=()):
    """The settings: this process's environment with ota-release.env sourced over it (the file is
    bash, with $HOME and cross-references). Returned as a lookup dict only. It is NOT handed to
    child processes."""
    if not ENVF.is_file():
        die(f"missing {ENVF} -- copy ota-release.env.example and fill it in")
    rc, cfg = _env0(["bash", "-c", 'set -a; . "$1" >/dev/null || exit 97; set +a; env -0', "bash", str(ENVF)])
    if rc != 0:
        die(f"could not source {ENVF}")
    for k in required:
        if not cfg.get(k):
            die(f"{k} is not set (in {ENVF})")
    return cfg


def need(cfg, *keys):
    for k in keys:
        if not cfg.get(k):
            die(f"{k} is not set (in {ENVF})")


def ota_manifest(ota_bytes, v, semver, otaurl, vid=0xFFF1, pid=0x8001, min_version=1):
    """The python-matter-server provider manifest for one .ota (key order is the wire order)."""
    return {"modelVersion": {
        "vid": vid, "pid": pid, "softwareVersion": v, "softwareVersionString": semver,
        "cdVersionNumber": 1, "firmwareInformation": "", "softwareVersionValid": True,
        "otaUrl": otaurl, "otaFileSize": len(ota_bytes),
        "otaChecksum": base64.b64encode(hashlib.sha256(ota_bytes).digest()).decode(),
        "otaChecksumType": 1, "minApplicableSoftwareVersion": min_version,
        "maxApplicableSoftwareVersion": v - 1, "releaseNotesUrl": ""}}



ARCHIVE_SUFFIX = ".archived"


def archive_plan(keep, manifests):
    """Manifests to archive so KEEP is the only active one for its vid/pid.

    manifests: file name -> parsed JSON (or None when unreadable). Unreadable files and other
    products are left alone, and so are stock-revert manifests (rac-stock-*): removing a way back
    is not this guard's call. The provider dir once held 90 active manifests."""
    def ident(doc):
        mv = (doc or {}).get("modelVersion") or {}
        return mv.get("vid"), mv.get("pid")
    if keep not in manifests or manifests[keep] is None:
        raise ValueError(f"keep manifest {keep} not in listing")
    want = ident(manifests[keep])
    return sorted(n for n, d in manifests.items()
                  if n != keep and d is not None and ident(d) == want and not n.startswith("rac-stock-"))


def parse_listing(text):
    """name<TAB>json lines -> {name: parsed JSON, or None when it does not parse}."""
    out = {}
    for line in text.splitlines():
        if "\t" not in line:
            continue
        name, body = line.split("\t", 1)
        name = os.path.basename(name.strip())
        try:
            out[name] = json.loads(body)
        except ValueError:
            out[name] = None
    return out


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
            plan = archive_plan(keep, parse_listing(listing))
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


SLOTS_RE = r"ok:\s*fw1_sn=(\d+)\s+fw2_sn=(\d+)\s+cur=(\d+)"
SLOT_MAX = 0x1AC000
IMAGE_KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e5f")


def serial_base(cfg):
    return int(cfg.get("SERIAL_BASE") or 1100)


def esphome_matter_image(image, serial, slot_len):
    """A LibreTiny AmebaZ2 app image (payload + 4-byte sum) re-signed with FWHS serial `serial`,
    as the bytes the Matter firmware's OTA writes into the inactive slot. Dies instead of returning
    anything the bootloader would refuse: it HANGS on a bad signature, with no fall-back (#75).

    The bootloader boots the valid slot with the higher serial, so `serial` has to be above the
    running Matter image's. LibreTiny builds with the same default key the Matter build signs with,
    which is why re-signing is all it takes."""
    import amebaz2_image
    if len(image) < amebaz2_image.HDR0 + amebaz2_image.HDR_LEN + 4:
        die(f"the ESPHome image is too short ({len(image)} bytes) to be an AmebaZ2 app image")
    payload, trailer = image[:-4], image[-4:]
    good, lines = amebaz2_image.verify(payload, trailer)
    if not good:
        die("the ESPHome image does not verify as built: " + "; ".join(amebaz2_image.failures(lines)))
    if len(image) > slot_len:
        die(f"the ESPHome image is {len(image):#x} bytes, the slot holds {slot_len:#x}")
    if not 0 < serial < 0xFFFFFFFF:
        die(f"serial {serial} is not a usable FWHS serial")
    out = amebaz2_image.resign(payload, serial=serial)
    out += amebaz2_image.bytesum(out)
    good, lines = amebaz2_image.verify(out[:-4], out[-4:])
    if not good or amebaz2_image.read_serial(out) != serial or len(out) != len(image):
        die("the re-signed image does not verify: " + "; ".join(amebaz2_image.failures(lines)))
    return out


def convert(ctx):
    """Wrap the built ESPHome image for a stock module as a Matter .ota (see the usage text)."""
    if ctx.target != "esphome" or ctx.board not in ESPHOME_CONVERT_BOARDS:
        die("convert is for esphome --board amebaz2-factory or amebaz2-sdk: it needs the Matter firmware "
            "running on the stock module to install the image (amebaz2-native has none)")
    opts = ctx.convert_opts
    cfg = load_env(("OTA_TOOL", "VID", "PID"))
    if opts.get("stage"):
        need(cfg, "PI_HOST", "PI_OTA_DIR", "PI_SSH_KEY")
    slot_len = ESPHOME_AMEBAZ2_SLOTS[ctx.board][1]
    name = ctx.name or "hisense-ac"
    src = ctx.esphome_slot_image()
    if not src.is_file():
        die(f"no {src} -- run: dev.py build esphome --board {ctx.board}" + (f" --name {name}" if ctx.name else ""))
    v = opts.get("software_version") or MATTER_FINAL_VERSION + 1
    serial = opts.get("serial") or serial_base(cfg) + v
    semver = int_to_semver(v)
    if ctx.board != "amebaz2-factory":
        warn(f"{ctx.board}: only the factory layout has been converted on hardware (2026-10-10)")
    say(f"convert {src.name} ({name}, {ctx.board[8:]} layout): FWHS serial {serial}, Matter softwareVersion {v}")
    import amebaz2_image
    image = src.read_bytes()
    say(f"  built image: {len(image):#x} bytes, serial {amebaz2_image.read_serial(image)}, "
        f"slot {slot_len:#x} ({100 * len(image) // slot_len} % used)")
    signed = esphome_matter_image(image, serial, slot_len)
    out = REPO / "firmware/built-images"
    out.mkdir(parents=True, exist_ok=True)
    raw, ota, manifest = (out / f"rac-v{v}-esphome.{ext}" for ext in ("bin", "ota", "json"))
    for f in (raw, ota, manifest):
        f.unlink(missing_ok=True)
    raw.write_bytes(signed)
    r = subprocess.run(["python3", cfg["OTA_TOOL"], "create", "-v", cfg["VID"], "-p", cfg["PID"],
                        "-vn", str(v), "-vs", semver, "-da", "sha256", "-mi", "1", "-ma", str(v - 1),
                        str(raw), str(ota)], stdout=subprocess.DEVNULL)
    if r.returncode != 0:
        die("ota_image_tool.py create failed")
    manifest.write_text(json.dumps(ota_manifest(ota.read_bytes(), v, semver, f"file:///{ota.name}")) + "\n")
    say(f"  image:    {raw}  (re-signed, every trailer and the byte-sum verified)")
    say(f"  ota:      {ota}  (+ .json manifest)")
    if opts.get("stage"):
        say(f"stage on {cfg['PI_HOST']}:{cfg['PI_OTA_DIR']} + restart matter-server")
        pi_stage(cfg, manifest, ota)
    else:
        say("  not staged: copy both to the OTA provider directory and restart matter-server, or re-run with --stage")
    say(f"next: check the unit's running serial is below {serial} (dev.py matter-slots <ip>),")
    say(f"  then install {semver} from the unit's update entity in Home Assistant. The unit reboots into ESPHome")
    say("  and leaves the Matter fabric: docs/guide/Converting-a-Stock-Module-to-ESPHome.md")


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


def matter_backup(args):
    import amebaz2_image
    cfg = load_env()
    need(cfg, "BREAKGLASS_TOKEN", "BREAKGLASS_PORT")
    ip = out = ""
    for a in args:
        if a.startswith("-"):
            die(f"unknown flag for matter-backup: {a}")
        elif not ip:
            ip = a
        elif not out:
            out = a
        else:
            die(f"unexpected argument for matter-backup: {a}")
    if not ip:
        die("usage: dev.py matter-backup <unit-ip> [out.bin]")
    token, port, base = cfg["BREAKGLASS_TOKEN"], int(cfg["BREAKGLASS_PORT"]), serial_base(cfg)
    say(f"matter-backup {ip}:{port} (fetch + validate the inactive-slot stock image)")

    def fail(msg):   # die loudly, save nothing
        print(f"[backup] FAILED: {msg}")
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
    print(f"[backup] backup received: addr={addr} len={length:#x}")
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
        print(f"[backup]   {ln}")
    if not good:
        fail("the backup does not verify: " + "; ".join(amebaz2_image.failures(lines)))
    if not out:
        out = str(REPO / "firmware/built-images" / f"stock-backup-{backup_file_tag(ip)}-sn{serial}.bin")
    with open(out, "wb") as f:
        f.write(buf)                               # raw slot bytes: image + trailer + 0xFF pad
    print(f"[backup] saved {out} ({len(buf):#x} bytes, stock serial {serial})")
    print(f"[backup] all checks passed: serial {serial} < {base}, manifest sig + every sub-image trailer + byte-sum OK")
    print("[backup] keep this file: once an update overwrites that slot, it is the only copy of the "
          "unit's vendor image short of a clip dump")


def matter_slots(args):
    """Read-only break-glass probe: prints the slot map and changes NOTHING. An answer PROVES the
    custom firmware is alive, silence proves nothing."""
    cfg = load_env()
    need(cfg, "BREAKGLASS_TOKEN", "BREAKGLASS_PORT")
    ip = args[0] if args else ""
    if not ip:
        die("usage: dev.py matter-slots <unit-ip>")
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


# ---- argument parsing + dispatch -------------------------------------------------------------
def usage(code):
    print(__doc__.strip())
    sys.exit(code)


def main(argv):
    if not argv:
        usage(1)
    if argv[0] in ("-h", "--help", "help"):
        usage(0)
    cmd = argv[0]
    if cmd == "ota":
        die(RETIRED)
    # The two probes take a unit address, not a target.
    if cmd == "matter-slots":
        matter_slots(argv[1:])
        return
    if cmd == "matter-backup":
        matter_backup(argv[1:])
        return
    target = argv[1] if len(argv) > 1 else ""
    if target in ("amebaz2", "esp32"):
        die(RETIRED)
    if target != "esphome":
        die(f"target must be esphome (got '{target}')")
    rest = argv[2:]

    board, port, sim_port, name, friendly_name, i = "c3", None, None, None, None, 0
    convert_opts = {}
    while i < len(rest):
        a = rest[i]
        if cmd == "convert" and a == "--stage":
            convert_opts["stage"] = True; i += 1
        elif cmd == "convert" and a in ("--serial", "--software-version") and i + 1 < len(rest):
            if not rest[i + 1].isdigit() or int(rest[i + 1]) < 1:
                die(f"{a} takes a positive integer (got '{rest[i + 1]}')")
            convert_opts[a[2:].replace("-", "_")] = int(rest[i + 1]); i += 2
        elif a == "--board" and i + 1 < len(rest):
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
    if name and not re.fullmatch(r"[a-z0-9]([a-z0-9-]{0,29}[a-z0-9])?", name):
        die(f"--name '{name}' is not a valid hostname (lowercase letters, digits, hyphens; max 31)")
    ctx = Ctx(target, board, port, sim_port, name, friendly_name)
    ctx.convert_opts = convert_opts
    if board in ESPHOME_AMEBAZ2_BOARDS and cmd not in ("doctor", "fetch", "test", "build", "convert"):
        die(ESPHOME_AMEBAZ2_ONLY)

    dispatch = {"walk": walk, "doctor": doctor, "fetch": fetch, "test": test_target,
                "build": build, "erase": erase, "flash": flash, "monitor": monitor,
                "bench": bench, "next": next_steps, "convert": convert}
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
