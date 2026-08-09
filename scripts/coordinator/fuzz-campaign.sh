#!/usr/bin/env bash
# One bounded libFuzzer campaign per target. Run from the repo root on Linux.
set -u
cd "$(dirname "$0")/../.."
total=0
for f in out/build/linux-fuzz/fuzz_*; do
    name="$(basename "$f")"
    echo "== $name"
    timeout 70 "$f" -max_total_time=60 -print_final_stats=1 2>&1 \
        | grep -E "DONE|cov:|exec/s|ERROR|SUMMARY|CRASH" | tail -5
    code=$?
    echo "   exit=$code"
    total=$((total+1))
done
echo "targets run: $total"
