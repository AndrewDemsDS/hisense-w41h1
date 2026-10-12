#!/usr/bin/env bash
# Line and branch coverage of the pure ESPHome product code under the host tests.
#
#   firmware/test/coverage/run.sh [--baseline] [--out DIR]
#
# Builds every host test that compiles the component (the same sources as
# firmware/test/run_tests.sh, plus gcov instrumentation), runs them, and reports per product file
# with gcovr: a text summary on stdout, the uncovered lines and branches, and an HTML report.
# Everything is written to DIR (default: a fresh directory under $TMPDIR), never into the tree.
#
#   --baseline   leave out the gap tests (test_esphome_*_gaps.cpp), to see what they add
#
# Not part of the default suite: it needs gcovr (uvx fetches it, or set GCOVR) and takes about 15 s.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
test_dir="$(dirname "$here")"
repo="$(cd "$test_dir/../.." && pwd)"
comp="$repo/firmware/esphome/components/hisense_ac"
module="$repo/firmware/esphome/components/w41h1_module"

baseline=0
out=""
while [ $# -gt 0 ]; do
  case "$1" in
    --baseline) baseline=1 ;;
    --out)
      out="$2"
      shift
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
  shift
done
[ -n "$out" ] || out="$(mktemp -d "${TMPDIR:-/tmp}/w41h1-coverage.XXXXXX")"
mkdir -p "$out/obj" "$out/html"

# -O0 and no inlining so that a line is a line. -fkeep-inline-functions makes the compiler emit
# every inline function of the pure headers (hisense_map.h and its parts), called or not: without it a function no test calls is
# missing from the report instead of showing as uncovered.
cov=(--coverage -O0 -g -fno-inline -fkeep-inline-functions)
ref=(-I"$test_dir/stubinc" -I"$test_dir" -I"$test_dir/reference-driver")

# build_and_run NAME COMPILER-ARGS...
build_and_run() {
  local name="$1"
  shift
  local dir="$out/obj/$name"
  mkdir -p "$dir"
  (cd "$dir" && g++ -std=c++17 -Wall "${cov[@]}" "$@" -o "$dir/$name")
  if ! (cd "$dir" && "./$name" >"$dir/run.log" 2>&1); then
    echo "FAILED: $name (see $dir/run.log)" >&2
    tail -20 "$dir/run.log" >&2
    exit 1
  fi
  echo "  ran $name: $(tail -1 "$dir/run.log")"
}

echo "== building and running the host tests with gcov instrumentation =="
# Layer 1d. The reference driver is C++11 and is not measured, so it is built plain.
mkdir -p "$out/obj/parity"
g++ -std=c++11 -Wall "${ref[@]}" -c "$test_dir/reference-driver/hisense_rs485.cpp" -o "$out/obj/parity/driver.o"
build_and_run test_esphome_codec_parity "${ref[@]}" -I"$comp" \
  "$test_dir/test_esphome_codec_parity.cpp" "$comp/hisense_protocol.cpp" "$out/obj/parity/driver.o"
build_and_run test_esphome_bus -I"$comp" \
  "$test_dir/test_esphome_bus.cpp" "$comp/hisense_bus.cpp" "$comp/hisense_protocol.cpp"
build_and_run test_esphome_edge_cases -I"$comp" \
  "$test_dir/test_esphome_edge_cases.cpp" "$comp/hisense_protocol.cpp"
build_and_run test_esphome_confirm -I"$comp" \
  "$test_dir/test_esphome_confirm.cpp" "$comp/hisense_protocol.cpp"
build_and_run test_esphome_setup_request -I"$comp" \
  "$test_dir/test_esphome_setup_request.cpp" "$comp/hisense_bus.cpp" "$comp/hisense_protocol.cpp"
build_and_run test_esphome_uart_policy -I"$module" "$test_dir/test_esphome_uart_policy.cpp"
if [ "$baseline" -eq 0 ]; then
  # The scheduler's own edge-case tests, so that the figures for hisense_bus.* mean something.
  build_and_run test_esphome_setup_edges -I"$comp" \
    "$test_dir/test_esphome_setup_edges.cpp" "$comp/hisense_bus.cpp" "$comp/hisense_protocol.cpp"
  build_and_run test_esphome_bus_stress -I"$comp" \
    "$test_dir/test_esphome_bus_stress.cpp" "$comp/hisense_bus.cpp" "$comp/hisense_protocol.cpp"
  build_and_run test_esphome_codec_gaps -I"$comp" \
    "$test_dir/test_esphome_codec_gaps.cpp" "$comp/hisense_protocol.cpp"
  build_and_run test_esphome_map_gaps -I"$comp" -I"$module" \
    "$test_dir/test_esphome_map_gaps.cpp" "$comp/hisense_protocol.cpp"
fi

# Product files only.
filters=(
  --filter "$comp/hisense_protocol\.cpp" --filter "$comp/hisense_protocol\.h"
  --filter "$comp/hisense_mapping\.h" --filter "$comp/hisense_special\.h"
  --filter "$comp/hisense_confirm\.h" --filter "$comp/hisense_telemetry\.h"
  --filter "$comp/hisense_commands\.h" --filter "$comp/hisense_bus\.cpp" --filter "$comp/hisense_bus\.h"
  --filter "$module/w41h1_uart_policy\.h"
)
# gcovr: $GCOVR when set (a command, with arguments if it needs any), otherwise through uvx at
# the version CI pins in .github/ci/lint/requirements.txt, so a laptop and CI print one table.
if [ -n "${GCOVR:-}" ]; then
  read -r -a gcovr <<< "$GCOVR"
else
  export UV_CONSTRAINT="${UV_CONSTRAINT:-$repo/.github/ci/lint/requirements.txt}"
  gcovr=(uvx gcovr)
fi
# Branches the compiler adds for exceptions and for unreachable code are not test targets.
common=(--root "$repo" "${filters[@]}" --exclude-throw-branches --exclude-unreachable-branches
  --merge-mode-functions=merge-use-line-min "$out/obj")

echo
echo "== line coverage =="
"${gcovr[@]}" "${common[@]}" --txt "$out/lines.txt" --html-details "$out/html/index.html" \
  --json "$out/coverage.json" >/dev/null
cat "$out/lines.txt"
echo
echo "== branch coverage =="
"${gcovr[@]}" "${common[@]}" --txt-metric branch --txt "$out/branches.txt" >/dev/null
cat "$out/branches.txt"

echo
echo "== uncovered lines and partly taken branches (file:line: source) =="
python3 "$here/uncovered.py" "$out/coverage.json" "$repo"
echo
echo "report directory: $out (HTML: $out/html/index.html)"
