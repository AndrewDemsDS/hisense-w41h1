#!/usr/bin/env bash
#
# ota-release.sh -- canonical build + Matter-OTA procedure for the room_air_conditioner
# firmware. Encodes the rules + traps documented in firmware/docs/10-firmware-ota-procedure.md
# (versioning, don't-hand-edit-outputs, CONTIGUOUS endpoints, dep-tracking touches, provider
# retry, A/B rollback detection). Run this instead of doing the steps by hand.
#
# Usage:
#   ota-release.sh lint                     # fast offline checks (host tests + .zap lint). git hook uses this.
#   ota-release.sh build   [--bump]         # sync mirror->SDK, build, verify (optionally bump version first)
#   ota-release.sh package                  # pad clip image + create .ota + manifest
#   ota-release.sh stage                    # scp .ota+manifest to the Pi, restart matter-server
#   ota-release.sh flash                    # update_node with retries, verify the reported version changed
#   ota-release.sh release [--bump] [--flash]   # build + package + stage (+ flash)
#   ota-release.sh revert --backup <unit-ip> [out.bin] # fetch+validate the inactive-slot stock image (#19)
#   ota-release.sh revert --flip <unit-ip> [--force]   # break-glass slot-flip back to stock (#19)
#   ota-release.sh revert --repackage <stock-dump.bin> # carve+resign stock app as a Matter .ota (#19)
#   ota-release.sh revert --slots <unit-ip>            # read-only: print both slots' FWHS serials
#   ota-release.sh revert --apply [--ip <ip>] [--yes]  # stage the stock .ota + push it via update_node
#
# Environment-specific paths/hosts come from ota-release.env (gitignored; copy .env.example).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
ENVF="${ENVF:-$HERE/ota-release.env}"
# shellcheck source=firmware/scripts/sync-files.sh
. "$HERE/sync-files.sh"   # SYNC_FILES{,_REQUIRED,_OPTIONAL}: the set copied into the SDK example
say() { printf '\033[1;36m[ota-release]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[ota-release] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# Source the CHIP/pigweed env. Isolated here because activate.sh references unbound vars, which
# under `set -u` is a FATAL error that kills the build subshell before it starts -> the parent's
# `set -e` then aborts silently right after "FULL CLEAN". Wrap the source in set +u/-u. (2026-07-08)
activate_chip_env() {
  # shellcheck disable=SC1091
  set +u; source "$SDK_ROOT/connectedhomeip/scripts/activate.sh" >/dev/null 2>&1 || die "activate.sh failed"; set -u
}

load_env() {
  [ -f "$ENVF" ] || die "missing $ENVF -- copy ota-release.env.example and fill it in"
  # shellcheck disable=SC1090
  . "$ENVF"
  : "${SDK_ROOT:?}" "${GCC_RELEASE:?}" "${CHIP_CONFIG_H:?}" "${EXAMPLE_DIR:?}" "${OTA_TOOL:?}"
  : "${VID:?}" "${PID:?}"
}

# ---- version helpers (unified semver, issue #77) ---------------------------
# Version source of truth is GIT-TRACKED firmware/src/version.txt, now holding a SEMVER
# (MAJOR.MINOR.PATCH, e.g. "1.2.0") so CI can gate it without the ~15GB SDK. The Matter
# softwareVersion INT is DERIVED from it: MAJOR*10000 + MINOR*100 + PATCH -> a readable,
# strictly-monotonic uint32. This keeps the human semver in the string + git tags while the
# int keeps climbing (the OTA provider only serves a strictly-greater int; the fleet is already
# at Ameba sw34, and 1.0.0 -> 10000 > 34 clears it). The SDK header (CHIPDeviceConfig.h,
# gitignored) is DERIVED at build time (set_header_version). Never hand-edit the header or the
# int -- edit the semver in version.txt and COMMIT it. Tag convention: amebaz2-vMAJOR.MINOR.PATCH.
VERSION_FILE="$REPO/firmware/src/version.txt"
semver_to_int() {  # "MAJOR.MINOR.PATCH" -> MAJOR*10000+MINOR*100+PATCH (the Matter softwareVersion int)
  local s="${1//[[:space:]]/}"
  [[ "$s" =~ ^[0-9]+$ ]] && { echo "$s"; return; }   # legacy raw-int version.txt (pre-#77 branches / CI base)
  [[ "$s" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || die "version '$s' is not semver MAJOR.MINOR.PATCH (issue #77)"
  local M="${BASH_REMATCH[1]}" m="${BASH_REMATCH[2]}" p="${BASH_REMATCH[3]}"
  (( m < 100 && p < 100 )) || die "minor/patch must be < 100 for the *10000+*100 int mapping: '$s'"
  echo $(( M*10000 + m*100 + p ))
}
cur_semver() {  # the human semver string (version.txt is authoritative + always git-tracked)
  [ -f "$VERSION_FILE" ] || die "missing $VERSION_FILE"
  tr -d '[:space:]' < "$VERSION_FILE"
}
cur_version() { semver_to_int "$(cur_semver)"; }   # the derived Matter softwareVersion int
set_header_version() {  # force the (gitignored) SDK header to match version.txt -- idempotent
  local vint semver; vint="$(cur_version)"; semver="$(cur_semver)"
  sed -i -E "s/(#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION )[0-9]+/\1$vint/" "$CHIP_CONFIG_H"
  sed -i -E "s/(DEVICE_SOFTWARE_VERSION_STRING \")[^\"]*(\")/\1$semver\2/" "$CHIP_CONFIG_H"
}
RELEASED_MARK="$REPO/firmware/built-images/.released-version"   # softwareVersion INT last CONFIRMED booted on-device
released_version() { [ -f "$RELEASED_MARK" ] && cat "$RELEASED_MARK" || echo 0; }
bump_version() {  # bump the git-tracked semver (default patch) + sync the SDK header (COMMIT version.txt!)
  local level="${1:-patch}" s M m p
  s="$(cur_semver)"
  [[ "$s" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || die "cannot bump non-semver version.txt '$s' (issue #77)"
  M="${BASH_REMATCH[1]}"; m="${BASH_REMATCH[2]}"; p="${BASH_REMATCH[3]}"
  case "$level" in
    major) M=$(( M+1 )); m=0; p=0 ;;
    minor) m=$(( m+1 )); p=0 ;;
    patch|*) p=$(( p+1 )) ;;
  esac
  local next="$M.$m.$p"
  echo "$next" > "$VERSION_FILE"
  set_header_version
  say "version bumped $s -> $next (softwareVersion int $(semver_to_int "$next")); firmware/src/version.txt + CHIPDeviceConfig.h -- commit version.txt"
}

# ---- lint (offline, safe for the git hook) ---------------------------------
lint_zap() {  # CONTIGUOUS endpoints (a gap boot-crashes the device) -- checks the committed mirror .zap
  local zap="$REPO/firmware/src/sdk-edits/room-air-conditioner-app.zap"
  [ -f "$zap" ] || die "mirror .zap not found: $zap"
  python3 - "$zap" <<'PY'
import json,sys
ids=sorted(e['endpointId'] for e in json.load(open(sys.argv[1]))['endpoints'])
if ids != list(range(len(ids))):
    print(f"  .zap endpoints NOT contiguous: {ids} -- an endpoint gap boot-crashes AmebaZ2 (docs/10 §3)")
    sys.exit(1)
print(f"  .zap endpoints contiguous: {ids}")
PY
}
lint_version() {  # must exceed the version last CONFIRMED booted on the device
  local cur rel; cur="$(cur_version)"; rel="$(released_version)"
  if [ "$cur" -le "$rel" ]; then
    die "CHIPDeviceConfig version ($cur) <= last on-device version ($rel) -- bump it (docs/10 §1). 'ota-release.sh build --bump'"
  fi
  say "version OK: config=$cur > on-device=$rel"
}
lint() {
  say "lint: host codec/map tests"
  ( cd "$REPO/firmware/test" && bash run_tests.sh >/tmp/ota-lint-tests.log 2>&1 ) \
    || { tail -20 /tmp/ota-lint-tests.log; die "host tests FAILED"; }
  say "  host tests passed"
  say "lint: .zap endpoint contiguity"; lint_zap
  say "lint: softwareVersion"; lint_version   # version.txt is git-tracked -> runs in CI, no SDK needed
  say "lint OK"
}

# ---- build -----------------------------------------------------------------
sync_mirror() {
  # File set is defined ONCE in sync-files.sh (sourced above) and shared with scripts/setup.sh,
  # so the two can never drift out of lockstep. matter_aircon_map.h was previously missing here,
  # so edits to the Matter<->Hisense mapping never reached a rebuild.
  # REQUIRED files copy unconditionally (a missing one hard-fails the release build); OPTIONAL
  # files (inlined/absent delegate + power estimate header) copy only if present.
  say "sync mirror -> SDK example dir"
  local f
  for f in "${SYNC_FILES_REQUIRED[@]}"; do
    cp "$REPO/$f" "$EXAMPLE_DIR/"
  done
  # #22/#23 build flavour. Release is the DEFAULT: the debug header is generated only for
  # `build --debug`, and removed otherwise, so the unauthenticated :2323 console cannot ship by
  # forgetting a flag. Only logging/console/diagnostics may differ between flavours.
  if [ "${HISENSE_FLAVOUR:-release}" = "debug" ]; then
    { echo "// GENERATED by ota-release.sh --debug -- do not commit."
      echo "#define HISENSE_DEBUG_BUILD 1"
    } > "$EXAMPLE_DIR/hisense_flavour.h"
    say "  flavour: DEBUG (:2323 console compiled in -- bench only, do not deploy)"
  else
    rm -f "$EXAMPLE_DIR/hisense_flavour.h"
    say "  flavour: release (no diagnostic console)"
  fi
  # #78 break-glass OTA target. Generated (never committed): the repo is public, so the real
  # server address lives in ota-release.env and is injected here. Without OTA_HTTP_HOST the
  # generated header is omitted and matter_drivers.cpp keeps its inert placeholder, which is why
  # the shipped 1.2.9 pointed at 192.168.1.50 and the escape hatch could not have worked.
  if [ -n "${OTA_HTTP_HOST:-}" ]; then
    { echo "// GENERATED by ota-release.sh from ota-release.env -- do not commit."
      echo "#define HISENSE_OTA_HOST     \"${OTA_HTTP_HOST}\""
      echo "#define HISENSE_OTA_PORT     ${OTA_HTTP_PORT:-8070}"
      echo "#define HISENSE_OTA_RESOURCE \"${OTA_HTTP_RESOURCE:-/rac-ota.bin}\""
      # #61: break-glass TRIGGER token. Ships in both flavours, so it is authenticated and
      # fails closed -- no token here means the listener is never opened. There is deliberately
      # no default: a default in a public repo is the same as no authentication.
      if [ -n "${BREAKGLASS_TOKEN:-}" ]; then
        echo "#define HISENSE_BREAKGLASS_TOKEN \"${BREAKGLASS_TOKEN}\""
        echo "#define HISENSE_BREAKGLASS_PORT  ${BREAKGLASS_PORT:-2324}"
      fi
    } > "$EXAMPLE_DIR/hisense_ota_config.h"
    say "  break-glass OTA target: ${OTA_HTTP_HOST}:${OTA_HTTP_PORT:-8070}${OTA_HTTP_RESOURCE:-/rac-ota.bin}"
    if [ -n "${BREAKGLASS_TOKEN:-}" ]; then
      say "  break-glass trigger: listening on :${BREAKGLASS_PORT:-2324} (token set, both flavours)"
    else
      say "  break-glass trigger: DISABLED (BREAKGLASS_TOKEN unset -- recovery needs a healthy Matter layer)"
    fi
  else
    rm -f "$EXAMPLE_DIR/hisense_ota_config.h"
    say "  OTA_HTTP_HOST unset -- break-glass OTA keeps the inert placeholder host"
  fi
  for f in "${SYNC_FILES_OPTIONAL[@]}"; do
    cp "$REPO/$f" "$EXAMPLE_DIR/" 2>/dev/null || true
  done
  # The custom mfg-cluster id header lives in connectedhomeip's zzz_generated tree (not the
  # example dir), so scripts/setup.sh places it there once -- but a plain rebuild must re-sync
  # it too, or a header change (e.g. new Attributes ids) never reaches the driver at build time.
  local CID="$SDK_ROOT/connectedhomeip/zzz_generated/app-common/clusters/HisenseAircon"
  mkdir -p "$CID"
  cp "$REPO/firmware/src/sdk-edits/HisenseAircon-ClusterId.h" "$CID/ClusterId.h"
  # The mfg-cluster ZCL definition (its <attribute> list) lives in the zcl data-model tree and is
  # what the ZAP GUI reads for the cluster's available attributes. Sync it too, or a newly-declared
  # attribute (e.g. Features1/Faults1, docs/14) never shows up to be enabled in the GUI, and a fresh
  # SDK loses the definition entirely.
  local ZCLDIR="$SDK_ROOT/connectedhomeip/src/app/zap-templates/zcl/data-model/chip"
  [ -d "$ZCLDIR" ] && cp "$REPO/firmware/src/sdk-edits/hisense-aircon-cluster.xml" "$ZCLDIR/hisense-aircon-cluster.xml"
}
apply_ota_hardening() {
  # #76: port the ESP32 OTA MRP tuning to AmebaZ2. The chip core+main already build -Os here, so
  # only MRP needs porting. AmebaZ2 has no Kconfig, so CHIP uses the weak upstream MRP defaults
  # (RETRANS=4/active 300/idle 500) that drop the long BDX OTA transfer on marginal Wi-Fi. Append
  # our overrides (canonical: firmware/src/sdk-edits/chip-ameba-ota-hardening.h) to the Ameba CHIP
  # platform config, which CHIPConfig.h includes before ReliableMessageProtocolConfig.h applies its
  # #ifndef defaults. Idempotent (grep-guarded) + self-healing across a full clean / SDK reinstall.
  local hdr="$SDK_ROOT/connectedhomeip/src/platform/Ameba/CHIPPlatformConfig.h"
  local block="$REPO/firmware/src/sdk-edits/chip-ameba-ota-hardening.h"
  [ -f "$hdr" ] || die "Ameba CHIPPlatformConfig.h not found: $hdr"
  [ -f "$block" ] || die "OTA-hardening block not found: $block"
  if grep -q 'HISENSE_OTA_HARDENING' "$hdr"; then
    say "  MRP OTA-hardening already present in Ameba CHIPPlatformConfig.h (#76)"
  else
    cat "$block" >> "$hdr"
    say "  injected MRP OTA-hardening into Ameba CHIPPlatformConfig.h (#76): RETRANS 4->8, active 300->500, idle 500->800ms"
  fi
}

apply_example_task_stack() {
  # The example init task is created with 2048 WORDS (8 KB), copied from the stock light example
  # whose init sets a couple of attributes. Ours does far more on that stack: nine UserLabel
  # writes, an ember write, the ElectricalPowerMeasurement delegate + Instance init, and the
  # ModeSelect manager. Overflowing it kills the task SILENTLY, part-way through, which is why
  # the labels appear (they run early) while ElectricalPowerMeasurement reads return
  # InteractionModelError Failure(0x1) and, fatally, the NEXT statement after the init call --
  # matter_interaction_start_downlink() -- never runs. With no downlink queue, every
  # PostDownlinkEvent from the bus task hits the `if (queue != NULL)` guard and is dropped, so
  # every status-derived Matter attribute stays frozen at its .zap default forever while the A/C
  # bus and the diag console look perfectly healthy. Measured on-device: queue NULL, task NULL,
  # init_flag 0, 50 posts, 0 handler runs. (docs/10 §17)
  # Idempotent + self-healing across a full clean / SDK reinstall.
  local f="$EXAMPLE_DIR/example_matter_room_air_conditioner.cpp"
  [ -f "$f" ] || die "example entry point not found: $f"
  if grep -q 'example_matter_room_air_conditioner_task"), 8192' "$f"; then
    say "  example init task stack already raised to 8192 words"
  else
    sed -i 's/example_matter_room_air_conditioner_task"), 2048/example_matter_room_air_conditioner_task"), 8192/' "$f"
    grep -q 'example_matter_room_air_conditioner_task"), 8192' "$f"       || die "failed to raise example init task stack in $f"
    say "  raised example init task stack 2048 -> 8192 words (our init is far heavier than the stock example's)"
  fi
}

apply_downlink_stack() {
  # The SDK creates DownlinkTask with a 1024-WORD (4 KB) stack, sized for the stock examples whose
  # downlink handlers set one or two attributes. Ours writes dozens of ember attributes, drives the
  # EPM delegate and logs, all on that stack. A stack overflow there kills the task silently:
  # PostDownlinkEvent keeps "succeeding" until the 10-slot queue fills, then drops every event with
  # no consumer left, so the A/C bus and the diag console stay perfectly healthy while every
  # status-derived Matter attribute sits frozen at its .zap default. That is exactly the observed
  # signature on node 14 (see docs/10 §17).
  # Idempotent + self-healing across a full clean / SDK reinstall, same as apply_ota_hardening.
  local f="$SDK_ROOT/ameba-rtos-z2/component/common/application/matter/core/matter_interaction.cpp"
  [ -f "$f" ] || die "matter_interaction.cpp not found: $f"
  if grep -q 'xTaskCreate(DownlinkTask, "Downlink", 4096' "$f"; then
    say "  DownlinkTask stack already raised to 4096 words"
  else
    sed -i 's/xTaskCreate(DownlinkTask, "Downlink", 1024/xTaskCreate(DownlinkTask, "Downlink", 4096/' "$f"
    grep -q 'xTaskCreate(DownlinkTask, "Downlink", 4096' "$f" \
      || die "failed to raise DownlinkTask stack in $f"
    say "  raised DownlinkTask stack 1024 -> 4096 words (our downlink handler is far heavier than the stock examples')"
  fi
}
apply_mode_select_span_guard() {
  # ZAP sizes supportedOptionsByEndpoints[] by MATTER_DM_MODE_SELECT_CLUSTER_SERVER_ENDPOINT_COUNT,
  # which counts endpoint TYPES, not endpoints. Our .zap carries an ORPHANED endpoint type with
  # ModeSelect server enabled and used by zero endpoints, so the macro is 2 while the SDK's .cpp
  # initialises exactly one entry (ep6). Element [1] is therefore zero-filled: mEndpointId 0 and an
  # empty Span whose data()/end() are both nullptr, and getModeOptionsProvider iterates the whole
  # array. getModeOptionByMode does guard begin()==nullptr, so this is defence in depth rather than
  # a proven crash -- skip the padding explicitly instead of relying on every caller to guard.
  # Idempotent + self-healing across a full clean / SDK reinstall, same as apply_ota_hardening.
  local f="$SDK_ROOT/ameba-rtos-z2/component/common/application/matter/drivers/matter_drivers/mode_select/ameba_mode_select_manager.cpp"
  [ -f "$f" ] || die "ameba_mode_select_manager.cpp not found: $f"
  if grep -q 'mSpan.data() == nullptr' "$f"; then
    say "  ModeSelect span guard already applied"
  else
    sed -i 's|        if (endpointSpanPair.mEndpointId == endpointId)|        if (endpointSpanPair.mSpan.data() == nullptr) { continue; }  // orphaned endpoint type pads this array\n        if (endpointSpanPair.mEndpointId == endpointId)|' "$f"
    grep -q 'mSpan.data() == nullptr' "$f" \
      || die "failed to apply ModeSelect span guard in $f"
    say "  applied ModeSelect null-span guard (orphaned endpoint type inflates the generated count)"
  fi
}
det_time_shim() {
  # Realtek's elf2bin.linux seeds `srand(time(NULL))` and derives part of the image header from it,
  # so packaging the SAME .axf twice yields two different images (~530 bytes of header churn --
  # proven by running `make is_matter` twice back to back). No JSON knob controls it: cipherkey,
  # cipheriv and privkey_hash were all tried and none of them is the source. The tool is a closed
  # prebuilt binary, so the only lever is the clock it seeds from -- pin time() with an LD_PRELOAD
  # shim reading SOURCE_DATE_EPOCH. Verified: with the shim, two runs over one .axf are identical.
  # Scoped to the packaging make only, and it overrides time() alone (date(1) uses clock_gettime,
  # so the build_info pin above is independent of this).
  local so dir
  # Build FRESH into a private dir every run: it is a ~100 ms gcc call, and reusing the
  # predictable /tmp/ota-det-time-<uid>.so would LD_PRELOAD whatever happens to sit at
  # that known path into the packaging make (and the -nt check could keep a stale shim).
  dir="$(mktemp -d "${TMPDIR:-/tmp}/ota-det-time.XXXXXX")" || die "mktemp failed"
  so="$dir/shim.so"
  command -v gcc >/dev/null || die "host gcc needed to build the deterministic-clock shim"
  gcc -shared -fPIC -O2 -x c -o "$so" - <<'CSHIM' || die "failed to build the deterministic-clock shim"
#include <time.h>
#include <stdlib.h>
time_t time(time_t *t) {
    const char *e = getenv("SOURCE_DATE_EPOCH");
    time_t v = e ? (time_t) strtoll(e, 0, 10) : 0;
    if (t) *t = v;
    return v;
}
CSHIM
  printf '%s' "$so"
}
apply_build_info_determinism() {
  # The SDK's `build_info` make target regenerates .ver on EVERY build by shelling out to `date`,
  # so RTL8710CFW_COMPILE_TIME / UTS_VERSION carry the wall-clock second the build started. That
  # single string is the whole reason two builds of one commit differ: it lands near 0x106900 and
  # the image header hashes cover it, so a 5-byte stamp becomes a ~574-byte diff. `id -u -n` is
  # baked in the same way, which additionally leaks the builder's username into a public image.
  # Pin both: the clock comes from SOURCE_DATE_EPOCH (set in build()), the identity is a constant.
  # Idempotent + self-healing across a full clean / SDK reinstall, same as the guards above.
  local m hit=0
  for m in "$SDK_ROOT/ameba-rtos-z2/project/realtek_amebaz2_v0_example/GCC-RELEASE/application.is.matter.mk" \
           "$SDK_ROOT/ameba-rtos-z2/project/realtek_amebaz2_v0_example/GCC-RELEASE/application.is.mk"; do
    [ -f "$m" ] || continue
    grep -q 'SOURCE_DATE_EPOCH' "$m" && { hit=1; continue; }
    perl -0777 -pi -e '
      s/`date \+/`date -u -d \@\$\${SOURCE_DATE_EPOCH:-0} +/g;
      s/`id -u -n`/builder/g;
      s/`\$\(HOSTNAME_APP\)`//g;
    ' "$m"
    grep -q 'SOURCE_DATE_EPOCH' "$m" || die "failed to pin build_info timestamps in $m"
    say "  pinned build_info clock+identity in $(basename "$m")"
  done
  [ "$hit" = 1 ] && say "  build_info determinism already applied"
  return 0
}
build() {
  load_env
  # --debug selects the bench flavour (#22): adds the :2323 console. Release is the default, so
  # omitting it can never accidentally ship the console.
  local a
  for a in "$@"; do
    case "$a" in
      --bump|--bump-patch) bump_version patch ;;
      --bump-minor)        bump_version minor ;;
      --bump-major)        bump_version major ;;
      --debug)             HISENSE_FLAVOUR=debug ;;
    esac
  done
  # Reproducible-build clock scrub. The Realtek SDK bakes __DATE__/__TIME__ into the image (the
  # "Build @ %s, %s" banner and the AT `COMPILE TIME` / `SW VERSION` strings around 0x106900).
  # Two builds of the SAME commit at different wall-clock times therefore differ -- and because
  # the image header carries hashes over that content, a 5-byte timestamp diff smears into ~574
  # differing bytes, which is what made CI output look like a whole different build. GCC honours
  # SOURCE_DATE_EPOCH for both macros (verified on this ASDK-10.3.1 toolchain), so pin it to the
  # HEAD commit's own timestamp: deterministic per source revision, and identical on any machine
  # that builds that revision. Override with SOURCE_DATE_EPOCH=... only to reproduce an old image.
  export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$REPO" log -1 --format=%ct)}"
  say "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH ($(date -u -d "@$SOURCE_DATE_EPOCH" '+%Y-%m-%d %H:%M:%S UTC')) -- __DATE__/__TIME__ pinned to HEAD"
  set_header_version   # SDK header (int + string) derives from the git-tracked semver in version.txt
  lint_zap
  sync_mirror
  apply_ota_hardening   # #76: MRP tuning into the Ameba CHIP platform config (idempotent)
  apply_downlink_stack  # DownlinkTask stack: 4KB overflows our heavy handler (idempotent)
  apply_example_task_stack  # init task stack: 8KB dies mid-init -> no downlink queue (idempotent)
  apply_mode_select_span_guard  # orphaned endpoint type pads the ModeSelect table (idempotent)
  apply_build_info_determinism  # .ver clock/username -> SOURCE_DATE_EPOCH + constant (idempotent)
  # Reproducible-build path scrub (public release): rewrite the absolute build path baked into
  # __FILE__ / debug info so the compiled image carries generic /build/... paths instead of the
  # developer's $HOME (e.g. .../connectedhomeip/src/app/server/Server.cpp leaked the full path).
  # -ffile-prefix-map=$(HOME)=/build maps $HOME's leading prefix (covers __FILE__ AND debug info;
  # -fmacro-prefix-map would cover only __FILE__). Applied to BOTH compile paths:
  #   - GN CHIP core (the connectedhomeip .cpp files, the actual leak source): appended to
  #     CHIP_CFLAGS/CHIP_CXXFLAGS, which the args.gn generator turns into target_cflags_c/cc ->
  #     gn cflags_c/cflags_cc (build/config/compiler/BUILD.gn). Injected idempotently into the SDK
  #     so a fresh checkout is covered (like the ccache CRMK patch below).
  #   - Ameba make app/main-lib: carried on CC/CXX (see CCMK).
  # $(HOME) is expanded by make at build time -> the developer's username never lands in this
  # tracked script (only the literal string "$(HOME)" is stored here).
  local MPROJ="$SDK_ROOT/ameba-rtos-z2/component/common/application/matter/project"
  local CCS
  for CCS in "$MPROJ/amebaz2/make/chip_core_sources.mk" "$MPROJ/amebaz2plus/make/chip_core_sources.mk"; do
    [ -f "$CCS" ] || continue
    grep -q 'ffile-prefix-map' "$CCS" 2>/dev/null \
      || sed -i 's#^CHIP_CXXFLAGS += \$(INCLUDES)#&\nCHIP_CFLAGS += -ffile-prefix-map=$(HOME)=/build\nCHIP_CXXFLAGS += -ffile-prefix-map=$(HOME)=/build#' "$CCS"
  done
  # ccache (OFFICIAL way, docs/10 §10): the GN core honours pw_command_launcher="ccache"
  # (pigweed generate_toolchain -> GN command_launcher), injected into args.gn via
  # chip_core_rules.mk; the make main-lib/app use a CC=ccache prefix. This makes the mandatory
  # full-clean rebuild fast on repeat (unchanged core = cache hits). NOT the masquerade hack.
  # PMAP: the -ffile-prefix-map flag, carried on the make CC/CXX so app/main-lib TUs get their
  # $HOME scrubbed from __FILE__ regardless of whether ccache is present. $(CROSS_COMPILE) and
  # $(HOME) stay literal here for make to expand (single quotes) -> no username in this script.
  local PMAP='-ffile-prefix-map=$(HOME)=/build'
  local CCMK=(CC="\$(CROSS_COMPILE)gcc $PMAP" CXX="\$(CROSS_COMPILE)g++ $PMAP")
  if command -v ccache >/dev/null; then
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.ccache}"; ccache -M "${CCACHE_MAXSIZE:-25G}" >/dev/null 2>&1 || true
    # ccache tuning (2026-07-14): base_dir rewrites absolute build paths to relative so the hash
    # is stable across clean rebuilds (was a big miss source); compiler_check=content survives
    # toolchain mtime/atime noise. Scoped via env -- no change to the user's global ccache.conf.
    # `time_macros` is deliberately NOT in the sloppiness list: it tells ccache to ignore
    # __DATE__/__TIME__ when hashing, so a TU compiled under an earlier SOURCE_DATE_EPOCH gets
    # replayed with its STALE baked-in timestamp. That is what made two builds of one commit
    # sometimes match and sometimes not (a cache hit preserved the old stamp, a miss stamped
    # fresh) -- nondeterminism disguised as reproducibility. Without it ccache simply declines to
    # cache the two or three SDK TUs that use the macros; everything else still caches.
    export CCACHE_BASEDIR="$HOME"
    export CCACHE_COMPILERCHECK=content
    export CCACHE_SLOPPINESS="include_file_mtime,include_file_ctime,pch_defines,locale,system_headers"
    ccache -z >/dev/null 2>&1 || true    # zero stats -> the post-build summary is per-build
    # Inject pw_command_launcher="ccache" into the GN args.gn generation. FIX (2026-07-14):
    # the build target `realtek_amebaz2_v0_example` reads the `amebaz2` (no "plus") variant of
    # chip_core_rules.mk; patching only `amebaz2plus` left the ninja CHIP core UNwrapped by
    # ccache (0 cacheable). Inject into BOTH variants defensively (SDK ships duplicates).
    local CRMK
    for CRMK in "$MPROJ/amebaz2/make/chip_core_rules.mk" "$MPROJ/amebaz2plus/make/chip_core_rules.mk"; do
      [ -f "$CRMK" ] || continue
      grep -q pw_command_launcher "$CRMK" 2>/dev/null \
        || sed -i 's#\(echo ameba_cpu = \\"ameba\\" >> \$(OUTPUT_DIR)/args.gn && \\\)#\1\n\techo pw_command_launcher = \\"ccache\\" >> $(OUTPUT_DIR)/args.gn \&\& \\#' "$CRMK"
    done
    CCMK=(CC="ccache \$(CROSS_COMPILE)gcc $PMAP" CXX="ccache \$(CROSS_COMPILE)g++ $PMAP")
    say "ccache ON (dir=$CCACHE_DIR, base_dir=$HOME, check=content): GN core via pw_command_launcher, make via CC prefix"
  else
    say "ccache not installed (sudo pacman -S ccache) -- building without it (path-scrub still on via CC)"
  fi
  local BSP="$SDK_ROOT/ameba-rtos-z2/component/soc/realtek/8710c/misc/bsp/lib/common/GCC"
  # MANDATORY full clean before EVERY build. The SDK's build cache otherwise reuses a
  # stale core (libCHIP.a) + main lib -> a "~77-second" fake build that ships an
  # INCONSISTENT image (rolled back on-device 3x, 2026-07-08). clean_matter_libs +
  # clean_matter clean the objects, but clean_matter_libs LEAVES the *copied* bsp libs
  # and the gn out dir -- so remove those too, or the cache wins. (docs/10 §4)
  say "FULL CLEAN (mandatory -- defeats the stale-core cache)"
  ( cd "$GCC_RELEASE"
    activate_chip_env
    make clean_matter_libs 2>&1 | tail -1
    make clean_matter 2>&1 | tail -1
    rm -f "$BSP/libCHIP.a" "$BSP/lib_main.a"
    rm -rf "$EXAMPLE_DIR/build/chip"
  )
  local JOBS="${BUILD_JOBS:-$(nproc)}"
  local DET_SHIM; DET_SHIM="$(det_time_shim)"; export DET_SHIM
  say "BUILD (genuine recompile; ameba make -j$JOBS -> ~110s; verify by ninja [N/353] + fresh libCHIP.a, NOT wall-clock)"
  ( cd "$GCC_RELEASE"
    activate_chip_env
    make room_air_conditioner_port -j"$JOBS" "${CCMK[@]}"
    # CRITICAL (root-caused 2026-07-08): AmebaZ2's bootloader boots the flash slot with the
    # HIGHER FWHS OTA serial -- the Matter softwareVersion is IRRELEVANT to it. If the serial
    # doesn't increase, the OTA transfers + applies but the bootloader keeps the OLD slot
    # (looks exactly like a "rollback"). Set serial = SERIAL_BASE + softwareVersion so it
    # always increases with the version. This wasted a whole debugging session. (docs/10 §11)
    python3 - amebaz2_firmware_is.json "$(( ${SERIAL_BASE:-1100} + $(cur_version) ))" <<'PYS'
import json,sys
f,s=sys.argv[1],int(sys.argv[2]); d=json.load(open(f)); d['FWHS']['header']['serial']=s
json.dump(d,open(f,'w'),indent=2); print(f"OTA FWHS serial set -> {s}")
PYS
    # LD_PRELOAD pins the clock elf2bin seeds its RNG from (see det_time_shim).
    LD_PRELOAD="$DET_SHIM" make is_matter -j"$JOBS" "${CCMK[@]}" 2>&1 | tee /tmp/ota-ismatter.log | tail -2
  )
  rm -rf "$(dirname "$DET_SHIM")"   # the shim's private mktemp dir (no-op if already gone)
  # ccache effectiveness for THIS build (helps tell a cold build from a warm one).
  if command -v ccache >/dev/null; then
    say "ccache: $(ccache -s 2>/dev/null | awk -F'[()]' '/Hits:/{h=$2} /Misses:/{m=$2} END{printf "%s hits / %s misses", h, m}')"
  fi
  # verify the built image actually carries the bumped serial (guard against a silent miss)
  local wantser=$(( ${SERIAL_BASE:-1100} + $(cur_version) ))
  grep -q "header-serial $wantser" /tmp/ota-ismatter.log \
    || die "built image serial != $wantser -- bootloader would roll back the OTA (docs/10 §11)"
  say "OTA image serial verified: $wantser (> on-device -> bootloader will keep the new slot)"
  local ec="$EXAMPLE_DIR/build/chip/codegen/zap-generated/endpoint_config.h"
  local arr; arr="$(grep FIXED_ENDPOINT_ARRAY "$ec" | grep -oP '\{[^}]*\}')"
  say "built: FIXED_ENDPOINT_ARRAY = $arr"
  echo "$arr" | grep -q '0x0000, 0x0001, 0x0002' || die "endpoints not contiguous in build output -- refusing (boot-crash risk)"
  say "build OK (v$(cur_version))"
}

