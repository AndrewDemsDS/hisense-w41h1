#!/usr/bin/env bash
# This repo's ESPHome components, read from the tree: every directory under
# firmware/esphome/components/. The gates that hold "our components" to a rule ask here, so none
# of them holds a list and a component added later is inside every gate without a change to one:
# the compiler-warning checks (esphome-compile-check.sh and the stock-module job of qa.yaml), the
# cppcheck step of qa.yaml and the lint scope of cpp-lint.py (which reads the directories itself).
#
#   esphome-components.sh          the names, one per line, sorted
#   esphome-components.sh regex    the names as one alternation for grep -E: a|b|c
#
# No component at all is an error (exit 1), so a gate cannot pass on an empty list.
# Not asked: esphome-upstream-check.sh, which takes hisense_ac alone on purpose.
set -euo pipefail

cd "$(dirname "$0")/../esphome/components"

names=()
for dir in */; do
  [ -d "$dir" ] || continue
  names+=("${dir%/}")
done
if [ "${#names[@]}" -eq 0 ]; then
  echo "esphome-components.sh: no component directory under firmware/esphome/components/" >&2
  exit 1
fi

case "${1:-}" in
  "") printf '%s\n' "${names[@]}" ;;
  regex) (IFS='|'; echo "${names[*]}") ;;
  *)
    echo "usage: $0 [regex]" >&2
    exit 2
    ;;
esac
