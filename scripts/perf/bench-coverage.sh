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

result="/tmp/qlperf-cov-$split.json"

# Linux-on-WSL needs the corpus copied off the 9p mount before timing. A native
# Windows executable is already reading native NTFS and cannot open MSYS /tmp
# paths, so use the extracted corpus list in place.
case "$exe" in
    *.exe)
        list="$root/out/corpus/$split/units.txt"
        if [ ! -f "$list" ]; then
            echo "error: $list is missing. Extract the corpus first." >&2
            exit 1
        fi
        coverage_list=$(cygpath -w "$list")
        ;;
    *)
        list=$("$root/scripts/perf/stage-corpus.sh" "$split" 1)
        coverage_list="$list"
        ;;
esac
units=$(wc -l < "$list")

best=""
run=1
while [ "$run" -le "$repeats" ]; do
    start=$(date +%s%N)
    "$exe" coverage "$coverage_list" > "$result"
    end=$(date +%s%N)
    ms=$(( (end - start) / 1000000 ))
    if [ -z "$best" ] || [ "$ms" -lt "$best" ]; then
        best=$ms
    fi
    echo "  run $run: ${ms} ms"
    run=$((run + 1))
done

digest=$(cksum < "$result" | cut -d' ' -f1)
definitions=$(sed -n 's/.*"definitions": \([0-9][0-9]*\).*/\1/p' "$result")
lowered=$(sed -n 's/.*"definitions_lowered": \([0-9][0-9]*\).*/\1/p' "$result")
verified=$(sed -n 's/.*"definitions_lower_verified": \([0-9][0-9]*\).*/\1/p' "$result")
unreadable=$(sed -n 's/.*"units_unreadable": \([0-9][0-9]*\).*/\1/p' "$result")

if [ -z "$definitions" ] || [ -z "$lowered" ] || [ -z "$verified" ] ||
    [ -z "$unreadable" ]; then
    echo "error: coverage output is missing G8 measurement fields" >&2
    exit 1
fi
if [ "$unreadable" -ne 0 ]; then
    echo "error: coverage could not read $unreadable staged units" >&2
    exit 1
fi

echo
echo "split          $split"
echo "units          $units"
echo "functions      $definitions"
echo "lowered        $lowered"
echo "verified       $verified"
echo "best wall      ${best} ms"
awk -v b="$best" -v u="$units" 'BEGIN { printf "per unit       %.4f ms\n", b / u }'
awk -v b="$best" -v f="$definitions" \
    'BEGIN { printf "functions/s    %.2f\n", f * 1000 / b }'
echo "result digest  $digest"
echo
echo "Compare against docs/perf/baseline.md. A changed digest means the"
echo "measurement itself moved and the timing comparison is void."