# ---- package ---------------------------------------------------------------
package() {
  load_env
  local v semver; v="$(cur_version)"; semver="$(cur_semver)"
  # Both flavours ship publicly, so their artifacts must be distinguishable. Same version int on
  # purpose (#77 keeps versioning unified); the FLAVOUR lives in the filename, never in the version.
  # A debug and a release image at one version are DIFFERENT binaries -- never treat one as the
  # other's delta base or recovery image (#82).
  local sfx=""; [ "${HISENSE_FLAVOUR:-release}" = "debug" ] && sfx="-debug"
  local fw="$GCC_RELEASE/application_is/Debug/bin/firmware_is.bin"
  local flash="$GCC_RELEASE/application_is/Debug/bin/flash_is.bin"
  local fwarch="$REPO/firmware/built-images/firmware_is-v$v$sfx.bin"
  local clip="$REPO/firmware/built-images/flash_rac-integrated-v$v$sfx.bin"
  local ota="$REPO/firmware/built-images/rac-v$v$sfx.ota"
  [ -f "$fw" ] || die "no firmware_is.bin -- build first"
  # The flavour comes from the environment but the CONTENT comes from whatever build/ holds, so a
  # `package` after the wrong `build` would silently mislabel an image -- and a debug image landing
  # under a release name is exactly the mistake #22 exists to prevent. Verify the binary against
  # the claimed flavour using the console's own log string.
  # NOTE: `strings ... | grep -q` is WRONG under `set -o pipefail`. grep -q exits the moment it
  # matches, strings then dies of SIGPIPE, and pipefail turns that into a failed pipeline -- so the
  # test reads FALSE exactly when the string IS present. Release builds passed only because grep
  # found nothing and strings ran to completion, which made the bug invisible until the first debug
  # package attempt. grep -c consumes all input, so strings exits cleanly.
  local console_hits
  console_hits="$(strings "$fw" | grep -c "diag console listening" || true)"
  if [ "${console_hits:-0}" -gt 0 ]; then
    [ "${HISENSE_FLAVOUR:-release}" = "debug" ] || \
      die "built image CONTAINS the :2323 console but flavour is release -- rebuild without --debug, or package with HISENSE_FLAVOUR=debug"
  else
    [ "${HISENSE_FLAVOUR:-release}" != "debug" ] || \
      die "flavour is debug but the built image has NO console -- rebuild with 'build --debug' first"
  fi
  # otaUrl (#79): default to a LOCAL file:// (staged into --ota-provider-dir). If OTA_RELEASE_BASE
  # is set, point at the GitHub release asset instead -- python-matter-server's OTA provider
  # downloads an http(s):// otaUrl (checksum-verified) then re-serves it over BDX, so the big .ota
  # can live in the release and only the small .json need be staged on the Pi.
  local otaurl="file:///rac-v$v$sfx.ota"
  [ -n "${OTA_RELEASE_BASE:-}" ] && otaurl="${OTA_RELEASE_BASE%/}/amebaz2-v$semver/rac-v$v$sfx.ota"
  say "package v$semver (softwareVersion $v, ${HISENSE_FLAVOUR:-release} flavour): raw image + clip image + .ota + manifest"
  python3 -c "d=open('$flash','rb').read(); open('$clip','wb').write(d + b'\xff'*(4194304-len(d)))"
  python3 "$OTA_TOOL" create -v "$VID" -p "$PID" -vn "$v" -vs "$semver" -da sha256 -mi 1 -ma "$((v-1))" "$fw" "$ota" >/dev/null
  # #2 loose end: archive the RAW firmware_is.bin too. publish() uploads firmware_is-v$v*.bin as
  # the byte-exact deployed payload (it is what the break-glass HTTP OTA streams), but nothing
  # ever created that filename, so publish silently depended on a manual copy.
  cp "$fw" "$fwarch"
  python3 - "$ota" "$v" "$semver" "$otaurl" > "$REPO/firmware/built-images/rac-v$v$sfx.json" <<'PY'
import sys,hashlib,base64,json
ota,v,semver,otaurl=sys.argv[1],int(sys.argv[2]),sys.argv[3],sys.argv[4]; d=open(ota,'rb').read()
print(json.dumps({"modelVersion":{"vid":0xFFF1,"pid":0x8001,"softwareVersion":v,"softwareVersionString":semver,"cdVersionNumber":1,"firmwareInformation":"","softwareVersionValid":True,"otaUrl":otaurl,"otaFileSize":len(d),"otaChecksum":base64.b64encode(hashlib.sha256(d).digest()).decode(),"otaChecksumType":1,"minApplicableSoftwareVersion":1,"maxApplicableSoftwareVersion":v-1,"releaseNotesUrl":""}}))
PY
  say "  raw:      $fwarch  (byte-exact deployed payload -- what publish() uploads, #2)"
  say "  clip:     $clip"
  say "  ota:      $ota  (+ .json manifest, otaUrl=$otaurl)"
}

