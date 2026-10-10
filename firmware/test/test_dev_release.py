#!/usr/bin/env python3
"""dev.py release engine: the pure rules behind `dev.py ota amebaz2 lint|verint|tag` (#143).

No SDK, no env file, no network. The goldens for the SDK edits and the version rules were taken
from ota-release.sh, the script dev.py replaced, before it was removed.
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


# --- semver <-> softwareVersion int ---
check(dev.semver_to_int("1.3.44") == 10344, "1.3.44 -> 10344")
check(dev.semver_to_int("1.0.0") == 10000, "1.0.0 -> 10000 (clears the legacy sw34 fleet)")
check(dev.semver_to_int(" 1.2.9\n") == 10209, "whitespace around version.txt is ignored")
check(dev.semver_to_int("34") == 34, "a legacy raw-int version passes through")
check(dev.semver_to_int("0") == 0, "CI's base fallback 0 passes through")
check(refused(dev.semver_to_int, "1.100.0"), "minor 100 is refused (it would collide with the next major)")
check(refused(dev.semver_to_int, "1.0.100"), "patch 100 is refused")
check(refused(dev.semver_to_int, "1.2"), "two-part version is refused")
check(refused(dev.semver_to_int, "v1.2.3"), "a v prefix is refused")
check(refused(dev.semver_to_int, ""), "an empty version is refused")
for sem in ("1.3.44", "1.0.0", "2.99.99", "0.0.1"):
    check(dev.int_to_semver(dev.semver_to_int(sem)) == sem, f"{sem} round-trips through the int")
check(dev.semver_to_int("1.3.44") > dev.semver_to_int("1.3.43")
      and dev.semver_to_int("1.4.0") > dev.semver_to_int("1.3.99")
      and dev.semver_to_int("2.0.0") > dev.semver_to_int("1.99.99"),
      "the int is strictly monotonic across patch, minor and major")

# --- .zap endpoint contiguity ---
check(dev.endpoints_contiguous([0, 1, 2, 3]) == ([0, 1, 2, 3], True), "0..3 is contiguous")
check(dev.endpoints_contiguous([3, 0, 2, 1])[1], "order in the .zap does not matter")
check(not dev.endpoints_contiguous([0, 1, 3])[1], "a gap is refused (boot-crashes AmebaZ2)")
check(not dev.endpoints_contiguous([1, 2, 3])[1], "endpoints must start at 0")
check(not dev.endpoints_contiguous([0, 1, 1])[1], "a duplicate endpoint id is refused")

# --- release-runner check: which repo to ask about ---
check(dev.github_slug("git@github.com:owner/repo.git") == "owner/repo", "ssh remote -> owner/repo")
check(dev.github_slug("https://github.com/owner/repo") == "owner/repo", "https remote -> owner/repo")
check(dev.github_slug("https://github.com/owner/repo.git/") == "owner/repo.git",
      "only one suffix is stripped, .git first (same as the script)")
check(dev.github_slug("https://github.com/owner/repo/") == "owner/repo", "trailing slash is stripped")
check(dev.github_slug("ssh://git@git.example.com:2222/owner/repo.git") == "", "a non-GitHub remote gives no slug")
check(dev.github_slug("") == "", "no origin gives no slug")

# --- version bump + the derived SDK header ---
check(dev.bumped("1.3.44", "patch") == "1.3.45", "bump patch")
check(dev.bumped("1.3.44", "minor") == "1.4.0", "bump minor resets patch")
check(dev.bumped("1.3.44", "major") == "2.0.0", "bump major resets minor and patch")
check(refused(dev.bumped, "34", "patch"), "a legacy raw-int version cannot be bumped")
HDR = ('#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION 10343\r\n'
       '#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING "1.3.43"\r\n'
       '#define OTHER "keep"\r\n')
check(dev.header_with_version(HDR, 10344, "1.3.44") ==
      HDR.replace("10343", "10344").replace("1.3.43", "1.3.44"),
      "the SDK header gets the int and the string, nothing else, line endings kept")

# --- SDK edits: each one against the text the script's sed/perl produced, and idempotent ---
def edit(fn, before, after, msg):
    got = fn(before)
    check(got == after and fn(got) == got, msg)


edit(dev.edit_downlink_stack,
     '    xTaskCreate(DownlinkTask, "Downlink", 1024, NULL, 1, &h);\n',
     '    xTaskCreate(DownlinkTask, "Downlink", 4096, NULL, 1, &h);\n', "DownlinkTask stack 1024 -> 4096")
edit(dev.edit_example_task_stack,
     'xTaskCreate(t, ((const char*)"example_matter_room_air_conditioner_task"), 2048, NULL, 1, NULL)\n',
     'xTaskCreate(t, ((const char*)"example_matter_room_air_conditioner_task"), 8192, NULL, 1, NULL)\n',
     "example init task stack 2048 -> 8192")
edit(dev.edit_mode_select_span_guard,
     "    {\n        if (endpointSpanPair.mEndpointId == endpointId)\n        {\n",
     "    {\n        if (endpointSpanPair.mSpan.data() == nullptr) { continue; }"
     "  // orphaned endpoint type pads this array\n"
     "        if (endpointSpanPair.mEndpointId == endpointId)\n        {\n", "ModeSelect null-span guard inserted")
edit(dev.edit_mode_select_sleep_profiles,
     'storage_value_type M::coffeeOptions[] = {\n'
     '    buildModeOptionStruct("Black", 0, List<const SemanticTag>(semanticTagsBlack)),\n'
     '    buildModeOptionStruct("Espresso", 7, List<const SemanticTag>(semanticTagsEspresso))\n'
     '};\n'
     '        EndpointSpanPair(1, Span<storage_value_type>(M::coffeeOptions)) // Options for Endpoint 1\n',
     'storage_value_type M::coffeeOptions[] = {\n'
     '    buildModeOptionStruct("Off", 0, List<const SemanticTag>()),\n'
     '    buildModeOptionStruct("General", 1, List<const SemanticTag>()),\n'
     '    buildModeOptionStruct("Old", 2, List<const SemanticTag>()),\n'
     '    buildModeOptionStruct("Young", 3, List<const SemanticTag>()),\n'
     '    buildModeOptionStruct("Kids", 4, List<const SemanticTag>())\n'
     '};\n'
     '        EndpointSpanPair(6, Span<storage_value_type>(M::coffeeOptions)) // Sleep profiles on endpoint 6\n',
     "ModeSelect options: stock coffee list on ep1 -> sleep profiles on ep6")
edit(dev.edit_build_info_determinism,
     '\t@echo \\#define UTS_VERSION \\"`date +%Y/%m/%d-%T`\\" >> .ver\n'
     '\t@echo \\#define RTL8710CFW_COMPILE_BY \\"`id -u -n`\\" >> .ver\n'
     '\t@echo \\#define RTL8710CFW_COMPILE_HOST \\"`$(HOSTNAME_APP)`\\" >> .ver\n',
     '\t@echo \\#define UTS_VERSION \\"`date -u -d @$${SOURCE_DATE_EPOCH:-0} +%Y/%m/%d-%T`\\" >> .ver\n'
     '\t@echo \\#define RTL8710CFW_COMPILE_BY \\"builder\\" >> .ver\n'
     '\t@echo \\#define RTL8710CFW_COMPILE_HOST \\"\\" >> .ver\n',
     "build_info: clock from SOURCE_DATE_EPOCH, constant builder, no hostname")
edit(dev.edit_build_info_order,
     "all: build_info application_is\nprerequirement:\n\t@echo x\nprerequirement: other\n",
     "all: build_info application_is\nprerequirement: build_info\n\t@echo x\nprerequirement: other\n",
     "build_info ordered before the objects, only the bare rule is touched")
edit(dev.edit_prefix_map,
     "CHIP_CXXFLAGS += $(INCLUDES)\nCHIP_CXXFLAGS += -DX\n",
     "CHIP_CXXFLAGS += $(INCLUDES)\nCHIP_CFLAGS += -ffile-prefix-map=$(HOME)=/build\n"
     "CHIP_CXXFLAGS += -ffile-prefix-map=$(HOME)=/build\nCHIP_CXXFLAGS += -DX\n",
     "path scrub added to the GN core flags, $(HOME) left for make to expand")
edit(dev.edit_ccache_launcher,
     '\techo ameba_cpu = \\"ameba\\" >> $(OUTPUT_DIR)/args.gn && \\\n\techo next\n',
     '\techo ameba_cpu = \\"ameba\\" >> $(OUTPUT_DIR)/args.gn && \\\n'
     '\techo pw_command_launcher = \\"ccache\\" >> $(OUTPUT_DIR)/args.gn && \\\n\techo next\n',
     "ccache launcher injected into the args.gn generation")
edit(lambda t: dev.edit_ota_hardening(t, "#define HISENSE_OTA_HARDENING 1\n"),
     "#pragma once\n", "#pragma once\n#define HISENSE_OTA_HARDENING 1\n", "MRP hardening block appended once")

# --- the sync list: dev.py reads the same file scripts/setup.sh sources ---
req, opt = dev.sync_file_lists()
bash = subprocess.run(
    ["bash", "-c", '. "$1"; printf "%s\\n" "${SYNC_FILES_REQUIRED[@]}"; echo --; printf "%s\\n" "${SYNC_FILES_OPTIONAL[@]}"',
     "bash", str(SCRIPTS / "sync-files.sh")], capture_output=True, text=True).stdout
b_req, b_opt = (part.split() for part in bash.split("--\n"))
check(req == b_req and opt == b_opt and len(req) >= 8, f"sync list parsed as bash sees it ({len(req)} required, {len(opt)} optional)")
check(all((ROOT / f).is_file() for f in req), "every required sync file exists in the tree")

# --- provider manifest ---
m = dev.ota_manifest(b"abc", 10344, "1.3.44", "file:///rac-v10344.ota")["modelVersion"]
check(m["otaFileSize"] == 3 and m["otaChecksum"] == "ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=",
      "manifest carries the .ota size and base64 sha256")
check(m["softwareVersion"] == 10344 and m["maxApplicableSoftwareVersion"] == 10343
      and m["minApplicableSoftwareVersion"] == 1, "manifest applies to every version below this one")
check(list(m)[:4] == ["vid", "pid", "softwareVersion", "softwareVersionString"], "manifest key order is stable")

# --- revert: the image recipe agrees with amebaz2_image.py, the format's reference module ---
import amebaz2_image as az  # noqa: E402
import json  # noqa: E402
import struct  # noqa: E402
import tempfile  # noqa: E402


def synth_image(sizes, serial):
    """A signature-valid image with len(sizes) sub-images, built by the reference module."""
    body, offs = bytearray(0xE0), []
    for n in sizes:
        offs.append(len(body))
        body += bytearray(0x60) + bytes(range(256)) * (n // 256) + bytes(0x20)
    for i, off in enumerate(offs):
        struct.pack_into("<I", body, off, sizes[i])
        struct.pack_into("<I", body, off + 4, 0xFFFFFFFF if i == len(offs) - 1 else offs[i + 1] - off)
    return az.resign(bytes(body), serial=serial)


img = synth_image([0x400, 0x800, 0x300], 100)
chain = dev.image_chain(img)
check(chain is not None and all(m for _, _, m in chain) and len(chain) == 3, "a valid 3-sub-image chain walks and verifies")
check([(h, e) for h, e, _ in chain] == [(hdr, end) for _, hdr, _, end in az.sub_images(img)],
      "the chain walk finds the same headers and trailers as amebaz2_image")
signed = dev.resign_stock(img, 11445)
ref = az.resign(img, serial=11445)
check(signed == ref + az.bytesum(ref), "resign_stock == amebaz2_image.resign + bytesum, byte for byte")
check(az.verify(signed[:-4], signed[-4:])[0] and az.read_serial(signed) == 11445,
      "the re-signed payload verifies in full and carries the new serial")
stale = bytearray(img)
struct.pack_into("<I", stale, 0xF4, 11445)   # the #75 brick: serial patched, trailers left alone
check(not all(m for _, _, m in dev.image_chain(bytes(stale))), "a serial patch without re-signing is caught (#75)")
check(dev.image_chain(img[:0x100]) is None, "a truncated image is unwalkable, not a crash")
dump = b"\x00" * 0x10000 + b"\xff" * 0x2000
check(dev.carve_stock(dump) is None, "a dump with no signed image carves nothing")

with tempfile.TemporaryDirectory() as td:
    bi = pathlib.Path(td)
    check(dev.newest_stock_pair(bi) is None, "no revert pair on disk -> None")
    for n in ("10340", "10345", "10350", "x1"):
        (bi / f"rac-stock-v{n}.ota").write_bytes(b"o")
    for n in ("10340", "10345"):
        (bi / f"rac-stock-v{n}.json").write_text("{}")
    check(dev.newest_stock_pair(bi) == 10345, "newest revert pair needs BOTH files (10350 has no manifest)")
    (bi / "p.bin").write_bytes(signed)
    (bi / "good.json").write_text(json.dumps({"modelVersion": {"payloadName": "p.bin"}}))
    (bi / "old.json").write_text(json.dumps({"modelVersion": {}}))
    (bi / "stale.bin").write_bytes(bytes(stale) + az.bytesum(bytes(stale)))
    (bi / "stale.json").write_text(json.dumps({"modelVersion": {"payloadName": "stale.bin"}}))
    (bi / "gone.json").write_text(json.dumps({"modelVersion": {"payloadName": "missing.bin"}}))
    check(dev.verify_stock_payload(bi, bi / "good.json")[0], "apply accepts a fully verified payload")
    check(not dev.verify_stock_payload(bi, bi / "old.json")[0], "apply refuses a manifest with no payloadName (pre-#75)")
    good, why = dev.verify_stock_payload(bi, bi / "stale.json")
    check(not good and "MISMATCH" in why, "apply refuses a payload with a stale trailer")
    check(not dev.verify_stock_payload(bi, bi / "gone.json")[0], "apply refuses when the payload file is missing")

check(dev.backup_file_tag("192.168.1.37") == "37", "IPv4 backup file tag is the last octet")
check(dev.backup_file_tag("fe80::1ff:fe23:4567%wlan0") == "fe80-1ff-fe23-4567", "IPv6 tag drops the zone and sanitises")

# --- staging: stale outputs are refused ---
with tempfile.TemporaryDirectory() as td:
    src, out = pathlib.Path(td) / "fw.bin", pathlib.Path(td) / "x.ota"
    out.write_bytes(b"o")
    src.write_bytes(b"f")
    os.utime(out, (1000, 1000))
    os.utime(src, (2000, 2000))
    check(refused(dev.guard_fresh, src, out), "an .ota older than the image it came from is not staged")
    os.utime(out, (3000, 3000))
    check(not refused(dev.guard_fresh, src, out), "a fresh .ota passes")
    check(refused(dev.guard_fresh, src, out, pathlib.Path(td) / "missing.json"), "a missing manifest is refused")

# --- ESP32: version, toolchain lock, manifest ---
check(dev.esp_semver_to_int("1.1.16") == 10116, "ESP32 1.1.16 -> 10116")
check(refused(dev.esp_semver_to_int, "34"), "ESP32 has no legacy raw-int form")
check(refused(dev.esp_semver_to_int, "1.1.100"), "ESP32 patch 100 is refused")
check(dev.esp_int() == dev.esp_semver_to_int(dev.esp_semver()), "PROJECT_VER parses from CMakeLists.txt")
LOCK = ("dependencies:\n  espressif/esp_delta_ota:\n    version: 1.1.4\n  idf:\n    source:\n      type: idf\n"
        "    version: 5.5.4\ndirect_dependencies:\n- idf\n")
check(dev.lock_idf_version(LOCK) == "5.5.4", "the IDF version is the one under the idf: block, not a component's")
check(dev.lock_idf_version(LOCK.replace("5.5.4", "'5.5.4'\r")) == "5.5.4", "quotes and CR are stripped")
check(dev.lock_idf_version("dependencies:\n  other:\n    version: 1.0.0\n") == "", "no idf: block gives no version")
m = dev.ota_manifest(b"abc", 10116, "1.1.16", "file:///esp32-v10116.ota", 0xFFF1, 0x8000, 0)["modelVersion"]
check(m["pid"] == 0x8000 and m["minApplicableSoftwareVersion"] == 0 and m["maxApplicableSoftwareVersion"] == 10115,
      "ESP32 manifest carries its own pid and min 0 (the int is unwired and reads 0)")
d = subprocess.run([sys.executable, str(SCRIPTS / "dev.py"), "ota", "esp32", "verint"], capture_output=True, text=True)
check(d.returncode == 0 and d.stdout.strip() == str(dev.esp_int()), "CLI esp32 verint prints only the int")

# --- flash pre-flight: wait for the node, and what the weak-link override may excuse ---
import ota_guards as og  # noqa: E402


def with_link(answers, fn, *args, **kw):
    """Run fn with dev.read_link replaced by a script of (ok, text) answers; the last one repeats."""
    calls = []

    def fake(cfg, node, min_rssi):
        calls.append(min_rssi)
        return answers[min(len(calls) - 1, len(answers) - 1)]
    real, dev.read_link = dev.read_link, fake
    try:
        return fn(*args, **kw), calls
    except dev.Die as e:
        return e, calls
    finally:
        dev.read_link = real


dead = (False, f"link read failed: ServerDisconnectedError\n{og.NO_READING} (node unreachable or 0/54/4 null)")
weak = (False, "RSSI -73 dBm is below -70 dBm (OTA announce/BDX fails on this link)")
fine = (True, "RSSI -60 dBm, read 0.10 s")
res, calls = with_link([dead, dead, fine], dev.wait_for_node, {}, 14, sleep=lambda s: None)
check(res == fine[1] and len(calls) == 3, "wait_for_node keeps polling through a restarting matter-server")
res, calls = with_link([dead], dev.wait_for_node, {}, 14, timeout_s=30, sleep=lambda s: None)
check(isinstance(res, dev.Die) and len(calls) == 4, "wait_for_node gives up after the timeout")
res, _ = with_link([weak], dev.wait_for_node, {}, 80, sleep=lambda s: None)
check(res == weak[1], "a weak reading still counts as the node answering")
res, _ = with_link([weak], dev.guard_link, {"OTA_ALLOW_WEAK_LINK": "1"}, 80)
check(not isinstance(res, dev.Die), "OTA_ALLOW_WEAK_LINK=1 lets a weak link through")
res, _ = with_link([weak], dev.guard_link, {}, 80)
check(isinstance(res, dev.Die), "a weak link is refused without the override")
real_wait, dev.wait_for_node = dev.wait_for_node, lambda cfg, node: ""
res, _ = with_link([dead], dev.guard_link, {"OTA_ALLOW_WEAK_LINK": "1"}, 80)
dev.wait_for_node = real_wait
check(isinstance(res, dev.Die) and "does not answer" in str(res),
      "the override does not excuse a node with no reading at all")
check(og.link_verdict(None, None)[1].startswith(og.NO_READING), "the no-reading verdict uses the shared marker")

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
    layout = json.loads((ROOT / "firmware/esphome/amebaz2" / f"layout-{board[8:]}.json").read_text())
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

# --- CLI ---
for args, why in ((["convert", "esphome", "--board", "amebaz2-native"], "native"),
                  (["convert", "esphome"], "an ESP32 board"),
                  (["convert", "esphome", "--board", "amebaz2-factory", "--serial", "x"], "a non-numeric serial"),
                  (["flash", "esphome", "--board", "amebaz2-factory"], "flash on the stock module")):
    d = subprocess.run([sys.executable, str(SCRIPTS / "dev.py"), *args], capture_output=True, text=True,
                       env={**os.environ, "ENVF": "/nonexistent/ota-release.env"})
    check(d.returncode == 1, f"CLI refuses {why}: exit {d.returncode}")
devpy = [sys.executable, str(SCRIPTS / "dev.py"), "ota", "amebaz2", "verint"]
for arg in ("1.3.44", "1.0.0", "34", "0", "1.100.0", "1.2", "bogus"):
    d = subprocess.run(devpy + [arg], capture_output=True, text=True)
    want_rc = 1 if refused(dev.semver_to_int, arg) else 0
    check(d.returncode == want_rc and (want_rc == 1 or d.stdout.strip() == str(dev.semver_to_int(arg))),
          f"CLI verint {arg}: exit {d.returncode}, stdout '{d.stdout.strip()}'")
d = subprocess.run(devpy, capture_output=True, text=True)
check(d.returncode == 0 and d.stdout.strip() == str(dev.cur_version()), "CLI verint with no argument reads version.txt")
d = subprocess.run(devpy + [""], capture_output=True, text=True)
check(d.returncode == 0 and d.stdout.strip() == str(dev.cur_version()), "an empty argument also reads version.txt")

print("== DEV RELEASE ENGINE OK ==" if ok else "== DEV RELEASE ENGINE FAILED ==")
sys.exit(0 if ok else 1)
