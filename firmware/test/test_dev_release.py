#!/usr/bin/env python3
"""dev.py: the pure rules behind `convert`, the provider staging and the command line (#143, #182).

No SDK, no env file, no network.
"""

import os
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

# --- softwareVersion int -> semver, as the conversion image is labelled ---
check(dev.int_to_semver(10351) == "1.3.51" and dev.int_to_semver(10000) == "1.0.0", "softwareVersion int -> semver")
check(dev.int_to_semver(dev.MATTER_FINAL_VERSION) == "1.3.50", "the last Matter release is 1.3.50")


# --- provider manifest ---
m = dev.ota_manifest(b"abc", 10344, "1.3.44", "file:///rac-v10344.ota")["modelVersion"]
check(m["otaFileSize"] == 3 and m["otaChecksum"] == "ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=",
      "manifest carries the .ota size and base64 sha256")
check(m["softwareVersion"] == 10344 and m["maxApplicableSoftwareVersion"] == 10343
      and m["minApplicableSoftwareVersion"] == 1, "manifest applies to every version below this one")
check(list(m)[:4] == ["vid", "pid", "softwareVersion", "softwareVersionString"], "manifest key order is stable")

# --- provider staging: only the manifest being shipped stays active for its product ---


def man(pid, v, vid=0xFFF1):
    return {"modelVersion": {"vid": vid, "pid": pid, "softwareVersion": v}}


listing = {"rac-v10351-esphome.json": man(0x8001, 10351), "rac-v10350.json": man(0x8001, 10350),
           "rac-v10349.json": man(0x8001, 10349), "rac-stock-v10332.json": man(0x8001, 10332),
           "esp32-v10114.json": man(0x8000, 10114), "broken.json": None}
plan = dev.archive_plan("rac-v10351-esphome.json", listing)
check(plan == ["rac-v10349.json", "rac-v10350.json"], "only the same product's other manifests are archived")
check("rac-stock-v10332.json" not in plan, "stock-revert manifests are never archived")
check("broken.json" not in plan and "esp32-v10114.json" not in plan, "unreadable and other-product files untouched")
try:
    dev.archive_plan("missing.json", listing)
    check(False, "a keep manifest that is not in the listing is refused")
except ValueError:
    check(True, "a keep manifest that is not in the listing is refused")
parsed = dev.parse_listing('a.json\t{"modelVersion": {"pid": 32768}}\nb.json\tnot json\nno tab here\n')
check(parsed.get("b.json", "x") is None and parsed["a.json"]["modelVersion"]["pid"] == 0x8000 and len(parsed) == 2,
      "a listing parses per line, an unreadable manifest becomes None")


check(dev.backup_file_tag("192.168.1.37") == "37", "IPv4 backup file tag is the last octet")
check(dev.backup_file_tag("fe80::1ff:fe23:4567%wlan0") == "fe80-1ff-fe23-4567", "IPv6 tag drops the zone and sanitises")

# --- convert: an ESPHome image for the stock module, re-signed for the Matter firmware's OTA ---
import hashlib  # noqa: E402
import json  # noqa: E402
import hmac as hmac_mod  # noqa: E402
import struct  # noqa: E402
import amebaz2_image as az  # noqa: E402


def fake_image(serial, body=0x400):
    """A three-sub-image payload signed the way the bootloader checks it, plus the byte-sum."""
    buf = bytearray(az.HDR0)
    for i in range(3):
        hdr = bytearray(az.HDR_LEN)
        last = i == 2
        struct.pack_into("<II", hdr, 0, body, az.LINK_END if last else az.HDR_LEN + body + az.MAC_LEN)
        buf += hdr + bytes([0x10 + i]) * body + bytes(az.MAC_LEN)
    signed = az.resign(bytes(buf), serial=serial)
    return signed + az.bytesum(signed)


lt = fake_image(1)
SLOT = dev.ESPHOME_AMEBAZ2_SLOTS["amebaz2-factory"][1]
out = dev.esphome_matter_image(lt, 11451, SLOT)
check(len(out) == len(lt), "convert: the re-signed image keeps its length")
check(az.read_serial(out) == 11451, "convert: the image carries the serial asked for")
check(az.verify(out[:-4], out[-4:])[0], "convert: manifest, every sub-image trailer and the byte-sum verify")
check(out[:az.MAC_LEN] == hmac_mod.new(az.KEY, out[az.HDR0:az.HDR0 + az.HDR_LEN], hashlib.sha256).digest(),
      "convert: the manifest signature covers the new serial")
changed = {i for i in range(len(lt)) if lt[i] != out[i]}
subs = az.sub_images(out[:-4])
allowed = set(range(0, az.MAC_LEN)) | set(range(az.SERIAL_OFF, az.SERIAL_OFF + 4)) | set(range(len(lt) - 4, len(lt)))
for _i, _hdr, _start, end in subs:
    allowed |= set(range(end, end + az.MAC_LEN))
