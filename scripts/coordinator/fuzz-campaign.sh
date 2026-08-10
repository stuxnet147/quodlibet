#!/usr/bin/env bash
# One bounded libFuzzer campaign per target. Run from the repo root on Linux.
set -u
cd "$(dirname "$0")/../.."
total=0
failed=0
log=$(mktemp "${TMPDIR:-/tmp}/quodlibet-fuzz-campaign.XXXXXX")
trap 'rm -f "$log"' EXIT HUP INT TERM

for f in out/build/linux-fuzz/fuzz_*; do
    [ -x "$f" ] || continue
    name="$(basename "$f")"
    echo "== $name"
    if timeout 70 "$f" -max_total_time=60 -print_final_stats=1 \
        >"$log" 2>&1; then
        code=0
    else
        code=$?
    fi
    grep -E "DONE|cov:|exec/s|ERROR|SUMMARY|CRASH" "$log" | tail -5 || true
    echo "   exit=$code"
    if [ "$code" -ne 0 ] ||
        grep -Eq "ERROR: (AddressSanitizer|UndefinedBehaviorSanitizer|libFuzzer)|SUMMARY: .*Sanitizer|Test unit written to" "$log"; then
        failed=$((failed+1))
    fi
    total=$((total+1))
done
echo "targets run: $total"
echo "targets failed: $failed"

if [ "$total" -eq 0 ] || [ "$failed" -ne 0 ]; then
    exit 1
fi
