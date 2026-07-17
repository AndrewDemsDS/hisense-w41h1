#!/usr/bin/env bash
# stop-slop: block the clearest AI-writing tell in NEW prose. Scans only the added lines
# of staged markdown for em dashes (U+2014), so the existing doc corpus is never retro-
# flagged, only new text. En dashes (U+2013, used in date/number ranges) are allowed.
# Recast an em dash as a period, comma, colon, or parentheses. Bypass: git commit --no-verify.
set -euo pipefail

EMDASH=$'—'
files=$(git diff --cached --name-only --diff-filter=ACM | grep -Ei '\.(md|markdown)$' || true)
[ -z "$files" ] && exit 0

fail=0
while IFS= read -r f; do
  [ -n "$f" ] || continue
  # added lines only (drop the +++ header), match the em dash literally
  hits=$(git diff --cached -U0 -- "$f" | grep '^+' | grep -v '^+++' | grep -F "$EMDASH" || true)
  if [ -n "$hits" ]; then
    echo "[stop-slop] em dash in new prose: $f"
    echo "$hits" | sed 's/^+/    /'
    fail=1
  fi
done <<< "$files"

if [ "$fail" -ne 0 ]; then
  echo "[stop-slop] recast em dashes as a period, comma, colon, or parens (bypass: git commit --no-verify)"
  exit 1
fi
exit 0