check(changed and changed <= allowed, "convert: only the serial, the signatures and the byte-sum change")
check(dev.esphome_matter_image(out, 11451, SLOT) == out, "convert: re-signing with the same serial is a no-op")
bad = bytearray(lt)
bad[az.HDR0 + az.HDR_LEN + 5] ^= 0x01
check(refused(dev.esphome_matter_image, bytes(bad), 11451, SLOT), "convert: an image that does not verify as built is refused")
check(refused(dev.esphome_matter_image, lt[:-1], 11451, SLOT), "convert: a truncated image is refused")
check(refused(dev.esphome_matter_image, lt[:64], 11451, SLOT), "convert: a stub is refused")
check(refused(dev.esphome_matter_image, lt, 11451, len(lt) - 1), "convert: an image larger than the slot is refused")
check(not refused(dev.esphome_matter_image, lt, 11451, len(lt)), "convert: an image that exactly fills the slot passes")
check(refused(dev.esphome_matter_image, lt, 0, SLOT), "convert: serial 0 is refused")
check(refused(dev.esphome_matter_image, lt, 0xFFFFFFFF, SLOT), "convert: the erased-flash serial is refused")
check(set(dev.ESPHOME_AMEBAZ2_SLOTS) == set(dev.ESPHOME_AMEBAZ2_BOARDS), "convert: one slot row per stock-module board")
for board, slot in dev.ESPHOME_AMEBAZ2_SLOTS.items():
    layout = json.loads((ROOT / "firmware/esphome/components/w41h1_module" / f"layout-{board[8:]}.json").read_text())
    addr, length = (int(x, 16) for x in layout["flash"]["ota1"].split("+"))
    check(slot == (addr, length), f"convert: {board} slot {slot[0]:#x}+{slot[1]:#x} is the layout file's first slot")
check("amebaz2-native" not in dev.ESPHOME_CONVERT_BOARDS, "convert: native has no Matter firmware to convert from")
c = dev.Ctx("esphome", "amebaz2-sdk", None, None, "ac-test")
check(str(c.esphome_slot_image()).endswith(".esphome/build/ac-test/.pioenvs/ac-test/image_firmware_is.0x00C000.bin"),
      "convert: the slot image is looked for where LibreTiny writes it, under the node's name")
m = dev.ota_manifest(b"x" * 10, 10351, "1.3.51", "file:///rac-v10351-esphome.ota")["modelVersion"]
check(m["softwareVersion"] == 10351 and m["maxApplicableSoftwareVersion"] == 10350 and m["otaFileSize"] == 10,
      "convert: the manifest offers the image to every version below it")

# --- ESPHome build cache ---
e = dev.esphome_env({"HOME": "/home/you"})
check(e["PLATFORMIO_BUILD_CACHE_DIR"] == "/home/you/.cache/w41h1-dev/pio-build-cache", "esphome env: build cache defaults under ~/.cache")
e = dev.esphome_env({"HOME": "/home/you", "XDG_CACHE_HOME": "/x"})
check(e["PLATFORMIO_BUILD_CACHE_DIR"] == "/x/w41h1-dev/pio-build-cache", "esphome env: XDG_CACHE_HOME is honoured")
e = dev.esphome_env({"HOME": "/home/you", "PLATFORMIO_BUILD_CACHE_DIR": "/mine"})
check(e["PLATFORMIO_BUILD_CACHE_DIR"] == "/mine", "esphome env: the caller's cache directory wins")
e = dev.esphome_env({"HOME": "/home/you", "PLATFORMIO_BUILD_CACHE_DIR": ""})
check("PLATFORMIO_BUILD_CACHE_DIR" not in e, "esphome env: an empty value switches the cache off")

# --- which board file and substitutions each --board builds ---
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
            dev.Ctx("esphome", board, None, None).esphome_run("config")
    finally:
        dev.run, dev.have, dev.ESPHOME_DIR = real
by_board = dict(zip(("c3", "c3-plus", "classic", "amebaz2-factory"), calls))
check("w41h1.yaml" in by_board["c3"] and "esp32-c3-devkitm-1" in by_board["c3"],
      "--board c3 builds w41h1.yaml with the C3 board and pins as -s overrides, as it always has")
check("w41h1-esp32c3-plus.yaml" in by_board["c3-plus"] and "-s" not in by_board["c3-plus"],
      "--board c3-plus builds its own board file and passes no pins")
check("w41h1.yaml" in by_board["classic"] and "esp32dev" in by_board["classic"], "--board classic builds w41h1.yaml")
check("w41h1-amebaz2-factory.yaml" in by_board["amebaz2-factory"], "--board amebaz2-factory builds its own board file")
for call in calls:
    check((ROOT / "firmware/esphome" / [a for a in call if a.endswith(".yaml")][0]).is_file(),
          f"the board file dev.py names exists: {[a for a in call if a.endswith('.yaml')][0]}")

# --- CLI ---
for args, why in ((["convert", "esphome", "--board", "amebaz2-native"], "native"),
                  (["convert", "esphome"], "an ESP32 board"),
                  (["convert", "esphome", "--board", "amebaz2-factory", "--serial", "x"], "a non-numeric serial"),
                  (["flash", "esphome", "--board", "amebaz2-factory"], "flash on the stock module")):
    d = subprocess.run([sys.executable, str(SCRIPTS / "dev.py"), *args], capture_output=True, text=True,
                       env={**os.environ, "ENVF": "/nonexistent/ota-release.env"})
    check(d.returncode == 1, f"CLI refuses {why}: exit {d.returncode}")
for args, why in ((["ota", "amebaz2", "build"], "the retired ota steps"),
                  (["build", "amebaz2"], "the retired amebaz2 target"),
                  (["build", "esp32"], "the retired esp32 target")):
    d = subprocess.run([sys.executable, str(SCRIPTS / "dev.py"), *args], capture_output=True, text=True)
    check(d.returncode == 1 and "matter-final" in d.stderr, f"CLI refuses {why} and names the matter-final tag")
d = subprocess.run([sys.executable, str(SCRIPTS / "dev.py"), "matter-slots"], capture_output=True, text=True,
                   env={**os.environ, "ENVF": "/nonexistent/ota-release.env"})
check(d.returncode == 1, "CLI matter-slots without a settings file is refused")

print("== DEV.PY OK ==" if ok else "== DEV.PY FAILED ==")
sys.exit(0 if ok else 1)
