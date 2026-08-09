#!/usr/bin/env bash
# One bounded VTune hotspots collection with call attribution.
#
#   vtune-hotspots.sh [split] [repeats]
#   QL_PERF_WORKLOAD="./out/build/linux-clang/quodlibet methods" vtune-hotspots.sh
#
# Runs on Linux (WSL Ubuntu-24.04 or a VM). Windows VTune is dead on this
# host; docs/perf/baseline.md records the five probes that established that.
# Needs `sudo sysctl -w kernel.yama.ptrace_scope=0` once per boot, otherwise
# collection fails immediately with "Cannot start data collection".
#
# Stack collection is on, because the flat function list cannot tell the
# frontend's parse from the lowering's parse and that split is the whole
# question in this workload.
#
# The caller runs this in the background and enforces the 120-second bound by
# hand; background timeouts are not enforced by the harness.
set -eu

cd "$(dirname "$0")/../.."
root=$(pwd)

split="${1:-val}"
repeats="${2:-10}"
vtune="${QL_VTUNE:-/opt/intel/oneapi/vtune/latest/bin64/vtune}"
result="${QL_PERF_RESULT:-/tmp/qlperf-vtune}"

if [ ! -x "$vtune" ]; then
    echo "error: $vtune missing. Set QL_VTUNE." >&2
    exit 1
fi

if [ -n "${QL_PERF_WORKLOAD:-}" ]; then
    workload="$QL_PERF_WORKLOAD"
else
    exe="${QL_PERF_EXE:-$root/out/build/linux-clang/quodlibet}"
    list=$("$root/scripts/perf/stage-corpus.sh" "$split" "$repeats")
    workload="$exe coverage $list"
fi

rm -rf "$result"
# shellcheck disable=SC2086
"$vtune" -collect hotspots -knob sampling-mode=sw \
    -knob enable-stack-collection=true -result-dir "$result" \
    -- $workload > "$result.log" 2>&1 || { tail -20 "$result.log" >&2; exit 1; }

"$vtune" -report summary -r "$result" 2>/dev/null |
    grep -E 'Elapsed|CPU Time' | head -3

echo
echo "=== call tree (share of CPU, depth <= 6) ==="
"$vtune" -report top-down -r "$result" -format csv -csv-delimiter=tab 2>/dev/null |
    awk -F'\t' 'NR > 1 {
        indent = $1
        sub(/[^ ].*/, "", indent)
        if (length(indent) <= 6) printf "%-68s %6.2f%%\n", $1, $2
    }'

echo
echo "=== flat, self time ==="
"$vtune" -report hotspots -r "$result" -format csv -csv-delimiter=tab 2>/dev/null |
    awk -F'\t' 'NR > 1 { printf "%-44s %-14s %8.3fs\n", $1, $6, $2 }' | head -20
