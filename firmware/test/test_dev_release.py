#!/usr/bin/env python3
"""dev.py release engine: the pure rules behind `dev.py ota amebaz2 lint|verint|tag` (#143).

No SDK, no env file, no network. While ota-release.sh still exists its `verint` must agree with
dev.py on every input here, so the port cannot drift from the script it replaces.
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

# --- CLI, and parity with the script it replaces ---
devpy = [sys.executable, str(SCRIPTS / "dev.py"), "ota", "amebaz2", "verint"]
script = SCRIPTS / "ota-release.sh"
for arg in ("1.3.44", "1.0.0", "34", "0", "1.100.0", "1.2", "bogus"):
    d = subprocess.run(devpy + [arg], capture_output=True, text=True)
    want_rc = 1 if refused(dev.semver_to_int, arg) else 0
    check(d.returncode == want_rc and (want_rc == 1 or d.stdout.strip() == str(dev.semver_to_int(arg))),
          f"CLI verint {arg}: exit {d.returncode}, stdout '{d.stdout.strip()}'")
    if script.is_file():
        s = subprocess.run(["bash", str(script), "verint", arg], capture_output=True, text=True)
        check((s.returncode, s.stdout.strip()) == (d.returncode, d.stdout.strip()),
              f"verint {arg}: dev.py and ota-release.sh agree")
d = subprocess.run(devpy, capture_output=True, text=True)
check(d.returncode == 0 and d.stdout.strip() == str(dev.cur_version()), "CLI verint with no argument reads version.txt")
d = subprocess.run(devpy + [""], capture_output=True, text=True)
check(d.returncode == 0 and d.stdout.strip() == str(dev.cur_version()), "an empty argument also reads version.txt")

print("== DEV RELEASE ENGINE OK ==" if ok else "== DEV RELEASE ENGINE FAILED ==")
sys.exit(0 if ok else 1)