# ---- stage on the Pi -------------------------------------------------------
stage() {
  load_env
  : "${PI_HOST:?}" "${PI_OTA_DIR:?}" "${PI_SSH_KEY:?}"
  local v; v="$(cur_version)"
  # Honour the flavour suffix that `package` writes. Without this, `package` produces
  # rac-vNNNNN-debug.ota while stage uploads the unsuffixed rac-vNNNNN.ota, so a debug
  # build silently deploys the RELEASE image: the flash succeeds, the version verifies
  # (both flavours share one version int by design, #77), and the only symptom is that
  # the console you built the image for is not there.
  local sfx=""; [ "${HISENSE_FLAVOUR:-release}" = "debug" ] && sfx="-debug"
  local src_ota="$REPO/firmware/built-images/rac-v$v$sfx.ota"
  local src_json="$REPO/firmware/built-images/rac-v$v$sfx.json"
  [ -f "$src_ota" ] || die "no $src_ota -- run 'package' for this flavour first"
  say "stage v$v${sfx:+ ($HISENSE_FLAVOUR)} on $PI_HOST:$PI_OTA_DIR + restart matter-server"
  # Upload under the manifest's OWN names: the .json's otaUrl already references
  # rac-v$v$sfx.ota, so renaming on upload would break the reference.
  scp -o BatchMode=yes -i "$PI_SSH_KEY" "$src_ota" "$src_json" \
    "$PI_HOST:$PI_OTA_DIR/" >/dev/null
  # Then remove any OTHER flavour's manifest at this same version int. Both flavours share
  # one version by design (#77), so leaving both on the provider gives it two candidates it
  # cannot disambiguate, and it may serve the one you did not build.
  local other=""; [ -n "$sfx" ] && other="rac-v$v.json" || other="rac-v$v-debug.json"
  ssh -o BatchMode=yes -i "$PI_SSH_KEY" "$PI_HOST" \
    "rm -f $PI_OTA_DIR/$other 2>/dev/null" >/dev/null 2>&1 || true
  # cache hygiene: prune the ephemeral OTA-provider junk that piles up per attempt
  # (KVS + per-run logs). Leave .ota/.json manifests (needed for rollback images).
  ssh -o BatchMode=yes -i "$PI_SSH_KEY" "$PI_HOST" \
    "rm -f $PI_OTA_DIR/chip_kvs_ota_provider_* $PI_OTA_DIR/ota_provider_*.log 2>/dev/null; \
     docker restart matter-server >/dev/null 2>&1"   # restart => reload manifests (loaded once at init)
  say "  staged + provider junk pruned + matter-server restarted (manifest cache reloaded)"
}

