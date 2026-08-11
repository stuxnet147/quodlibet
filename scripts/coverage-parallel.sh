#!/usr/bin/env bash
# Shards a unit list across processes and sums the per-shard JSON.
#
#   scripts/coverage-parallel.sh <units.txt> [detail.tsv] [shards]
#
# The coverage command is single-threaded and a train pass costs about a
# minute and a half of one core. Each unit is independent, so the only reason
# to pay that serially is not having split the list.
set -euo pipefail

list=${1:?usage: coverage-parallel.sh <units.txt> [detail.tsv] [shards]}
detail=${2:-}
shards=${3:-12}
exe=${QL_EXE:-./out/build/windows-clang/quodlibet.exe}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

total=$(wc -l < "$list")
per=$(((total + shards - 1) / shards))
split -l "$per" -d -a 3 "$list" "$work/part."

pids=()
for part in "$work"/part.*; do
    if [ -n "$detail" ]; then
        "$exe" coverage "$(cygpath -w "$part" 2>/dev/null || echo "$part")" \
            "$(cygpath -w "$part.tsv" 2>/dev/null || echo "$part.tsv")" \
            > "$part.json" &
    else
        "$exe" coverage \
            "$(cygpath -w "$part" 2>/dev/null || echo "$part")" \
            > "$part.json" &
    fi
    pids+=($!)
done
for pid in "${pids[@]}"; do
    wait "$pid"
done

if [ -n "$detail" ]; then
    cat "$work"/part.*.tsv > "$detail"
fi

python - "$work" <<'PY'
import glob, json, sys

total = {}
diagnostics = {}
frontend = {}
for path in sorted(glob.glob(sys.argv[1] + '/part.*.json')):
    with open(path, encoding='utf-8') as handle:
        shard = json.load(handle)
    for key, value in shard.items():
        if key == 'schema_version':
            total[key] = value
        elif key == 'lower_diagnostics':
            for name, count in value.items():
                diagnostics[name] = diagnostics.get(name, 0) + count
        elif key == 'frontend_diagnostics':
            for name, count in value.items():
                frontend[name] = frontend.get(name, 0) + count
        else:
            total[key] = total.get(key, 0) + value
total['frontend_diagnostics'] = frontend
total['lower_diagnostics'] = diagnostics
print(json.dumps(total, indent=2))
PY
