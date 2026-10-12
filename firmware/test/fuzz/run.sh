#!/usr/bin/env bash
# Fuzzing of the Hisense RS-485 codec and bus scheduler. One libFuzzer harness per fuzz_*.cpp (each
# documents its input format and properties at the top), fuzz_common.h for the shared wire model,
# regressions/KNOWN_FAILURES for the properties that fail today, make_seeds.py for the seed corpus.
#
#   run.sh --smoke                  replay seeds, corpus and regressions through every harness and
#                                   check that every known failure still fails. Deterministic, no
#                                   libFuzzer needed (g++ or clang++), 10 to 20 s, mostly compiling.
#   run.sh <seconds> [harness...]   fuzz each harness for that long, all of them in parallel
#                                   (clang++ with libFuzzer). New inputs go to build/corpus/, crash
#                                   reproducers to build/artifacts/<harness>/.
#   run.sh --coverage               replay everything under clang source coverage and print what
#                                   the corpus reaches in the product sources.
#   run.sh --merge                  fold build/corpus/ back into corpus/<harness>.hex
#                                   (coverage-minimised, one input per line as text).
#
# Environment: CXX (smoke compiler, default clang++ then g++), FUZZ_STRICT=1 (fuzz with the known
# failures switched on, to look at them or to check a fix), FUZZ_ARGS (extra libFuzzer flags).
set -uo pipefail
cd "$(dirname "$0")" || exit 1

HARNESSES=(assembler parsers setup_request builders roundtrip differential bus)
PORT=../../esphome/components/hisense_ac
BUILD=build
SAN=("-fsanitize=address,undefined" -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1)
WARN=(-Wall -Wextra -Wno-unused-parameter)
# handle_abort: a failed FUZZ_CHECK ends as an ASan report and an exit code, not a killed process.
export ASAN_OPTIONS="detect_leaks=1:exitcode=97:detect_stack_use_after_return=1:handle_abort=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:exitcode=98"

usage() {
  sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
}

# Sources and include paths of one harness. The differential harness #includes the reference
# driver's source, which needs the HAL stubs; the bus harness links the scheduler.
harness_sources() {
  case "$1" in
    bus) echo "fuzz_bus.cpp $PORT/hisense_bus.cpp $PORT/hisense_protocol.cpp" ;;
    *) echo "fuzz_$1.cpp $PORT/hisense_protocol.cpp" ;;
  esac
}
harness_includes() {
  case "$1" in
    differential) echo "-I. -I$PORT -I../stubinc -I.. -I../reference-driver" ;;
    *) echo "-I. -I$PORT" ;;
  esac
}

# build <harness> <variant: replay|fuzz|cov> <strict: 0|1> -> path of the binary on stdout
build() {
  local h=$1 variant=$2 strict=$3 cxx flags=() out
  out="$BUILD/bin/$h-$variant$([ "$strict" = 1 ] && echo -strict)"
  read -r -a srcs <<<"$(harness_sources "$h")"
  read -r -a incs <<<"$(harness_includes "$h")"
  case "$variant" in
    fuzz)
      cxx=clang++
      flags=("${SAN[@]}" -fsanitize=fuzzer)
      ;;
    cov)
      cxx=clang++
      flags=(-g -O0 -fprofile-instr-generate -fcoverage-mapping)
      srcs+=(standalone_main.cpp)
      ;;
    *)
      cxx=$SMOKE_CXX
      flags=("${SAN[@]}")
      srcs+=(standalone_main.cpp)
      ;;
  esac
  [ "$strict" = 1 ] && flags+=(-DFUZZ_STRICT)
  mkdir -p "$BUILD/bin"
  if ! "$cxx" -std=c++17 "${WARN[@]}" "${flags[@]}" "${incs[@]}" "${srcs[@]}" -o "$out" 2>"$out.log"; then
    echo "BUILD FAILED: $h ($variant)" >&2
    sed 's/^/    /' "$out.log" | head -40 >&2
    return 1
  fi
  echo "$out"
}

pick_smoke_compiler() {
  if [ -n "${CXX:-}" ]; then
    SMOKE_CXX=$CXX
  elif command -v clang++ >/dev/null 2>&1; then
    SMOKE_CXX=clang++
  else
    SMOKE_CXX=g++
  fi
}

