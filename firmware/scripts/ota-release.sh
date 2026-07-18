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
#
# Environment-specific paths/hosts come from ota-release.env (gitignored; copy .env.example).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
ENVF="$HERE/ota-release.env"
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
  # #78 break-glass OTA target. Generated (never committed): the repo is public, so the real
  # server address lives in ota-release.env and is injected here. Without OTA_HTTP_HOST the
  # generated header is omitted and matter_drivers.cpp keeps its inert placeholder, which is why
  # the shipped 1.2.9 pointed at 192.168.1.50 and the escape hatch could not have worked.
  if [ -n "${OTA_HTTP_HOST:-}" ]; then
    { echo "// GENERATED by ota-release.sh from ota-release.env -- do not commit."
      echo "#define HISENSE_OTA_HOST     \"${OTA_HTTP_HOST}\""
      echo "#define HISENSE_OTA_PORT     ${OTA_HTTP_PORT:-8070}"
      echo "#define HISENSE_OTA_RESOURCE \"${OTA_HTTP_RESOURCE:-/rac-ota.bin}\""
    } > "$EXAMPLE_DIR/hisense_ota_config.h"
    say "  break-glass OTA target: ${OTA_HTTP_HOST}:${OTA_HTTP_PORT:-8070}${OTA_HTTP_RESOURCE:-/rac-ota.bin}"
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
build() {
  load_env
  case "${1:-}" in
    --bump|--bump-patch) bump_version patch ;;
    --bump-minor)        bump_version minor ;;
    --bump-major)        bump_version major ;;
  esac
  set_header_version   # SDK header (int + string) derives from the git-tracked semver in version.txt
  lint_zap
  sync_mirror
  apply_ota_hardening   # #76: MRP tuning into the Ameba CHIP platform config (idempotent)
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
    # toolchain mtime/atime noise; sloppiness lets TUs with __DATE__/__TIME__ + PCH cache instead
    # of being skipped. Scoped via env -- no change to the user's global ccache.conf.
    export CCACHE_BASEDIR="$HOME"
    export CCACHE_COMPILERCHECK=content
    export CCACHE_SLOPPINESS="time_macros,include_file_mtime,include_file_ctime,pch_defines,locale,system_headers"
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
    make is_matter -j"$JOBS" "${CCMK[@]}" 2>&1 | tee /tmp/ota-ismatter.log | tail -2
  )
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
  local fw="$GCC_RELEASE/application_is/Debug/bin/firmware_is.bin"
  local flash="$GCC_RELEASE/application_is/Debug/bin/flash_is.bin"
  local clip="$REPO/firmware/built-images/flash_rac-integrated-v$v.bin"
  local ota="$REPO/firmware/built-images/rac-v$v.ota"
  [ -f "$fw" ] || die "no firmware_is.bin -- build first"
  # otaUrl (#79): default to a LOCAL file:// (staged into --ota-provider-dir). If OTA_RELEASE_BASE
  # is set, point at the GitHub release asset instead -- python-matter-server's OTA provider
  # downloads an http(s):// otaUrl (checksum-verified) then re-serves it over BDX, so the big .ota
  # can live in the release and only the small .json need be staged on the Pi.
  local otaurl="file:///rac-v$v.ota"
  [ -n "${OTA_RELEASE_BASE:-}" ] && otaurl="${OTA_RELEASE_BASE%/}/amebaz2-v$semver/rac-v$v.ota"
  say "package v$semver (softwareVersion $v): clip image + .ota + manifest"
  python3 -c "d=open('$flash','rb').read(); open('$clip','wb').write(d + b'\xff'*(4194304-len(d)))"
  python3 "$OTA_TOOL" create -v "$VID" -p "$PID" -vn "$v" -vs "$semver" -da sha256 -mi 1 -ma "$((v-1))" "$fw" "$ota" >/dev/null
  python3 - "$ota" "$v" "$semver" "$otaurl" > "$REPO/firmware/built-images/rac-v$v.json" <<'PY'
