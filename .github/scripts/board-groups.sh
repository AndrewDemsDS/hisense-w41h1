#!/usr/bin/env bash
# Which toolchain builds which board file, read from the board files themselves. The workflows ask
# here, so none of them holds a list of board files and a file added later is compiled without a
# workflow change.
#
#   board-groups.sh esp32     the files that include packages/esp32.yaml (ESP-IDF, cached)
#   board-groups.sh amebaz2   the files that include packages/amebaz2.yaml (LibreTiny, cached)
#   board-groups.sh other     every other board file (ESP8266, RP2040: PlatformIO, no cache)
#   board-groups.sh matrix    the groups that have a file, as a GitHub Actions matrix (JSON)
#
# Paths are relative to firmware/esphome/, which is what esphome-compile-check.sh takes.
set -euo pipefail

cd "$(dirname "$0")/../../firmware/esphome"

group_of() {
  if grep -qE '^[[:space:]]+[a-z0-9_]+: !include packages/esp32\.yaml' "$1"; then
    echo esp32
  elif grep -qE '^[[:space:]]+[a-z0-9_]+: !include packages/amebaz2\.yaml' "$1"; then
    echo amebaz2
  else
    echo other
  fi
}

files_of() {
  local board out=""
  for board in w41h1*.yaml; do
    if [ "$(group_of "$board")" = "$1" ]; then
      out="$out $board"
    fi
  done
  echo "${out# }"
}

case "${1:-}" in
  esp32 | amebaz2 | other)
    files_of "$1"
    ;;
  matrix)
    entries=""
    for group in esp32 amebaz2 other; do
      files="$(files_of "$group")"
      if [ -n "$files" ]; then
        entries="$entries,{\"toolchain\":\"$group\",\"configs\":\"$files\"}"
      fi
    done
    echo "{\"include\":[${entries#,}]}"
    ;;
  *)
    echo "usage: $0 esp32|amebaz2|other|matrix" >&2
    exit 2
    ;;
esac
