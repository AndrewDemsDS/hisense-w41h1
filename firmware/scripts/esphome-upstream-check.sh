#!/usr/bin/env bash
# esphome-upstream-check.sh -- run ESPHome's own CI gates against the hisense_ac component.
#
# The component is meant to go to upstream ESPHome, and their CI decides whether it gets in. This
# drops the component and its tests into a checkout of esphome/esphome at the places they would
# occupy in a pull request, then runs the scripts their workflow runs:
#
#   script/ci-custom.py              their source rules (namespaces, includes, line endings, ...)
#   script/build_codeowners.py       regenerates CODEOWNERS from the component's CODEOWNERS list
#   ruff check / ruff format --check their Python lint and format
#   pylint                           their Python lint config, on the component only
#   script/test_build_components.py  validates tests/components/hisense_ac for every platform
#
# clang-format and clang-tidy are not repeated here: cpp-lint.sh runs both with ESPHome's pins and
# ESPHome's configs on every commit.
#
#   esphome-upstream-check.sh            run everything
#   ESPHOME_UPSTREAM_REF=dev esphome-upstream-check.sh   check against their dev branch instead
#
# The default ref is the release this repo builds with, so the gate is reproducible. Checking
# against dev is the last step before opening the upstream pull request.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
COMPONENT="$REPO/firmware/esphome/components/hisense_ac"
TESTS="$REPO/firmware/esphome/tests/components/hisense_ac"

# Kept in the esphome==X.Y.Z form so Renovate bumps it with the other ESPHome pins.
ESPHOME_SPEC="esphome==2026.7.4"
REF="${ESPHOME_UPSTREAM_REF:-${ESPHOME_SPEC#esphome==}}"

CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/hisense-w41h1/esphome-upstream"
SRC="$CACHE/src-$REF"
VENV="$CACHE/venv-$REF"

say() { printf '\033[1;36m[upstream]\033[0m %s\n' "$*"; }

if [ ! -d "$SRC/.git" ]; then
  say "cloning esphome/esphome at $REF"
  mkdir -p "$CACHE"
  git clone --quiet --depth 1 --branch "$REF" https://github.com/esphome/esphome.git "$SRC"
elif [ "$REF" = dev ]; then
  say "updating the dev checkout"
  git -C "$SRC" fetch --quiet --depth 1 origin dev
  git -C "$SRC" reset --quiet --hard FETCH_HEAD
fi
# Start from their tree exactly, whatever an earlier run left behind.
git -C "$SRC" reset --quiet --hard
git -C "$SRC" clean --quiet -fd

if [ ! -x "$VENV/bin/python" ]; then
  say "creating the tool environment (once per ref)"
  if command -v uv >/dev/null 2>&1; then
    uv venv --quiet "$VENV"
    uv pip install --quiet --python "$VENV/bin/python" \
      -r "$SRC/requirements.txt" -r "$SRC/requirements_test.txt" -e "$SRC"
  else
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install --quiet \
      -r "$SRC/requirements.txt" -r "$SRC/requirements_test.txt" -e "$SRC"
  fi
fi

say "placing the component and its tests in the checkout"
mkdir -p "$SRC/esphome/components/hisense_ac" "$SRC/tests/components/hisense_ac"
(cd "$COMPONENT" && git ls-files -z . | xargs -0 -I{} cp --parents {} "$SRC/esphome/components/hisense_ac/")
(cd "$TESTS" && git ls-files -z . | xargs -0 -I{} cp --parents {} "$SRC/tests/components/hisense_ac/")
# Their scripts list files through git, so the new ones have to be tracked.
git -C "$SRC" add --all esphome/components/hisense_ac tests/components/hisense_ac

cd "$SRC"
export PATH="$VENV/bin:$PATH"
status=0
gate() {  # gate <label> <command...>
  say "$1"
  shift
  if ! "$@"; then
    status=1
    printf '\033[1;31m[upstream] FAILED:\033[0m %s\n' "$*"
  fi
}

gate "script/ci-custom.py" python script/ci-custom.py
gate "script/build_codeowners.py" python script/build_codeowners.py
gate "ruff check" ruff check esphome/components/hisense_ac
gate "ruff format --check" ruff format --check esphome/components/hisense_ac
gate "pylint" pylint -f parseable --persistent=n esphome/components/hisense_ac
# This one prints every validated config (well over a thousand lines), so its output is shown only
# when it fails.
say "script/test_build_components.py (config, every platform)"
if python script/test_build_components.py -e config -c hisense_ac >"$CACHE/test_build.log" 2>&1; then
  grep -E '^> |Test Summary' "$CACHE/test_build.log" || true
else
  status=1
  cat "$CACHE/test_build.log"
  printf '\033[1;31m[upstream] FAILED:\033[0m script/test_build_components.py\n'
fi

if [ "$status" -eq 0 ]; then
  say "all upstream gates passed against $REF"
else
  say "one or more upstream gates failed against $REF"
fi
exit "$status"
