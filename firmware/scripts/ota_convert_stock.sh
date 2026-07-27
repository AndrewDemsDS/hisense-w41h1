#!/usr/bin/env bash
# Convert a STOCK AEH-W41H1 to our custom firmware purely over-the-air (no CH341A).
# Proven 2026-07-09 (kitchen unit). Full rationale: firmware/docs/12-ota-convert-stock-unit.md
#
# This automates the mechanical/deterministic parts. Two steps are inherently manual and are
# prompted for: pressing "77" on the unit, and (once) having the laptop on the device's L2.
#
# Usage:
#   ota_convert_stock.sh commission        # step A: bypass-commission the stock unit, print identity
#   ota_convert_stock.sh ota <VID> <PID> [--dry-run]   # step B: repackage .ota for <VID>/<PID> and push it
#
# Env overrides (defaults are generic placeholders, not this bench's real values -- this file
# is git-tracked/public; export real values in your shell before running, e.g.
# `export IOT_SSID=... IOT_PW_FILE=... BREAKGLASS_TOKEN=...`, or lift them from the gitignored
# ota-release.env with `set -a; source ota-release.env; set +a` first):
#   CHIP        ~/ameba-dev/connectedhomeip
#   OTA_SRC     firmware/built-images/… or the provider dir copy (the current .ota to ship)
#   STORE/WORK  ~/w41h1-ota-convert(/ct-store)  -- generic, not device-specific: this script
#               converts ANY unit, not just the kitchen one it was first proven on 2026-07-09
#   IOT_SSID    your-iot-ssid          IOT_PW_FILE  ~/.iot_wifi_pw
#   NODE        100 (target on chip-tool fabric)   PROV_NODE 1
#   NEWVER      23   CURVER_MAX 22     CODE 34970112332
#   UNIT_IP     unset (optional -- the unit's IPv6/link-local addr; enables the post-OTA
#               break-glass probe in the `ota` subcommand, needs BREAKGLASS_TOKEN too)
#   BREAKGLASS_TOKEN / BREAKGLASS_PORT   unset / 2324 (see ota-release.env; fails closed --
#               unset just skips the post-OTA probe instead of faking a result)
# Hardened like its sibling scripts: -e halts a mid-sequence failure (e.g. a failed chip-tool
# write) instead of silently continuing; -u catches unset vars; pipefail propagates pipe failures.
# Informational `... | grep | tail` filters (which legitimately match nothing) and the interactive
# teardown are explicitly `|| true`-guarded so they don't trip -e.
set -euo pipefail
CHIP="${CHIP:-$HOME/ameba-dev/connectedhomeip}"
CT="$CHIP/examples/chip-tool/out/host/chip-tool"
OP="$CHIP/examples/ota-provider-app/linux/out/host/chip-ota-provider-app"
TOOL="$CHIP/src/app/ota_image_tool.py"
STORE="${STORE:-$HOME/w41h1-ota-convert/ct-store}"
WORK="${WORK:-$HOME/w41h1-ota-convert}"
OTA_SRC="${OTA_SRC:-$WORK/rac-v23.ota}"
IOT_SSID="${IOT_SSID:-your-iot-ssid}"; IOT_PW_FILE="${IOT_PW_FILE:-$HOME/.iot_wifi_pw}"
NODE="${NODE:-100}"; PROV_NODE="${PROV_NODE:-1}"
NEWVER="${NEWVER:-23}"; CURVER_MAX="${CURVER_MAX:-22}"; CODE="${CODE:-34970112332}"
UNIT_IP="${UNIT_IP:-}"; BREAKGLASS_TOKEN="${BREAKGLASS_TOKEN:-}"; BREAKGLASS_PORT="${BREAKGLASS_PORT:-2324}"
mkdir -p "$STORE" "$WORK"
die(){ echo "ERROR: $*" >&2; exit 1; }
for f in "$CT" "$OP" "$TOOL"; do [ -x "$f" ] || [ -f "$f" ] || die "missing $f — build chip-tool/ota-provider first (see docs/12)"; done