# ---- flash (update_node with retries + rollback detection) -----------------
# #64: after the python gate confirms the new version AND a working subscription (fatal
# re-interview + node-available poll -- the availability transition IS the assertion), confirm
# the actual 'Subscription succeeded' line in the matter-server log when it is readable from
# this box (same ssh pattern stage() uses). That line is exactly what was missing in the
# 2026-07-19 'Invalid TLV tag' regression. The line is matched per-node (<Node:N> ...) -- a
# bare grep once 'confirmed' node 14's flash with node 35's earlier line. An unreadable log
# (PI_* unset) falls back to the availability transition alone, loudly.
check_subscription_log() {
  local node="$1"
  if [ -z "${PI_HOST:-}" ] || [ -z "${PI_SSH_KEY:-}" ]; then
    say "  PI_HOST/PI_SSH_KEY unset -- cannot read the matter-server log; node availability stands as the subscription assertion (#64)"
    return 0
  fi
  # After an OTA the device REBOOTS, so the healthy post-flash signal is usually a
  # '<Node:N> Re-Subscription succeeded' logged a few seconds after the re-interview, NOT the
  # plain 'Subscription succeeded' (that one is the PRE-reboot subscription and is often already
  # >15m old, i.e. out of the window, by the time we check). So: match BOTH forms; take the most
  # RECENT with 'tail -1' (grep -m1 took the oldest); and poll briefly, because the resubscribe
  # can land a few seconds after we start looking. The old single-shot 'Subscription succeeded'
  # grep false-alarmed two genuinely-healthy flashes on 2026-07-22 (nodes 14 and 62).
  local line=""
  for _ in 1 2 3 4 5 6; do
    if ! line="$(ssh -o BatchMode=yes -o ConnectTimeout=10 -i "$PI_SSH_KEY" "$PI_HOST" \
        "docker logs --since 15m matter-server 2>&1 | sed -E 's/\x1b\[[0-9;]*m//g' | grep -E '<Node:$node> (Re-)?Subscription succeeded' | tail -1 || true" 2>/dev/null)"; then
      say "  could not read the matter-server log on $PI_HOST -- node availability stands as the subscription assertion (#64)"
      return 0
    fi
    [ -n "$line" ] && break
    sleep 10
  done
  if [ -n "$line" ]; then
    say "  matter-server log confirms: ${line:0:120}"
  else
    # The flash gate already re-interviewed the node and polled it back to available, and
    # matter-server only marks a node available once its subscription is up -- so availability IS
    # the subscription assertion (#64). matter-server sometimes RESUMES a subscription after the
    # re-interview without logging a fresh '(Re-)Subscription succeeded' line (seen on the 2026-07-23
    # reject flip), so a missing line here is not proof of a break. Warn, do not die: the primary
    # gate already passed, and a false die aborts a healthy flash mid-run.
    say "  no fresh '(Re-)Subscription succeeded' for node $node in 15m -- availability after re-interview already asserted the subscription (#64); matter-server likely resumed it without a new line. OK."
  fi
}
flash() {
  load_env
  : "${OTAENV_PY:?}" "${MS_WS:?}" "${NODE_ID:?}"
  local v; v="$(cur_version)"
  say "flash v$v to node $NODE_ID (retries; then verify the reported version changed)"
  if "$OTAENV_PY" - "$MS_WS" "$NODE_ID" "$v" <<'PY'
import asyncio,json,sys,aiohttp
URL,NODE,V=sys.argv[1],int(sys.argv[2]),int(sys.argv[3])
async def call(ws,c,a,m,t=600):
    await ws.send_json({"message_id":m,"command":c,"args":a})
    while True:
        d=json.loads((await ws.receive(timeout=t)).data)
        if d.get("message_id")==m: return d
async def sw(ws):
    # FRESH read (read_attribute), NOT get_node -- get_node returns matter-server's
    # cached attributes, which can lie (stale version) after a reboot/rollback.
    r=await call(ws,"read_attribute",{"node_id":NODE,"attribute_path":"0/40/9"},"g",30)
    res=r.get("result")
    return res.get("0/40/9") if isinstance(res,dict) else res
async def main():
    for n in range(1,8):
        print(f"[flash] update_node attempt {n} -> v{V}",flush=True)
        try:
            async with aiohttp.ClientSession() as s:
                async with s.ws_connect(URL,heartbeat=30) as ws:
                    await ws.receive(timeout=10)
                    r=await call(ws,"update_node",{"node_id":NODE,"software_version":V},str(n),600)
                    if r.get("error_code") is None: print("[flash] provider reports finished"); break
                    print("[flash] declined:",r.get("error_code"),r.get("details",""))
        except Exception as e: print("[flash] exc",repr(e)[:120])
        await asyncio.sleep(15)
    # verify the DEVICE actually booted the new version. Require it SUSTAINED across 3
    # consecutive fresh reads -- a single read right after a matter-server restart can
    # return a stale cached value (this false-positived the flash repeatedly 2026-07-08).
    good=0
    for _ in range(30):
        try:
            async with aiohttp.ClientSession() as s:
                async with s.ws_connect(URL,heartbeat=30) as ws:
                    await ws.receive(timeout=8)
                    v=await sw(ws)
                    if v==V:
                        good+=1; print(f"[flash] device reports v{V} ({good}/3)")
                        if good>=3:
                            # #64: a sustained version read is NOT enough -- the 2026-07-19
                            # regression passed every read gate while wildcard subscriptions
                            # failed with 'Invalid TLV tag'. The re-interview is FATAL now, and
                            # the node must then come back available: matter-server only marks a
                            # node available once its subscription is up, so that transition is
                            # the subscription assertion (docs/10 §16).
                            r=await call(ws,"interview_node",{"node_id":NODE},"iv",120)
                            if not r or r.get("error_code") is not None:
                                print(f"[flash] FAILED: re-interview rejected: {r.get('error_code') if r else 'no reply'} -- data-model/subscription break? (#64, docs/10 §16)")
                                sys.exit(3)
                            print("[flash] re-interviewed node for HA (docs/10 §9)")
                            ok=False
                            for _ in range(25):   # ~75 s
                                try:
                                    g=await call(ws,"get_node",{"node_id":NODE},"gn",15)
                                    n=g.get("result") if g else None
                                    if isinstance(n,dict) and n.get("available") is True:
                                        ok=True; break
                                except Exception: pass
                                await asyncio.sleep(3)
                            if not ok:
                                print(f"[flash] FAILED: node never became available after re-interview (~75 s) -- subscription broken (#64, docs/10 §16)")
                                sys.exit(3)
                            print(f"[flash] SUCCESS: device booted v{V} and is subscribable (available after re-interview)")
                            return
                    else:
                        good=0; print(f"[flash] device reports v{v} (want {V}) ...")
        except Exception: pass
        await asyncio.sleep(12)
    print(f"[flash] FAILED: device never sustained v{V} -- OTA serial not bumped (rollback) or boot crash (docs/10 §7,§11)")
    sys.exit(2)
asyncio.run(main())
PY
  then echo "$v" > "$RELEASED_MARK"; say "recorded on-device version $v"; check_subscription_log "$NODE_ID"
  else die "flash verification failed for v$v -- version not sustained (rollback/boot crash, docs/10 §7,§11) or the subscription gate failed (#64, docs/10 §16); see the [flash] lines above"; fi
}

