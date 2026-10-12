#!/usr/bin/env python3
"""matter-convert.py: the rules behind `convert`, the provider staging and the break-glass probes,
and the script at its edges (#143, #182).

It feeds the script what a person gets wrong: a settings file that does not parse, an image cut
short, a unit that answers with rubbish. No hardware and no network: the units and the OTA provider
host are stand-ins (a TCP listener on 127.0.0.1, recorded ssh and scp calls), ota_image_tool.py is
a small script in a temporary directory, and nothing is written outside that directory.
"""

import hashlib
import hmac as hmac_mod
import json
import os
import pathlib
import shlex
import socket
import subprocess
import sys
import tempfile
import threading

import edge_harness
from edge_harness import (CLEAN_ENV, SCRIPTS, check, died, environ, known_failure, load_script,  # noqa: F401
                          patched, quiet, raised, script)
import amebaz2_image as az
import dev
from image_fixtures import fake_image

TOOL = SCRIPTS / "matter-convert.py"
mc = load_script("matter_convert", TOOL)
NO_ENVF = "/nonexistent/ota-release.env"


def cli(*args, stdin="", **env):
    return subprocess.run([sys.executable, str(TOOL), *args], input=stdin, capture_output=True, text=True,
                          env={**CLEAN_ENV, "ENVF": NO_ENVF, **env}, timeout=60)


def refused(fn, *args):
    return died(fn, *args) is not None


tmp_holder = tempfile.TemporaryDirectory(prefix="matter-convert-")
TMP = pathlib.Path(tmp_holder.name)

print("== matter-convert.py: the pure rules ==")

# --- softwareVersion int -> semver, as the conversion image is labelled ---
check(mc.int_to_semver(10351) == "1.3.51" and mc.int_to_semver(10000) == "1.0.0", "softwareVersion int -> semver")
check(mc.int_to_semver(mc.MATTER_FINAL_VERSION) == "1.3.50", "the last Matter release is 1.3.50")


# --- provider manifest ---
m = mc.ota_manifest(b"abc", 10344, "1.3.44", "file:///rac-v10344.ota")["modelVersion"]
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
plan = mc.archive_plan("rac-v10351-esphome.json", listing)
check(plan == ["rac-v10349.json", "rac-v10350.json"], "only the same product's other manifests are archived")
check("rac-stock-v10332.json" not in plan, "stock-revert manifests are never archived")
check("broken.json" not in plan and "esp32-v10114.json" not in plan, "unreadable and other-product files untouched")
try:
    mc.archive_plan("missing.json", listing)
    check(False, "a keep manifest that is not in the listing is refused")
except ValueError:
    check(True, "a keep manifest that is not in the listing is refused")
parsed = mc.parse_listing('a.json\t{"modelVersion": {"pid": 32768}}\nb.json\tnot json\nno tab here\n')
check(parsed.get("b.json", "x") is None and parsed["a.json"]["modelVersion"]["pid"] == 0x8000 and len(parsed) == 2,
      "a listing parses per line, an unreadable manifest becomes None")


check(mc.backup_file_tag("192.168.1.37") == "37", "IPv4 backup file tag is the last octet")
check(mc.backup_file_tag("fe80::1ff:fe23:4567%wlan0") == "fe80-1ff-fe23-4567", "IPv6 tag drops the zone and sanitises")

