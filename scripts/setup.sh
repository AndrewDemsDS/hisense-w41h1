#!/usr/bin/env bash
# setup.sh -- prepare the build tree from fresh SDK checkouts: apply our patches + Matter-overlay
# edits, and copy our source into the room_air_conditioner example. Run once after cloning + getting
# the SDKs (see NOTICE.md for the pinned commits). Idempotent-ish; re-running re-copies our files.
#
#   export AMEBA_SDK=/path/to/ameba-rtos-z2
#   export CHIP_SDK=/path/to/connectedhomeip
#   scripts/setup.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"          # repo root
: "${AMEBA_SDK:?set AMEBA_SDK=/path/to/ameba-rtos-z2}"
: "${CHIP_SDK:?set CHIP_SDK=/path/to/connectedhomeip}"
[ -d "$AMEBA_SDK" ] && [ -d "$CHIP_SDK" ] || { echo "ERROR: AMEBA_SDK / CHIP_SDK must be existing dirs"; exit 1; }

# Pinned base commits the patches were generated against (NOTICE.md).
AMEBA_BASE=0ee0460 ; CHIP_BASE=cc74311
chk(){ local got; got=$(git -C "$1" rev-parse --short=7 HEAD 2>/dev/null || echo none)
       [ "$got" = "$2" ] || echo "  [warn] $(basename "$1") at $got; patches made against $2 -- '--3way' will try to merge, may fuzz/conflict"; }
chk "$AMEBA_SDK" "$AMEBA_BASE"; chk "$CHIP_SDK" "$CHIP_BASE"

# The patches are `git -C $SDK diff` against the pinned base, so they already contain what earlier
# steps did to those files (matter_setup.sh's ENABLE_MATTER flip and ctype.h removal) and values that
# `ota-release.sh build` re-derives on every build (the version header, the FWHS serial). Reset exactly
# the paths a patch touches to the pinned commit, then apply it: deterministic, and safe to re-run.
# This used to be `git apply --3way ... && echo ok`. On a tree matter_setup.sh or a build had touched,
# the apply failed ("does not match index"), and set -e does not stop on a failure inside an && list,
# so setup reported success on an unpatched SDK and codegen later died with
# "Unhandled server cluster: HISENSE_AIRCON_CLUSTER".
apply_sdk_patch() {  # $1=SDK checkout  $2=patch file
  local sdk="$1" patch="$2" p paths=()
  while IFS= read -r p; do
    git -C "$sdk" cat-file -e "HEAD:$p" 2>/dev/null && paths+=("$p")
  done < <(git -C "$sdk" apply --numstat "$patch" | cut -f3)
  [ "${#paths[@]}" -eq 0 ] || git -C "$sdk" checkout -q HEAD -- "${paths[@]}"
  if git -C "$sdk" apply "$patch"; then
    echo "  [ok] $(basename "$patch")"
  else
    echo "  [!!] $(basename "$patch") does not apply to $sdk; is it at the commit pinned in versions.env?"
    exit 1
  fi
}

echo "== 1/4  apply SDK patches (base-tracked files) =="
apply_sdk_patch "$AMEBA_SDK" "$HERE/patches/ameba-rtos-z2.patch"
# ameba-rtos-z2.patch deletes this SDK ctype.h (it shadows <cctype> -> 'ispunct not declared').
rm -f "$AMEBA_SDK/component/soc/realtek/8710c/misc/utilities/include/ctype.h"
apply_sdk_patch "$CHIP_SDK" "$HERE/patches/connectedhomeip.patch"

echo "== 2/4  Matter-overlay in-place edits (untracked layer, can't be a git patch) =="
AMEBA_SDK="$AMEBA_SDK" bash "$HERE/scripts/apply-matter-edits.sh"

echo "== 3/4  copy our source into the example + custom cluster into CHIP =="
EX="$AMEBA_SDK/component/common/application/matter/examples/room_air_conditioner"
mkdir -p "$EX"
# our driver + glue + config (MIT). Copy whatever exists (delegate is optional/inlined).
# File set is defined ONCE in firmware/scripts/sync-files.sh (shared with ota-release.sh's
# sync_mirror) so the two lists can never drift out of lockstep.
# shellcheck source=firmware/scripts/sync-files.sh
. "$HERE/firmware/scripts/sync-files.sh"
for f in "${SYNC_FILES[@]}"; do
  [ -f "$HERE/$f" ] && cp "$HERE/$f" "$EX/" && echo "  [ok] $(basename "$f")"
done
# custom 0xFFF1FC00 manufacturer cluster into connectedhomeip
cp "$HERE/firmware/src/sdk-edits/hisense-aircon-cluster.xml" \
   "$CHIP_SDK/src/app/zap-templates/zcl/data-model/chip/" && echo "  [ok] hisense-aircon-cluster.xml"
CID="$CHIP_SDK/zzz_generated/app-common/clusters/HisenseAircon"
mkdir -p "$CID"
cp "$HERE/firmware/src/sdk-edits/HisenseAircon-ClusterId.h" "$CID/ClusterId.h" && echo "  [ok] HisenseAircon/ClusterId.h"

echo "== 4/4  done =="
cat <<EOF

Next:
  1. (optional) firmware/scripts/gen-creds.sh   # your own commissioning code
  2. cp firmware/scripts/ota-release.env.example firmware/scripts/ota-release.env  # edit paths
  3. python3 firmware/scripts/dev.py build amebaz2   # -> firmware_is.bin + clip image + .ota
  4. python3 firmware/scripts/dev.py flash amebaz2   # prints the clip and OTA paths; read them first

If any [!!]/[MANUAL] lines appeared above, resolve them (paths/flag names differ across SDK versions).
The .zap cluster must be GUI-enabled on ep1 if codegen doesn't count the hand-added block -- see
firmware/src/sdk-edits/README.md.
EOF