case "${1:-}" in
  commission)
    echo ">>> Ensure the LAPTOP is on '$IOT_SSID' (same L2 as the unit) and press '77' NOW."
    read -rp "Press Enter once the panel shows 77... " _
    echo ">>> Commissioning (bypass). Retry this subcommand if you see IM 0x0501 (transient)."
    "$CT" pairing code-wifi "$NODE" "$IOT_SSID" "$(cat "$IOT_PW_FILE")" "$CODE" \
      --bypass-attestation-verifier 1 --storage-directory "$STORE" 2>&1 | \
      grep -iE "Commissioning complete|CHIP Error|IM Error|Timeout|success" | tail -6 || true
    echo ">>> Identity + OTA Requestor check:"
    for a in vendor-id product-id software-version; do
      "$CT" basicinformation read "$a" "$NODE" 0 --storage-directory "$STORE" 2>&1 | grep -iE "VendorID|ProductID|SoftwareVersion" | tail -1 || true
    done
    echo ">>> Prior-life check (a surviving Matter DCT silently blocks the post-OTA BLE window):"
    CF_LINE="$("$CT" operationalcredentials read commissioned-fabrics "$NODE" 0 --storage-directory "$STORE" 2>&1 | grep -iE "CommissionedFabrics" | tail -1)"
    echo "    $CF_LINE"
    CF_N="$(echo "$CF_LINE" | grep -oE '[0-9]+' | tail -1 || true)"
    if [ -n "${CF_N:-}" ] && [ "$CF_N" -gt 1 ]; then
      echo ">>> WARNING: $CF_N commissioned fabrics already on this stock unit (we only just added"
      echo "    one ourselves) -- it has a prior custom/cloud life. The Matter DCT (0x3E0000/0x3ED000)"
      echo "    SURVIVES the stock<->custom OTA, so after conversion FabricCount()!=0 and the firmware"
      echo "    will NOT auto-open a BLE commissioning window (connectedhomeip Server.cpp). Plan to run"
      echo "    '<token>:wipekv' over the break-glass listener BEFORE commissioning the converted unit"
      echo "    (docs/12 'SendTrustedRootCert wedge' section)."
    fi
    "$CT" descriptor read server-list "$NODE" 0 --storage-directory "$STORE" 2>&1 | grep -iE "42 \(Ota" && \
      echo ">>> OTA Requestor present. Run: $0 ota <VID> <PID>" || echo ">>> NO OTA Requestor (42) — use CH341A flash instead."
    ;;
  ota)
    VID="${2:?need target VID}"; PID="${3:?need target PID}"
    TGT="$WORK/target-v${NEWVER}.ota"
    echo ">>> Repackaging $OTA_SRC -> $TGT with header vid=$VID pid=$PID ver=$NEWVER"
    python3 "$TOOL" extract "$OTA_SRC" "$WORK/payload.bin" >/dev/null 2>&1 || die "extract failed"
    python3 "$TOOL" create -v "$VID" -p "$PID" -vn "$NEWVER" -vs "${NEWVER}.0" -mi 1 -ma "$CURVER_MAX" \
      -da sha256 "$WORK/payload.bin" "$TGT" >/dev/null 2>&1 || die "create failed"
    python3 "$TOOL" show "$TGT" | grep -iE "Vendor Id|Product Id|Version:" || true
    if printf '%s\n' "$@" | grep -qx -- --dry-run; then
      echo ">>> --dry-run: .ota repackaged + header verified (host-only); skipping provider/commission/OTA."
      exit 0
    fi
    echo ">>> Starting provider + driving OTA"
    rm -f /tmp/ota_prov.kvs
    "$OP" --discriminator 22 --secured-device-port 5560 --KVS /tmp/ota_prov.kvs --filepath "$TGT" > "$WORK/provider.log" 2>&1 &
    PP=$!; sleep 4
    "$CT" pairing onnetwork "$PROV_NODE" 20202021 --storage-directory "$STORE" 2>&1 | grep -iE "complete|Error" | tail -2 || true
    "$CT" accesscontrol write acl "[{\"fabricIndex\":1,\"privilege\":5,\"authMode\":2,\"subjects\":[112233],\"targets\":null},{\"fabricIndex\":1,\"privilege\":3,\"authMode\":2,\"subjects\":null,\"targets\":null}]" "$PROV_NODE" 0 --storage-directory "$STORE" >/dev/null 2>&1
    "$CT" otasoftwareupdaterequestor write default-otaproviders "[{\"fabricIndex\":1,\"providerNodeID\":$PROV_NODE,\"endpoint\":0}]" "$NODE" 0 --storage-directory "$STORE" >/dev/null 2>&1
    "$CT" otasoftwareupdaterequestor announce-otaprovider "$PROV_NODE" 0 0 0 "$NODE" 0 --storage-directory "$STORE" 2>&1 | grep -iE "Status=0x0|Error" | tail -1 || true
    echo ">>> Announced. Watching transfer (Ctrl-C when you see ApplyUpdateRequest)..."
    timeout 300 tail -f "$WORK/provider.log" | grep --line-buffered -iE "QueryImage|UpdateAvailable|BlockAckEOF|ApplyUpdateRequest" || true
    kill "$PP" 2>/dev/null || true
    echo ">>> On ApplyUpdateRequest the unit reboots into our firmware (allow ~20-30s to come back up)."
    if [ -n "$UNIT_IP" ] && [ -n "$BREAKGLASS_TOKEN" ]; then
      echo ">>> Probing the break-glass listener at $UNIT_IP:$BREAKGLASS_PORT (docs/10 #61)..."
      sleep 20
      BG_REPLY="$(python3 - "$UNIT_IP" "$BREAKGLASS_TOKEN" "$BREAKGLASS_PORT" <<'PY'
import socket,sys
ip,token,port=sys.argv[1],sys.argv[2],int(sys.argv[3])
try:
    with socket.create_connection((ip,port),timeout=10) as s:
        s.sendall(f"{token}:slots".encode()); s.settimeout(10)
        print(s.recv(256).decode(errors="replace").strip())
except OSError as e:
    print(f"unreachable: {e}")
PY
)"
      echo "    break-glass reply: $BG_REPLY"
    else
      echo ">>> (set UNIT_IP + BREAKGLASS_TOKEN to auto-probe the break-glass listener here -- docs/12)"
    fi
    echo ">>> If this unit has ANY prior custom/cloud life (see the commission-step warning above, or"
    echo "    docs/12 'SendTrustedRootCert wedge'), its Matter DCT survived the OTA intact:"
    echo "    FabricCount()!=0 means NO auto BLE window opens -- run '<token>:wipekv' over the"
    echo "    break-glass listener FIRST, then commission."
    echo ">>> Otherwise: the post-77 commissioning window on custom firmware is only 180s"
    echo "    (kRecommissionWindowSec, matter_drivers.cpp:309). Have this ready BEFORE you press 77:"
    echo "        $CT pairing code $NODE $CODE --storage-directory $STORE"
    echo "    (or in HA: Matter -> Add device -> ${CODE:0:4}-${CODE:4:3}-${CODE:7}, or matter-server's"
    echo "    commission_with_code(..., network_only=True))"
    echo ">>> Command staged above -- press 77 now, then run it immediately."
    ;;
  *) echo "usage: $0 {commission | ota <VID> <PID> [--dry-run]}  (see firmware/docs/12-ota-convert-stock-unit.md)";;
esac
