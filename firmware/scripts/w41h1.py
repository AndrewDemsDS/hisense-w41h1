#!/usr/bin/env python3
"""w41h1.py -- noob-proof wizard: stock AEH-W41H1 -> custom firmware, with an automatic
stock backup and a safe way back. Issue #74.

This is glue, not a new primitive. Every real action SHELLS OUT to the proven scripts next to
this file:
  - ota_convert_stock.sh   commission | ota <VID> <PID>   (docs/12, proven 2026-07-09/07-21)
  - ota-release.sh         revert --backup | --flip       (docs/10 §17, proven 2026-07-21)
It never reimplements flashing, repackaging, or the break-glass protocol -- if a step needs new
device-side behaviour, that belongs in the shell scripts, not here.

Flow (subcommand "convert"):
  1. preflight       -- check chip-tool/provider/ota_image_tool.py + the two shell scripts exist
  2. join the IoT SSID (nmcli on Linux; manual prompt elsewhere)
  3. "press 77" window, with a timeout + re-prompt loop (the window closes)
  4. commission the stock unit (bypass-attestation), auto-retrying the known transient IM 0x0501
  5. convert: repackage + serve the current .ota under our target vid/pid, watch for BDX completion
  6. auto-backup: discover the unit on the LAN, pull the inactive (stock) slot, save it
  7. print a summary + the two recovery commands

Subcommand "revert": discover a custom unit, explain the stock-serial guard in plain words, and
offer to flip it back to stock.

Explicitly OUT of scope here (BLOCKED, does not boot -- issue #19, docs/10 §17 Path 2): this
wizard never calls `revert --repackage` or `revert --apply`. Worst-case recovery is the CH341A
clip, referenced in the final report, not automated here.

--dry-run prints every command this would run and touches no device: no subprocess of either
shell script is ever invoked in that mode, only local, read-only preflight checks. --yes answers
every confirmation (so this doubles as an automation entry point); --dry-run implies --yes so it
never blocks on input. --ssid/--unit skip the corresponding prompt.

OS support: Linux is the primary target (nmcli for the SSID join, ip/avahi-browse for discovery,
matching the rest of firmware/scripts/). On macOS/Windows, both are gated behind an OS check and
fall back to a plain prompt: join the SSID yourself, type in the unit's address yourself. Full
non-Linux automation (networksetup/netsh, a stdlib mDNS resolver) is deferred.
"""

import argparse
import os
import pathlib
import platform
import re
import select
import shlex
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
OTA_CONVERT = HERE / "ota_convert_stock.sh"
OTA_RELEASE = HERE / "ota-release.sh"
ENV_FILE = HERE / "ota-release.env"
BUILT_IMAGES = REPO / "firmware" / "built-images"

# AmebaZ2 Realtek OUI (e0:3e:cb, confirmed on the kitchen/office units -- firmware/test/hil_frame_diff.py).
# A unit's SLAAC IPv6 address is derived from this MAC by the standard EUI-64 rule (insert
# ff:fe, flip the universal/local bit 0xe0 -> 0xe2), e.g. e0:3e:cb:31:64:c6 -> ...e23e:cbff:fe31:64c6.
# That signature substring is a reliable way to spot the unit's address regardless of whether it
# came from `ip neigh` (which has the real lladdr) or mDNS (which only has the address).
OUI_PREFIX = "e0:3e:cb"
SLAAC_SIG = "e23e:cbff:fe"

GUARD_TEXT = """
  Plain-language guard: the unit keeps its old (stock) firmware in one flash slot and the
  currently-running (custom) firmware in the other. A "flip" tells the bootloader to boot
  whichever slot ISN'T currently running. Before doing that, the tool checks the OTHER slot's
  serial number: stock always carries serial 100, custom images carry 1100-plus, so anything
  1100 or higher clearly is NOT stock. If the other slot looks like a newer custom build instead
  of stock, the flip is refused -- that is what stops you from accidentally "reverting" onto a
  different custom firmware rather than real stock. (There's a --force escape hatch for that
  case, but it is not offered here; run ota-release.sh directly if you really mean it.)
"""