# ---- revert to stock (issue #19) -------------------------------------------
# Two ways back to the stock ConnectLife firmware without opening the case:
#   --flip <unit-ip>: 1.3.8+ carries a break-glass TCP listener on BREAKGLASS_PORT.
#     `<token>:slots` reports both slots' FWHS serials; `<token>:revert` invalidates the
#     running image's signature and resets. The bootloader boots the signature-valid slot
#     with the HIGHEST serial, so invalidating the custom slot (serial SERIAL_BASE+v) lets
#     the stock slot (serial 100) win. Plain `<token>` with no colon still triggers the
#     HTTPS OTA, so only the colon commands are used here.
#   --repackage <dump>: carve the stock app out of a stock flash dump and re-sign it
#     (serial patch + HMAC-SHA256 + bytesum trailer; recipe proven byte-exact against
#     firmware_is-v10307.bin and the dump's fw1), then wrap it as a Matter .ota so the
#     normal update_node channel pushes stock back onto the unit.
#   --apply: stage the repackaged image on the Pi + drive update_node for $NODE_ID, then
#     CLASSIFY the outcome (reverted / not reverted / ambiguous). Never infers success from
#     silence: stock leaves this fabric and a bootloader-rejected module does too (#75).
#   --slots <unit-ip>: read-only :slots probe used for triage (changes nothing).
#   --backup <unit-ip>: 1.3.9+ answers `<token>:backup` with a header line
#     `ok: backup <addr_hex> <len_dec>\r\n` + exactly <len> raw bytes of the INACTIVE slot
#     (0x1AC000 max, trailing 0xFF padding). Fetch it once right after the first conversion
#     and the unit keeps a way back to stock even after later OTAs overwrite the stock slot.
revert_version() {  # int the revert image must carry: strictly above BOTH version markers
  local c r; c="$(cur_version)"; r="$(released_version)"
  echo $(( c > r ? c + 1 : r + 1 ))
}
revert_backup() {
  load_env
  : "${BREAKGLASS_TOKEN:?}" "${BREAKGLASS_PORT:?}"
  local ip="" out="" a
  for a in "$@"; do
    case "$a" in
      -*) die "unknown flag for revert --backup: $a" ;;
      *) if [ -z "$ip" ]; then ip="$a"
         elif [ -z "$out" ]; then out="$a"
         else die "unexpected argument for revert --backup: $a"; fi ;;
    esac
  done
  [ -n "$ip" ] || die "usage: ota-release.sh revert --backup <unit-ip> [out.bin]"
  say "revert --backup $ip:$BREAKGLASS_PORT (fetch + validate the inactive-slot stock image)"
  PYTHONPATH="$HERE" python3 - "$ip" "$BREAKGLASS_TOKEN" "$BREAKGLASS_PORT" "$out" \
    "$REPO/firmware/built-images" "${SERIAL_BASE:-1100}" <<'PY'
import socket,sys,re,os
from amebaz2_image import failures,read_serial,verify
ip,token,port,out,bi,base=sys.argv[1],sys.argv[2],int(sys.argv[3]),sys.argv[4],sys.argv[5],int(sys.argv[6])
def fail(msg):  # die loudly, save nothing
    print(f"[revert] FAILED: {msg}"); sys.exit(1)
try:
    s=socket.create_connection((ip,port),timeout=15)   # handles IPv6 link-local (fe80::..%if)
except OSError as e:
    fail(f"cannot reach {ip}:{port}: {e} -- if the unit is otherwise up, it likely runs pre-1.3.9 firmware with no :backup support")
s.settimeout(60)
try:
    s.sendall(f"{token}:backup".encode())
    buf=b""
    while b"\r\n" not in buf:
        b=s.recv(256)
        if not b: fail("connection closed before the backup header")
        buf+=b
        if len(buf)>4096: fail("no backup header in the first 4 KB")
    hdr,buf=buf.split(b"\r\n",1)
    hdr=hdr.decode(errors="replace").strip()
    if hdr.startswith("err:"):
        fail(f"unit refused: {hdr}")
    m=re.fullmatch(r"ok:\s*backup\s+(\S+)\s+(\d+)",hdr)
    if not m:
        fail(f"unexpected :backup reply: {hdr!r} -- pre-1.3.9 firmware has no :backup support")
    addr,length=m.group(1),int(m.group(2))
    if not 0<length<=0x1AC000:
        fail(f"implausible backup length {length:#x} (slot is 0x1AC000 max)")
    while len(buf)<length:
        b=s.recv(min(65536,length-len(buf)))
        if not b: fail(f"connection closed at {len(buf):#x} of {length:#x} bytes")
        buf+=b
    buf=buf[:length]
finally:
    s.close()
print(f"[revert] backup received: addr={addr} len={length:#x}")
# Process like the repackage carve: image end = first 4096-byte 0xFF run, then a 4-byte trailer.
i=buf.find(b'\xff'*4096)
if i<0: fail("no 4096-byte 0xFF run in the backup -- slot empty or not a stock image?")
imglen=i-4
if not 0x100000<=imglen<=0x180000: fail(f"carved length {imglen:#x} outside [0x100000,0x180000]")
image,trailer=buf[:imglen],buf[imglen:imglen+4]
serial=read_serial(image)
if serial>=base:
    fail(f"serial@0xF4={serial} >= {base} -- the inactive slot holds a CUSTOM image, not stock (nothing to back up; a second custom OTA already overwrote it)")
ok,lines=verify(image,trailer)   # manifest sig + EVERY sub-image trailer + byte-sum (#75)
for ln in lines: print(f"[revert]   {ln}")
if not ok: fail("the backup does not verify: "+"; ".join(failures(lines)))
if not out:
    tag=ip.split('%')[0]                          # drop an IPv6 zone id for the filename
    if re.fullmatch(r"(\d{1,3}\.){3}\d{1,3}",tag): tag=tag.rsplit('.',1)[1]
    else: tag=re.sub(r"[^0-9A-Za-z]+","-",tag).strip('-')
    out=os.path.join(bi,f"stock-backup-{tag}-sn{serial}.bin")
open(out,'wb').write(buf)                         # raw slot bytes: image + trailer + 0xFF pad
print(f"[revert] saved {out} ({len(buf):#x} bytes, stock serial {serial})")
print(f"[revert] all checks passed: serial {serial} < {base}, manifest sig + every sub-image trailer + byte-sum OK")
print(f"[revert] next (when needed): ota-release.sh revert --repackage {out}")
PY
}
revert_flip() {
  load_env
  : "${BREAKGLASS_TOKEN:?}" "${BREAKGLASS_PORT:?}"
  local ip="" force=0 a
  for a in "$@"; do
    case "$a" in
      --force) force=1 ;;
      -*) die "unknown flag for revert --flip: $a" ;;
      *) [ -z "$ip" ] && ip="$a" || die "unexpected argument for revert --flip: $a" ;;
    esac
  done
  [ -n "$ip" ] || die "usage: ota-release.sh revert --flip <unit-ip> [--force]"
  say "revert --flip $ip:$BREAKGLASS_PORT (query slots, then invalidate the running slot)"
  python3 - "$ip" "$BREAKGLASS_TOKEN" "$BREAKGLASS_PORT" "$force" "${SERIAL_BASE:-1100}" <<'PY'
import socket,sys,re
ip,token,port=sys.argv[1],sys.argv[2],int(sys.argv[3])
force=sys.argv[4]=="1"; base=int(sys.argv[5])
def query(msg,timeout=10):
    with socket.create_connection((ip,port),timeout=timeout) as s:
        s.sendall(msg.encode()); s.settimeout(timeout); data=b""
        try:
            while b"\n" not in data and len(data)<512:
                b=s.recv(256)
                if not b: break
                data+=b
        except socket.timeout: pass
        return data.decode(errors="replace").strip()
try:
    r=query(f"{token}:slots")
except OSError as e:
    print(f"[revert] cannot reach {ip}:{port}: {e}")
    print("[revert] if the unit is otherwise up, it likely runs pre-1.3.8 firmware with NO break-glass listener -- use 'revert --repackage' + 'revert --apply' instead")
    sys.exit(2)
m=re.match(r"ok:\s*fw1_sn=(\d+)\s+fw2_sn=(\d+)\s+cur=(\d+)",r)
if not m:
    print(f"[revert] unexpected :slots reply: {r!r}")
    print("[revert] pre-1.3.8 firmware has no :slots support (there a bare <token> with no colon triggers HTTPS OTA) -- use 'revert --repackage' + 'revert --apply' instead")
    sys.exit(2)
fw1,fw2,cur=int(m.group(1)),int(m.group(2)),int(m.group(3))
other=fw2 if cur==1 else fw1
print(f"[revert] slots: fw1_sn={fw1} fw2_sn={fw2} cur=fw{cur} -> other slot serial {other}")
if other>=base and not force:
    print(f"[revert] REFUSED: other slot serial {other} >= {base} is a custom image, not stock (stock serial is 100)")
    print("[revert] re-run with --force to flip to that older custom image anyway")
    sys.exit(3)
