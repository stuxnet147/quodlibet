#!/usr/bin/env bash
# How much parallel headroom does this machine have for our kind of work?
#
#   bench-scaling.sh [split] [worker,worker,...]
#
# The batch judgement path stops scaling at 4.67x on twenty-four logical
# cores (docs/perf/baseline.md). That number means nothing until we know what
# this machine gives a workload that is definitely not contending on anything
# of ours: K independent processes, each parsing and lowering the whole split,
# sharing no state, no locks, no pipes, no children.
#
# If this control also caps near 4.67x, the batch path is at the machine's
# limit and there is nothing to fix. If it scales further, the gap is ours.
set -eu

cd "$(dirname "$0")/../.."
root=$(pwd)

split="${1:-val}"
exe="${QL_PERF_EXE:-$root/out/build/linux-clang/quodlibet}"

if [ -n "${2:-}" ]; then
    ladder=$(printf '%s' "$2" | tr ',' ' ')
else
    ladder="1 2 4 8 16"
fi

list=$("$root/scripts/perf/stage-corpus.sh" "$split" 1)
units=$(wc -l < "$list")

printf 'cpus       %s\n' "$(nproc)"
printf 'split      %s (%s units per process)\n\n' "$split" "$units"
printf '%8s %10s %12s %9s\n' workers 'wall s' 'units/s' speedup

base=""
for workers in $ladder; do
    start=$(date +%s%N)
    i=1
    while [ "$i" -le "$workers" ]; do
        "$exe" coverage "$list" > /dev/null &
        i=$((i + 1))
    done
    wait
    end=$(date +%s%N)
    wall=$(( (end - start) / 1000000 ))
    total=$((units * workers))
    if [ -z "$base" ]; then
        base=$wall
    fi
    awk -v w="$workers" -v ms="$wall" -v t="$total" -v b="$base" 'BEGIN {
        printf "%8d %10.3f %12.0f %8.2fx\n", w, ms / 1000, t / (ms / 1000),
               (w * b) / ms
    }'
done

echo
echo "Speedup is aggregate throughput against one process. These processes"
echo "share nothing, so this is the ceiling any of our parallel paths can"
echo "reach on this machine for parse-and-lower shaped work."