# --- convert: an ESPHome image for the stock module, re-signed for the Matter firmware's OTA ---
lt = fake_image(1)
SLOT = dev.ESPHOME_AMEBAZ2_SLOTS["amebaz2-factory"][1]
out = mc.esphome_matter_image(lt, 11451, SLOT)
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
check(mc.esphome_matter_image(out, 11451, SLOT) == out, "convert: re-signing with the same serial is a no-op")
bad = bytearray(lt)
bad[az.HDR0 + az.HDR_LEN + 5] ^= 0x01
check(refused(mc.esphome_matter_image, bytes(bad), 11451, SLOT), "convert: an image that does not verify as built is refused")
check(refused(mc.esphome_matter_image, lt[:-1], 11451, SLOT), "convert: a truncated image is refused")
check(refused(mc.esphome_matter_image, lt[:64], 11451, SLOT), "convert: a stub is refused")
check(refused(mc.esphome_matter_image, lt, 11451, len(lt) - 1), "convert: an image larger than the slot is refused")
check(not refused(mc.esphome_matter_image, lt, 11451, len(lt)), "convert: an image that exactly fills the slot passes")
check(refused(mc.esphome_matter_image, lt, 0, SLOT), "convert: serial 0 is refused")
check(refused(mc.esphome_matter_image, lt, 0xFFFFFFFF, SLOT), "convert: the erased-flash serial is refused")
check("amebaz2-native" not in mc.CONVERT_BOARDS, "convert: native has no Matter firmware to convert from")
m = mc.ota_manifest(b"x" * 10, 10351, "1.3.51", "file:///rac-v10351-esphome.ota")["modelVersion"]
check(m["softwareVersion"] == 10351 and m["maxApplicableSoftwareVersion"] == 10350 and m["otaFileSize"] == 10,
      "convert: the manifest offers the image to every version below it")


# ================================================================================================
print("== matter-convert.py: the settings file (ota-release.env) ==")

envf = TMP / "ota-release.env"


def load(text, required=(), **env):
    envf.write_text(text)
    with patched(mc, ENVF=envf), environ(**env):
        return mc.load_env(required)


with patched(mc, ENVF=TMP / "absent.env"):
    msg = died(mc.load_env)
check(msg and "missing" in msg and "ota-release.env.example" in msg, "a missing settings file is refused and names the example")
with patched(mc, ENVF=TMP):
    check(died(mc.load_env) is not None, "a directory in place of the settings file is refused")

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
with patched(mc, ENVF=envf), environ(OTA_TOOL=None):
    msg = died(mc.load_env, ("OTA_TOOL", "VID", "PID"))
check(msg and msg.startswith("OTA_TOOL is not set"), "a required setting that is missing is named")
envf.write_text('OTA_TOOL=""\n')
with patched(mc, ENVF=envf), environ(OTA_TOOL=None):
    check(died(mc.load_env, ("OTA_TOOL",)) is not None, "a required setting that is empty counts as missing")

envf.write_text("OTA_TOOL=/t\nif then\n")
r = cli("slots", "127.0.0.1", ENVF=str(envf))
check(r.returncode == 1 and "could not source" in r.stderr and "syntax error" in r.stderr,
      "a settings file with a syntax error is refused, and bash's own message is shown")

# Two things a settings file can do that the script does not see. Both are behaviour today.
envf.write_text("OTA_TOOL=/t\nexit 0\n")
with patched(mc, ENVF=envf), environ(OTA_TOOL=None):
    cfg = mc.load_env()
check(cfg == {}, "a settings file that calls `exit 0` loads as NO settings at all, with no error")
envf.write_text('OTA_TOOL=/t\n[ -n "$NOT_SET_ANYWHERE" ] && X=1\n')
with patched(mc, ENVF=envf), environ(NOT_SET_ANYWHERE=None):
    msg = died(mc.load_env)
check(msg and "could not source" in msg,
      "a settings file whose last line is a false test is 'could not source', though every setting was read")

if os.geteuid() != 0:
    envf.write_text("OTA_TOOL=/t\n")
    envf.chmod(0)
    r = cli("slots", "127.0.0.1", ENVF=str(envf))
    envf.chmod(0o600)
    check(r.returncode == 1 and "could not source" in r.stderr, "a settings file that cannot be read is refused")

msg = died(mc.need, {"A": "1", "B": ""}, "A", "B")
check(msg and msg.startswith("B is not set"), "need() names the first setting that is missing or empty")

