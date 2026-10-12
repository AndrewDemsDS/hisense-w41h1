#!/usr/bin/env python3
"""matter-convert.py -- move a stock module off this project's retired Matter firmware (issue #182).

A stock module that still runs the Matter firmware (tag matter-final) installs whatever its OTA
provider sends into the slot it is not running from, and its bootloader boots the valid slot with
the higher FWHS serial. `convert` re-signs an ESPHome image with a serial in the Matter numbering
and wraps it as a Matter .ota. `slots` and `backup` read a unit's slot state over that firmware's
break-glass listener, which is how the flash layout is found. Nothing here is needed to build,
flash or update the ESPHome firmware itself: that is dev.py.

  matter-convert.py convert --board amebaz2-factory|amebaz2-sdk [--name N]
                            [--software-version V] [--serial S] [--stage]
        Run `dev.py build esphome --board amebaz2-<layout>` first. Re-signs that image with an
        FWHS serial above the running Matter image (default SERIAL_BASE + version, the Matter
        build's own rule) and wraps it as a Matter .ota at a softwareVersion one above the last
        Matter release (default), with its provider manifest, in firmware/built-images/.
        --stage also puts both on the OTA provider host and restarts matter-server. Needs
        connectedhomeip's ota_image_tool.py (OTA_TOOL in the settings file).
  matter-convert.py slots <unit-ip>
        Read both firmware slots' serials from the unit (read-only).
  matter-convert.py backup <unit-ip> [out.bin]
        Save the unit's inactive slot, and print its address, which tells the flash layout
        (read-only).

Settings: firmware/scripts/ota-release.env (copy ota-release.env.example), or the file ENVF names.
The whole procedure: docs/guide/Converting-a-Stock-Module-to-ESPHome.md.
"""

import argparse
import base64
import hashlib
import json
import os
import re
import shlex
import socket
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import amebaz2_image  # noqa: E402
import dev  # noqa: E402
from dev import C, Die, die  # noqa: E402

REPO = HERE.parent.parent
# The settings file (see ota-release.env.example).
ENVF = Path(os.environ.get("ENVF") or HERE / "ota-release.env")
# Layouts a unit running the Matter firmware can have. native is absent: it has LibreTiny's
# bootloader and no Matter firmware to convert from.
CONVERT_BOARDS = ("amebaz2-factory", "amebaz2-sdk")


def say(m): print(f"{C['cyan']}[matter-convert]{C['off']} {m}")
def warn(m): print(f"{C['yellow']}[matter-convert] WARNING:{C['off']} {m}", file=sys.stderr)


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



# A manifest name that may go into the root container's command line. The provider directory is
# on another machine, so what it holds is input, not something this script wrote.
SAFE_MANIFEST = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*\.json")


def pi_stage(cfg, manifest, *others):
    """Install files into the root-owned provider dir through a throwaway root container (the user
    is in the docker group, sudo needs a password), archive every other manifest for the same
    product, and restart matter-server (manifests are read once at init).

    Every path that reaches the remote shell is quoted, and a failed step stops the run before the
    next one: nothing is installed after a listing that failed."""
    q = shlex.quote
    files = [manifest, *others]
    keep = os.path.basename(str(manifest))
    host, ota_dir = cfg["PI_HOST"], cfg["PI_OTA_DIR"]
    rc, tmp = pi_ssh(cfg, "mktemp -d /tmp/ota-stage.XXXXXX", capture=True)
    tmp = tmp.strip()
    if rc != 0 or not re.fullmatch(r"/tmp/ota-stage\.\w{6}", tmp):
        die(f"cannot reach {host}")

    def give_up(message):
        pi_ssh(cfg, f"rm -rf {q(tmp)}", quiet=True)
        die(message)

    if pi_scp(cfg, files, f"{tmp}/") != 0:
        give_up(f"scp to {host}:{tmp} failed")
    plan = []
    if cfg.get("OTA_KEEP_MANIFESTS") != "1":
        # listing = name<TAB>json for every manifest on the Pi, plus the local one being shipped
        rc, listing = pi_ssh(cfg, f'cd {q(ota_dir)} && for f in *.json; do [ -r "$f" ] || continue; '
                                  "printf '%s\\t' \"$f\"; tr -d '\\n' < \"$f\"; echo; done", capture=True)
        if rc != 0:
            give_up(f"could not list the manifests in {ota_dir} on {host}: nothing was staged "
                    "(OTA_KEEP_MANIFESTS=1 stages without archiving the others)")
        listing += f"{keep}\t{Path(manifest).read_text().replace(chr(10), '')}\n"
        try:
            plan = archive_plan(keep, parse_listing(listing))
        except ValueError:
            give_up("could not plan manifest archiving")
        odd = [n for n in plan if not SAFE_MANIFEST.fullmatch(n)]
        if odd:
            give_up(f"the provider directory holds a manifest for this product with an unusual name ({odd[0]!r}): "
                    "nothing was staged. Move it away by hand, or set OTA_KEEP_MANIFESTS=1")
    steps = ["install -m 0644 -o root -g root " + " ".join(q(f"/src/{os.path.basename(str(f))}") for f in files) + " /ota/",
             "rm -f /ota/chip_kvs_ota_provider_* /ota/ota_provider_*.log"]
    steps += [f"mv {q('/ota/' + f)} {q('/ota/' + f + '.archived')}" for f in plan]
    rc, _ = pi_ssh(cfg, f"docker run --rm -v {q(ota_dir + ':/ota')} -v {q(tmp + ':/src')} alpine sh -c {q(' && '.join(steps))} "
                        f"&& rm -rf {q(tmp)} && docker restart matter-server >/dev/null")
    if rc != 0:
        give_up(f"root-container install on {host} failed")
    if plan:
        say(f"  archived {len(plan)} other manifest(s) for this product (renamed *.json.archived, reversible)")
    say("  staged via root container, provider junk pruned; waiting for matter-server to reload (~100 s)")
    rc, _ = pi_ssh(cfg, 'for i in $(seq 60); do bash -c "echo > /dev/tcp/127.0.0.1/5580" 2>/dev/null '
                        "&& exit 0; sleep 3; done; exit 1")
    if rc != 0:
        die("matter-server did not reopen :5580 within 3 min of the restart")
    say("  matter-server is serving again")


