#!/usr/bin/env bash
#
# esp32-release.sh -- build + Matter-OTA procedure for the ESP32 (esp-matter) firmware, the
# counterpart to ota-release.sh for the AmebaZ2 path. It gives the ESP32 target the same
# discipline ota-release.sh gives AmebaZ2, and mechanises the two things that have actually
# gone wrong here:
#   * issue #82 -- ESP-IDF builds are NOT byte-reproducible and `idf.py build` overwrites build/,
#     so the exact DEPLOYED image (the delta base) is easily lost. `build` REFUSES to run unless
#     the currently-released base is already archived in built-images/, then archives the fresh
#     image itself. (Losing the 1.0.3 base once stranded the node on USB-only flashing.)
#   * delta-only OTA -- CONFIG_ENABLE_DELTA_OTA=y makes the device REJECT a full image, so `package`
#     builds a delta patch against the archived base and wraps THAT (not the .bin) as the .ota.
#
# The version is DERIVED from CMakeLists.txt PROJECT_VER (MAJOR*10000+MINOR*100+PATCH), the same
# unified scheme as AmebaZ2 (issue #77); esp32-lint.sh enforces PROJECT_VER<->sdkconfig sync.
#
# Usage:
#   esp32-release.sh build                  # enforce #82 base-archive, idf.py build, archive new image
#   esp32-release.sh package [--full]       # delta patch vs archived base -> wrapped .ota + manifest
#   esp32-release.sh stage                  # scp .ota+manifest to the Pi, restart matter-server
#   esp32-release.sh flash                  # update_node with retries, verify the reported version
#   esp32-release.sh release [--flash]      # build + package + stage (+ flash)
#   esp32-release.sh verint                 # print the derived softwareVersion int (no env/IDF needed)
#
# IDF/esp-matter env (idf.py, the IDF python, the delta + ota_image tools) must be on the shell
# already: `. $IDF_PATH/export.sh && . $ESP_MATTER_PATH/export.sh`. Paths/hosts come from
# ota-release.env (gitignored; copy .env.example). CI builds this on the self-hosted sdk-builder.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
ENVF="$HERE/ota-release.env"
ESP="$REPO/firmware/esp32-matter"
IMG="$REPO/firmware/built-images"
CMAKE="$ESP/CMakeLists.txt"
NEW_BIN="$ESP/build/hisense_ac_matter.bin"
RELEASED_MARK="$IMG/.released-version-esp32"   # softwareVersion INT last CONFIRMED booted on the ESP32

