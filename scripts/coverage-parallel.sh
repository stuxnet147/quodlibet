#!/usr/bin/env bash
# Runs a unit list through a dynamically scheduled process pool and sums the
# per-batch JSON.
#
#   scripts/coverage-parallel.sh <units.txt> [detail.tsv] [workers]
#
# Each coverage process is single-threaded. More batches than workers keep a
# large or pathological unit from leaving every other core idle at the tail.
set -euo pipefail

list=${1:?usage: coverage-parallel.sh <units.txt> [detail.tsv] [workers]}
detail=${2:-}
if [ "$#" -ge 3 ]; then
    workers=$3
elif [ -n "${QL_WORKERS:-}" ]; then
    workers=$QL_WORKERS
elif command -v getconf >/dev/null 2>&1; then
    workers=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
else
    workers=
fi
if [ -z "${workers:-}" ] && command -v nproc >/dev/null 2>&1; then
    workers=$(nproc)
fi
workers=${workers:-1}
exe=${QL_EXE:-./out/build/windows-clang/quodlibet.exe}

case $workers in
    ''|*[!0-9]*|0)
        echo "error: workers must be a positive integer" >&2
        exit 2
        ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

total=$(wc -l < "$list")
if [ "$total" -eq 0 ]; then
    echo "error: unit list is empty" >&2
    exit 2
fi
multiplier=${QL_BATCH_MULTIPLIER:-8}
case $multiplier in
    ''|*[!0-9]*|0)
        echo "error: QL_BATCH_MULTIPLIER must be a positive integer" >&2
        exit 2
        ;;
esac
batches=$((workers * multiplier))
max_units_per_batch=${QL_MAX_UNITS_PER_BATCH:-64}
case $max_units_per_batch in
    ''|*[!0-9]*|0)
        echo "error: QL_MAX_UNITS_PER_BATCH must be a positive integer" >&2
        exit 2
        ;;
esac
minimum_batches=$(((total + max_units_per_batch - 1) / max_units_per_batch))
if [ "$batches" -lt "$minimum_batches" ]; then
    batches=$minimum_batches
fi
if [ "$batches" -gt "$total" ]; then
    batches=$total
fi
per=$(((total + batches - 1) / batches))
split -l "$per" -d -a 6 "$list" "$work/batch."
part_count=$(find "$work" -maxdepth 1 -type f -name 'batch.??????' | wc -l)
echo "coverage: units=$total workers=$workers batches=$part_count" >&2

run_coverage_part() {
    part=$1
    part_arg=$part
    detail_arg=$part.tsv
    if command -v cygpath >/dev/null 2>&1; then
        part_arg=$(cygpath -w "$part")
        detail_arg=$(cygpath -w "$part.tsv")
    fi
    if [ -n "$detail" ]; then
        "$exe" coverage "$part_arg" "$detail_arg" > "$part.json" \
            2> "$part.err"
    else
        "$exe" coverage "$part_arg" > "$part.json" 2> "$part.err"
    fi
}
export detail exe
export -f run_coverage_part

set +e
find "$work" -maxdepth 1 -type f -name 'batch.??????' -print0 | sort -z | \
    xargs -0 -r -n 1 -P "$workers" bash -c 'run_coverage_part "$1"' _
pool_status=$?
set -e
if [ "$pool_status" -ne 0 ]; then
    cat "$work"/batch.*.err >&2
    echo "error: at least one coverage batch failed" >&2
    exit "$pool_status"
fi

if [ -n "$detail" ]; then
    cat "$work"/batch.*.tsv > "$detail"
fi

if [ -n "${PYTHON:-}" ]; then
    python_cmd=$PYTHON
elif command -v python3 >/dev/null 2>&1; then
    python_cmd=python3
else
    python_cmd=python
fi

"$python_cmd" - "$work" "$part_count" <<'PY'
import glob, json, sys

total = {}
diagnostics = {}
frontend = {}
paths = sorted(glob.glob(sys.argv[1] + '/batch.*.json'))
expected = int(sys.argv[2])
if len(paths) != expected:
    raise SystemExit(
        f'expected {expected} batch results, found {len(paths)}')
for path in paths:
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
