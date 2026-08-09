#!/usr/bin/env sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)

if [ "$#" -gt 1 ]; then
    printf 'usage: %s [cmake-preset]\n' "$0" >&2
    exit 2
fi

if [ "$#" -eq 1 ]; then
    preset=$1
else
    case "$(uname -s)" in
        Linux*) preset=linux-clang ;;
        MINGW*|MSYS*|CYGWIN*) preset=windows-clang ;;
        *)
            printf 'error: unsupported host for a built-in preset: %s\n' "$(uname -s)" >&2
            exit 1
            ;;
    esac
fi

cd "$project_dir"
cmake --preset "$preset"
cmake --build --preset "$preset" --parallel
ctest --preset "$preset"
