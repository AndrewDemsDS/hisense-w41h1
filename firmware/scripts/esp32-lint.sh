#!/usr/bin/env bash
# esp32-lint.sh -- host-only consistency gate for the ESP32 (esp-matter) firmware tree.
# No IDF/toolchain needed: pure text parsing, so it runs in the hardware-free CI and pre-commit.
# It gives the ESP32 path the version discipline that ota-release.sh already enforces for AmebaZ2
# (issue #77), which ota-release.sh does NOT cover:
#   1. PROJECT_VER (CMakeLists.txt) is MAJOR.MINOR.PATCH with minor/patch < 100 -- the bound that
#      keeps the derived int (MAJOR*10000+MINOR*100+PATCH) monotonic + collision-free.
#   2. sdkconfig.defaults' CONFIG_DEVICE_SOFTWARE_VERSION_{NUMBER,STRING} equal PROJECT_VER. The int
#      is injected from CMake and wins, but the STRING comes from sdkconfig -- so a drift there ships
#      a stale softwareVersionString (this is exactly the drift found at v1.0.5 vs v1.0.7).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESP="$HERE/../esp32-matter"
CMAKE="$ESP/CMakeLists.txt"
SDKCFG="$ESP/sdkconfig.defaults"
die(){ echo "[esp32-lint] ERROR: $*" >&2; exit 1; }

[ -f "$CMAKE" ]  || die "missing $CMAKE"
[ -f "$SDKCFG" ] || die "missing $SDKCFG"

ver=$(sed -n 's/^set(PROJECT_VER "\(.*\)").*/\1/p' "$CMAKE")
[ -n "$ver" ] || die "could not parse PROJECT_VER from $CMAKE"

IFS=. read -r MA MI PA <<< "$ver"
[[ "$MA" =~ ^[0-9]+$ && "$MI" =~ ^[0-9]+$ && "$PA" =~ ^[0-9]+$ ]] \
  || die "PROJECT_VER '$ver' is not MAJOR.MINOR.PATCH integers"
{ [ "$MI" -lt 100 ] && [ "$PA" -lt 100 ]; } \
  || die "PROJECT_VER '$ver': minor/patch must be < 100 (derived-int collision)"
want_int=$((MA*10000 + MI*100 + PA))

got_num=$(sed -n 's/^CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER=\([0-9]*\).*/\1/p' "$SDKCFG")
got_str=$(sed -n 's/^CONFIG_DEVICE_SOFTWARE_VERSION_STRING="\(.*\)".*/\1/p' "$SDKCFG")

[ "$got_num" = "$want_int" ] \
  || die "sdkconfig NUMBER=${got_num:-<unset>} != derived $want_int from PROJECT_VER $ver -- fix sdkconfig.defaults"
[ "$got_str" = "$ver" ] \
  || die "sdkconfig STRING=\"${got_str:-<unset>}\" != PROJECT_VER \"$ver\" -- fix sdkconfig.defaults"

echo "[esp32-lint] OK: PROJECT_VER $ver -> int $want_int; sdkconfig NUMBER/STRING in sync"