# The slot-selection serial base.
check(mc.serial_base({}) == 1100 and mc.serial_base({"SERIAL_BASE": ""}) == 1100, "SERIAL_BASE defaults to 1100")
check(mc.serial_base({"SERIAL_BASE": " 2000 "}) == 2000, "SERIAL_BASE is read as a decimal integer, spaces ignored")
check(mc.serial_base({"SERIAL_BASE": "0"}) == 0, "SERIAL_BASE=0 is taken as 0 (the string is not empty)")
known_failure(raised(mc.serial_base, {"SERIAL_BASE": "eleven hundred"}) in (None, "Die"),
              "a SERIAL_BASE that is not a number is refused with a message",
              "matter-convert.py (was dev.py:541) serial_base(): int() raises ValueError, which reaches the user as a traceback")
known_failure(raised(mc.serial_base, {"SERIAL_BASE": "0x44C"}) in (None, "Die"),
              "a hex SERIAL_BASE is read, or refused with a message",
              "matter-convert.py (was dev.py:541) serial_base(): same ValueError, while VID and PID in the same file are hex")

# ================================================================================================
print("== matter-convert.py: convert, from the built image to the .ota and its manifest ==")

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
    """Run convert against a temporary tree. Returns (died-with, stdout, stderr)."""
    envf.write_text(f'OTA_TOOL="{ota_tool}"\nVID="0xFFF1"\nPID="0x8001"\n{settings}')
    with patched(mc, REPO=repo, ENVF=envf), patched(dev, ESPHOME_DIR=conv_esphome), environ(SERIAL_BASE=None, PI_HOST=None):
        src = dev.slot_image(board, name)
        src.parent.mkdir(parents=True, exist_ok=True)
        if image is None:
            src.unlink(missing_ok=True)
        else:
            src.write_bytes(image)
        with quiet() as (out, err):
            msg = died(mc.convert, board, name, **opts)
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
check("not staged" in out and "matter-convert.py slots" in out, "without --stage it says the files still have to be put on the provider")

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
              "matter-convert.py (was dev.py:593) convert(): read_serial() runs for the progress line before the length check in "
              "esphome_matter_image(), so a file under 0xF8 bytes ends in an ImageFormatError traceback")

msg, out, err = convert(image=good, settings='VID="0xFFF2"\nPID="0x8002"\n')
mv = json.loads(manifest.read_text())["modelVersion"] if manifest.is_file() else {}
known_failure(mv.get("vid") == 0xFFF2 and mv.get("pid") == 0x8002,
              "the manifest carries the VID and PID of the settings file",
              "matter-convert.py (was dev.py:607) convert(): ota_manifest() is called without vid/pid, so the manifest always says "
              "0xFFF1/0x8001 while the .ota header is built from the settings file's VID/PID")

# ================================================================================================
print("== matter-convert.py: staging on the OTA provider host ==")

stage_cfg = {"PI_HOST": "root@provider", "PI_OTA_DIR": "/data/ota", "PI_SSH_KEY": "/keys/id"}
local_manifest = TMP / "rac-v10351-esphome.json"
local_manifest.write_text(json.dumps(mc.ota_manifest(b"x", 10351, "1.3.51", "file:///rac-v10351-esphome.ota")) + "\n")
local_ota = TMP / "rac-v10351-esphome.ota"
local_ota.write_bytes(b"x")


def man(pid, v):
    return json.dumps({"modelVersion": {"vid": 0xFFF1, "pid": pid, "softwareVersion": v}})


LISTING = (f"rac-v10350.json\t{man(0x8001, 10350)}\nrac-stock-v10332.json\t{man(0x8001, 10332)}\n"
           f"esp32-v10114.json\t{man(0x8000, 10114)}\n")


TMPDIR = "/tmp/ota-stage.Ab12Cd"          # what the host's mktemp answers in these tests


def stage(cfg=None, listing=LISTING, fail_at=None, scp_rc=0, listing_rc=0, tmpdir=TMPDIR):
    """Run mc.pi_stage with ssh and scp recorded. Returns (died-with, ssh commands, scp calls)."""
    ssh, scp = [], []

    def fake_ssh(cfg, command, capture=False, quiet=False):  # noqa: ARG001 - pi_ssh's signature
        ssh.append(command)
        if fail_at is not None and len(ssh) - 1 == fail_at:
            return 1, ""
        if command.startswith("mktemp"):
            return 0, tmpdir + "\n"
        if capture:
            return listing_rc, listing
        return 0, ""

    def fake_scp(cfg, files, dest):  # noqa: ARG001
        scp.append(([os.path.basename(str(f)) for f in files], dest))
        return scp_rc

    with patched(mc, pi_ssh=fake_ssh, pi_scp=fake_scp), quiet():
        msg = died(mc.pi_stage, cfg or stage_cfg, local_manifest, local_ota)
    return msg, ssh, scp


