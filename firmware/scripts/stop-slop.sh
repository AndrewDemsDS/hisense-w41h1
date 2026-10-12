#!/usr/bin/env bash
# stop-slop: block the clearest AI-writing tell in NEW prose. Scans only the added lines
# of staged markdown (.md, .markdown, .mdx) for em dashes (U+2014), so the existing doc corpus
# is never retro-flagged, only new text. En dashes (U+2013, used in date/number ranges) are
# allowed. Recast an em dash as a period, comma, colon, or parentheses.
# Bypass: git commit --no-verify.
set -euo pipefail

EMDASH=$'—'
fail=0

check_file() {  # $1 = the file as staged, $2 = its name before a rename (or the same name)
  local hits
  # Both names, so a renamed file is compared with what it was: only the lines added to it count.
  hits=$(git diff --cached -M -U0 -- "$2" "$1" | grep '^+' | grep -v '^+++' | grep -F "$EMDASH" || true)
  if [ -n "$hits" ]; then
    echo "[stop-slop] em dash in new prose: $1"
    # shellcheck disable=SC2001  # one leading + per line, which ${var//} cannot anchor
    echo "$hits" | sed 's/^+/    /'
    fail=1
  fi
}

# NUL-separated, so a name with a space, a quote or a non-ASCII letter arrives as it is (git
# quotes and escapes such a name in its line output). A rename or copy has two names.
while IFS= read -r -d '' status; do
  IFS= read -r -d '' old
  new="$old"
  case "$status" in R*|C*) IFS= read -r -d '' new ;; esac
  case "${new,,}" in
    *.md|*.markdown|*.mdx) check_file "$new" "$old" ;;
  esac
done < <(git diff --cached --name-status -z -M --diff-filter=ACMR)

if [ "$fail" -ne 0 ]; then
  echo "[stop-slop] recast em dashes as a period, comma, colon, or parens (bypass: git commit --no-verify)"
  exit 1
fi
exit 0
