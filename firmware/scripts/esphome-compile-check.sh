#!/usr/bin/env bash
# Compile ESPHome configurations as a gate: each one has to build, this repo's components have to
# build without a compiler warning, and a board file's image has to carry the firmware version.
# The QA workflow runs it on the ESP32 board files and the release workflow on every board file.
#
#   firmware/scripts/esphome-compile-check.sh w41h1-esp32c3.yaml tests/build.hub-only.esp32-c3-idf.yaml
#
# Paths are relative to firmware/esphome/. ESPHOME names the esphome command (default: esphome).
# It needs firmware/esphome/secrets.yaml and copies the placeholder file when there is none, so
# an image from a fresh checkout or a CI runner holds publicly known keys: never flash one.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/../esphome"
esphome_cmd="${ESPHOME:-esphome}"

[ $# -gt 0 ] || { echo "usage: $0 <config.yaml>..." >&2; exit 2; }
[ -f secrets.yaml ] || cp secrets.yaml.example secrets.yaml

version="$(python3 "$HERE/version-lint.py" --print)"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT

for config in "$@"; do
  # Its own node name, so each file gets its own build directory.
  name="ci-$(basename "$config" .yaml | tr '.' '-')"
  if ! "$esphome_cmd" -s name "$name" compile "$config" > "$log" 2>&1; then
    tail -n 200 "$log"
    echo "::error::$config does not compile"
    exit 1
  fi
  echo "== $config"
  # PlatformIO puts a colour reset in front of its size lines, so they are not anchored.
  grep -a -E '(RAM|Flash): +\[|Successfully compiled' "$log" || true
  # The SDKs warn about their own code. Ours has to build clean on every compiler. What counts
  # is a compiler diagnostic that names one of our files, `<path>:<line>[:<col>]: warning:`, with
  # any colour codes removed first. A file that merely has "error" in its name is not one.
  if sed 's/\x1b\[[0-9;]*[mK]//g' "$log" \
      | grep -a -E 'components/(hisense_ac|w41h1_module)/[^: ]+:[0-9]+(:[0-9]+)?: +(warning|error|fatal error):'; then
    echo "::error::hisense_ac or w41h1_module does not compile cleanly for $config (see above)"
    exit 1
  fi
  # A board file takes packages/node.yaml and with it packages/version.yaml, the one place the
  # version is written. The files under tests/ take no package and carry none.
  rel="${config#"$PWD"/}"
  while [ "${rel#./}" != "$rel" ]; do rel="${rel#./}"; done
  case "$rel" in
    tests/*) ;;
    *)
      defines="$(dirname "$config")/.esphome/build/$name/src/esphome/core/defines.h"
      if ! grep -a -F "ESPHOME_PROJECT_VERSION \"$version\"" "$defines"; then
        echo "::error::$config was built without firmware version $version (packages/version.yaml)"
        exit 1
      fi
      ;;
  esac
done
echo "compiled clean: $*"