# ---- smoke ---------------------------------------------------------------------------------------
smoke() {
  pick_smoke_compiler
  local failed=0 pids=() h
  echo "== fuzz smoke: replay seeds, corpus and regressions ($SMOKE_CXX, ASan + UBSan) =="
  # Build everything first, in parallel: the default build of every harness, and a strict build
  # of each harness that has a known failure.
  mapfile -t KF < <(grep -v '^[[:space:]]*\(#\|$\)' regressions/KNOWN_FAILURES 2>/dev/null)
  for h in "${HARNESSES[@]}"; do
    (build "$h" replay 0 >/dev/null) &
    pids+=($!)
    if printf '%s\n' "${KF[@]}" | grep -q "^[^|]*|[[:space:]]*${h}[[:space:]]*|"; then
      (build "$h" replay 1 >/dev/null) &
      pids+=($!)
    fi
  done
  for p in "${pids[@]}"; do
    wait "$p" || failed=1
  done
  [ "$failed" = 0 ] || return 1

  for h in "${HARNESSES[@]}"; do
    local log="$BUILD/smoke-$h.log"
    if "$BUILD/bin/$h-replay" "seeds/$h" "corpus/$h.hex" "regressions/$h" >"$log" 2>&1; then
      printf '  ok    %-14s %s\n' "$h" "$(tail -1 "$log")"
    else
      printf '  FAIL  %-14s\n' "$h"
      grep -v '^replay ' "$log" | head -30 | sed 's/^/        /'
      echo "        input: $(grep '^replay ' "$log" | tail -1)"
      failed=1
    fi
  done

  # Every known failure must still fail under the strict build. One that passes was fixed (or the
  # harness stopped looking): either way the entry and its exclusion have to go.
  local line id file
  for line in "${KF[@]}"; do
    IFS='|' read -r id h file _ <<<"$line"
    id=$(echo "$id" | xargs)
    h=$(echo "$h" | xargs)
    file=$(echo "$file" | xargs)
    if [ ! -f "regressions/$file" ]; then
      printf '  FAIL  known failure %s: reproducer regressions/%s is missing\n' "$id" "$file"
      failed=1
    elif "$BUILD/bin/$h-replay-strict" "regressions/$file" >"$BUILD/known-$id.log" 2>&1; then
      printf '  FAIL  known failure %s no longer fails. Fixed? Remove it from KNOWN_FAILURES and drop the\n' "$id"
      printf '        matching STRICT exclusion in fuzz_%s.cpp so the property is checked by default.\n' "$h"
      failed=1
    elif ! "$BUILD/bin/$h-replay" "regressions/$file" >/dev/null 2>&1; then
      printf '  FAIL  known failure %s also fails the default build\n' "$id"
      failed=1
    else
      printf '  known %-28s still fails as recorded (%s)\n' "$id" "$(grep -m1 -o 'FUZZ_CHECK failed at [^:]*:[0-9]*' "$BUILD/known-$id.log" || echo sanitizer)"
    fi
  done

  # The hand-written edge cases (LEN extremes, stuffing at buffer boundaries, every split point).
  if "$SMOKE_CXX" -std=c++17 "${WARN[@]}" "${SAN[@]}" -I. -I"$PORT" edge_cases.cpp "$PORT/hisense_protocol.cpp" \
    -o "$BUILD/bin/edge_cases" 2>"$BUILD/bin/edge_cases.log" && "$BUILD/bin/edge_cases" >"$BUILD/edge_cases.out" 2>&1; then
    printf '  ok    %-14s %s\n' "edge_cases" "$(tail -1 "$BUILD/edge_cases.out")"
  else
    printf '  FAIL  edge_cases\n'
    cat "$BUILD/bin/edge_cases.log" "$BUILD/edge_cases.out" 2>/dev/null | tail -30 | sed 's/^/        /'
    failed=1
  fi

  if [ "$failed" = 0 ]; then
    echo "FUZZ SMOKE OK"
  else
    echo "FUZZ SMOKE FAILED"
  fi
  return "$failed"
}