def installs(ssh):
    return [c for c in ssh if c.startswith("docker run")]


msg, ssh, scp = stage()
check(msg is None and ssh[0] == "mktemp -d /tmp/ota-stage.XXXXXX"
      and scp == [(["rac-v10351-esphome.json", "rac-v10351-esphome.ota"], f"{TMPDIR}/")],
      "staging copies the manifest and the .ota into a temporary directory the host makes")
docker = installs(ssh)[0]
check("-v /data/ota:/ota" in docker and "install -m 0644 -o root -g root /src/rac-v10351-esphome.json /src/rac-v10351-esphome.ota /ota/" in docker,
      "a root container installs both into the provider directory")
check("mv /ota/rac-v10350.json /ota/rac-v10350.json.archived" in docker, "the other manifest of the same product is archived")
check("rac-stock-v10332.json" not in docker and "esp32-v10114.json" not in docker,
      "the stock-revert manifest and the other product's manifest are left alone")
check(f"rm -rf {TMPDIR}" in docker and "docker restart matter-server" in docker,
      "the temporary directory is removed and matter-server restarted in the same command")
check("rm -f /ota/chip_kvs_ota_provider_* /ota/ota_provider_*.log" in docker,
      "the provider's own state files and logs are deleted on every staging")
check("/dev/tcp/127.0.0.1/5580" in ssh[-1], "it waits for matter-server's port to come back")

msg, ssh, scp = stage(cfg={**stage_cfg, "OTA_KEEP_MANIFESTS": "1"})
check(msg is None and not any("for f in *.json" in c for c in ssh) and ".archived" not in "".join(ssh),
      "OTA_KEEP_MANIFESTS=1 lists and archives nothing")
msg, ssh, scp = stage(fail_at=0)
check(msg == "cannot reach root@provider" and not scp, "a host that cannot be reached is an error before anything is copied")
for answer in ("", "/etc", "/tmp/ota-stage.Ab12Cd; reboot", "/tmp/ota-stage.Ab12Cd/../x"):
    msg, ssh, scp = stage(tmpdir=answer)
    check(msg == "cannot reach root@provider" and not scp and len(ssh) == 1,
          f"a host whose mktemp answers {answer!r} is not trusted with a path: nothing is copied")
msg, ssh, scp = stage(scp_rc=1)
check(msg and "scp to root@provider" in msg and not installs(ssh), "a failed copy stops before the install")
check(ssh[-1] == f"rm -rf {TMPDIR}", "and the temporary directory on the host is removed")
msg, ssh, scp = stage(fail_at=2)
check(msg == "root-container install on root@provider failed", "a failed install is an error")
msg, ssh, scp = stage(fail_at=3)
check(msg and "did not reopen :5580" in msg, "matter-server not coming back is an error")

# A listing that fails (the directory cannot be read, the connection drops) used to count as an
# empty directory: the new manifest went in BESIDE the old ones with no warning.
msg, ssh, scp = stage(listing="", listing_rc=255)
check(msg and "could not list the manifests in /data/ota" in msg and not installs(ssh),
      "a failed manifest listing stops the run: nothing is installed")
msg, ssh, scp = stage(listing="")
check(msg is None and installs(ssh) and ".archived" not in installs(ssh)[0], "an empty provider directory is fine: nothing to archive")

# Everything that reaches the remote shell is quoted.
msg, ssh, scp = stage(cfg={**stage_cfg, "PI_OTA_DIR": "/data/my ota"})
docker = installs(ssh)[0]
check("-v '/data/my ota:/ota'" in docker and "cd '/data/my ota' &&" in "".join(ssh),
      "a provider directory with a space in it is quoted for the remote shell")