r=query(f"{token}:revert")
print(f"[revert] revert reply: {r!r}")
print("[revert] running image invalidated + reset issued; the bootloader now boots the other slot. Give the unit ~30 s.")
PY
}
# Probe the break-glass listener with `<token>:slots`. Prints the raw `ok: ...` reply and
# returns 0 ONLY when the unit answered. Asymmetric on purpose: an answer is decisive (only
# the custom firmware serves :slots), silence proves nothing at all -- stock has no listener,
# so a healthy stock unit and a bootloader-rejected module are both silent here.
breakglass_slots() {  # <ip> [timeout-seconds]
  local ip="$1" t="${2:-8}"
  [ -n "${BREAKGLASS_TOKEN:-}" ] && [ -n "${BREAKGLASS_PORT:-}" ] || return 1
  python3 - "$ip" "$BREAKGLASS_TOKEN" "$BREAKGLASS_PORT" "$t" <<'PY'
import socket,sys
ip,token,port,t=sys.argv[1],sys.argv[2],int(sys.argv[3]),float(sys.argv[4])
d=b""
try:
    with socket.create_connection((ip,port),timeout=t) as s:   # handles IPv6 link-local (fe80::..%if)
        s.sendall(f"{token}:slots".encode()); s.settimeout(t)
        while b"\n" not in d and len(d)<512:
            b=s.recv(256)
            if not b: break
            d+=b
except OSError:
    sys.exit(1)
r=d.decode(errors="replace").strip()
if not r.startswith("ok:"): sys.exit(1)
print(r)
PY
}
revert_repackage() {
  load_env
  local dump="${1:-}"
  [ -n "$dump" ] || die "usage: ota-release.sh revert --repackage <stock-dump.bin>"
  [ -f "$dump" ] || die "stock dump not found: $dump"
  local v semver; v="$(revert_version)"; semver="$(int_to_semver "$v")-stock"
  local bi="$REPO/firmware/built-images"
  local payload="$bi/rac-stock-v$v-payload.bin"
  local ota="$bi/rac-stock-v$v.ota"
  # Self-check the recipe against known-good bytes BEFORE trusting the carve: every archived
  # custom firmware_is-v*.bin and the dump's unpatched fw1 must HMAC/bytesum-verify. A dump
  # from a different build or a wrong key fails loudly here instead of on the device.
  say "revert --repackage v$v (serial $(( ${SERIAL_BASE:-1100} + v ))): verify the signing recipe first"
  python3 - "$dump" "$bi" "$payload" "$(( ${SERIAL_BASE:-1100} + v ))" "${SERIAL_BASE:-1100}" <<'PY'
import glob,struct,hmac,hashlib,sys,os
dump,bi,out,newser,base=sys.argv[1],sys.argv[2],sys.argv[3],int(sys.argv[4]),int(sys.argv[5])
KEY=bytes.fromhex('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e5f')
def inner_off(payload):
    # #75: besides the manifest signature the bootloader verifies ONE HMAC trailer PER
    # sub-image. For sub-image i at header offset H: S=u32le(img[H]) is the segment SIZE,
    # the trailer is at END=H+0x60+S and covers img[START:END] with START=0 for i==0 else H.
    # Sub-image 0 sits at H=0xE0, so END = 0xE0+0x60+S = u32le(img[0xE0])+0x140. Only that
    # first span reaches the serial at +0xF4 and the +0x00 signature, so patching the serial
    # staleness ONLY this trailer -- which is why recomputing just this one is sufficient.
    #
    # NAMING TRAP (this comment used to get it wrong): u32le(img[0xE0]) is the SIZE field, it
    # is NOT next_img. next_img is u32le(img[H+4]) and is a RELATIVE offset (next header =
    # H + that value; 0xFFFFFFFF terminates). Confusing the two gives 0x4060 instead of 0x3d80
    # on a real image, so a verifier built on the wrong name rejects every genuine build.
    #
    # boot_load compares the trailer, prints "Hash Result Incorrect!" and hangs with the flash
    # QE bit cleared. Missing this is what bricked the office unit on 2026-07-21.
    return struct.unpack_from('<I',payload,0xE0)[0]+0x140
def chain(payload):
    # Walk the sub-image chain and return [(H,END,match)] for EVERY trailer, not just #0.
    # Measured to reproduce all three trailers on all 23 archived firmware_is-v*.bin (custom
    # layout 0xE0/0x4000/0xF8000) and on every stock carve (TWO layouts in the wild:
    # 0xE0/0x8000/0x114000 and 0xE0/0x8000/0x120000 -- never hardcode these). It fails only on
    # the four pre-#75 rac-stock-v*-payload.bin. Do NOT assert the last trailer ends at EOF:
    # elf2bin pads 0 or 0x20 trailing 0x87 bytes (both occur across the archive). And note the
    # walk is slot-agnostic by construction: boot_load takes the image base from the partition
    # table, stock has booted fine from FW2, so never gate any of this on a slot index.
    H=0xE0; i=0; out=[]
    while True:
        if H+0x60>len(payload): return None
        S=struct.unpack_from('<I',payload,H)[0]; nxt=struct.unpack_from('<I',payload,H+4)[0]
        END=H+0x60+S; START=0 if i==0 else H
        if END+0x20>len(payload): return None
        out.append((H,END,hmac.new(KEY,payload[START:END],hashlib.sha256).digest()==payload[END:END+0x20]))
        if nxt==0xffffffff: return out
        if nxt==0 or i>15: return None
        H+=nxt; i+=1
def verify(payload,trailer,name):
    mac_ok=hmac.new(KEY,payload[0xE0:0x140],hashlib.sha256).digest()==payload[0:32]
    c=chain(payload)
    in_ok=c is not None and all(m for _,_,m in c)
    sum_ok=struct.pack('<I',sum(payload)&0xffffffff)==trailer
    serial=struct.unpack_from('<I',payload,0xF4)[0]
    sub="UNWALKABLE" if c is None else " ".join("%#x=%s"%(e,"OK" if m else "MISMATCH") for _,e,m in c)
    print(f"  {name}: len={len(payload):#x} serial@0xF4={serial} hmac={'OK' if mac_ok else 'MISMATCH'} trailers[{sub}] bytesum={'OK' if sum_ok else 'MISMATCH'}")
    return mac_ok and in_ok and sum_ok
refs=sorted(glob.glob(os.path.join(bi,'firmware_is-v*.bin')))
if not refs:
    print("  no firmware_is-v*.bin in built-images/ -- cannot self-check the recipe"); sys.exit(1)
ok=True
for f in refs:
    d=open(f,'rb').read()
    ok &= verify(d[:-4],d[-4:],os.path.basename(f))
d=open(dump,'rb').read()
# Two accepted inputs: a full flash dump (stock fw1 app at 0x10000) or a raw slot image from
# revert --backup (app at 0x0). Carve at the first offset yielding an HMAC-valid image.
img=None; imglen=0; off=0
for off in (0x10000,0):
    c=d[off:]
    i=c.find(b'\xff'*4096)                       # end of image = first 4 KB run of erased flash
    if i<0: continue
    l=i-4                                        # minus the 4-byte bytesum trailer
    if not 0x100000<=l<=0x180000: continue
    if hmac.new(KEY,c[0xE0:0x140],hashlib.sha256).digest()!=c[0:32]: continue
    img=c; imglen=l; break
if img is None:
    print("  no HMAC-valid stock image at 0x10000 (dump) or 0x0 (backup) -- refusing"); sys.exit(1)
# The carve proves "a signed image is here", NOT "that image is stock". A dump taken from an
# already-converted unit carves just as cleanly (dumps/office_postfail2_20260727.bin -> serial
# 11430), and shipping that as a "revert to stock" .ota would push CUSTOM firmware under a
# stock label. revert --backup already applies this exact test; --repackage did not.
# Stock carries serial 100; custom carries SERIAL_BASE+versionInt. This is a SERIAL test, not
# a slot test: boot_load is slot-agnostic and stock has booted fine from FW2, so never gate
# any revert path on which slot the bytes came from.
_s=struct.unpack_from('<I',img,0xF4)[0]
if _s>=base:
    print(f"  carved image serial@0xF4={_s} >= {base}: this is a CUSTOM image, not stock -- refusing"); sys.exit(1)
ok &= verify(img[:imglen],img[imglen:imglen+4],f"input image @0x{off:x} (unpatched)")
if not ok:
    print("recipe self-check FAILED -- not building a revert image from unverified bytes"); sys.exit(1)
payload=bytearray(img[:imglen])
# Order matters: each step's input includes the previous step's output.
struct.pack_into('<I',payload,0xF4,newser)       # serial so the bootloader prefers this slot
payload[0:32]=hmac.new(KEY,bytes(payload[0xE0:0x140]),hashlib.sha256).digest()   # manifest sig
_L=inner_off(payload)                            # inner image HMAC (#75) -- covers [0,L), incl. the sig
payload[_L:_L+0x20]=hmac.new(KEY,bytes(payload[0:_L]),hashlib.sha256).digest()
payload+=struct.pack('<I',sum(payload)&0xffffffff)                               # transport byte-sum
open(out,'wb').write(payload)
print(f"  signed payload: {out} ({len(payload):#x} bytes, serial {newser})")
PY
  python3 "$OTA_TOOL" create -v "$VID" -p "$PID" -vn "$v" -vs "$semver" -da sha256 -mi 1 -ma "$((v-1))" "$payload" "$ota" >/dev/null
  # Same manifest shape as package(), plus payloadName so the carved stock payload behind
  # the .ota stays identifiable. otaUrl is the staged file:// name (revert_apply uploads
  # both files under these exact names).
  python3 - "$ota" "$v" "$semver" "$(basename "$payload")" > "$bi/rac-stock-v$v.json" <<'PY'
import sys,hashlib,base64,json
ota,v,semver,pname=sys.argv[1],int(sys.argv[2]),sys.argv[3],sys.argv[4]; d=open(ota,'rb').read()
print(json.dumps({"modelVersion":{"vid":0xFFF1,"pid":0x8001,"softwareVersion":v,"softwareVersionString":semver,"cdVersionNumber":1,"firmwareInformation":"","softwareVersionValid":True,"otaUrl":f"file:///rac-stock-v{v}.ota","otaFileSize":len(d),"otaChecksum":base64.b64encode(hashlib.sha256(d).digest()).decode(),"otaChecksumType":1,"minApplicableSoftwareVersion":1,"maxApplicableSoftwareVersion":v-1,"releaseNotesUrl":"","payloadName":pname}}))
PY
  say "  ota:      $ota  (+ .json manifest, payloadName=$(basename "$payload"))"
  say "  not staged. next: ota-release.sh revert --apply"
  say "  NOTE: the 2026-07-21 brick (docs/10 §17 'Path 2') is root-caused (#75): the old recipe left"
  say "  sub-image 0's inner HMAC (at size@0xE0 + 0x140) stale. This payload recomputes it and"
  say "  self-checks EVERY sub-image trailer, on this payload and on every archived image."
  say "  CONFIRMED ON HARDWARE 2026-07-27: a repackaged stock image booted (VID 5004 / PID 13825 / sw 2)."
  say "  Recovery if it ever does fail is still the CH341A clip; a rejected image hangs with QE CLEARED."
  # Version-consumption guard: the revert image carries serial SERIAL_BASE+v, so once it is
  # applied the next custom OTA must EXCEED v or the bootloader ties and the stock slot wins.
  # Keep version.txt ahead of every revert int ever handed out here.
  if [ "$(cur_version)" -le "$v" ]; then
    local nv; nv="$(int_to_semver "$((v+1))")"
    echo "$nv" > "$VERSION_FILE"
    set_header_version
    say "WARNING: the revert consumed fleet version $v -- version.txt bumped to $nv (int $((v+1)))"
    say "         so the next custom build beats serial $(( ${SERIAL_BASE:-1100} + v )). COMMIT firmware/src/version.txt."
  fi
}
# Read-only break-glass probe. Prints the slot map and changes NOTHING (unlike --flip, which
# invalidates the running image). This is the triage command: an answer PROVES the custom
# firmware is alive, silence proves nothing.
revert_slots() {  # <unit-ip>
  load_env
  : "${BREAKGLASS_TOKEN:?}" "${BREAKGLASS_PORT:?}"
  local ip="${1:-}" r
  [ -n "$ip" ] || die "usage: ota-release.sh revert --slots <unit-ip>"
  if r="$(breakglass_slots "$ip" 8)"; then
    say "$ip:$BREAKGLASS_PORT answered: $r"
    say "  ANSWERED = the CUSTOM firmware is running (only it serves :slots). The module is alive."
    return 0
  fi
  say "$ip:$BREAKGLASS_PORT did not answer."
  say "  This is NOT a verdict. Stock carries no break-glass listener, and stock also joins its"
  say "  OWN factory-provisioned network, so a healthy reverted unit is not even at this address."
  say "  A bootloader-rejected module is equally silent. Discriminate with the flash QE bit:"
  say "  python3 $REPO/firmware/flasher/ch341_sr.py  (QE cleared = the bootloader rejected it)."
  return 1
}
# Printed whenever --apply cannot tell "stock is running" from "the bootloader rejected the
# image". Network silence is NOT a verdict: stock leaves this fabric, joins its own network
# and has no break-glass listener, so a healthy revert and a #75-class rejection look
# identical from here. Two healthy units were declared bricked on 2026-07-26 for exactly this
# reason. These are the checks that actually discriminate.
revert_triage() {  # <unit-ip-or-placeholder>
  local ip="${1:-<unit-ip>}"
  say "----------------------------------------------------------------------"
  say "TRIAGE: the unit is silent. Silence alone means NOTHING. Run these, in order."
  say ""
  say "1. VENDOR APP (no tools). A reverted unit IS stock: it rejoins ConnectLife by itself"
  say "   and reappears in the ConnectLife app as the same appliance. If the app sees it, the"
  say "   revert WORKED. Note the A/C keeps working from the IR remote either way, so 'the"
  say "   A/C responds' says nothing about the Wi-Fi module."
  say ""
  say "2. BREAK-GLASS (no tools, read-only). Stock does not carry the listener, so:"
  say "     ota-release.sh revert --slots $ip"
  say "   ANSWERS -> the CUSTOM firmware is still running: the revert did not take, the unit"
  say "              is healthy, nothing is bricked. Retry --apply, or flip if stock is in the"
  say "              other slot. SILENT -> consistent with stock AND with a rejected image;"
  say "              not a verdict."
  say ""
  say "3. FLASH QE BIT (CH341A clip; the definitive test). boot_load routes every image"
  say "   rejection through its shared failure sink (hal_flash_return_spi), which CLEARS the"
  say "   GD25Q32 QE bit; a healthy boot leaves it SET (docs/10 §17 Path 2, issue #75)."
  say "     python3 $REPO/firmware/flasher/ch341_sr.py"
  say "   QE=0 -> the bootloader REJECTED the image (bricked, and it will NOT fall back to the"
  say "           other slot). QE=1 -> it did not reject anything; chase the network instead."
  say ""
  say "4. READ THE SLOTS OUT OF A DUMP (CH341A clip; settles it definitively)."
  say "     python3 $REPO/firmware/flasher/ch341dump.py /tmp/unit.bin"
  say "   fw1 app @0x10000, fw2 app @0x190000; serial = u32le at app+0xF4 (stock 100, custom"
  say "   ${SERIAL_BASE:-1100}+versionInt). 'revert --repackage /tmp/unit.bin' re-verifies the"
  say "   FULL acceptance recipe (manifest sig + #75 inner HMAC + bytesum) on fw1; for fw2,"
  say "   slice it out first: dd if=/tmp/unit.bin of=/tmp/fw2.bin bs=4096 skip=400."
  say ""
  say "5. RECOVERY, only if 3/4 say rejected: clip-write the unit's own dump with fw1 replaced"
  say "   by ORIGINAL stock slot bytes (factory signature, from 'revert --backup') and fw2"
  say "   erased to 0xFF, via ch341flash-full.py (it re-sets QE). docs/10 §17."
  say "----------------------------------------------------------------------"
}
# One-shot Basic Information read for the pre-apply summary: proves NODE_ID resolves to a unit
# that is reachable RIGHT NOW, and gives the operator something to match against the appliance
# in front of them. Empty output = the node did not answer.
ms_identity() {  # -> "sw=<int> vendor=<int> product=<int> label=<str>"
  "$OTAENV_PY" - "$MS_WS" "$NODE_ID" 2>/dev/null <<'PY'
import asyncio,json,sys,aiohttp
URL,NODE=sys.argv[1],int(sys.argv[2])
async def call(ws,c,a,m,t=20):
    await ws.send_json({"message_id":m,"command":c,"args":a})
    while True:
        d=json.loads((await ws.receive(timeout=t)).data)
        if d.get("message_id")==m: return d
async def main():
    out={}
    async with aiohttp.ClientSession() as s:
        async with s.ws_connect(URL,heartbeat=30) as ws:
            await ws.receive(timeout=10)
            for k,p in (("sw","0/40/9"),("vendor","0/40/1"),("product","0/40/2"),("label","0/40/5")):
                try:
                    r=await call(ws,"read_attribute",{"node_id":NODE,"attribute_path":p},k,20)
                    res=r.get("result"); out[k]=res.get(p) if isinstance(res,dict) else res
                except Exception: out[k]="?"
    print(f"softwareVersion={out.get('sw')} vendor={out.get('vendor')} product={out.get('product')} label={out.get('label')!r}")
asyncio.run(main())
PY
}
revert_apply() {
  load_env
  : "${OTAENV_PY:?}" "${MS_WS:?}" "${NODE_ID:?}" "${PI_HOST:?}" "${PI_OTA_DIR:?}" "${PI_SSH_KEY:?}"
  # --ip is optional but strongly recommended: the break-glass listener is the ONLY no-clip
  # signal that separates "stock is running" from "the bootloader rejected the image", so
  # without it the post-apply verdict can never be better than AMBIGUOUS. --yes skips the
  # confirmation prompt (scripted runs only).
  local ip="" assume_yes=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --ip)     ip="${2:-}"; [ -n "$ip" ] || die "--ip needs the unit's IP"; shift 2 ;;
      --ip=*)   ip="${1#--ip=}"; shift ;;
      --yes|-y) assume_yes=1; shift ;;
      *) die "unknown argument for revert --apply: $1 (usage: revert --apply [--ip <unit-ip>] [--yes])" ;;
    esac
  done
  # Do NOT recompute revert_version() here. revert_repackage() consumes int v and then bumps
  # version.txt to v+1 (its guard `cur_version <= v` is a tautology, since v is defined as
  # max(cur,released)+1). A separately-invoked apply would therefore compute v+2 and die looking
  # for a pair that was never built. Use the newest pair actually present on disk instead.
  local bi="$REPO/firmware/built-images" v="" f n
  for f in "$bi"/rac-stock-v*.ota; do
    [ -e "$f" ] || continue
    n="${f##*/rac-stock-v}"; n="${n%.ota}"
    case "$n" in *[!0-9]*) continue ;; esac
    [ -f "$bi/rac-stock-v$n.json" ] || continue
    if [ -z "$v" ] || [ "$n" -gt "$v" ]; then v="$n"; fi
  done
  [ -n "$v" ] || die "no rac-stock-v*.{ota,json} pair in $bi -- run 'revert --repackage <stock-dump.bin>' first"
  local src_ota="$bi/rac-stock-v$v.ota" src_json="$bi/rac-stock-v$v.json"

  # ---- pre-apply confirmation ------------------------------------------------------------
  # State, before anything is written: which node, which physical unit, which slot the image
  # lands in, and that the unit leaves this fabric. The 2026-07-26 confusion started with
  # nobody having written down what "healthy afterwards" was supposed to look like.
  local slots="" fw1="" fw2="" cur="" tgt="" other=""
  if [ -n "$ip" ]; then
    if slots="$(breakglass_slots "$ip" 8)"; then
      fw1="$(printf '%s' "$slots" | sed -n 's/.*fw1_sn=\([0-9]*\).*/\1/p')"
      fw2="$(printf '%s' "$slots" | sed -n 's/.*fw2_sn=\([0-9]*\).*/\1/p')"
      cur="$(printf '%s' "$slots" | sed -n 's/.*cur=\([0-9]*\).*/\1/p')"
      if [ "$cur" = "1" ]; then tgt=2; other="$fw2"; else tgt=1; other="$fw1"; fi
    else
      slots=""
      say "WARNING: $ip:${BREAKGLASS_PORT:-?} did not answer :slots. Wrong IP, BREAKGLASS_TOKEN/PORT"
      say "         unset in $ENVF, or the unit is already not running custom firmware. Fix it"
      say "         BEFORE applying: with no working break-glass probe the post-apply verdict"
      say "         can only ever be AMBIGUOUS."
    fi
  fi
  local pre; pre="$(ms_identity || true)"
  say "----------------------------------------------------------------------"
  say "revert --apply: about to push STOCK firmware onto a LIVE unit."
  say "  env file       : $ENVF"
  say "  target node    : $NODE_ID  (read from $ENVF -- an exported NODE_ID is IGNORED, load_env"
  say "                   sources the env file AFTER the environment. To target another unit,"
  say "                   copy the env file and run with ENVF=<copy>.)"
  say "  node reads now : ${pre:-NO ANSWER (node $NODE_ID unreachable through matter-server; update_node will likely fail)}"
  say "  matter-server  : $MS_WS  (staging on $PI_HOST:$PI_OTA_DIR)"
  say "  image          : $(basename "$src_ota")  softwareVersion int $v, FWHS serial $(( ${SERIAL_BASE:-1100} + v ))"
  say "  unit ip        : ${ip:-NOT GIVEN -- no break-glass probe, verdicts will be weaker}"
  if [ -n "$slots" ]; then
    say "  slots now      : fw1_sn=$fw1 fw2_sn=$fw2 running=fw$cur"
    say "  OTA lands in   : fw$tgt (the INACTIVE slot; whatever it holds now is overwritten)"
    if [ -n "$other" ] && [ "$other" -lt "${SERIAL_BASE:-1100}" ]; then
      say "  NOTE: fw$tgt ALREADY holds a stock image (serial $other < ${SERIAL_BASE:-1100}). Prefer"
      say "        'ota-release.sh revert --flip $ip': it boots that slot with NO flash write and"
      say "        NO re-signed payload, so it carries none of the #75 image-rejection risk. It"
      say "        also keeps the factory-signed stock bytes instead of overwriting them."
    fi
  fi
  say "  after this     : the unit LEAVES this Matter fabric, rejoins ConnectLife on its own"
  say "                   network, and loses the break-glass listener and the :2323 console."
  say "                   Silence afterwards is EXPECTED and is NOT proof of success."
  say "----------------------------------------------------------------------"
  if [ "$assume_yes" = 1 ]; then
    say "--yes given: proceeding without confirmation"
  else
    local reply=""
    # Fails CLOSED: a missing/unreadable /dev/tty makes `read` fail, which aborts here rather
    # than silently proceeding to push firmware at a unit nobody is watching.
    read -r -p "[ota-release] type 'revert node $NODE_ID' to proceed: " reply </dev/tty \
      || die "no interactive terminal for the confirmation prompt -- re-run with --yes if you are sure"
    [ "$reply" = "revert node $NODE_ID" ] || die "aborted (got '$reply')"
  fi

  say "applying newest repackaged revert image on disk: rac-stock-v$v"
  # Re-verify the payload behind this .ota BEFORE it goes near a device. --repackage checks its
  # own output, but --apply picks the newest pair on disk, which may have been produced by an
  # older revision of this script: built-images/ still holds the four pre-#75
  # rac-stock-v*-payload.bin that bricked the office unit, and a version accident is all it
  # takes for one of them to be "newest". Costs milliseconds.
  say "  re-verifying the payload (manifest sig + EVERY sub-image HMAC + bytesum)"
  python3 - "$bi" "$src_json" <<'PY'