def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def confirm(args, prompt):
    """y/N prompt. --yes (and --dry-run, which implies --yes) always answers yes."""
    if args.yes:
        return True
    ans = input(f"{prompt} [y/N] ").strip().lower()
    return ans in ("y", "yes")


def plan(cmd, env_overrides=None):
    """--dry-run output: the exact command that would run. Never executed from here."""
    line = " ".join(shlex.quote(str(c)) for c in cmd)
    if env_overrides:
        prefix = " ".join(
            f"{k}={shlex.quote(str(v))}" for k, v in env_overrides.items()
        )
        line = f"{prefix} {line}"
    print(f"  WOULD RUN: {line}")


def read_env(key, default=None):
    """$key from the real environment, else the same key parsed from ota-release.env, else default.
    Mirrors ms_ws.py's _env() so a bare invocation still targets the right VID/PID/tokens without
    requiring the caller to source the env file first."""
    v = os.environ.get(key)
    if v:
        return v
    try:
        with open(ENV_FILE) as f:
            for line in f:
                m = re.match(
                    rf'\s*{re.escape(key)}\s*=\s*"?([^"#\n]*?)"?\s*(?:#.*)?$', line
                )
                if m and m.group(1):
                    return m.group(1)
    except OSError:
        pass
    return default


def run_streaming(cmd, env=None):
    """Run a real command, echoing its output live (so the child's own prompts -- e.g. the
    "press Enter" in ota_convert_stock.sh commission -- reach the terminal) while also
    capturing it for the caller to grep for success/failure markers afterwards. stdin is left
    inherited so the child's own `read` still works."""
    proc = subprocess.Popen(
        [str(c) for c in cmd],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    lines = []
    for line in proc.stdout:
        print(line, end="")
        lines.append(line)
    proc.wait()
    return proc.returncode, "".join(lines)


def unit_tag(addr):
    """Short filesystem-safe label for a unit address, for the backup filename. Same idea as
    ota-release.sh's own default-name logic (last IPv4 octet / sanitized IPv6 tail)."""
    a = addr.split("%")[0]
    if re.fullmatch(r"(\d{1,3}\.){3}\d{1,3}", a):
        return a.rsplit(".", 1)[1]
    parts = [p for p in a.split(":") if p]
    tag = parts[-1] if parts else a
    return re.sub(r"[^0-9A-Za-z]+", "-", tag).strip("-") or "unit"


# ---- preflight --------------------------------------------------------------------------------


def preflight(args):
    print("\n[preflight] checking dependencies...")
    chip_root = pathlib.Path(
        os.path.expanduser(read_env("CHIP", "~/ameba-dev/connectedhomeip"))
    )
    checks = [
        ("bash", shutil.which("bash") is not None, "install bash"),
        (
            "ota_convert_stock.sh",
            OTA_CONVERT.is_file(),
            f"missing {OTA_CONVERT} -- check the checkout",
        ),
        (
            "ota-release.sh",
            OTA_RELEASE.is_file(),
            f"missing {OTA_RELEASE} -- check the checkout",
        ),
        (
            "ota-release.env",
            ENV_FILE.is_file(),
            f"copy {ENV_FILE}.example -> {ENV_FILE} and fill it in (needed for VID/PID + break-glass token)",
        ),
        (
            "chip-tool",
            (chip_root / "examples/chip-tool/out/host/chip-tool").is_file(),
            "build it from ~/ameba-dev/connectedhomeip -- see firmware/docs/12 prerequisites",
        ),
        (
            "chip-ota-provider-app",
            (
                chip_root
                / "examples/ota-provider-app/linux/out/host/chip-ota-provider-app"
            ).is_file(),
            "build it from ~/ameba-dev/connectedhomeip -- see firmware/docs/12 prerequisites",
        ),
        (
            "ota_image_tool.py",
            (chip_root / "src/app/ota_image_tool.py").is_file(),
            "part of the connectedhomeip checkout -- check CHIP/SDK_ROOT points at it",
        ),
    ]
    pw_file = pathlib.Path(
        os.path.expanduser(read_env("IOT_PW_FILE", "~/.iot_wifi_pw"))
    )
    checks.append(
        (
            "IoT Wi-Fi password file",
            pw_file.is_file(),
            f"create {pw_file} (chmod 600) with the IoT SSID password",
        )
    )

    missing = []
    for name, ok, hint in checks:
        print(f"  [{'OK' if ok else 'MISSING'}] {name}")
        if not ok:
            missing.append((name, hint))

    system = platform.system()
    if system == "Linux":
        for tool in ("nmcli", "avahi-browse", "ip"):
            print(f"  [{'OK' if shutil.which(tool) else 'optional, not found'}] {tool}")
    else:
        print(
            f"  [info] running on {system}: SSID join + auto-discovery fall back to manual prompts."
        )

    ota_src = pathlib.Path(
        os.path.expanduser(read_env("OTA_SRC", "~/kitchen-ota/rac-v23.ota"))
    )
    # convert needs OTA_SRC (ota_convert_stock.sh's `ota` step extracts it and dies without
    # it under set -e); revert does not. Hard-fail it only on the convert path so a real run
    # surfaces the missing source up front instead of deep inside the BDX watch.
    ota_required = args.command == "convert"
    ota_ok = ota_src.is_file()
    print(
        f"  [{'OK' if ota_ok else ('MISSING' if ota_required else 'optional, not found')}]"
        f" OTA_SRC ({ota_src}) -- the current custom .ota to convert onto the unit"
    )
    if ota_required and not ota_ok:
        missing.append(
            ("OTA_SRC", f"set OTA_SRC to the custom .ota to push (looked at {ota_src})")
        )

    if missing and not args.dry_run:
        print("\n[preflight] FAILED, missing:")
        for name, hint in missing:
            print(f"  - {name}: {hint}")
        die(
            "fix the above, then re-run (or use --dry-run to preview the rest of the flow)"
        )
    elif missing:
        print(
            "\n[preflight] --dry-run: continuing despite the gaps above (a real run would abort here)."
        )
    else:
        print("[preflight] all required dependencies present.")


# ---- Wi-Fi join ---------------------------------------------------------------------------------


def join_ssid(args, ssid):
    print(
        f"\n[wifi] the controller (this machine) must be on the unit's IoT SSID ('{ssid}') for "
        "BLE range + L2 reasons (docs/12 traps #1-#2) during commissioning and the OTA."
    )
    system = platform.system()
    if system != "Linux":
        print(f"  [wifi] automatic join isn't wired up for {system} yet (needs nmcli).")
        print(f"  [wifi] join '{ssid}' yourself now (Wi-Fi settings), then continue.")
        if not args.yes:
            input("  press Enter once connected... ")
        return
    if not shutil.which("nmcli"):
        print("  [wifi] nmcli not found; join the SSID yourself, then continue.")
        if not args.yes:
            input("  press Enter once connected... ")
        return
    cmd_display = ["nmcli", "dev", "wifi", "connect", ssid, "password", "<redacted>"]
    if args.dry_run:
        plan(cmd_display)
        return
    pw_file = pathlib.Path(
        os.path.expanduser(read_env("IOT_PW_FILE", "~/.iot_wifi_pw"))
    )
    if not pw_file.is_file():
        die(
            f"no Wi-Fi password file at {pw_file}. Create it, or join '{ssid}' manually and re-run with --yes."
        )
    if not confirm(args, f"Join '{ssid}' via nmcli now?"):
        print("  [wifi] skipping automatic join; make sure you're on the SSID already.")
        return
    print("  " + " ".join(shlex.quote(c) for c in cmd_display))
    real_cmd = [
        "nmcli",
        "dev",
        "wifi",
        "connect",
        ssid,
        "password",
        pw_file.read_text().strip(),
    ]
    rc = subprocess.run(real_cmd, capture_output=True, text=True).returncode
    print(
        "  joined."
        if rc == 0
        else "  nmcli reported a problem (already connected? wrong password?)"
        " -- continuing, verify manually if the next step fails."
    )


# ---- "press 77" window --------------------------------------------------------------------------


def _wait_enter(prompt, timeout):
    print(prompt)
    if os.name != "posix":
        input("  press Enter when ready (no timeout support on this OS)... ")
        return True
    sys.stdout.write(
        f"  press Enter when ready (window ~{timeout}s before it may close)... "
    )
    sys.stdout.flush()
    ready, _, _ = select.select([sys.stdin], [], [], timeout)
    if ready:
        # select() reports a closed/EOF stdin as readable; readline() then returns "".
        # Treat that as "no confirmation" so a non-interactive run cannot silently
        # confirm the 77 press and go on to a real pairing attempt. Fail closed.
        return sys.stdin.readline() != ""
    print()
    return False


def press77_window(args, attempt=1):
    """The stock unit's pairing window is short-lived (docs/12: 'the window closes; tonight it
    expired twice mid-flow'). Loop until the user confirms within the window, re-prompting (and
    telling them to press 77 again) if they miss it."""
    if args.yes:
        print("[77] --yes set: assuming the panel already shows 77, not waiting.")
        return
    while True:
        msg = (
            "\n>>> Press '77' on the unit now (swing the louver back and forth x6 until the "
            "display shows '77'). This opens a short pairing window."
        )
        if attempt > 1:
            msg += f"\n>>> (retry {attempt}: the previous window likely closed -- press 77 again.)"
        if _wait_enter(msg, timeout=90):
            return
        print(
            "[77] no response in ~90s; the window may have closed. Prompting again..."
        )
        attempt += 1


# ---- commission (stock) --------------------------------------------------------------------------


def commission_stock(args, max_attempts=4):
    env = os.environ.copy()
    if args.ssid:
        env["IOT_SSID"] = args.ssid
    cmd = [str(OTA_CONVERT), "commission"]

    for attempt in range(1, max_attempts + 1):
        press77_window(args, attempt)
        if args.dry_run:
            plan(cmd, env_overrides={"IOT_SSID": args.ssid} if args.ssid else None)
            print("[commission] --dry-run: not invoking chip-tool.")
            return True
        print(f"[commission] attempt {attempt}/{max_attempts}: {' '.join(cmd)}")
        rc, out = run_streaming(cmd, env=env)
        if re.search(r"no ota requestor", out, re.I):
            die(
                "the unit has no OTA Requestor cluster (42) -- it cannot be converted over the "
                "air. Use the CH341A clip flash path instead (firmware/docs/10-firmware-ota-procedure.md)."
            )
        if re.search(r"ota requestor present", out, re.I):
            print("[commission] OK: stock unit commissioned, OTA Requestor confirmed.")
            return True
        is_0501 = (
            re.search(r"im error.*0x0*501|0x0*501.*im error", out, re.I) is not None
        )
        has_im_error = re.search(r"im error", out, re.I) is not None
        if rc != 0 or has_im_error:
            if is_0501:
                print(
                    f"[commission] transient IM 0x0501 on attempt {attempt} (docs/12 trap #3) -- "
                    "this is the known last-handshake glitch, just retry."
                )
            else:
                print(
                    f"[commission] attempt {attempt} did not confirm success (see output above)."
                )
            if attempt < max_attempts:
                print("[commission] retrying -- press 77 again.")
                continue
        else:
            # rc == 0 but neither marker matched: ambiguous, not a known failure mode.
            print(
                "[commission] attempt finished without a clear success/failure marker -- see output above."
            )
            if attempt < max_attempts and not confirm(args, "Retry commissioning?"):
                break
    die(
        "commissioning did not succeed after retries. Checklist: is this machine on the IoT "
        "SSID? Was '77' pressed right before hitting Enter? See firmware/docs/12 traps #1-#3."
    )


# ---- convert --------------------------------------------------------------------------------------


def convert_to_custom(args, vid, pid):
    cmd = [str(OTA_CONVERT), "ota", vid, pid]
    if args.dry_run:
        plan(cmd)
        print(
            "[convert] --dry-run: not repackaging, not starting the provider, not touching the device."
        )
        return True
    print("[convert] " + " ".join(cmd))
    print(
        "[convert] this repackages the .ota for the target vid/pid, starts the local OTA "
        "provider, and drives the transfer. Watching for ApplyUpdateRequest (a generous "
        "watch -- BDX can legitimately take minutes on a lossy link; docs/12 trap #4)."
    )
    rc, out = run_streaming(cmd)
    if "ApplyUpdateRequest" not in out:
        print(
            "[convert] did not see ApplyUpdateRequest in the log -- the watch window may have "
            "closed before the transfer finished (the underlying script's watch is 300s)."
        )
        if confirm(
            args, "Re-run the convert step (re-serves the same image, safe to retry)?"
        ):
            rc, out = run_streaming(cmd)
    ok = "ApplyUpdateRequest" in out
    if ok:
        print(
            "[convert] ApplyUpdateRequest seen -- the unit is applying + rebooting into custom firmware."
        )
    else:
        print(
            "[convert] still no ApplyUpdateRequest confirmation. The unit may still be mid-transfer; "
            "check firmware/docs/12 traps, or re-run 'w41h1.py convert' once it's idle again."
        )
    return ok


# ---- discovery --------------------------------------------------------------------------------------


def _discover_ip_neigh():
    if not shutil.which("ip"):
        return []
    try:
        out = subprocess.run(
            ["ip", "-6", "neigh", "show"], capture_output=True, text=True, timeout=5
        ).stdout
    except Exception:
        return []
    found = []
    for line in out.splitlines():
        fields = line.split()
        if not fields:
            continue
        addr = fields[0]
        if SLAAC_SIG in addr.lower() or re.search(
            rf"lladdr\s+{re.escape(OUI_PREFIX)}", line, re.I
        ):
            found.append(addr)
    return found


def _discover_avahi():
    if not shutil.which("avahi-browse"):
        return []
    try:
        # -a all services, -r resolve, -p parseable, -t terminate after the initial scan.
        out = subprocess.run(
            ["avahi-browse", "-a", "-r", "-p", "-t"],
            capture_output=True,
            text=True,
            timeout=8,
        ).stdout
    except Exception:
        return []
    found = []
    for line in out.splitlines():
        if not line.startswith("="):
            continue
        if SLAAC_SIG not in line.lower():
            continue
        fields = line.split(";")
        if len(fields) > 7 and fields[7]:
            found.append(fields[7])
    return found


def discover_unit(args):
    if args.unit:
        return args.unit
    print(
        "\n[discover] looking for the unit (mDNS / neighbor table / MAC OUI e0:3e:cb)..."
    )
    system = platform.system()
    candidates = []
    if system == "Linux":
        candidates += _discover_ip_neigh()
        candidates += _discover_avahi()
    else:
        print(
            f"[discover] automatic discovery isn't wired up for {system} yet (needs ip/avahi-browse)."
        )
    candidates = sorted(set(candidates))

    if len(candidates) == 1:
        print(f"[discover] found: {candidates[0]}")
        if confirm(args, f"Use {candidates[0]}?"):
            return candidates[0]
    elif len(candidates) > 1:
        print("[discover] multiple candidates found:")
        for i, c in enumerate(candidates, 1):
            print(f"  {i}. {c}")
        if args.yes:
            print(f"[discover] --yes set: using the first candidate {candidates[0]}")
            return candidates[0]
        choice = input(
            f"Pick one [1-{len(candidates)}] or Enter to type one in: "
        ).strip()
        if choice.isdigit() and 1 <= int(choice) <= len(candidates):
            return candidates[int(choice) - 1]

    if args.yes:
        print(
            "[discover] no automatic match and --yes set: using placeholder '<unit-ip>'."
        )
        return "<unit-ip>"
    print(
        "[discover] no automatic match. Join the IoT SSID and find the unit yourself, e.g.:"
    )
    print("  Linux:  ip -6 neigh show | grep -i e0:3e:cb")
    print("  Router: check the DHCP/neighbor table for a Realtek device (OUI e0:3e:cb)")
    addr = input("Enter the unit's IPv6/IPv4 address or hostname: ").strip()
    if not addr:
        die("no unit address given; cannot continue without a target")
    return addr


# ---- auto-backup --------------------------------------------------------------------------------


def auto_backup(args, unit, custom_version):
    tag = unit_tag(unit)
    out_path = BUILT_IMAGES / f"stock-backup-{tag}-v{custom_version}.bin"
    cmd = [str(OTA_RELEASE), "revert", "--backup", unit, str(out_path)]
    if args.dry_run:
        plan(cmd)
        print(f"[backup] --dry-run: would save to {out_path}")
        return None
    print(
        "\n[backup] fetching the inactive-slot stock image -- this is the unit's way back."
    )
    rc, out = run_streaming(cmd)
    if rc != 0 or "all checks passed" not in out:
        print("[backup] FAILED or incomplete -- see the [revert] lines above.")
        print(
            "[backup] common causes: unit not yet on custom firmware >= 1.3.9, BREAKGLASS_TOKEN "
            "unset in ota-release.env, or the break-glass port unreachable from this host's VLAN."
        )
        return None
    print(f"[backup] saved: {out_path}")
    print(
        "[backup] KEEP THIS FILE. It is the unit's way back to stock ConnectLife even after a "
        "later custom OTA overwrites the stock slot."
    )
    return str(out_path)


# ---- final report --------------------------------------------------------------------------------


def print_final_report(unit, backup, code):
    print("\n" + "=" * 70)
    print("W41H1 CONVERT: SUMMARY")
    print("=" * 70)
    print(f"  unit address:  {unit or '(dry-run: not discovered)'}")
    print(f"  stock backup:  {backup or '(dry-run: not created)'}")
    print(f"  pairing code:  {code}  (Home Assistant: Matter -> Add device)")
    print("  recovery, easiest first:")
    print(f"    1. ota-release.sh revert --flip {unit or '<unit-ip>'}")
    print("       (break-glass slot flip back to stock; also: 'w41h1.py revert')")
    print(
        "    2. CH341A clip flash of the archived stock backup (worst case, needs the case open)"
    )
    print(
        "       see firmware/docs/10-firmware-ota-procedure.md §17 + firmware/flasher/ch341flash.py"
    )
    print(
        "  this wizard never runs 'revert --repackage' / 'revert --apply' (blocked, does not"
    )
    print("  boot on hardware -- issue #19, docs/10 §17 Path 2).")
    print("=" * 70)


# ---- subcommands --------------------------------------------------------------------------------


def cmd_convert(args):
    vid = read_env("VID", "0xFFF1")
    pid = read_env("PID", "0x8001")
    newver = read_env("NEWVER", "23")
    code = read_env("CODE", "34970112332")
    ssid = args.ssid or read_env("IOT_SSID", "your-iot-ssid")

    print(
        "W41H1 CONVERT: stock -> custom firmware, with an automatic stock backup afterwards."
    )
    if args.dry_run:
        print(
            "(--dry-run: printing every command this would run; no device is touched.)"
        )

    preflight(args)
    join_ssid(args, ssid)
    commission_stock(args)
    confirm(args, "Push the new firmware to the commissioned unit now?")
    ok = convert_to_custom(args, vid, pid)

    if args.dry_run:
        print_final_report(unit=None, backup=None, code=code)
        return
    if not ok:
        die(
            "conversion did not confirm ApplyUpdateRequest. Re-run 'w41h1.py convert' once the "
            "unit is idle again, or work through firmware/docs/12 traps by hand."
        )

    print(
        "\n[convert] the unit is rebooting into custom firmware; give it ~20-30s to come back on IPv6."
    )
    if not args.yes:
        input("Press Enter once you believe it's back up (or just wait a bit)... ")
    unit = discover_unit(args)
    backup = auto_backup(args, unit, newver)
    print_final_report(unit=unit, backup=backup, code=code)


def cmd_revert(args):
    print("W41H1 REVERT: flip a custom unit back to stock (break-glass slot flip).")
    if args.dry_run:
        print(
            "(--dry-run: printing every command this would run; no device is touched.)"
        )

    unit = discover_unit(args)
    print(f"\n[revert] target unit: {unit}")
    print(GUARD_TEXT)

    cmd = [str(OTA_RELEASE), "revert", "--flip", unit]
    if args.dry_run:
        plan(cmd)
        return
    if not confirm(args, "Flip this unit back to stock now?"):
        print("[revert] cancelled, no changes made.")
        return
    rc, _ = run_streaming(cmd)
    if rc != 0:
        die(
            "flip did not complete -- see the [revert] lines above (refused guard, unreachable "
            "port, or pre-1.3.8 firmware with no break-glass listener)."
        )
    print(
        "\n[revert] flip issued. Give the unit ~30s; it should rejoin ConnectLife as itself, "
        "no re-provisioning needed (docs/10 §17: the Wi-Fi profile and cloud config survive "
        "the custom firmware untouched)."
    )


# ---- CLI --------------------------------------------------------------------------------------


def _add_common_flags(ap, suppress):
    # `suppress=True` (used on the SUBparser copies) means: if the flag isn't given at the
    # subcommand level, don't put a default on the namespace at all. argparse's subparsers
    # parse into a FRESH namespace and then copy every key onto the real one -- unconditionally,
    # including defaults -- so a plain default=False here would silently stomp a --dry-run given
    # BEFORE the subcommand (`w41h1.py --dry-run convert`). SUPPRESS means "not given" leaves
    # nothing to copy, so the top-level parser's value survives; giving the flag on either side
    # of the subcommand still works.
    flag_default = argparse.SUPPRESS if suppress else False
    other_default = argparse.SUPPRESS if suppress else None
    ap.add_argument(
        "--ssid",
        default=other_default,
        help="IoT Wi-Fi SSID the unit joins (default: IOT_SSID from ota-release.env)",
    )
    ap.add_argument(
        "--unit",
        default=other_default,
        help="unit's IPv6/IPv4 address or hostname (skips auto-discovery)",
    )
    ap.add_argument(
        "--yes",
        action="store_true",
        default=flag_default,
        help="assume yes on every confirmation (for automation)",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        default=flag_default,
        help="print every command this would run; touch no device (implies --yes)",
    )


def build_parser():
    # Two copies of the same flags: real defaults at the top level, SUPPRESS at the subcommand
    # level (see _add_common_flags), so `w41h1.py --dry-run convert` and
    # `w41h1.py convert --dry-run` both work regardless of which side the flag lands on.
    common_top = argparse.ArgumentParser(add_help=False)
    _add_common_flags(common_top, suppress=False)
    common_sub = argparse.ArgumentParser(add_help=False)
    _add_common_flags(common_sub, suppress=True)

    p = argparse.ArgumentParser(
        prog="w41h1.py",
        description="Noob-proof wizard: stock W41H1 -> custom firmware, with an automatic stock "
        "backup and a safe way back. Wraps ota_convert_stock.sh + ota-release.sh; "
        "never reimplements flashing (issue #74).",
        parents=[common_top],
    )
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser(
        "convert",
        help="stock -> custom firmware, with an automatic stock-backup afterwards",
        parents=[common_sub],
    )
    sub.add_parser(
        "revert",
        help="discover a custom unit and offer to flip it back to stock",
        parents=[common_sub],
    )
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.dry_run:
        args.yes = True  # dry-run never blocks on input; it only prints plans.
    try:
        if args.command == "convert":
            cmd_convert(args)
        elif args.command == "revert":
            cmd_revert(args)
    except KeyboardInterrupt:
        print("\n[w41h1] interrupted, exiting.")
        sys.exit(130)
    except EOFError:
        print(
            "\n[w41h1] no input available on this terminal. "
            "Pass --yes for unattended use, or run it interactively."
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
