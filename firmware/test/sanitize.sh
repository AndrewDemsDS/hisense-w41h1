#!/usr/bin/env bash
# Build and run every C++ host test under AddressSanitizer and UndefinedBehaviorSanitizer, once
# with g++ and once with clang++. The build recipes mirror run_tests.sh (same sources, same
# include paths, same language standard per translation unit); only the sanitizer flags are added.
#
# A sanitizer report aborts the test (-fno-sanitize-recover=all), so "ok" means: the test's own
# checks passed AND no out-of-bounds access, use-after-free, leak, signed overflow, bad shift,
# misaligned access or out-of-range array index happened anywhere on the way.
#
#   firmware/test/sanitize.sh            # both compilers
#   CXXS="clang++" firmware/test/sanitize.sh
#
# Runtime: about 25 s for both compilers on a 16-thread desktop. Not part of run_tests.sh.
# Exit status: 0 when every test passed under every compiler that is installed, 1 otherwise.
# A compiler that is not installed is reported as skipped and does not fail the run.
set -uo pipefail
cd "$(dirname "$0")" || exit 1

SAN_FLAGS=("-fsanitize=address,undefined" -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1)
read -r -a COMPILERS <<<"${CXXS:-g++ clang++}"

PORT=../esphome/components/hisense_ac
MODULE=../esphome/components/w41h1_module
SETUP_MODE=../esphome/components/wifi_setup_mode
REF_INC=(-Istubinc -I. -Ireference-driver)

# name | include flags | translation units as "std:source", space separated.
# The reference driver stays C++11 as it shipped; the ESPHome port needs C++17 (nested namespaces).
TESTS=(
  "test_codec|${REF_INC[*]}|c++11:test_codec.cpp c++11:reference-driver/hisense_rs485.cpp"
  "test_reference_map|${REF_INC[*]}|c++11:test_reference_map.cpp c++11:reference-driver/hisense_rs485.cpp"
  "test_esphome_map|${REF_INC[*]}|c++11:test_esphome_map.cpp c++11:reference-driver/hisense_rs485.cpp"
  "test_esphome_codec_parity|${REF_INC[*]} -I$PORT|c++17:test_esphome_codec_parity.cpp c++17:$PORT/hisense_protocol.cpp c++11:reference-driver/hisense_rs485.cpp"
  "test_esphome_bus|-I$PORT|c++17:test_esphome_bus.cpp c++17:$PORT/hisense_bus.cpp c++17:$PORT/hisense_protocol.cpp"
  "test_esphome_edge_cases|-I$PORT|c++17:test_esphome_edge_cases.cpp c++17:$PORT/hisense_protocol.cpp"
  "test_esphome_confirm|-I$PORT|c++17:test_esphome_confirm.cpp c++17:$PORT/hisense_protocol.cpp"
  "test_esphome_entities|-I$PORT|c++17:test_esphome_entities.cpp c++17:$PORT/hisense_protocol.cpp"
  "test_esphome_setup_request|-I$PORT|c++17:test_esphome_setup_request.cpp c++17:$PORT/hisense_bus.cpp c++17:$PORT/hisense_protocol.cpp"
  "test_esphome_uart_policy|-I$MODULE|c++17:test_esphome_uart_policy.cpp"
  "test_esphome_setup_edges|-I$PORT|c++17:test_esphome_setup_edges.cpp c++17:$PORT/hisense_bus.cpp c++17:$PORT/hisense_protocol.cpp"
  "test_esphome_bus_stress|-I$PORT|c++17:test_esphome_bus_stress.cpp c++17:$PORT/hisense_bus.cpp c++17:$PORT/hisense_protocol.cpp"
  "test_esphome_codec_gaps|-I$PORT|c++17:test_esphome_codec_gaps.cpp c++17:$PORT/hisense_protocol.cpp"
  "test_esphome_map_gaps|-I$PORT -I$MODULE|c++17:test_esphome_map_gaps.cpp c++17:$PORT/hisense_protocol.cpp"
  "test_esphome_setup_mode|-I$SETUP_MODE|c++17:test_esphome_setup_mode.cpp"
)

