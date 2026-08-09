#!/usr/bin/env bash
# The same judgements across processes instead of threads.
#
#   bench-processes.sh [pairs-per-worker] [worker,worker,...]
#
# check_batch stops scaling at 4.65x where independent parse-and-lower
# processes reach 10.97x (docs/perf/baseline.md). Two things could produce
# that: contention on something inside our process, or a ceiling that belongs
# to Bitwuzla and the memory system and would bind any arrangement.
#
# Separate processes share no interpreter, no allocator, no core global. If
# processes scale where threads did not, the contention is ours. If processes
# stop at the same place, it is not, and the batch API is already delivering
# what this workload can deliver.
set -eu

cd "$(dirname "$0")/../.."
root=$(pwd)

per="${1:-6}"
if [ -n "${2:-}" ]; then
    ladder=$(printf '%s' "$2" | tr ',' ' ')
else
    ladder="1 2 4 8 16"
fi

: "${PYTHONPATH:=$root/out/build/linux-clang/bindings/python/package}"
export PYTHONPATH

printf 'cpus       %s\n' "$(nproc)"
printf 'pairs      %s per worker, distinct across workers\n\n' "$per"
printf '%8s %10s %12s %9s\n' workers 'wall s' 'pairs/s' speedup

base=""
for workers in $ladder; do
    start=$(date +%s%N)
    i=0
    while [ "$i" -lt "$workers" ]; do
        # Distinct offsets so no two workers judge the same pair.
        python3 "$root/scripts/perf/bench-batch.py" serial "$per" \
            $((i * per + 1000)) > /dev/null &
        i=$((i + 1))
    done
    wait
    end=$(date +%s%N)
    wall=$(( (end - start) / 1000000 ))
    total=$((per * workers))
    if [ -z "$base" ]; then
        base=$wall
    fi
    awk -v w="$workers" -v ms="$wall" -v t="$total" -v b="$base" 'BEGIN {
        printf "%8d %10.3f %12.1f %8.2fx\n", w, ms / 1000, t / (ms / 1000),
               (w * b) / ms
    }'
done

echo
echo "Speedup is aggregate throughput against one process. Each worker pays"
echo "its own interpreter start and backend probe, so the absolute pairs/s"
echo "sits below the threaded ladder; the shape of the curve is the point."