# ---- fuzz ----------------------------------------------------------------------------------------
fuzz() {
  local seconds=$1
  shift
  local list=("$@") h pids=() failed=0 strict=${FUZZ_STRICT:-0}
  [ ${#list[@]} -eq 0 ] && list=("${HARNESSES[@]}")
  if ! command -v clang++ >/dev/null 2>&1; then
    echo "fuzzing needs clang++ with libFuzzer" >&2
    return 2
  fi
  read -r -a extra <<<"${FUZZ_ARGS:-}"
  echo "== fuzzing ${list[*]} for ${seconds}s each, in parallel (strict=$strict) =="
  for h in "${list[@]}"; do
    (
      bin=$(build "$h" fuzz "$strict") || exit 1
      mkdir -p "$BUILD/corpus/$h" "$BUILD/artifacts/$h"
      python3 corpus_tool.py unpack "corpus/$h.hex" "$BUILD/corpus/$h" >/dev/null
      dirs=("$BUILD/corpus/$h")
      for d in "seeds/$h" "regressions/$h"; do
        [ -d "$d" ] && dirs+=("$d")
      done
      # -max_len: the largest frame is 200 bytes; scripts for the bus harness want more room.
      "$bin" -max_total_time="$seconds" -max_len=1024 -timeout=10 -print_final_stats=1 \
        -artifact_prefix="$BUILD/artifacts/$h/" "${extra[@]}" "${dirs[@]}" >"$BUILD/fuzz-$h.log" 2>&1
    ) &
    pids+=($!)
  done
  local i=0
  for p in "${pids[@]}"; do
    wait "$p" || failed=1
    h=${list[$i]}
    i=$((i + 1))
    local log="$BUILD/fuzz-$h.log"
    local execs cov crashes
    execs=$(grep -o 'stat::number_of_executed_units: [0-9]*' "$log" | grep -o '[0-9]*$')
    cov=$(grep -o 'cov: [0-9]* ft: [0-9]*' "$log" | tail -1)
    crashes=$(find "$BUILD/artifacts/$h" -type f 2>/dev/null | wc -l)
    printf '  %-14s executions %-12s %-24s artifacts %s\n' "$h" "${execs:-?}" "${cov:-?}" "$crashes"
  done
  if [ "$failed" != 0 ]; then
    echo "At least one harness stopped on a crash. Reproducers: $BUILD/artifacts/<harness>/, logs: $BUILD/fuzz-<harness>.log"
    echo "Minimise with: $BUILD/bin/<harness>-fuzz -minimize_crash=1 -max_total_time=60 <reproducer>"
  fi
  return "$failed"
}

# ---- coverage ------------------------------------------------------------------------------------
coverage() {
  local h bins=() profs=()
  echo "== coverage of the product sources reached by seeds + corpus + regressions + build/corpus =="
  for h in "${HARNESSES[@]}"; do
    local bin
    bin=$(build "$h" cov 0) || return 1
    LLVM_PROFILE_FILE="$BUILD/$h.profraw" "$bin" "seeds/$h" "corpus/$h.hex" "regressions/$h" "$BUILD/corpus/$h" \
      >/dev/null 2>&1 || echo "  (replay of $h exited non-zero)"
    bins+=(-object "$bin")
    profs+=("$BUILD/$h.profraw")
  done
  llvm-profdata merge -o "$BUILD/all.profdata" "${profs[@]}" || return 1
  # The first binary is positional for llvm-cov, the rest are -object.
  local first=${bins[1]}
  llvm-cov report "$first" "${bins[@]:2}" -instr-profile="$BUILD/all.profdata" \
    "$PORT/hisense_protocol.cpp" "$PORT/hisense_bus.cpp" "$PORT/hisense_protocol.h"
  echo
  echo "Lines never executed (hisense_protocol.cpp, hisense_bus.cpp):"
  llvm-cov show "$first" "${bins[@]:2}" -instr-profile="$BUILD/all.profdata" \
    "$PORT/hisense_protocol.cpp" "$PORT/hisense_bus.cpp" -show-line-counts-or-regions 2>/dev/null |
    awk -F'|' '/^\/|^[^ ].*:$/ { file=$0 } $2 ~ /^ *0$/ { print "  " file " line" $1 ":" $3 }' | sed 's/ \+/ /g'
}

# ---- merge ---------------------------------------------------------------------------------------
merge() {
  local h
  for h in "${HARNESSES[@]}"; do
    [ -d "$BUILD/corpus/$h" ] || [ -f "corpus/$h.hex" ] || continue
    local bin
    bin=$(build "$h" fuzz 0) || return 1
    python3 corpus_tool.py unpack "corpus/$h.hex" "$BUILD/corpus/$h" >/dev/null
    rm -rf "$BUILD/merged/$h"
    mkdir -p "$BUILD/merged/$h"
    # Seeds first: an input that adds no coverage over the seeds is not kept.
    "$bin" -set_cover_merge=1 -max_len=1024 "$BUILD/merged/$h" "seeds/$h" "$BUILD/corpus/$h" >"$BUILD/merge-$h.log" 2>&1
    printf '  %-14s %s inputs in corpus/%s.hex\n' "$h" \
      "$(python3 corpus_tool.py pack "$BUILD/merged/$h" "corpus/$h.hex" "seeds/$h")" "$h"
  done
}

mkdir -p "$BUILD"
case "${1:-}" in
  --smoke) smoke ;;
  --coverage) coverage ;;
  --merge) merge ;;
  '' | -h | --help) usage ;;
  *[!0-9]*) usage ;;
  *) fuzz "$@" ;;
esac