say() { printf '\033[1;36m[esp32-release]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[esp32-release] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

# ---- version (derived from PROJECT_VER; no IDF/env needed, so CI + hooks can call verint) ----
cur_semver() {
  local v; v=$(sed -n 's/^set(PROJECT_VER "\(.*\)").*/\1/p' "$CMAKE")
  [ -n "$v" ] || die "could not parse PROJECT_VER from $CMAKE"
  echo "$v"
}
semver_to_int() {
  local s="${1//[[:space:]]/}"
  [[ "$s" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || die "PROJECT_VER '$s' is not semver MAJOR.MINOR.PATCH"
  local M="${BASH_REMATCH[1]}" m="${BASH_REMATCH[2]}" p="${BASH_REMATCH[3]}"
  (( m < 100 && p < 100 )) || die "minor/patch must be < 100 for the int mapping: '$s'"
  echo $(( M*10000 + m*100 + p ))
}
cur_int() { semver_to_int "$(cur_semver)"; }
released_int() { [ -f "$RELEASED_MARK" ] && cat "$RELEASED_MARK" || echo 0; }

# ---- IDF toolchain guard -------------------------------------------------------------------
# dependencies.lock records the IDF that produced the last committed build. Sourcing a different
# IDF export.sh silently builds against another toolchain: it still boots, but the whole binary
# shifts, which (a) blows up the delta-OTA patch (a 45 KB patch became 854 KB) and (b) rewrites
# the lock as a side effect, so the drift only shows up in `git status` after the fact. This bit
# us on 1.0.9 (built on 5.3.1 against a 5.5.4 lock) and cost a version + an extra OTA cycle.
# Checked BEFORE the build, because the build itself rewrites the lock.
lock_idf_version() {   # the `version:` under the top-level `idf:` block whose source type is idf
  awk '/^  idf:$/{f=1;next} f&&/^    version:/{gsub(/[ \t]*version:[ \t]*/,"");gsub(/[\r"'"'"']/,"");print;exit}' \
    "$ESP/dependencies.lock" 2>/dev/null
}
live_idf_version() {   # "ESP-IDF v5.5.4" / "ESP-IDF v5.5.4-dirty" -> 5.5.4
  idf.py --version 2>/dev/null | sed -n 's/.*ESP-IDF v\([0-9][0-9.]*\).*/\1/p' | tail -1
}
assert_idf_matches_lock() {
  local want live; want="$(lock_idf_version)"; live="$(live_idf_version)"
  if [ -z "$want" ]; then say "no IDF version in dependencies.lock -- skipping toolchain check"; return 0; fi
  if [ -z "$live" ]; then die "could not determine the live IDF version from 'idf.py --version'"; fi
  if [ "$want" != "$live" ]; then
    die "IDF MISMATCH: dependencies.lock expects v$want but the sourced IDF is v$live (IDF_PATH=${IDF_PATH:-unset}).
     Source the matching export.sh and rebuild, e.g.  source ~/esp/esp-idf-v$want/export.sh
     Building on the wrong IDF still boots but shifts the whole binary: the delta-OTA patch
     balloons and dependencies.lock is silently rewritten. If the bump is INTENTIONAL, re-run with
     ESP32_ALLOW_IDF_MISMATCH=1 and commit the resulting dependencies.lock change deliberately."
  fi
  say "IDF v$live matches dependencies.lock"
}
int_to_semver_bin() {  # archived full-image path for a given INT, by scanning built-images
  local want="$1" f v
  for f in "$IMG"/esp32-hisense_ac_matter-v*.bin; do
    [ -e "$f" ] || continue
    v=$(sed -n 's#.*-v\([0-9]*\.[0-9]*\.[0-9]*\)\(-DELTA-BASE\)\?\.bin$#\1#p' <<< "$f")
    [ -n "$v" ] && [ "$(semver_to_int "$v")" = "$want" ] && { echo "$f"; return; }
  done
  return 1
}

load_env() {
  [ -f "$ENVF" ] || die "missing $ENVF -- copy ota-release.env.example and fill it in"
  # shellcheck disable=SC1090
  . "$ENVF"
  : "${VID:?}" "${PID:?}"
}

# ---- build (issue #82: archive-before-overwrite, then archive the fresh image) --------------
build() {
  command -v idf.py >/dev/null || die "idf.py not on PATH -- source the IDF + esp-matter env first"
  if [ "${ESP32_ALLOW_IDF_MISMATCH:-0}" = "1" ]; then
    say "WARNING: ESP32_ALLOW_IDF_MISMATCH=1 -- toolchain check skipped (lock v$(lock_idf_version) vs live v$(live_idf_version))"
  else
    assert_idf_matches_lock
  fi
  local semver int rel; semver="$(cur_semver)"; int="$(cur_int)"; rel="$(released_int)"
  bash "$HERE/esp32-lint.sh"   # PROJECT_VER<->sdkconfig sync + semver bounds (fails hard on drift)
  (( int > rel )) || die "PROJECT_VER $semver (int $int) is not > last released ($rel) -- bump PROJECT_VER + sdkconfig (#77)"

  # #82 gate: the currently-DEPLOYED image is the delta base for the NEXT release. It must already
  # be archived before this build overwrites build/ (IDF builds aren't byte-reproducible).
  if (( rel > 0 )); then
    int_to_semver_bin "$rel" >/dev/null \
      || die "released image (int $rel) not archived in built-images/ -- recover the exact deployed .bin FIRST (#82); refusing to overwrite build/"
    say "#82 ok: released base (int $rel) is archived"
  else
    say "#82: no prior release recorded -- first build, nothing to preserve"
  fi

  # Flavour. node 28 runs the DEBUG flavour on purpose (it is the dev unit), and the :2323 diag
  # console + `tx` bench probe are gated on CONFIG_HISENSE_DEBUG_BUILD, which lives ONLY in
  # sdkconfig.debug. Building without that overlay silently ships an image with no console -- and
  # on a node whose Matter link is flaky, that console is the only way in. Default to debug here
  # for exactly that reason. Opt out with ESP32_FLAVOUR=release (an env var, NOT a --release
  # flag -- this script does not parse one).
  # Recovery credentials. The Identify=88 OTA fetch target and the :2324 break-glass listener are
  # both baked at BUILD time and are the only two remote ways back into this node. Building without
  # them silently ships an image whose listener never opens and whose OTA URL is a non-resolvable
  # placeholder: the device looks healthy and is quietly USB-only.
  #
  # Not hypothetical -- every esp32 build on 2026-07-20 used bare `idf.py build`, so node 35 ended
  # up with :2324 closed and a placeholder URL and could not be updated over the air at all.
  #
  # NAME NORMALISATION MATTERS HERE. ota-release.env defines BREAKGLASS_TOKEN / BREAKGLASS_PORT
  # (what ota-release.sh reads for the ameba half), but esp32-matter/CMakeLists.txt consumes
  # HISENSE_BREAKGLASS_TOKEN / HISENSE_BREAKGLASS_PORT. Checking one name and exporting neither is
  # a guard that PASSES while still building a listener-less image -- the very failure it exists to
  # catch. So: accept either spelling, then export the HISENSE_* names the build actually reads.
  : "${HISENSE_BREAKGLASS_TOKEN:=${BREAKGLASS_TOKEN:-}}"
  : "${HISENSE_BREAKGLASS_PORT:=${BREAKGLASS_PORT:-}}"
  export HISENSE_OTA_URL HISENSE_BREAKGLASS_TOKEN HISENSE_BREAKGLASS_PORT

  if [ "${ESP32_ALLOW_NO_RECOVERY:-0}" != "1" ]; then
    [ -n "${HISENSE_OTA_URL:-}" ] \
      || die "HISENSE_OTA_URL is unset -- the Identify=88 OTA fetch would bake a placeholder URL and
     the image would be USB-only. Set it (ota-release.env or the environment), or pass
     ESP32_ALLOW_NO_RECOVERY=1 if you really want a bench image with no remote recovery."
    [ -n "${HISENSE_BREAKGLASS_TOKEN:-}" ] \
      || die "no break-glass token (set BREAKGLASS_TOKEN or HISENSE_BREAKGLASS_TOKEN) -- the :2324
     listener fails closed and never opens, so the image would be USB-only. Set it, or pass
     ESP32_ALLOW_NO_RECOVERY=1."
    say "recovery credentials present (OTA URL + break-glass token exported to the build)"
  else
    say "WARNING: ESP32_ALLOW_NO_RECOVERY=1 -- image will have NO remote recovery path (USB only)"
  fi

  local sdkdef="sdkconfig.defaults"
  if [ "${ESP32_FLAVOUR:-debug}" = "debug" ]; then
    sdkdef="sdkconfig.defaults;sdkconfig.debug"
    say "flavour: DEBUG (:2323 console + tx probe)"
  else
    say "flavour: RELEASE (no console) -- node 28 normally wants debug"
  fi

  say "idf.py build ($semver, int $int)"
  ( cd "$ESP" && idf.py -DSDKCONFIG_DEFAULTS="$sdkdef" set-target esp32 \
              && idf.py -DSDKCONFIG_DEFAULTS="$sdkdef" build )
  [ -f "$NEW_BIN" ] || die "build produced no $NEW_BIN"
  # Fail loudly rather than shipping a consoleless image by accident.
  if [ "${ESP32_FLAVOUR:-debug}" = "debug" ]; then
    grep -q '^CONFIG_HISENSE_DEBUG_BUILD=y' "$ESP/sdkconfig" \
      || die "debug flavour requested but CONFIG_HISENSE_DEBUG_BUILD is not set in the generated sdkconfig -- the :2323 console would be MISSING from this image"
    say "verified: CONFIG_HISENSE_DEBUG_BUILD=y (console present)"
  fi

  local archive="$IMG/esp32-hisense_ac_matter-v$semver.bin"
  mkdir -p "$IMG"; cp "$NEW_BIN" "$archive"
  say "archived fresh image -> $archive"
}

# ---- package (delta patch vs archived base -> wrapped .ota + matter-server manifest) --------
package() {
  load_env
  local full=0; [ "${1:-}" = "--full" ] && full=1
  local semver int rel; semver="$(cur_semver)"; int="$(cur_int)"; rel="$(released_int)"
  # The ESP32 commissions with esp-matter's TEST PID 0x8000, DISTINCT from the AmebaZ2's 0x8001.
  # matter-server only serves an OTA whose manifest pid matches the device's -- so the manifest
  # MUST carry the ESP32's pid, not the shared $PID. (Confirmed on-device: node 28 reports 0x8000.)
  local pid="${ESP32_PID:-0x8000}"
  [ -f "$NEW_BIN" ] || die "no $NEW_BIN -- run build first"
  : "${OTA_IMAGE_TOOL:?set OTA_IMAGE_TOOL to the connectedhomeip src/app/ota_image_tool.py path}"
  local ota="$IMG/esp32-v$int.ota" payload
  local otaurl="file:///esp32-v$int.ota"
  [ -n "${OTA_RELEASE_BASE:-}" ] && otaurl="${OTA_RELEASE_BASE%/}/esp32-v$semver/esp32-v$int.ota"

  if (( full == 1 || rel == 0 )); then
    (( full == 1 )) && say "packaging FULL image (--full)" || say "packaging FULL image (no prior release to delta against)"
    say "NOTE: a delta-OTA-enabled device REJECTS a full image -- only use --full for the first flash / a base recovery"
    payload="$NEW_BIN"
  else
    : "${DELTA_PATCH_GEN:?set DELTA_PATCH_GEN to esp_delta_ota_patch_gen.py}" "${IDF_PYTHON:?set IDF_PYTHON to the IDF python env (has detools+esptool)}"
    local base; base="$(int_to_semver_bin "$rel")" || die "delta base for int $rel not in built-images/ (#82)"
    payload="$IMG/esp32-v$int.patch"
    say "delta patch vs base $(basename "$base") -> $(basename "$payload")"
    "$IDF_PYTHON" "$DELTA_PATCH_GEN" create_patch --chip esp32 \
      --base_binary "$base" --new_binary "$NEW_BIN" --patch_file_name "$payload"
  fi

  # minApplicableSoftwareVersion=0 (NOT 1 like AmebaZ2): this ESP32 firmware leaves the
  # softwareVersion INT unwired (reports 0), so an OTA with min=1 is NOT applicable to it and
  # matter-server never offers it. min=0 covers the device's reported 0. (Confirmed working recipe.)
  say "wrap $(basename "$payload") as $(basename "$ota") (vn=$int vs=$semver vid=$VID pid=$pid)"
  python3 "$OTA_IMAGE_TOOL" create -v "$VID" -p "$pid" -vn "$int" -vs "$semver" \
    -da sha256 -mi 0 -ma "$((int-1))" "$payload" "$ota" >/dev/null
  python3 - "$ota" "$int" "$semver" "$otaurl" "$VID" "$pid" > "$IMG/esp32-v$int.json" <<'PY'
import sys,hashlib,base64,json
ota,v,semver,otaurl,vid,pid=sys.argv[1],int(sys.argv[2]),sys.argv[3],sys.argv[4],int(sys.argv[5],0),int(sys.argv[6],0)
d=open(ota,'rb').read()
print(json.dumps({"modelVersion":{"vid":vid,"pid":pid,"softwareVersion":v,"softwareVersionString":semver,"cdVersionNumber":1,"firmwareInformation":"","softwareVersionValid":True,"otaUrl":otaurl,"otaFileSize":len(d),"otaChecksum":base64.b64encode(hashlib.sha256(d).digest()).decode(),"otaChecksumType":1,"minApplicableSoftwareVersion":0,"maxApplicableSoftwareVersion":v-1,"releaseNotesUrl":""}}))
PY
  say "  ota:  $ota"
  say "  json: $IMG/esp32-v$int.json  (otaUrl=$otaurl)"
}

# ---- stage on the Pi (mirror of ota-release.sh stage) --------------------------------------
stage() {
  load_env
  : "${PI_HOST:?}" "${PI_OTA_DIR:?}" "${PI_SSH_KEY:?}"
  local int; int="$(cur_int)"
  say "stage esp32-v$int on $PI_HOST:$PI_OTA_DIR + restart matter-server"
  scp -o BatchMode=yes -i "$PI_SSH_KEY" \
    "$IMG/esp32-v$int.ota" "$IMG/esp32-v$int.json" "$PI_HOST:$PI_OTA_DIR/" >/dev/null
  ssh -o BatchMode=yes -i "$PI_SSH_KEY" "$PI_HOST" \
    "rm -f $PI_OTA_DIR/chip_kvs_ota_provider_* $PI_OTA_DIR/ota_provider_*.log 2>/dev/null; \
     docker restart matter-server >/dev/null 2>&1"
  say "  staged + provider junk pruned + matter-server restarted"
}

# ---- flash (update_node retries + rollback detection; mirror of ota-release.sh flash) -------
flash() {
  load_env
  : "${OTAENV_PY:?}" "${MS_WS:?}" "${ESP32_NODE_ID:?set ESP32_NODE_ID in ota-release.env (the ESP32 node, e.g. 28)}"
  local int semver; int="$(cur_int)"; semver="$(cur_semver)"
  say "flash esp32-v$int ($semver) to node $ESP32_NODE_ID (retries; verify the reported version changed)"
  # update_node selects the OTA by the INT (V); but VERIFY by the STRING (0/40/10), NOT the int
  # (0/40/9): this ESP32 firmware leaves the softwareVersion INT unwired (reads 0), so checking the
  # int would never confirm success. The STRING is wired and moves 1.0.7 -> 1.0.8.
  if "$OTAENV_PY" - "$MS_WS" "$ESP32_NODE_ID" "$int" "$semver" <<'PY'
import asyncio,json,sys,aiohttp
URL,NODE,V,VS=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),sys.argv[4]
async def call(ws,c,a,m,t=600):
    await ws.send_json({"message_id":m,"command":c,"args":a})
    while True:
        d=json.loads((await ws.receive(timeout=t)).data)
        if d.get("message_id")==m: return d
async def swstr(ws):  # SoftwareVersionString (0/40/10) -- wired on this ESP32; the int (0/40/9) is not
    r=await call(ws,"read_attribute",{"node_id":NODE,"attribute_path":"0/40/10"},"g",30)
    res=r.get("result")
    return res.get("0/40/10") if isinstance(res,dict) else res
async def main():
    for n in range(1,8):
        print(f"[flash] update_node attempt {n} -> v{V} ({VS})",flush=True)
        try:
            async with aiohttp.ClientSession() as s:
                async with s.ws_connect(URL,heartbeat=30) as ws:
                    await ws.receive(timeout=10)
                    r=await call(ws,"update_node",{"node_id":NODE,"software_version":V},str(n),600)
                    if r.get("error_code") is None: print("[flash] provider reports finished"); break
                    print("[flash] declined:",r.get("error_code"),r.get("details",""))
        except Exception as e: print("[flash] exc",repr(e)[:120])
        await asyncio.sleep(15)
    good=0
    for _ in range(30):
        try:
            async with aiohttp.ClientSession() as s:
                async with s.ws_connect(URL,heartbeat=30) as ws:
                    await ws.receive(timeout=8)
                    v=await swstr(ws)
                    if v==VS:
                        good+=1; print(f"[flash] device reports {VS} ({good}/3)")
                        if good>=3:
                            try: await call(ws,"interview_node",{"node_id":NODE},"iv",120); print("[flash] re-interviewed node for HA")
                            except Exception as e: print("[flash] interview warn",repr(e)[:60])
                            print(f"[flash] SUCCESS: device booted {VS}"); return
                    else:
                        good=0; print(f"[flash] device reports {v} (want {VS}) ...")
        except Exception: pass
        await asyncio.sleep(12)
    print(f"[flash] FAILED: device never sustained {VS} -- delta base mismatch (safe), full image rejected by delta target, or boot crash")
    sys.exit(2)
asyncio.run(main())
PY
  then echo "$int" > "$RELEASED_MARK"; say "recorded on-device version $int"
  else die "flash did not confirm v$int on-device"; fi
}

# ---- tag -----------------------------------------------------------------------------------
tag_release() {
  local semver t; semver="$(cur_semver)"; t="esp32-v$semver"
  git -C "$REPO" rev-parse -q --verify "refs/tags/$t" >/dev/null \
    && { say "tag $t already exists -- leaving it"; return; }
  git -C "$REPO" tag -s "$t" -m "ESP32 firmware $semver (softwareVersion $(cur_int))"
  say "tagged $t -- push with: git push origin $t"
}

# ---- top-level -----------------------------------------------------------------------------
cmd="${1:-}"; shift || true
# ---- publish the DEPLOYED artifacts to the GitHub release (#89) -------------------------------
# Most acute on this target: delta OTA embeds the BASE image's hash and the device verifies it
# against its running partition, so a patch built against a CI REBUILD is rejected. ESP-IDF builds
# are not byte-reproducible (measured: rebuild c73d1de8 vs deployed 3d003d66), so the release asset
# has to be the archived deployed .bin, not whatever CI produced. Losing that binary once already
# stranded a node on USB-only flashing (#82).
publish() {
  command -v gh >/dev/null || die "gh not on PATH -- needed to upload release assets"
  local rel semver base n=0
  rel="$(released_int)"
  [ "$rel" != 0 ] || die "no on-device version recorded -- run 'flash' first"
  semver="$(( rel / 10000 )).$(( (rel / 100) % 100 )).$(( rel % 100 ))"
  gh release view "esp32-v$semver" >/dev/null 2>&1 || die "no release esp32-v$semver -- push the tag first"
  base="$(int_to_semver_bin "$rel")" \
    || die "deployed image (int $rel) not archived in built-images/ (#82) -- nothing trustworthy to publish"
  gh release upload "esp32-v$semver" "$base" --clobber >/dev/null && { say "  uploaded $(basename "$base")"; n=1; }
  local f
  for f in "$IMG/esp32-v$rel.ota" "$IMG/esp32-v$rel.json"; do
    [ -f "$f" ] || continue
    gh release upload "esp32-v$semver" "$f" --clobber >/dev/null && { say "  uploaded $(basename "$f")"; n=$((n+1)); }
  done
  say "published $n deployed artifact(s) to esp32-v$semver -- THIS is the valid delta base for the next release"
}

case "$cmd" in
  build)   build ;;
  package) package "${1:-}" ;;
  stage)   stage ;;
  flash)   flash ;;
  tag)     tag_release ;;
  publish) publish ;;
  verint)  cur_int ;;
  release)
    FLASH=0; for a in "$@"; do [ "$a" = --flash ] && FLASH=1; done
    build; package; stage
    [ "$FLASH" = 1 ] && flash || say "staged, not flashed. run: esp32-release.sh flash"
    ;;
  *) die "usage: esp32-release.sh {build|package [--full]|stage|flash|tag|publish|verint|release [--flash]}" ;;
esac
