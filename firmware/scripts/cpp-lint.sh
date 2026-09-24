#!/usr/bin/env bash
# cpp-lint.sh -- the one entry point for C/C++ lint across every tree we own.
#
#   cpp-lint.sh check [--no-tidy] [PATH...]   clang-format --dry-run -Werror, custom rules, clang-tidy
#   cpp-lint.sh fix   [--no-tidy] [PATH...]   apply: whitespace rules, clang-tidy --fix (safe checks
#                                             only), then clang-format -i
#   cpp-lint.sh files [PATH...]               list the files in scope
#   cpp-lint.sh compdb [OUT_DIR]              write compile_commands.json (host-compilable set)
#
# Scope, exclusions and the rules live in cpp-lint.py; configs are .clang-format / .clang-tidy at
# the repo root plus per-directory overrides. Tools are pinned to the versions ESPHome pins, since
# another clang-format major formats differently and CI must agree with every laptop:
CLANG_FORMAT_VERSION=13.0.1
CLANG_TIDY_VERSION=22.1.8
#
# Tool lookup, first hit wins: $CLANG_FORMAT/$CLANG_TIDY, a binary on PATH reporting the pinned
# version, then a private venv under ~/.cache/hisense-w41h1/cpp-lint (created on first use with
# uv or python3 -m venv; set CPP_LINT_NO_INSTALL=1 to forbid that). Exit 3 = pinned tool missing.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="${XDG_CACHE_HOME:-$HOME/.cache}/hisense-w41h1/cpp-lint"

has_version() {  # has_version <binary> <version>
  command -v "$1" >/dev/null 2>&1 && "$1" --version 2>/dev/null | grep -qF "version $2"
}

bootstrap_venv() {
  [ "${CPP_LINT_NO_INSTALL:-0}" = 1 ] && return 1
  echo "[cpp-lint] installing clang-format==$CLANG_FORMAT_VERSION clang-tidy==$CLANG_TIDY_VERSION into $VENV" >&2
  mkdir -p "$(dirname "$VENV")"
  if command -v uv >/dev/null 2>&1; then
    uv venv -q "$VENV" >&2 &&
      uv pip install -q --python "$VENV/bin/python" \
        "clang-format==$CLANG_FORMAT_VERSION" "clang-tidy==$CLANG_TIDY_VERSION" >&2
  else
    python3 -m venv "$VENV" >&2 &&
      "$VENV/bin/pip" install -q "clang-format==$CLANG_FORMAT_VERSION" "clang-tidy==$CLANG_TIDY_VERSION" >&2
  fi
}

resolve() {  # resolve <tool> <version> <override-value>
  local tool=$1 ver=$2 override=$3
  if [ -n "$override" ]; then echo "$override"; return 0; fi
  if has_version "$tool" "$ver"; then command -v "$tool"; return 0; fi
  if has_version "$VENV/bin/$tool" "$ver"; then echo "$VENV/bin/$tool"; return 0; fi
  return 1
}

mode="${1:-check}"
[ $# -gt 0 ] && shift
case "$mode" in
  files|compdb) exec python3 "$HERE/cpp-lint.py" "$mode" "$@" ;;
  check|fix) ;;
  *) sed -n '2,8p' "$0"; exit 2 ;;
esac

need_tidy=1
for a in "$@"; do [ "$a" = --no-tidy ] && need_tidy=0; done

if ! fmt=$(resolve clang-format "$CLANG_FORMAT_VERSION" "${CLANG_FORMAT:-}") ||
   { [ $need_tidy = 1 ] && ! resolve clang-tidy "$CLANG_TIDY_VERSION" "${CLANG_TIDY:-}" >/dev/null; }; then
  bootstrap_venv || true
  fmt=$(resolve clang-format "$CLANG_FORMAT_VERSION" "${CLANG_FORMAT:-}") || {
    echo "[cpp-lint] clang-format $CLANG_FORMAT_VERSION not found (pip install clang-format==$CLANG_FORMAT_VERSION)" >&2
    exit 3
  }
fi
tidy=""
if [ $need_tidy = 1 ]; then
  tidy=$(resolve clang-tidy "$CLANG_TIDY_VERSION" "${CLANG_TIDY:-}") || {
    echo "[cpp-lint] clang-tidy $CLANG_TIDY_VERSION not found (pip install clang-tidy==$CLANG_TIDY_VERSION)" >&2
    exit 3
  }
fi

exec python3 "$HERE/cpp-lint.py" "$mode" --clang-format "$fmt" ${tidy:+--clang-tidy "$tidy"} "$@"