# Known reports, switched off one check at a time so everything else in that test still runs under
# the full set. Format: test|extra compiler flag|what and where. Remove the entry with the fix.
KNOWN=(
)

# Every *.cpp test in this directory must be in the table, so a new test cannot be forgotten here.
missing=0
for src in test_*.cpp; do
  name=${src%.cpp}
  found=0
  for t in "${TESTS[@]}"; do
    [ "${t%%|*}" = "$name" ] && found=1
  done
  if [ "$found" = 0 ]; then
    echo "sanitize.sh: $src is not in the TESTS table (add its build recipe)" >&2
    missing=1
  fi
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/w41h1-sanitize.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# Leaks count, and a report must not be swallowed by a test that exits 0 afterwards.
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:exitcode=97:strict_string_checks=1:detect_stack_use_after_return=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:exitcode=98"

results=()
failed=$missing
for cxx in "${COMPILERS[@]}"; do
  if ! command -v "$cxx" >/dev/null 2>&1; then
    results+=("$cxx|(all)|SKIP (compiler not installed)")
    continue
  fi
  for t in "${TESTS[@]}"; do
    IFS='|' read -r name incs units <<<"$t"
    read -r -a inc_arr <<<"$incs"
    out="$WORK/$cxx-$name"
    extra=()
    for k in "${KNOWN[@]}"; do
      IFS='|' read -r kname kflag _ <<<"$k"
      [ "$kname" = "$name" ] && extra+=("$kflag")
    done
    mkdir -p "$out"
    objs=()
    build_ok=1
    i=0
    for unit in $units; do
      std=${unit%%:*}
      src=${unit#*:}
      obj="$out/$i.o"
      i=$((i + 1))
      if ! "$cxx" "-std=$std" -Wall "${SAN_FLAGS[@]}" "${extra[@]}" "${inc_arr[@]}" -c "$src" -o "$obj" 2>"$out/build.log"; then
        build_ok=0
        break
      fi
      objs+=("$obj")
    done
    if [ "$build_ok" = 1 ] && ! "$cxx" "${SAN_FLAGS[@]}" "${objs[@]}" -o "$out/test" 2>>"$out/build.log"; then
      build_ok=0
    fi
    if [ "$build_ok" = 0 ]; then
      results+=("$cxx|$name|BUILD FAILED")
      failed=1
      sed 's/^/    /' "$out/build.log" | head -40
      continue
    fi
    start=$(date +%s.%N)
    "$out/test" >"$out/run.log" 2>&1
    rc=$?
    secs=$(awk -v a="$start" -v b="$(date +%s.%N)" 'BEGIN { printf "%.1f", b - a }')
    if [ "$rc" = 0 ]; then
      results+=("$cxx|$name|ok (${secs}s)")
    else
      case "$rc" in
        97) why="AddressSanitizer report" ;;
        98) why="UndefinedBehaviorSanitizer report" ;;
        *) why="exit $rc" ;;
      esac
      results+=("$cxx|$name|FAILED: $why")
      failed=1
      echo "---- $cxx $name: $why (last 60 lines) ----"
      tail -60 "$out/run.log" | sed 's/^/    /'
    fi
  done
done

echo
printf '%-9s %-28s %s\n' "compiler" "test" "result"
for r in "${results[@]}"; do
  IFS='|' read -r c n res <<<"$r"
  printf '%-9s %-28s %s\n' "$c" "$n" "$res"
done
for k in "${KNOWN[@]}"; do
  IFS='|' read -r kname kflag kwhy <<<"$k"
  echo "KNOWN ($kname built with $kflag): $kwhy"
done
echo
if [ "$failed" = 0 ]; then
  echo "SANITIZERS: ALL CLEAN"
else
  echo "SANITIZERS: FAILURES (see above)"
fi
exit "$failed"
