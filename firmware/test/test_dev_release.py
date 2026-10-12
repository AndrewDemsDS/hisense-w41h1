#!/usr/bin/env python3
"""dev.py: the build cache, which board file each --board builds, the stock module's slot table
and the command line (#143).

No SDK, no env file, no network.
"""

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "firmware" / "scripts"
sys.path.insert(0, str(SCRIPTS))
import dev  # noqa: E402

ok = True



def check(cond, msg):
    global ok
    print(("  ok  " if cond else "  FAIL ") + msg)
    ok = ok and cond


def refused(fn, *args):
    try:
        fn(*args)
    except dev.Die:
        return True
    return False

# --- the stock module's slot table: where a firmware slot starts and how long it is ---
import json  # noqa: E402

check(set(dev.ESPHOME_AMEBAZ2_SLOTS) == set(dev.ESPHOME_AMEBAZ2_BOARDS), "one slot row per stock-module board")
for board, slot in dev.ESPHOME_AMEBAZ2_SLOTS.items():
    layout = json.loads((ROOT / "firmware/esphome/components/w41h1_module" / f"layout-{board[8:]}.json").read_text())
    addr, length = (int(x, 16) for x in layout["flash"]["ota1"].split("+"))
    check(slot == (addr, length), f"{board} slot {slot[0]:#x}+{slot[1]:#x} is the layout file's first slot")
check(str(dev.slot_image("amebaz2-sdk", "ac-test")).endswith(".esphome/build/ac-test/.pioenvs/ac-test/image_firmware_is.0x00C000.bin"),
      "the slot image is looked for where LibreTiny writes it, under the node's name")

# --- ESPHome build cache ---
e = dev.esphome_env({"HOME": "/home/you"})
check(e["PLATFORMIO_BUILD_CACHE_DIR"] == "/home/you/.cache/w41h1-dev/pio-build-cache", "esphome env: build cache defaults under ~/.cache")
e = dev.esphome_env({"HOME": "/home/you", "XDG_CACHE_HOME": "/x"})
check(e["PLATFORMIO_BUILD_CACHE_DIR"] == "/x/w41h1-dev/pio-build-cache", "esphome env: XDG_CACHE_HOME is honoured")
e = dev.esphome_env({"HOME": "/home/you", "PLATFORMIO_BUILD_CACHE_DIR": "/mine"})
check(e["PLATFORMIO_BUILD_CACHE_DIR"] == "/mine", "esphome env: the caller's cache directory wins")
e = dev.esphome_env({"HOME": "/home/you", "PLATFORMIO_BUILD_CACHE_DIR": ""})
check("PLATFORMIO_BUILD_CACHE_DIR" not in e, "esphome env: an empty value switches the cache off")

# --- which board file each --board builds ---
import tempfile  # noqa: E402

calls = []
real = dev.run, dev.have, dev.ESPHOME_DIR
with tempfile.TemporaryDirectory() as tmp:
    (pathlib.Path(tmp) / "secrets.yaml").write_text("")
    dev.run = lambda args, env=None, cwd=None, check=True: calls.append([str(a) for a in args])
    dev.have = lambda tool: True
    dev.ESPHOME_DIR = pathlib.Path(tmp)
    try:
        for board in ("c3", "c3-plus", "classic", "amebaz2-factory"):
            dev.Ctx(board, None, None).esphome_run("config")
    finally:
        dev.run, dev.have, dev.ESPHOME_DIR = real
by_board = dict(zip(("c3", "c3-plus", "classic", "amebaz2-factory"), calls))
for board, config in (("c3", "w41h1-esp32c3.yaml"), ("c3-plus", "w41h1-esp32c3-plus.yaml"), ("classic", "w41h1.yaml"),
                      ("amebaz2-factory", "w41h1-amebaz2-factory.yaml")):
    check(by_board[board][-2:] == ["config", config] and "-s" not in by_board[board],
          f"--board {board} builds {config} and overrides nothing in it")
check(set(dev.BOARDS) == {"c3", "c3-plus", "classic", *dev.ESPHOME_AMEBAZ2_BOARDS}, "every --board value has a board file")
check(len(set(dev.BOARDS.values())) == len(dev.BOARDS), "and no two boards share one: one board, one image")
for board, config in dev.BOARDS.items():
    check((ROOT / "firmware/esphome" / config).is_file(), f"the board file dev.py names for {board} exists: {config}")

# --- CLI ---
d = subprocess.run([sys.executable, str(SCRIPTS / "dev.py"), "flash", "esphome", "--board", "amebaz2-factory"],
                   capture_output=True, text=True)
check(d.returncode == 1 and "has test and build" in d.stderr, f"CLI refuses flash on the stock module: exit {d.returncode}")

print("== DEV.PY OK ==" if ok else "== DEV.PY FAILED ==")
sys.exit(0 if ok else 1)
