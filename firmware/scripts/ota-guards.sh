# shellcheck shell=bash
# ota-guards.sh -- pre-flight + staging guards SOURCED by ota-release.sh and esp32-release.sh,
# so both targets refuse the same unsafe OTA the same way (the sync-files.sh pattern: define
# once, two consumers). Decisions live in ota_guards.py (host-tested); this file only runs them.
# Callers provide say/die, load_env has run, and HERE points at firmware/scripts.
#
# Overrides (each one names the risk it accepts):
#   OTA_ALLOW_WEAK_LINK=1   flash even when RSSI/read latency fail the link check
#   OTA_KEEP_MANIFESTS=1    stage without archiving the other manifests for this product

GUARDS_PY="$HERE/ota_guards.py"
: "${OTA_MIN_RSSI:=-70}"

# The flash helper venv and (ESP32 delta) detools. A missing venv used to surface as
# "rollback/boot crash", pointing at the device instead of this box.
guard_tools() {  # $1 = "delta" to also require detools in IDF_PYTHON
  : "${OTAENV_PY:?set OTAENV_PY to the venv python that has aiohttp}"
  "$OTAENV_PY" -c 'import aiohttp' 2>/dev/null \
    || die "OTAENV_PY ($OTAENV_PY) cannot import aiohttp -- python3 -m venv <dir> && <dir>/bin/pip install aiohttp"
  if [ "${1:-}" = delta ]; then
    : "${IDF_PYTHON:?set IDF_PYTHON to the IDF python env}"
    "$IDF_PYTHON" -c 'import detools' 2>/dev/null \
      || die "IDF_PYTHON ($IDF_PYTHON) cannot import detools -- $IDF_PYTHON -m pip install detools"
  fi
}

# Built image flavour vs the requested one, read from the bytes (never trust the file name).
guard_flavour() {  # $1 image  $2 console marker string  $3 wanted flavour
  local out
  out="$(python3 "$GUARDS_PY" check-flavour "$1" "$2" "$3")" || die "$out ($1)"
  say "  $out"
}

# Warn (not refuse) when nothing that compiles into the image changed since the last tag:
# version-only releases are legitimate, but they should be deliberate.
guard_functional_delta() {  # $1 tag glob  $2... paths that compile into the image
  local glob="$1" tag n; shift
  tag="$(git -C "$REPO" describe --tags --abbrev=0 --match "$glob" HEAD^ 2>/dev/null || true)"
  [ -n "$tag" ] || return 0
  n="$(git -C "$REPO" log --oneline "$tag"..HEAD -- "$@" | wc -l)"
  if [ "$n" -eq 0 ]; then
    say "  NOTE: no commits under $* since $tag -- this release is version-only"
  else
    say "  $n commit(s) touch the image since $tag"
  fi
}

# Every staged file must be newer than the image it was derived from. Catches a half-failed
# package leaving an older build's .ota/.json behind.
guard_fresh() {  # $1 source image  $2... outputs
  local out
  out="$(python3 "$GUARDS_PY" stale "$@")" \
    || die "refusing to stage stale outputs, re-run package:
$out"
}

# RSSI (0/54/4) and read latency through matter-server, before any update_node.
guard_link() {  # $1 node id
  local out rc=0
  out="$("$OTAENV_PY" "$GUARDS_PY" link "$MS_WS" "$1" "$OTA_MIN_RSSI")" || rc=$?
  if [ "$rc" -eq 0 ]; then say "  link ok: $out"; return 0; fi
  [ "${OTA_ALLOW_WEAK_LINK:-0}" = 1 ] && { say "  WARNING: OTA_ALLOW_WEAK_LINK=1 -- $out"; return 0; }
  die "link pre-flight failed for node $1: $out
     Move the node/AP closer or fix Wi-Fi first, or pass OTA_ALLOW_WEAK_LINK=1."
}

pi_ssh() { ssh -o BatchMode=yes -o ConnectTimeout=10 -i "$PI_SSH_KEY" "$PI_HOST" "$@"; }
pi_now() { pi_ssh date +%s; }   # the Pi's clock, so log filtering is immune to skew and TZ

# Install files into the root-owned provider dir through a throwaway root container (the user is
# in the docker group, sudo needs a password), then archive every other manifest for the same
# product and restart matter-server (manifests are read once at init).
pi_stage() {  # $1 local manifest (.json) to keep active  $2... other files to install
  local json="$1" keep tmp names plan="" mv="" f
  keep="$(basename "$json")"
  tmp="/tmp/ota-stage.$$"
  pi_ssh "mkdir -p $tmp" || die "cannot reach $PI_HOST"
  scp -o BatchMode=yes -i "$PI_SSH_KEY" "$@" "$PI_HOST:$tmp/" >/dev/null || die "scp to $PI_HOST:$tmp failed"
  names=""; for f in "$@"; do names="$names /src/$(basename "$f")"; done
  if [ "${OTA_KEEP_MANIFESTS:-0}" != 1 ]; then
    # listing = name<TAB>json for every manifest on the Pi, plus the local one being shipped
    plan="$( { pi_ssh "cd $PI_OTA_DIR && for f in *.json; do [ -r \"\$f\" ] || continue; printf '%s\t' \"\$f\"; tr -d '\n' < \"\$f\"; echo; done"
               printf '%s\t%s\n' "$keep" "$(tr -d '\n' < "$json")"; } \
             | python3 "$GUARDS_PY" archive-plan "$keep")" || die "could not plan manifest archiving"
  fi
  for f in $plan; do mv="$mv && mv /ota/$f /ota/$f.archived"; done
  pi_ssh "docker run --rm -v $PI_OTA_DIR:/ota -v $tmp:/src alpine sh -c 'install -m 0644 -o root -g root$names /ota/ && rm -f /ota/chip_kvs_ota_provider_* /ota/ota_provider_*.log$mv' && rm -rf $tmp && docker restart matter-server >/dev/null" \
    || die "root-container install on $PI_HOST failed"
  [ -z "$plan" ] || say "  archived $(wc -w <<< "$plan") other manifest(s) for this product (renamed *.json.archived, reversible)"
  say "  staged via root container, provider junk pruned; waiting for matter-server to reload (~100 s)"
  pi_ssh 'for i in $(seq 60); do bash -c "echo > /dev/tcp/127.0.0.1/5580" 2>/dev/null && exit 0; sleep 3; done; exit 1' \
    || die "matter-server did not reopen :5580 within 3 min of the restart"
  say "  matter-server is serving again"
}