check(shlex.split(docker)[:9] == ["docker", "run", "--rm", "-v", "/data/my ota:/ota", "-v", f"{TMPDIR}:/src", "alpine", "sh"],
      "and the remote shell reads it back as one argument")
inner = shlex.split(docker)[10]
check(shlex.split(inner)[:9] == ["install", "-m", "0644", "-o", "root", "-g", "root", "/src/rac-v10351-esphome.json",
                                 "/src/rac-v10351-esphome.ota"], "the container's own command line survives the quoting")
# A manifest name is what the provider host says it is. One that is not a plain file name never
# reaches the root container's shell: the run stops and says which.
for name in ("x;touch pwned.json", "$(reboot).json", "a b.json", "-rf.json", "it's.json"):
    msg, ssh, scp = stage(listing=LISTING + f"{name}\t{man(0x8001, 10001)}\n")
    check(msg and "unusual name" in msg and name in msg and not installs(ssh),
          f"a manifest named {name!r} on the host stops the run before the root container")
msg, ssh, scp = stage(listing=LISTING + f"x;touch pwned.json\t{man(0x8000, 10001)}\n")
check(msg is None and "pwned" not in installs(ssh)[0], "the same name on another product's manifest is not touched, so it does not matter")

# ================================================================================================
print("== matter-convert.py: the break-glass probes (slots, backup) ==")


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
reply = mc.breakglass_slots("127.0.0.1", "tok", unit.port, timeout=2)
unit.close()
check(reply == "ok: fw1_sn=11450 fw2_sn=42 cur=1" and unit.got == b"tok:slots", "slots: `<token>:slots` is sent and the ok: line returned")
unit = FakeUnit(lambda c: c.sendall(b"err: bad token\n"))
check(mc.breakglass_slots("127.0.0.1", "tok", unit.port, timeout=2) is None, "slots: a reply that is not ok: counts as no answer")
unit.close()
unit = FakeUnit(lambda c: c.recv(1))
check(mc.breakglass_slots("127.0.0.1", "tok", unit.port, timeout=0.2) is None, "slots: a unit that accepts and says nothing counts as no answer")
unit.close()
check(mc.breakglass_slots("127.0.0.1", "tok", closed_port(), timeout=1) is None, "slots: a refused connection counts as no answer")
check(mc.breakglass_slots("", "tok", 1) is None and mc.breakglass_slots("127.0.0.1", "", 1) is None
      and mc.breakglass_slots("127.0.0.1", "tok", "") is None, "slots: no address, no token or no port asks nothing")
unit = FakeUnit(lambda c: c.sendall(b"ok: " + b"x" * 700))
reply = mc.breakglass_query("127.0.0.1", unit.port, "tok:slots", 2)
unit.close()
check(512 <= len(reply) <= 768, "slots: a reply with no newline is cut off after 512 bytes, it does not read for ever")

slots_env = TMP / "slots.env"
slots_env.write_text(f'BREAKGLASS_TOKEN="tok"\nBREAKGLASS_PORT={closed_port()}\n')
r = cli("slots", "127.0.0.1", ENVF=str(slots_env))
check(r.returncode == 1 and "did not answer" in r.stdout and "NOT a verdict" in r.stdout,
      "matter-slots: silence exits 1 and says it proves nothing")
for cmd in ("slots", "backup"):
    r = cli(cmd, ENVF=str(slots_env))
    check(r.returncode == 1 and "the following arguments are required: unit-ip" in r.stderr and r.stderr.count("\n") == 1,
          f"{cmd} with no address says what is missing, in one line")
    r = cli(cmd, "--help")
    check(r.returncode == 0 and "unit-ip" in r.stdout, f"{cmd} --help prints its usage and needs no settings file")