import json,os,struct,hmac,hashlib,sys
bi,mf=sys.argv[1],sys.argv[2]
KEY=bytes.fromhex('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e5f')
m=json.load(open(mf))["modelVersion"]
pn=m.get("payloadName")
if not pn:
    print(f"  {os.path.basename(mf)} has no payloadName -- built by a pre-#75 script revision. REFUSING."); sys.exit(1)
p=os.path.join(bi,pn)
if not os.path.exists(p):
    print(f"  payload {pn} missing -- cannot verify what this .ota carries. REFUSING."); sys.exit(1)
d=open(p,'rb').read(); img,tr=d[:-4],d[-4:]
if hmac.new(KEY,img[0xE0:0x140],hashlib.sha256).digest()!=img[0:32]:
    print("  manifest signature MISMATCH -- REFUSING"); sys.exit(1)
if struct.pack('<I',sum(img)&0xffffffff)!=tr:
    print("  bytesum trailer MISMATCH -- REFUSING"); sys.exit(1)
# Full sub-image chain: END=H+0x60+u32le(img[H]); START=0 for the first, else H; next header
# = H + u32le(img[H+4]) (RELATIVE), 0xFFFFFFFF terminates. Trailing 0x87 pad after the last
# trailer is normal (0 or 0x20 bytes), so do not require END+0x20 == len.
H=0xE0; i=0
while True:
    if H+0x60>len(img): print("  chain UNWALKABLE -- REFUSING"); sys.exit(1)
    S=struct.unpack_from('<I',img,H)[0]; nxt=struct.unpack_from('<I',img,H+4)[0]
    END=H+0x60+S; START=0 if i==0 else H
    if END+0x20>len(img): print("  chain trailer past EOF -- REFUSING"); sys.exit(1)
    if hmac.new(KEY,img[START:END],hashlib.sha256).digest()!=img[END:END+0x20]:
        print(f"  sub-image {i} trailer @{END:#x} MISMATCH (issue #75 signature) -- REFUSING to stage a brick"); sys.exit(1)
    if nxt==0xffffffff: break
    if nxt==0 or i>15: print("  malformed chain -- REFUSING"); sys.exit(1)
    H+=nxt; i+=1
print(f"  {pn}: manifest sig OK, {i+1} sub-image trailers OK, bytesum OK, serial@0xF4={struct.unpack_from('<I',img,0xF4)[0]}")
PY
  # stage() is keyed to the cur_version rac-v* names, so this duplicates its scp + junk
  # prune + matter-server restart for the rac-stock-* pair instead of refactoring it.
  say "stage rac-stock-v$v on $PI_HOST:$PI_OTA_DIR + restart matter-server"
  scp -o BatchMode=yes -i "$PI_SSH_KEY" "$src_ota" "$src_json" \
    "$PI_HOST:$PI_OTA_DIR/" >/dev/null
  ssh -o BatchMode=yes -i "$PI_SSH_KEY" "$PI_HOST" \
    "rm -f $PI_OTA_DIR/chip_kvs_ota_provider_* $PI_OTA_DIR/ota_provider_*.log 2>/dev/null; \
     docker restart matter-server >/dev/null 2>&1"   # restart => reload manifests (loaded once at init)
  say "  staged + provider junk pruned + matter-server restarted (manifest cache reloaded)"
  say "apply rac-stock-v$v to node $NODE_ID (update_node with retries, then classify the outcome)"
  local rc=0
  "$OTAENV_PY" - "$MS_WS" "$NODE_ID" "$v" "$ip" "${BREAKGLASS_TOKEN:-}" "${BREAKGLASS_PORT:-0}" \
    "${SERIAL_BASE:-1100}" <<'PY' || rc=$?
import asyncio,json,re,socket,sys,aiohttp
URL,NODE,V=sys.argv[1],int(sys.argv[2]),int(sys.argv[3])
IP,TOKEN,PORT,BASE=sys.argv[4],sys.argv[5],int(sys.argv[6]),int(sys.argv[7])
# Exit codes: 0 = CONFIRMED on stock, 3 = CONFIRMED still on custom (revert did not take, unit
# healthy), 4 = AMBIGUOUS. Never report success from silence: a healthy stock unit and a
# bootloader-rejected module both vanish from this fabric, which is how two healthy units got
# called bricks on 2026-07-26.
async def call(ws,c,a,m,t=600):
    await ws.send_json({"message_id":m,"command":c,"args":a})
    while True:
        d=json.loads((await ws.receive(timeout=t)).data)
        if d.get("message_id")==m: return d
def slots():
    """`<token>:slots` over the break-glass listener, or None if the unit did not answer.
    An answer is decisive (only the custom firmware serves it); silence is not evidence."""
    if not IP or not TOKEN: return None
    d=b""
    try:
        with socket.create_connection((IP,PORT),timeout=8) as s:
            s.sendall(f"{TOKEN}:slots".encode()); s.settimeout(8)
            while b"\n" not in d and len(d)<512:
                b=s.recv(256)
                if not b: break
                d+=b
    except OSError:
        return None
    r=d.decode(errors="replace").strip()
    return r if r.startswith("ok:") else None
