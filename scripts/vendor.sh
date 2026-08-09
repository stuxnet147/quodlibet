#!/usr/bin/env sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
vendor_dir="$project_dir/third_party"
temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/quodlibet-vendor.XXXXXX")

cleanup() {
    rm -rf -- "$temp_dir"
}
trap cleanup EXIT HUP INT TERM

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'error: required tool not found: %s\n' "$1" >&2
        exit 1
    fi
}

require_tool curl
require_tool sha256sum
require_tool tar

fetch() {
    name=$1
    version=$2
    expected_sha256=$3
    url=$4
    target="$vendor_dir/$name"
    marker="$target/.quodlibet-vendor"

    if [ -d "$target" ]; then
        if [ -f "$marker" ] &&
           grep -Fx "version=$version" "$marker" >/dev/null 2>&1 &&
           grep -Fx "sha256=$expected_sha256" "$marker" >/dev/null 2>&1; then
            printf '%s %s already present\n' "$name" "$version"
            return
        fi
        printf 'error: %s exists but does not match the lock; remove it explicitly before updating\n' "$target" >&2
        exit 1
    fi

    archive="$temp_dir/$name.tar.gz"
    stage="$temp_dir/$name"
    printf 'fetching %s %s\n' "$name" "$version"
    curl -fL --retry 3 --connect-timeout 15 --max-time 180 \
        -o "$archive" "$url"
    printf '%s  %s\n' "$expected_sha256" "$archive" | sha256sum -c -

    mkdir -p "$stage"
    tar -xzf "$archive" --strip-components=1 -C "$stage"
    {
        printf 'version=%s\n' "$version"
        printf 'sha256=%s\n' "$expected_sha256"
        printf 'source=%s\n' "$url"
    } >"$stage/.quodlibet-vendor"
    mv -- "$stage" "$target"
}

mkdir -p "$vendor_dir"

fetch libuv v1.52.1 \
    66d511b9e6e334c0e62279eb234fbfb2b3110b1479c09b95b44c7afca8cff9e7 \
    https://dist.libuv.org/dist/v1.52.1/libuv-v1.52.1.tar.gz
fetch yyjson 0.12.0 \
    b16246f617b2a136c78d73e5e2647c6f1de1313e46678062985bdcf1f40bb75d \
    https://codeload.github.com/ibireme/yyjson/tar.gz/refs/tags/0.12.0
fetch xxhash v0.8.3 \
    aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80 \
    https://codeload.github.com/Cyan4973/xxHash/tar.gz/refs/tags/v0.8.3
fetch googletest v1.17.0 \
    65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c \
    https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.17.0
fetch blake3 1.8.6 \
    da7b5b0b6cf7106fe54b7d718d1ea371cce434cd15ebe5e56ca011b645cbef0e \
    https://codeload.github.com/BLAKE3-team/BLAKE3/tar.gz/refs/tags/1.8.6
fetch tree-sitter v0.26.12 \
    428e2b182fe38eddc100d8bd851e47c96921a69281b66abafc25ba4b0aaeeeab \
    https://codeload.github.com/tree-sitter/tree-sitter/tar.gz/refs/tags/v0.26.12
fetch tree-sitter-c v0.24.2 \
    2eeb4db31f8fa0865e45488503d13403923bcb485a1bdb637abff8c42dd97364 \
    https://codeload.github.com/tree-sitter/tree-sitter-c/tar.gz/refs/tags/v0.24.2

printf 'all required sources are present in %s\n' "$vendor_dir"
