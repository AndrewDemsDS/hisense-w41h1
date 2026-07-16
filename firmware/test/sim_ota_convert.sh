#!/usr/bin/env bash
# No-hardware simulation of the stock -> custom Matter OTA conversion (F2 plumbing).
#
# Runs connectedhomeip's Linux chip-ota-requestor-app (stands in for the A/C's OTA
# Requestor), chip-ota-provider-app (serves our .ota) and chip-tool, all on loopback,
# and asserts the real Matter OTA sequence: QueryImage -> UpdateAvailable -> BDX blocks
# -> ApplyUpdateRequest. This validates the .ota packaging + OTA transport end-to-end
# with zero hardware. It does NOT exercise the AmebaZ2 image processor / real boot.
#
# Prereqs: the connectedhomeip host tools (chip-tool, chip-ota-provider-app,
# chip-ota-requestor-app). Build them with:
#   cd $SDK_ROOT/connectedhomeip && source scripts/activate.sh
#   for t in chip-tool ota-provider-app/linux ota-requestor-app/linux; do
#     (cd examples/$t && gn gen out/host --args='treat_warnings_as_errors=false is_debug=false' && ninja -C out/host); done
set -uo pipefail

SDK_ROOT="${SDK_ROOT:-$HOME/ameba-dev}"
CHIP="$SDK_ROOT/connectedhomeip"
CT="$CHIP/examples/chip-tool/out/host/chip-tool"
OP="$CHIP/examples/ota-provider-app/linux/out/host/chip-ota-provider-app"
RQ="$CHIP/examples/ota-requestor-app/linux/out/host/chip-ota-requestor-app"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# .ota to serve: arg $1, else the highest-versioned committed fixture.
OTA="${1:-$(ls -1 "$REPO"/firmware/built-images/rac-v*.ota 2>/dev/null | sort -V | tail -1)}"

for f in "$CT" "$OP" "$RQ"; do
  [ -x "$f" ] || { echo "MISSING host tool: $f — build it (see header)."; exit 3; }
done
[ -f "$OTA" ] || { cat <<EOF
no .ota to serve. .ota images are build artifacts (gitignored), so a fresh clone has none. Either:
  - build one:  firmware/scripts/ota-release.sh build && firmware/scripts/ota-release.sh package
                (drops firmware/built-images/rac-vN.ota, which this script auto-finds), or
  - pass one:   firmware/test/sim_ota_convert.sh /path/to/some.ota
EOF
exit 3; }
echo "== sim OTA convert: serving $(basename "$OTA") =="

WORK="$(mktemp -d /tmp/sim_ota.XXXXXX)"; STORE="$WORK/ct"; mkdir -p "$STORE"
PP=""; RP=""
cleanup(){ [ -n "$PP" ] && kill "$PP" 2>/dev/null; [ -n "$RP" ] && kill "$RP" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

echo "-- start provider (node will be 1) + requestor (node 2), loopback --"
"$OP" --discriminator 111 --secured-device-port 5561 --KVS "$WORK/prov.kvs" \
      --filepath "$OTA" > "$WORK/provider.log" 2>&1 & PP=$!
"$RQ" --discriminator 222 --secured-device-port 5562 --KVS "$WORK/req.kvs" \
      --vendor-id 0xFFF1 --product-id 0x8001 \
      --otaDownloadPath "$WORK/downloaded.bin" > "$WORK/requestor.log" 2>&1 & RP=$!
sleep 4

# Capture to a file then grep it — piping chip-tool into `grep -q` trips SIGPIPE under
# `set -o pipefail` (grep exits early, chip-tool dies 141, pipeline reports failure).
pair(){ timeout 90 "$CT" pairing onnetwork-long "$1" 20202021 "$2" --storage-directory "$STORE" > "$WORK/pair_$1.log" 2>&1
        grep -qiE "commissioning complete for node.*success|completed with success" "$WORK/pair_$1.log"; }
echo "-- commission provider (1) + requestor (2) --"
pair 1 111 && echo "   provider commissioned" || { echo "FAIL: provider commission"; tail -5 "$WORK/provider.log"; exit 1; }
pair 2 222 && echo "   requestor commissioned" || { echo "FAIL: requestor commission"; tail -5 "$WORK/requestor.log"; exit 1; }

echo "-- provider ACL + point requestor at provider + announce --"
timeout 60 "$CT" accesscontrol write acl '[{"fabricIndex":1,"privilege":5,"authMode":2,"subjects":[112233],"targets":null},{"fabricIndex":1,"privilege":3,"authMode":2,"subjects":null,"targets":null}]' 1 0 --storage-directory "$STORE" >/dev/null 2>&1
timeout 60 "$CT" otasoftwareupdaterequestor write default-otaproviders '[{"fabricIndex":1,"providerNodeID":1,"endpoint":0}]' 2 0 --storage-directory "$STORE" >/dev/null 2>&1
timeout 60 "$CT" otasoftwareupdaterequestor announce-otaprovider 1 0 0 0 2 0 --storage-directory "$STORE" >/dev/null 2>&1

echo "-- watch for QueryImage -> BDX -> ApplyUpdateRequest (up to 240s; 1.2MB over loopback is slow) --"
ok=0
for i in $(seq 1 80); do
  if grep -qiE "ApplyUpdateRequest|Applying.*update|kApplying|BlockAckEOF|Transfer completed" "$WORK/provider.log" "$WORK/requestor.log" 2>/dev/null; then ok=1; break; fi
  sleep 3
done
echo "--- provider OTA lines ---";  grep -iE "QueryImage|UpdateAvailable|ApplyUpdateRequest|BDX" "$WORK/provider.log"  2>/dev/null | tail -6
echo "--- requestor OTA lines ---"; grep -iE "QueryImage|UpdateAvailable|Downloading|ApplyUpdate|Applying|transfer" "$WORK/requestor.log" 2>/dev/null | tail -8
if [ "$ok" = 1 ]; then echo "== SIM OTA PASSED: requestor pulled the image and requested apply =="; exit 0
else echo "== SIM OTA FAILED: no ApplyUpdateRequest observed (see logs in provider/requestor above) =="; exit 1; fi
