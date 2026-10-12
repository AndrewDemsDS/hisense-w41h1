#!/usr/bin/env python3
"""dev.py -- one portable entry point for the ESPHome firmware: check the tools, test, build, flash
and bench it (issues #118, #143).

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

Board: --board c3 (ESP32-C3 SuperMini, default), --board c3-plus (ESP32-C3 SuperMini Plus, the
one with an RGB pixel on GPIO8) or --board classic (ESP32-D0WDQ6), the ESP32 replacement board.
--board amebaz2-factory, amebaz2-sdk or amebaz2-native is the stock module
through LibreTiny, by the flash layout the unit has: test and build (factory has run on a
module, sdk and native have not). Node identity: --name <hostname> and --friendly-name <text>
(default hisense-ac / "Air Conditioner"; give every node after the first its own). Env: ESPHOME
(esphome command, default `esphome`). ESPHome builds reuse compiled objects through
PLATFORMIO_BUILD_CACHE_DIR (default ~/.cache/w41h1-dev/pio-build-cache; set it yourself to move it,
or to an empty string to switch it off).

Moving a stock module off the retired Matter firmware is a script of its own, matter-convert.py
(docs/guide/Converting-a-Stock-Module-to-ESPHome.md). The Matter firmwares themselves (AmebaZ2 and
esp-matter builds, their release steps, the way back to the vendor firmware over the air) are
retired. The last tree that has them is the tag `matter-final`.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
ESPHOME_DIR = REPO / "firmware/esphome"
TEST = REPO / "firmware/test"

RETIRED = ("the Matter firmwares are retired (issue #182). dev.py builds ESPHome only; the last tree "
           "with the amebaz2 and esp32 targets and the `ota` steps is the tag matter-final")
ESPHOME_AMEBAZ2_BOARDS = ("amebaz2-factory", "amebaz2-sdk", "amebaz2-native")
ESPHOME_AMEBAZ2_ONLY = ("esphome --board amebaz2-* has test and build. A first flash is matter-convert.py (over the "
                        "air from the retired Matter firmware), the clip or UART download mode, and later updates are "
                        "`esphome upload` (firmware/docs/15-esphome-path.md)")
# First slot address (LibreTiny names the app image after it) and slot length, per layout. The
# host test holds this equal to firmware/esphome/components/w41h1_module/layout-*.json.
ESPHOME_AMEBAZ2_SLOTS = {"amebaz2-factory": (0x010000, 0x170000), "amebaz2-sdk": (0x00C000, 0x1AC000),
                         "amebaz2-native": (0x010000, 0x1AC000)}
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
        return build_dir(self.name)

    def esphome_slot_image(self):
        return slot_image(self.board, self.name)

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


def build_dir(name=None):
    name = name or "hisense-ac"
    return ESPHOME_DIR / ".esphome/build" / name / ".pioenvs" / name


def slot_image(board, name=None):
    """The application image LibreTiny builds for a firmware slot (stock module boards only).
    matter-convert.py re-signs it, and its name carries the first slot's address."""
    return build_dir(name) / f"image_firmware_is.0x{ESPHOME_AMEBAZ2_SLOTS[board][0]:06X}.bin"


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
    target = argv[1] if len(argv) > 1 else ""
    if target in ("amebaz2", "esp32"):
        die(RETIRED)
    if target != "esphome":
        die(f"target must be esphome (got '{target}')")
    rest = argv[2:]

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
