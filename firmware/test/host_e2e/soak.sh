#!/usr/bin/env bash
# The default scenarios several times in a row, one line per run: how to look for a flaky check.
#
#   firmware/test/host_e2e/soak.sh [runs] [run.py arguments]     default: 10 runs
#
# Builds once, then runs one after another, never side by side. Each run's output goes to a file
# in a temporary directory; for a run that fails, everything but its passed checks is printed.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
runs="${1:-10}"
[ "$#" -gt 0 ] && shift
out="$(mktemp -d -t hisense-e2e-soak.XXXXXX)"
python3 "$here/run.py" boot "$@" > "$out/build.log" 2>&1 || { tail -n 40 "$out/build.log"; exit 1; }
failed=0
for i in $(seq 1 "$runs"); do
  python3 "$here/run.py" --no-build "$@" > "$out/run-$i.log" 2>&1
  rc=$?
  echo "run $i: rc=$rc $(tail -n 1 "$out/run-$i.log")"
  if [ "$rc" != 0 ]; then
    failed=$((failed + 1))
    grep -v '^  ok ' "$out/run-$i.log" | cut -c1-600
  fi
done
echo "== soak: $((runs - failed)) of $runs runs passed (logs in $out) =="
[ "$failed" = 0 ]
