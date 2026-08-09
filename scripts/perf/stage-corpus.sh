#!/usr/bin/env bash
# Stage a corpus split onto native disk and print the unit-list path.
#
#   stage-corpus.sh [split] [repeats]
#
# Profiling or timing a workload that lives on /mnt (9p) inflates fopen/fseek
# to a third of CPU time and buries the engine (measured 2026-08-10). Every
# W8 measurement runs off /tmp instead. `repeats` concatenates the list N
# times to lengthen a short run for sampling statistics; it does not change
# per-unit cost.
set -eu

split="${1:-val}"
repeats="${2:-1}"

root=$(cd "$(dirname "$0")/../.." && pwd)
units="$root/out/corpus/$split/units"

if [ ! -d "$units" ]; then
    echo "error: $units missing. Extract it first:" >&2
    echo "  python tools/corpus/extract.py --corpus \$QL_CORPUS --split $split --out out/corpus/$split" >&2
    exit 1
fi

stage="/tmp/qlperf-$split"
list="/tmp/qlperf-$split-x$repeats.txt"

if [ ! -d "$stage" ] || [ "$units" -nt "$stage" ]; then
    rm -rf "$stage"
    cp -r "$units" "$stage"
fi

ls "$stage" | sed "s|^|$stage/|" > "$list.1"
: > "$list"
i=1
while [ "$i" -le "$repeats" ]; do
    cat "$list.1" >> "$list"
    i=$((i + 1))
done
rm -f "$list.1"

echo "$list"
