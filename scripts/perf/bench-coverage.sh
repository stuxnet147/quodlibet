#!/usr/bin/env bash
# Wall-clock regression gate for the parse + frontend + lowering path.
#
#   bench-coverage.sh [split] [repeats]
#
# Reports the minimum of `repeats` runs, because the minimum is the estimate
# least polluted by scheduler noise, plus the coverage JSON digest so a
# speedup that changed the measurement is caught in the same command. A
# digest change is a correctness regression, not a win.
#
# This is the G6 regression gate. It needs no profiler, so it runs anywhere
# the binary runs. VTune answers "where does the time go"; this answers "did
# the total move". docs/perf/baseline.md holds the recorded numbers.
set -eu

cd "$(dirname "$0")/../.."
root=$(pwd)

split="${1:-val}"
repeats="${2:-5}"
exe="${QL_PERF_EXE:-$root/out/build/linux-clang/quodlibet}"

if [ ! -x "$exe" ]; then
    echo "error: $exe is not executable. Build it or set QL_PERF_EXE." >&2
    exit 1
fi

list=$("$root/scripts/perf/stage-corpus.sh" "$split" 1)
units=$(wc -l < "$list")

best=""
run=1
while [ "$run" -le "$repeats" ]; do
    start=$(date +%s%N)
    "$exe" coverage "$list" > "/tmp/qlperf-cov-$split.json"
    end=$(date +%s%N)
    ms=$(( (end - start) / 1000000 ))
    if [ -z "$best" ] || [ "$ms" -lt "$best" ]; then
        best=$ms
    fi
    echo "  run $run: ${ms} ms"
    run=$((run + 1))
done

digest=$(cksum < "/tmp/qlperf-cov-$split.json" | cut -d' ' -f1)

echo
echo "split          $split"
echo "units          $units"
echo "best wall      ${best} ms"
awk -v b="$best" -v u="$units" 'BEGIN { printf "per unit       %.4f ms\n", b / u }'
echo "result digest  $digest"
echo
echo "Compare against docs/perf/baseline.md. A changed digest means the"
echo "measurement itself moved and the timing comparison is void."