SLOT_MAX = 0x1AC000


def serial_base(cfg):
    return int(cfg.get("SERIAL_BASE") or 1100)


def esphome_matter_image(image, serial, slot_len):
    """A LibreTiny AmebaZ2 app image (payload + 4-byte sum) re-signed with FWHS serial `serial`,
    as the bytes the Matter firmware's OTA writes into the inactive slot. Dies instead of returning
    anything the bootloader would refuse: it HANGS on a bad signature, with no fall-back (#75).

    The bootloader boots the valid slot with the higher serial, so `serial` has to be above the
    running Matter image's. LibreTiny builds with the same default key the Matter build signs with,
    which is why re-signing is all it takes."""
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


def convert(board, name=None, software_version=None, serial=None, stage=False):
    """Wrap the built ESPHome image for a stock module as a Matter .ota (see the usage text)."""
    if board not in CONVERT_BOARDS:
        die("convert is for --board amebaz2-factory or amebaz2-sdk: it needs the Matter firmware "
            "running on the stock module to install the image (amebaz2-native has none)")
    cfg = load_env(("OTA_TOOL", "VID", "PID"))
    if stage:
        need(cfg, "PI_HOST", "PI_OTA_DIR", "PI_SSH_KEY")
    slot_len = dev.ESPHOME_AMEBAZ2_SLOTS[board][1]
    src = dev.slot_image(board, name)
    if not src.is_file():
        die(f"no {src} -- run: dev.py build esphome --board {board}" + (f" --name {name}" if name else ""))
    name = name or "hisense-ac"
    v = software_version or MATTER_FINAL_VERSION + 1
    serial = serial or serial_base(cfg) + v
    semver = int_to_semver(v)
    if board != "amebaz2-factory":
        warn(f"{board}: only the factory layout has been converted on hardware (2026-10-10)")
    say(f"convert {src.name} ({name}, {board[8:]} layout): FWHS serial {serial}, Matter softwareVersion {v}")
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
    if stage:
        say(f"stage on {cfg['PI_HOST']}:{cfg['PI_OTA_DIR']} + restart matter-server")
        pi_stage(cfg, manifest, ota)
    else:
        say("  not staged: copy both to the OTA provider directory and restart matter-server, or re-run with --stage")
    say(f"next: check the unit's running serial is below {serial} (matter-convert.py slots <ip>),")
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


def backup(ip, out=""):
    cfg = load_env()
    need(cfg, "BREAKGLASS_TOKEN", "BREAKGLASS_PORT")
    token, port, base = cfg["BREAKGLASS_TOKEN"], int(cfg["BREAKGLASS_PORT"]), serial_base(cfg)
    say(f"backup {ip}:{port} (fetch + validate the inactive-slot stock image)")

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


def slots(ip):
    """Read-only break-glass probe: prints the slot map and changes NOTHING. An answer PROVES the
    custom firmware is alive, silence proves nothing."""
    cfg = load_env()
    need(cfg, "BREAKGLASS_TOKEN", "BREAKGLASS_PORT")
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


# ---- command line ------------------------------------------------------------------------------
class Parser(argparse.ArgumentParser):
    """A usage error is one ERROR line and exit 1, like every other refusal."""
    def error(self, message):
        die(message)


def positive_int(text):
    if not text.isdigit() or int(text) < 1:
        raise argparse.ArgumentTypeError(f"takes a positive integer (got '{text}')")
    return int(text)


def hostname(text):
    if text and not re.fullmatch(r"[a-z0-9]([a-z0-9-]{0,29}[a-z0-9])?", text):
        raise argparse.ArgumentTypeError(f"'{text}' is not a valid hostname (lowercase letters, digits, hyphens; max 31)")
    return text


def main(argv):
    ap = Parser(prog="matter-convert.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", metavar="{convert,slots,backup}")
    p = sub.add_parser("convert", help="wrap the built ESPHome image as a Matter .ota")
    p.add_argument("--board", required=True)
    p.add_argument("--name", type=hostname)
    p.add_argument("--software-version", type=positive_int)
    p.add_argument("--serial", type=positive_int)
    p.add_argument("--stage", action="store_true")
    p = sub.add_parser("slots", help="read both slots' serials from a unit (read-only)")
    p.add_argument("ip", metavar="unit-ip")
    p = sub.add_parser("backup", help="save a unit's inactive slot (read-only)")
    p.add_argument("ip", metavar="unit-ip")
    p.add_argument("out", nargs="?", default="", metavar="out.bin")
    a = ap.parse_args(argv)
    if a.cmd == "convert":
        convert(a.board, a.name, a.software_version, a.serial, a.stage)
    elif a.cmd == "slots":
        slots(a.ip)
    elif a.cmd == "backup":
        backup(a.ip, a.out)
    else:
        ap.print_help()
        sys.exit(1)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except Die as e:
        print(f"{C['red']}[matter-convert] ERROR:{C['off']} {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        sys.exit(130)
