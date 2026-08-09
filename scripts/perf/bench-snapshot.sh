#!/usr/bin/env bash
# What share of a judgement is the private Bitwuzla snapshot?
#
#   bench-snapshot.sh [iterations] [worker,worker,...]
#
# Builds and runs scripts/perf/snapshot_probe.c, which replays the file work
# one judgement does for the snapshot and nothing else. Set the result against
# the 87 ms per judgement in docs/perf/baseline.md.
#
# The ladder matters as much as the single number: the snapshot touches the
# filesystem, which every worker shares, so a cost that is small alone can
# still be what stops the batch scaling.
set -eu

cd "$(dirname "$0")/../.."
root=$(pwd)

iterations="${1:-20}"
if [ -n "${2:-}" ]; then
    ladder=$(printf '%s' "$2" | tr ',' ' ')
else
    ladder="1 8 16"
fi

source_exe="${QL_BITWUZLA:-$root/third_party/bitwuzla-linux-x86_64/bin/bitwuzla}"
if [ ! -x "$source_exe" ]; then
    echo "error: $source_exe is not executable. Set QL_BITWUZLA." >&2
    exit 1
fi

probe=/tmp/quodlibet-snapshot-probe
cc="${CC:-clang}"
# Build BLAKE3 the way the library builds it (third_party/CMakeLists.txt), or
# the probe measures a different program than the one being profiled. Set
# QL_PROBE_PORTABLE=1 to force the pre-SIMD build for a before/after pair; the
# digest is printed either way so the two can be shown to agree.
b3="$root/third_party/blake3/c"
b3_sources="$b3/blake3.c $b3/blake3_dispatch.c $b3/blake3_portable.c"
b3_flags="-DBLAKE3_USE_NEON=0"
if [ -n "${QL_PROBE_PORTABLE:-}" ]; then
    b3_flags="$b3_flags -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41"
    b3_flags="$b3_flags -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512"
    printf 'blake3     portable (QL_PROBE_PORTABLE set)\n'
else
    b3_sources="$b3_sources $b3/blake3_sse2_x86-64_unix.S"
    b3_sources="$b3_sources $b3/blake3_sse41_x86-64_unix.S"
    b3_sources="$b3_sources $b3/blake3_avx2_x86-64_unix.S"
    b3_sources="$b3_sources $b3/blake3_avx512_x86-64_unix.S"
    printf 'blake3     x86-64 assembly, as the library builds it\n'
fi
# shellcheck disable=SC2086
"$cc" -O2 -o "$probe" "$root/scripts/perf/snapshot_probe.c" $b3_sources \
    $b3_flags -I "$b3"

# A worktree on /mnt is reached over 9p, so a snapshot read from there also
# measures the share protocol. Stage a native-disk copy and report both, or
# the 9p toll gets reported as the snapshot's cost.
native_exe=/tmp/quodlibet-snapshot-source
cp "$source_exe" "$native_exe"
chmod +x "$native_exe"

printf 'snapshot   the OS temp directory, as uv_os_tmpdir reports it\n\n'

for label_path in "as configured:$source_exe" "native source:$native_exe"; do
    label=${label_path%%:*}
    path=${label_path#*:}

    echo "=== $label ==="
    printf '  %s\n' "$path"
    "$probe" "$path" "$iterations" | sed 's/^/  /'

    echo
    printf '  %8s %14s\n' workers 'ms/judgement'
    for workers in $ladder; do
        pids=""
        i=0
        while [ "$i" -lt "$workers" ]; do
            "$probe" "$path" "$iterations" > "/tmp/snapprobe.$i" &
            pids="$pids $!"
            i=$((i + 1))
        done
        for pid in $pids; do
            if ! wait "$pid"; then
                echo "error: a probe worker failed at workers=$workers" >&2
                exit 1
            fi
        done
        # Mean across workers; each worker's total already averages its
        # own iterations.
        awk -v w="$workers" '
            /^total/ { sum += $2; n += 1 }
            END { printf "  %8d %14.3f\n", w, sum / n }
        ' /tmp/snapprobe.*
        rm -f /tmp/snapprobe.*
    done
    echo
done

rm -f "$native_exe"