import sys,hashlib,base64,json
ota,v,semver,otaurl=sys.argv[1],int(sys.argv[2]),sys.argv[3],sys.argv[4]; d=open(ota,'rb').read()
print(json.dumps({"modelVersion":{"vid":0xFFF1,"pid":0x8001,"softwareVersion":v,"softwareVersionString":semver,"cdVersionNumber":1,"firmwareInformation":"","softwareVersionValid":True,"otaUrl":otaurl,"otaFileSize":len(d),"otaChecksum":base64.b64encode(hashlib.sha256(d).digest()).decode(),"otaChecksumType":1,"minApplicableSoftwareVersion":1,"maxApplicableSoftwareVersion":v-1,"releaseNotesUrl":""}}))
PY
  say "  clip:     $clip"
  say "  ota:      $ota  (+ .json manifest, otaUrl=$otaurl)"
}

# ---- stage on the Pi -------------------------------------------------------
stage() {
  load_env
  : "${PI_HOST:?}" "${PI_OTA_DIR:?}" "${PI_SSH_KEY:?}"
  local v; v="$(cur_version)"
  say "stage v$v on $PI_HOST:$PI_OTA_DIR + restart matter-server"
  scp -o BatchMode=yes -i "$PI_SSH_KEY" \
    "$REPO/firmware/built-images/rac-v$v.ota" "$REPO/firmware/built-images/rac-v$v.json" \
    "$PI_HOST:$PI_OTA_DIR/" >/dev/null
  # cache hygiene: prune the ephemeral OTA-provider junk that piles up per attempt
  # (KVS + per-run logs). Leave .ota/.json manifests (needed for rollback images).
  ssh -o BatchMode=yes -i "$PI_SSH_KEY" "$PI_HOST" \
    "rm -f $PI_OTA_DIR/chip_kvs_ota_provider_* $PI_OTA_DIR/ota_provider_*.log 2>/dev/null; \
     docker restart matter-server >/dev/null 2>&1"   # restart => reload manifests (loaded once at init)
  say "  staged + provider junk pruned + matter-server restarted (manifest cache reloaded)"
}

# ---- flash (update_node with retries + rollback detection) -----------------
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
                            try: await call(ws,"interview_node",{"node_id":NODE},"iv",120); print("[flash] re-interviewed node for HA (docs/10 §9)")
                            except Exception as e: print("[flash] interview warn",repr(e)[:60])
                            print(f"[flash] SUCCESS: device booted v{V}"); return
                    else:
                        good=0; print(f"[flash] device reports v{v} (want {V}) ...")
        except Exception: pass
        await asyncio.sleep(12)
    print(f"[flash] FAILED: device never sustained v{V} -- OTA serial not bumped (rollback) or boot crash (docs/10 §7,§11)")
    sys.exit(2)
asyncio.run(main())
PY
  then echo "$v" > "$RELEASED_MARK"; say "recorded on-device version $v"
  else die "flash did not confirm v$v on-device -- boot crash + A/B rollback? (docs/10 §7)"; fi
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
case "$cmd" in
  lint)    lint ;;
  build)   build "${1:-}" ;;
  package) package ;;
  stage)   stage ;;
  flash)   flash ;;
  tag)     tag_release ;;
  verint)  semver_to_int "${1:-$(cur_semver)}" ;;   # semver -> Matter softwareVersion int (CI uses this; no SDK/env)
  release)
    BUMP=""; FLASH=0; TAG=0
    for a in "$@"; do
      case "$a" in
        --bump|--bump-patch|--bump-minor|--bump-major) BUMP="$a" ;;
        --flash) FLASH=1 ;;
        --tag)   TAG=1 ;;
      esac
    done
    build $BUMP; package; stage
    [ "$TAG" = 1 ] && tag_release || true
    [ "$FLASH" = 1 ] && flash || say "staged, not flashed. run: ota-release.sh flash"
    ;;
  *) die "usage: ota-release.sh {lint|build [--bump[-minor|-major]]|package|stage|flash|tag|verint [semver]|release [--bump[-minor|-major]] [--tag] [--flash]}" ;;
esac
