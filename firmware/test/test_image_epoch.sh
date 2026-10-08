#!/usr/bin/env bash
# test_image_epoch.sh -- the AmebaZ2 build clock survives merge and cherry-pick (#137).
#
# The build pins SOURCE_DATE_EPOCH, which lands in the image as __DATE__/__TIME__ and the
# build_info stamp. It used to be the HEAD commit time, so a branch build flashed before merge
# never matched the tag rebuild of the merge commit. This drives `dev.py ota amebaz2 epoch` over a
# throwaway repo with fixed dates and checks the rule: the author date of the newest commit that
# touches the image inputs. No SDK, no env file.
#
# While ota-release.sh is still the builder (#143), its `epoch` must give the same answer in every
# case below, so each check runs both and fails on any difference.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPTS="$HERE/../scripts"

T="$(mktemp -d "${TMPDIR:-/tmp}/image-epoch.XXXXXX")"
trap 'rm -rf "$T"' EXIT

# Hermetic git: the fixture must not pick up the developer's hooks, signing or identity. Under the
# pre-commit hook git also exports GIT_DIR/GIT_INDEX_FILE/..., which would point every command
# below at the REAL repository (git init would re-init it as bare, git add would clobber its
# index), so drop git's repo-local environment first.
# shellcheck disable=SC2046  # word splitting of the variable list is the point
unset $(git rev-parse --local-env-vars)
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.com
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.com

fail=0
check() {  # $1 description  $2 got  $3 want
  if [ "$2" = "$3" ]; then echo "  ok   $1 ($2)"; else echo "  FAIL $1: got '$2', want '$3'"; fail=1; fi
}
at() {  # run git with both dates pinned: $1 author epoch  $2 committer epoch  $3... git args
  local a="$1" c="$2"; shift 2
  GIT_AUTHOR_DATE="@$a +0000" GIT_COMMITTER_DATE="@$c +0000" git -C "$R" "$@"
}
epoch() {  # prints the epoch, or a DIVERGED line (which no check accepts) when the two differ
  local d s rd=0 rs=0
  d="$(python3 "$1/firmware/scripts/dev.py" ota amebaz2 epoch 2>/dev/null)" || rd=$?
  s="$(bash "$1/firmware/scripts/ota-release.sh" epoch 2>/dev/null)" || rs=$?
  if [ "$d" != "$s" ] || [ "$rd" != "$rs" ]; then
    echo "DIVERGED dev.py='$d' (rc $rd) ota-release.sh='$s' (rc $rs)"; return 1
  fi
  [ "$rd" = 0 ] || return "$rd"
  printf '%s\n' "$d"
}
export PYTHONDONTWRITEBYTECODE=1   # no __pycache__ in the fixture trees

R="$T/repo"
mkdir -p "$R/firmware/scripts" "$R/firmware/src" "$R/patches"
cp "$SCRIPTS/dev.py" "$SCRIPTS/ota_guards.py" \
   "$SCRIPTS/ota-release.sh" "$SCRIPTS/sync-files.sh" "$SCRIPTS/ota-guards.sh" "$R/firmware/scripts/"
echo 'int a;' > "$R/firmware/src/a.c"
echo '1.0.0' > "$R/firmware/src/version.txt"
echo 'readme' > "$R/README.md"
git -C "$R" init -q -b main
[ "$(git -C "$R" rev-parse --show-toplevel)" = "$(cd "$R" && pwd -P)" ] \
  || { echo "  FAIL fixture repo is not isolated, refusing to go on"; exit 1; }
git -C "$R" add -A
at 1000 1000 commit -qm base
check "base commit" "$(epoch "$R")" 1000

# Feature branch: one input change, then docs only (repo-level and markdown under firmware/src).
git -C "$R" checkout -qb feat
echo 'int a = 1;' > "$R/firmware/src/a.c"
at 2000 2000 commit -qam 'input change'
src="$(git -C "$R" rev-parse HEAD)"
mkdir -p "$R/firmware/src/sub"
echo 'more' >> "$R/README.md"; echo 'notes' > "$R/firmware/src/NOTES.md"
echo 'nested notes' > "$R/firmware/src/sub/README.md"
git -C "$R" add -A
at 3000 3000 commit -qm 'docs only'
check "branch head after a docs-only commit" "$(epoch "$R")" 2000

# The release shape: a --no-ff merge commit on main, made well after the branch build.
git -C "$R" checkout -q main
at 4000 4000 merge -q --no-ff -m 'Merge pull request' feat
check "merge commit (HEAD time differs)" "$(epoch "$R")" 2000
[ "$(git -C "$R" log -1 --format=%ct)" = 4000 ] || { echo "  FAIL fixture: merge commit time"; fail=1; }

# A cherry-pick onto another line keeps the author date, so it keeps the clock too.
git -C "$R" checkout -q -b pick main~1
at 5000 5000 cherry-pick "$src" >/dev/null
check "cherry-picked onto another branch" "$(epoch "$R")" 2000

# Anything that does feed the image moves the clock: a patch, and the version bump.
git -C "$R" checkout -q main
echo 'diff' > "$R/patches/x.patch"; git -C "$R" add -A
at 6000 6000 commit -qm 'patch'
check "a patches/ change moves the clock" "$(epoch "$R")" 6000
echo '1.0.1' > "$R/firmware/src/version.txt"
at 7000 7000 commit -qam 'bump'
check "a version.txt bump moves the clock" "$(epoch "$R")" 7000

# A depth-1 clone makes HEAD look like it touched everything, so it must refuse, not guess.
git clone -q --depth 1 "file://$R" "$T/shallow"
if out="$(epoch "$T/shallow")"; then echo "  FAIL shallow clone was accepted"; fail=1
elif [ -n "$out" ]; then echo "  FAIL shallow clone: $out"; fail=1
else echo "  ok   shallow clone refused"; fi

# No .git at all (a source tarball): git archive stamps files with the commit time.
mkdir "$T/tarball"
git -C "$R" archive HEAD | tar -x -C "$T/tarball"
check "tarball fallback uses the archived mtimes" "$(epoch "$T/tarball")" 7000

[ "$fail" = 0 ] || { echo "image epoch rule FAILED"; exit 1; }
echo "image epoch rule OK"