r = cli("backup", "127.0.0.1", "-x", ENVF=str(slots_env))
check(r.returncode == 1 and "unrecognized arguments: -x" in r.stderr, "backup refuses a flag")
r = cli("backup", "127.0.0.1", "a.bin", "b.bin", ENVF=str(slots_env))
check(r.returncode == 1 and "unrecognized arguments: b.bin" in r.stderr, "backup refuses a third argument")
slots_env.write_text('BREAKGLASS_TOKEN=""\nBREAKGLASS_PORT=2324\n')
r = cli("slots", "127.0.0.1", ENVF=str(slots_env))
check(r.returncode == 1 and "BREAKGLASS_TOKEN is not set" in r.stderr, "the probes refuse to run with an empty token")
slots_env.write_text('BREAKGLASS_TOKEN="tok"\nBREAKGLASS_PORT=abc\n')
for cmd in ("slots", "backup"):
    r = cli(cmd, "127.0.0.1", ENVF=str(slots_env))
    known_failure(r.returncode == 1 and "Traceback" not in r.stderr,
                  f"{cmd}: a BREAKGLASS_PORT that is not a number is refused with a message",
                  "matter-convert.py (was dev.py:645) and matter-convert.py (was dev.py:675: int() on the setting raises ValueError, which reaches the user as a traceback")

# matter-backup against a stand-in unit. The image is 1 MiB and more because the carve only
# accepts [0x100000, 0x180000], the range of a real application image.
stock = fake_image(5, body=0x56000)                       # serial 5: below SERIAL_BASE, so "stock"
SLOT = stock + b"\xff" * 0x2000                           # as read from a slot: image, byte-sum, erased flash


def backup(reply, out_name="backup.bin", base="1100"):
    """Run backup against a unit that sends `reply`. Returns (exit code, stdout, file)."""
    unit = FakeUnit(lambda c: c.sendall(reply))
    out_file = TMP / out_name
    if out_file.parent.is_dir():
        out_file.unlink(missing_ok=True)
    cfg = {"BREAKGLASS_TOKEN": "tok", "BREAKGLASS_PORT": str(unit.port), "SERIAL_BASE": base}
    code = None
    with patched(mc, load_env=lambda required=(): cfg), quiet() as (out, err):
        try:
            mc.backup("127.0.0.1", str(out_file))
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
        ("a length beyond the largest slot", header(b"", length=mc.SLOT_MAX + 1), "implausible backup length"),
        ("the stream ending early", header(SLOT[:0x1000], length=len(SLOT)), "connection closed at 0x1000"),
        ("no header in the first 4 KB", b"x" * 5000, "no backup header in the first 4 KB"),
        ("the connection closing at once", b"", "connection closed before the backup header"),
        ("a slot with no erased tail", header(stock), "no application image in the backup"),
        ("an erased slot", header(b"\xff" * 0x100000), "no application image in the backup"),
        ("an image too small to be one", header(fake_image(5) + b"\xff" * 0x2000), "0x100000 to 0x180000 bytes long"),
        ("one flipped byte", header(SLOT[:0x400] + bytes([SLOT[0x400] ^ 1]) + SLOT[0x401:]), "does not verify"),
        ("a stale byte-sum", header(stock[:-1] + bytes([stock[-1] ^ 1]) + b"\xff" * 0x2000), "byte-sum = MISMATCH"),
):
    code, out, f, got = backup(reply)
    check(code == 1 and needle in out and not f.exists(), f"backup: {label} fails and saves nothing")
code, out, f, got = backup(header(SLOT), base="5")
check(code == 1 and "CUSTOM image, not stock" in out and not f.exists(),
      "backup: a serial at or above SERIAL_BASE is this project's image, not the vendor's, and is not saved")

# With no file name the backup goes to firmware/built-images/, which a fresh tree does not have.
unit = FakeUnit(lambda c: c.sendall(header(SLOT)))
fresh = TMP / "fresh-clone"
cfg = {"BREAKGLASS_TOKEN": "tok", "BREAKGLASS_PORT": str(unit.port)}
with patched(mc, load_env=lambda required=(): cfg, REPO=fresh), quiet():
    how = raised(mc.backup, "127.0.0.1")
