#!/usr/bin/env python3
"""OTA pre-flight + staging guards (firmware/scripts/ota_guards.py).

Pure decisions only: no network, no SDK, no images from built-images/ (gitignored), so CI runs
all of it. Each case replays a near miss from the 2026-09-14 fleet update.
"""

import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "firmware" / "scripts"))
import ota_guards as g  # noqa: E402

ok = True


def check(cond, msg):
    global ok
    print(("  ok  " if cond else "  FAIL ") + msg)
    ok = ok and cond


ESP_MARK = b"diagnostic console listening"
AMEBA_MARK = b"diag console listening"

# --- flavour: read from the bytes ---
debug_img = b"\x00" * 64 + b"I (123) diag: " + ESP_MARK + b" on :2323\x00" + b"\xff" * 64
release_img = b"\x00" * 64 + b"hisense_ac_matter\x00" + b"\xff" * 64
check(g.image_flavour(debug_img, ESP_MARK) == "debug", "ESP32 image with the console string reads debug")
check(g.image_flavour(release_img, ESP_MARK) == "release", "ESP32 image without it reads release")
check(g.image_flavour(b"x" + AMEBA_MARK, AMEBA_MARK) == "debug", "AmebaZ2 marker detected")
check(g.image_flavour(debug_img, AMEBA_MARK) == "release",
      "markers are per target: the AmebaZ2 string does not match the ESP32 one")
check(g.flavour_mismatch("debug", "release") is not None and ":2323" in g.flavour_mismatch("debug", "release"),
      "near miss: debug image under a release request is refused and names the console")
check(g.flavour_mismatch("release", "debug") is not None, "release image under a debug request is refused")
check(g.flavour_mismatch("release", "release") is None, "matching flavour passes")
check(g.flavour_mismatch("release", "") is not None, "empty wanted flavour is refused, not assumed")

# --- stale outputs ---
check(g.stale_outputs(100.0, {"a.ota": 50.0, "a.json": 150.0}) == ["a.ota"],
      "near miss: an .ota older than the fresh build is flagged")
check(g.stale_outputs(100.0, {"a.ota": None}) == ["a.ota"], "a missing output counts as stale")
check(g.stale_outputs(100.0, {"a.ota": 100.0, "a.json": 101.0}) == [], "outputs at or after the build pass")

# --- link ---
check(g.link_verdict(-76, 14.4)[0] is False, "near miss: -76 dBm (node that failed announce) refused")
check(g.link_verdict(-58, 0.08)[0] is True, "-58 dBm / 0.08 s (node that updated cleanly) passes")
check(g.link_verdict(-70, 0.2)[0] is True, "exactly the -70 dBm threshold passes")
check(g.link_verdict(-65, 3.0)[0] is False, "good RSSI but a 3 s read is refused")
check(g.link_verdict(None, None)[0] is False, "no reading is refused, never assumed healthy")


# --- manifest archive plan ---
def man(pid, v, vid=0xFFF1):
    return {"modelVersion": {"vid": vid, "pid": pid, "softwareVersion": v}}


listing = {
    "esp32-v10115.json": man(0x8000, 10115),
    "esp32-v10114.json": man(0x8000, 10114),
    "esp32-v10030.json": man(0x8000, 10030),
    "rac-v10333.json": man(0x8001, 10333),
    "rac-stock-v10332.json": man(0x8001, 10332),
    "broken.json": None,
}
check(g.archive_plan("esp32-v10115.json", listing) == ["esp32-v10030.json", "esp32-v10114.json"],
      "only the same product's other manifests are archived")
plan = g.archive_plan("rac-v10333.json", listing)
check("rac-stock-v10332.json" not in plan, "stock-revert manifests are never archived")
check("broken.json" not in plan and "esp32-v10114.json" not in plan, "unreadable and other-product files untouched")
try:
    g.archive_plan("missing.json", listing)
    check(False, "a keep manifest absent from the listing raises")
except ValueError:
    check(True, "a keep manifest absent from the listing raises")

text = "a.json\t" + json.dumps(man(0x8000, 1)) + "\nb.json\tnot json\n\n"
parsed = g.parse_listing(text)
check(parsed.get("b.json", "x") is None and parsed["a.json"]["modelVersion"]["pid"] == 0x8000,
      "listing parser keeps unreadable manifests as None")

# --- CLI contract the shell side depends on (exit codes) ---
cli = [sys.executable, str(ROOT / "firmware" / "scripts" / "ota_guards.py")]
with tempfile.TemporaryDirectory() as d:
    img = os.path.join(d, "img.bin")
    with open(img, "wb") as f:
        f.write(debug_img)
    r = subprocess.run(cli + ["check-flavour", img, ESP_MARK.decode(), "release"], capture_output=True, text=True)
    check(r.returncode == 1, "CLI check-flavour exits 1 on mismatch")
    r = subprocess.run(cli + ["check-flavour", img, ESP_MARK.decode(), "debug"], capture_output=True, text=True)
    check(r.returncode == 0, "CLI check-flavour exits 0 on match")
    out = os.path.join(d, "img.ota")
    with open(out, "wb") as f:
        f.write(b"x")
    os.utime(out, (1, 1))
    r = subprocess.run(cli + ["stale", img, out], capture_output=True, text=True)
    check(r.returncode == 1 and "img.ota" in r.stdout, "CLI stale exits 1 and names the old output")
    r = subprocess.run(cli + ["archive-plan", "esp32-v10115.json"], input=
                       "\n".join(f"{n}\t{json.dumps(v)}" for n, v in listing.items() if v), capture_output=True, text=True)
    check(r.returncode == 0 and r.stdout.split() == ["esp32-v10030.json", "esp32-v10114.json"], "CLI archive-plan prints the plan")

print("== OTA GUARDS OK ==" if ok else "== OTA GUARDS FAILED ==")
sys.exit(0 if ok else 1)