def slots_note(r):
    m=re.match(r"ok:\s*fw1_sn=(\d+)\s+fw2_sn=(\d+)\s+cur=(\d+)",r)
    if not m: return f"break-glass answered {r!r}"
    f1,f2,c=int(m.group(1)),int(m.group(2)),int(m.group(3))
    run=f1 if c==1 else f2
    return (f"break-glass answered: fw1_sn={f1} fw2_sn={f2} running=fw{c} "
            f"(serial {run} = {'CUSTOM' if run>=BASE else 'stock'})")
def not_reverted(why):
    print(f"[revert] VERDICT: NOT REVERTED -- {why}")
    print("[revert] the module is HEALTHY and still running the custom firmware; nothing is bricked.")
    print("[revert] retry 'revert --apply', or use 'revert --flip <ip>' if the other slot holds stock.")
    sys.exit(3)
async def read_ident():
    """Fresh (softwareVersion, vendorId) read, or (None,None) when the node is silent."""
    try:
        async with aiohttp.ClientSession() as s:
            async with s.ws_connect(URL,heartbeat=30) as ws:
                await ws.receive(timeout=8)
                r=await call(ws,"read_attribute",{"node_id":NODE,"attribute_path":"0/40/9"},"g",30)
                res=r.get("result"); v=res.get("0/40/9") if isinstance(res,dict) else res
                rv=await call(ws,"read_attribute",{"node_id":NODE,"attribute_path":"0/40/1"},"gv",30)
                rres=rv.get("result"); vend=rres.get("0/40/1") if isinstance(rres,dict) else rres
                return v,vend
    except Exception:
        return None,None
async def node_unavailable():
    try:
        async with aiohttp.ClientSession() as s:
            async with s.ws_connect(URL,heartbeat=30) as ws:
                await ws.receive(timeout=8)
                g=await call(ws,"get_node",{"node_id":NODE},"gn",15)
                n=g.get("result") if g else None
                return isinstance(n,dict) and n.get("available") is False
    except Exception:
        return False
async def main():
    for n in range(1,8):
        print(f"[revert] update_node attempt {n} -> v{V}",flush=True)
        try:
            async with aiohttp.ClientSession() as s:
                async with s.ws_connect(URL,heartbeat=30) as ws:
                    await ws.receive(timeout=10)
                    r=await call(ws,"update_node",{"node_id":NODE,"software_version":V},str(n),600)
                    if r.get("error_code") is None: print("[revert] provider reports finished"); break
                    print("[revert] declined:",r.get("error_code"),r.get("details",""))
        except Exception as e: print("[revert] exc",repr(e)[:120])
        await asyncio.sleep(15)
    # Classify. Stock reports softwareVersion 4 / vendor 5004 (0xFFF1 is the custom line), so a
    # SUSTAINED 4 is a real confirmation. Nothing else is: this loop only gathers evidence and
    # never concludes from silence. The decision happens after it.
    good=0; cust=0; silent=0
    for _ in range(30):
        v,vend=await read_ident()
        if v==4:
            good+=1; cust=0; silent=0
            print(f"[revert] unit reports stock softwareVersion 4 (vendor {vend}) ({good}/3)",flush=True)
            if good>=3:
                print("[revert] VERDICT: REVERTED -- unit is on stock firmware (softwareVersion 4 sustained)")
                return
        elif v is not None:
            good=0; silent=0; cust+=1
            print(f"[revert] unit still answers on softwareVersion {v} (vendor {vend}) ({cust} in a row) ...",flush=True)
        else:
            good=0; cust=0; silent+=1
            print(f"[revert] no answer from node {NODE} ({silent} in a row) -- expected on stock, "
                  "equally expected on a dead module. Not a verdict.",flush=True)
        await asyncio.sleep(12)
    # No sustained stock version. Use the one signal that discriminates without a clip.
    print("[revert] ~6 min without a sustained stock version; probing the break-glass listener",flush=True)
    r=slots()
    if r: not_reverted(slots_note(r)+" -- so the custom firmware is still running and the OTA never took")
    # Timed re-check: a unit whose OTA failed can take a while to rejoin the fabric, and that
    # comeback is a decisive negative. Give it one more window before declaring ambiguity.
    print("[revert] silent. Waiting 180 s for a late comeback, then re-probing",flush=True)
    await asyncio.sleep(180)
    v,vend=await read_ident()
    if v==4:
        print(f"[revert] VERDICT: REVERTED -- unit answered on stock softwareVersion 4 (vendor {vend}) after the wait")
        return
    if v is not None:
        not_reverted(f"node {NODE} came back on softwareVersion {v} (vendor {vend}) -- the OTA never took")
    r=slots()
    if r: not_reverted(slots_note(r)+" (after the wait) -- so the custom firmware is still running")
    unavail=await node_unavailable()
    print("[revert] VERDICT: AMBIGUOUS -- the unit is silent on every channel we own.")
    print(f"[revert]   matter-server reports node {NODE} available=False: {unavail}. That is expected")
    print("[revert]   for BOTH a healthy stock unit and a dead module, so it is NOT a verdict.")
    if not IP or not TOKEN:
        print("[revert]   no --ip (or no BREAKGLASS_TOKEN), so the one no-clip discriminator never ran.")
    else:
        print(f"[revert]   break-glass at {IP}:{PORT} did not answer: consistent with stock (which has")
        print("[revert]   no listener) AND with a bootloader rejection.")
    print("[revert] Stock also joins its OWN factory-provisioned network, so it may be invisible to")
    print("[revert] us while perfectly healthy: absence from our VLAN is NOT evidence either.")
    print("[revert] Do not record this as success and do not record it as a brick. Triage below.")
    sys.exit(4)
asyncio.run(main())
PY
  case "$rc" in
    0)
      say "revert applied: the unit runs STOCK firmware now (it will NOT report v$v -- expected)."
      say "next steps: the unit speaks ConnectLife again. To put it back on custom firmware,"
      say "re-convert from scratch: firmware/docs/12-ota-convert-stock-unit.md (firmware/scripts/ota_convert_stock.sh)."
      say "AFTER that re-conversion, run the break-glass ':wipekv' BEFORE trying to commission:"
      say "the Matter DCT (0x3E0000/0x3ED000) survives the stock round trip, so the stack sees a"
      say "fabric, takes the 'already commissioned' branch and NEVER advertises BLE (docs/12 step 7)."
      # Deliberately NOT writing .released-version: it tracks the CUSTOM line, and a later
      # custom OTA must still be strictly greater than the version that was rolled back.
      ;;
    3) die "revert did NOT take -- the unit is still running the custom firmware (healthy, not bricked); see the [revert] lines above" ;;
    4)
      revert_triage "${ip:-<unit-ip>}"
      printf '\033[1;31m[ota-release] AMBIGUOUS:\033[0m %s\n' \
        "revert outcome UNKNOWN -- silence is neither success nor a brick; run the triage checks above" >&2
      exit 4
      ;;
    *) die "revert --apply failed unexpectedly (exit $rc) -- see the [revert] lines above" ;;
  esac
}
revert() {
  case "${1:-}" in
    --backup)    shift; revert_backup "$@" ;;
    --flip)      shift; revert_flip "$@" ;;
    --repackage) shift; revert_repackage "$@" ;;
    --apply)     shift; revert_apply "$@" ;;
    --slots)     shift; revert_slots "$@" ;;
    *) die "usage: ota-release.sh revert {--backup <unit-ip> [out.bin]|--flip <unit-ip> [--force]|--slots <unit-ip>|--repackage <stock-dump.bin>|--apply [--ip <unit-ip>] [--yes]}" ;;
  esac
}

# ---- tag (issue #77) -------------------------------------------------------
tag_release() {  # create the path-prefixed semver tag amebaz2-vX.Y.Z locally (never pushed here)
  local semver t; semver="$(cur_semver)"; t="amebaz2-v$semver"
  git -C "$REPO" rev-parse -q --verify "refs/tags/$t" >/dev/null \
    && { say "tag $t already exists -- leaving it"; return; }
  git -C "$REPO" tag -s "$t" -m "AmebaZ2 firmware $semver (softwareVersion $(cur_version))"
  say "tagged $t (softwareVersion $(cur_version)) -- push with: git push origin $t"
}

# ---- top-level -------------------------------------------------------------
cmd="${1:-}"; shift || true
# ---- publish the DEPLOYED artifacts to the GitHub release (#89) -------------------------------
# The release workflow REBUILDS at tag time and attaches that output, and neither target is
# byte-reproducible (measured: AmebaZ2 rebuild 6763c8c5 vs deployed 184da838). So the CI asset is
# NOT the image any device booted. Harmless-looking until someone reaches for "the release" as a
# delta base or a recovery image, which is how the ESP32 1.0.3 base was lost (#82). This uploads
# the bytes a device actually booted, under the CANONICAL names, so the plain filename is always
# authoritative and the CI rebuild carries the -CI-REBUILD suffix instead.
int_to_semver() { echo "$(( $1 / 10000 )).$(( ($1 / 100) % 100 )).$(( $1 % 100 ))"; }
publish() {
  command -v gh >/dev/null || die "gh not on PATH -- needed to upload release assets"
  local v tag n=0 f
  # Only ever publish what THIS box confirmed booted: `flash` writes the marker after the device
  # sustained the new version across three fresh reads. Publishing an image no device ran would
  # recreate exactly the problem this fixes.
  v="$(released_version)"
  [ "$v" != 0 ] || die "no on-device version recorded -- run 'flash' first"
  tag="amebaz2-v$(int_to_semver "$v")"
  gh release view "$tag" >/dev/null 2>&1 || die "no release $tag -- push the tag first ('ota-release.sh tag')"
  # firmware_is-v<N>*.bin FIRST: on this path the raw firmware_is.bin is what the break-glass HTTP
  # OTA actually streams to the device, so the archived copy is the byte-exact deployed payload.
  # The clip/.ota are derived and only exist if `package` ran on this box for this build -- and
  # regenerating them would mean rebuilding, whose bytes differ (non-reproducible), so they are
  # published only when genuinely available rather than manufactured on demand.
  for f in "firmware_is-v$v.bin" "firmware_is-v$v-debug.bin" \
           "flash_rac-integrated-v$v.bin" "rac-v$v.ota" "rac-v$v.json" \
           "flash_rac-integrated-v$v-debug.bin" "rac-v$v-debug.ota" "rac-v$v-debug.json"; do
    [ -f "$REPO/firmware/built-images/$f" ] || continue
    gh release upload "$tag" "$REPO/firmware/built-images/$f" --clobber >/dev/null \
      && { say "  uploaded $f"; n=$((n+1)); }
  done
  (( n > 0 )) || die "no artifacts for v$v in built-images/ -- build + package first"
  say "published $n deployed artifact(s) to $tag (on-device version $v)"
}

case "$cmd" in
  lint)    lint ;;
  build)   build "$@" ;;   # forward EVERY flag: "${1:-}" silently dropped --debug after --bump
  package) package ;;
  stage)   stage ;;
  flash)   flash ;;
  tag)     tag_release ;;
  verint)  semver_to_int "${1:-$(cur_semver)}" ;;   # semver -> Matter softwareVersion int (CI uses this; no SDK/env)
  release)
    BUMP=""; FLASH=0; TAG=0; DEBUGF=""
    for a in "$@"; do
      case "$a" in
        --bump|--bump-patch|--bump-minor|--bump-major) BUMP="$a" ;;
        --flash) FLASH=1 ;;
        --tag)   TAG=1 ;;
        --debug) DEBUGF="--debug" ;;
        # Anything else is almost certainly a typo or a flag that only `build` understands.
        # Silently ignoring it is how a build ends up the wrong flavour: `release --debug`
        # used to accept the flag, drop it, and ship a console-less image that looks fine
        # until the console does not answer.
        -*) die "unknown flag for release: $a" ;;
      esac
    done
    build $BUMP $DEBUGF; package; stage
    [ "$TAG" = 1 ] && tag_release || true
    [ "$FLASH" = 1 ] && flash || say "staged, not flashed. run: ota-release.sh flash"
    ;;
  publish) publish ;;
  revert)  revert "$@" ;;
  *) die "usage: ota-release.sh {lint|build [--bump[-minor|-major]] [--debug]|package|stage|flash|tag|publish|verint [semver]|release [--bump[-minor|-major]] [--tag] [--flash] [--debug]|revert {--backup <unit-ip> [out.bin]|--flip <unit-ip> [--force]|--slots <unit-ip>|--repackage <stock-dump.bin>|--apply [--ip <unit-ip>] [--yes]}}" ;;
esac