unit.close()
saved = fresh / "firmware/built-images/stock-backup-1-sn5.bin"
check(how is None and saved.is_file() and saved.read_bytes() == SLOT,
      "backup: the default file is written on a tree that has no built-images directory yet")
code, out, f, got = backup(header(SLOT), out_name="new/dir/backup.bin")
check(code is None and f.read_bytes() == SLOT, "backup: a directory named in the file name is created")
# A save that cannot work (the path runs through a file) must not lose a slot that was fetched and verified.
(TMP / "a-file").write_text("")
code, out, f, got = backup(header(SLOT), out_name="a-file/backup.bin")
rescue = [ln.split(" is in ", 1)[1].split(" instead", 1)[0] for ln in out.splitlines() if "the backup is in " in ln]
check(code is None and len(rescue) == 1 and pathlib.Path(rescue[0]).read_bytes() == SLOT,
      "backup: when the file cannot be written the slot is saved in a temporary file, and the path is printed")
for r in rescue:
    pathlib.Path(r).unlink()

for ip, tag in (("10.0.0.7", "7"), ("1.2.3", "1-2-3"), ("unit.local", "unit-local"), ("::1", "1"), ("", "")):
    check(mc.backup_file_tag(ip) == tag, f"backup file tag of {ip!r} is {tag!r}")

# ================================================================================================
print("== matter-convert.py: the command line ==")

r = cli()
check(r.returncode == 1 and "matter-convert.py convert --board" in r.stdout, "no arguments prints the usage and exits 1")
r = cli("--help")
check(r.returncode == 0 and "matter-convert.py convert --board" in r.stdout, "--help prints the usage and exits 0")
for args, needle, why in (
        (["frobnicate"], "invalid choice: 'frobnicate'", "an unknown command is named"),
        (["convert"], "the following arguments are required: --board", "convert without a board"),
        (["convert", "--board", "c3"], "convert is for --board amebaz2-factory", "convert for an ESP32 board"),
        (["convert", "--board", "amebaz2-native"], "amebaz2-native has none", "convert for the native layout"),
        (["convert", "--board", "amebaz2-factory", "--serial", "0"], "positive integer (got '0')", "serial 0"),
        (["convert", "--board", "amebaz2-factory", "--serial", "-1"], "positive integer (got '-1')", "a negative serial"),
        (["convert", "--board", "amebaz2-factory", "--serial", "1.5"], "positive integer", "a fractional serial"),
        (["convert", "--board", "amebaz2-factory", "--serial", "0x10"], "positive integer", "a hex serial"),
        (["convert", "--board", "amebaz2-factory", "--serial"], "argument --serial: expected one argument", "--serial with no value"),
        (["convert", "--board", "amebaz2-factory", "--software-version", "0"], "positive integer", "softwareVersion 0"),
        (["convert", "--board", "amebaz2-factory", "--name", "Bad_Name"], "is not a valid hostname", "a node name that is no hostname"),
        (["convert", "--board", "amebaz2-factory", "--bogus"], "unrecognized arguments: --bogus", "an unknown option"),
        (["convert", "--board", "amebaz2-factory"], f"missing {NO_ENVF}", "convert without a settings file"),
        (["slots", "127.0.0.1"], f"missing {NO_ENVF}", "slots without a settings file"),
        (["backup", "127.0.0.1"], f"missing {NO_ENVF}", "backup without a settings file"),
):
    r = cli(*args)
    check(r.returncode == 1 and needle in r.stderr and r.stderr.count("\n") == 1 and "[matter-convert] ERROR:" in r.stderr
          and r.stdout == "", f"refused in one ERROR line, exit 1: {why}")

check(mc.SLOT_MAX == max(length for _, length in dev.ESPHOME_AMEBAZ2_SLOTS.values()),
      "SLOT_MAX, the backup length limit, is the largest slot of any layout")
check(mc.int_to_semver(mc.MATTER_FINAL_VERSION + 1) == "1.3.51", "the default conversion version is labelled 1.3.51")

tmp_holder.cleanup()
sys.exit(edge_harness.finish("MATTER-CONVERT"))
