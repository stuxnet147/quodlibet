#!/usr/bin/env bash
# What `cmake --install` actually produces, and whether it survives being moved.
#
#   scripts/check-install.sh [preset]
#
# Two questions, and the script answers both by doing rather than by reading
# the CMake:
#
#   1. Is the install self-contained? A consumer compiled against the installed
#      headers and linked against the installed library either links or it does
#      not.
#   2. Is it relocatable? The whole prefix is moved to a different path and the
#      consumer is run from there. Bitwuzla must be found beside the running
#      executable, not through the absolute path baked in at build time.
#
# Question 2 needs care on a developer's machine: the build-time default still
# exists there, so a lookup that quietly fell back to it would look like
# success. The check is therefore negative as well as positive - the adjacent
# copy is replaced with something that is not Bitwuzla, and the run must fail.
# If it succeeded, the adjacent copy was not what was used.
#
# Nothing here is hidden: every check prints PASS or FAIL with what it saw, and
# the script exits non-zero if any FAILed.

set -u

cd "$(dirname "$0")/.."
root=$(pwd)

preset="${1:-}"
if [ -z "$preset" ]; then
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) preset=windows-clang ;;
        *) preset=linux-clang ;;
    esac
fi

build="$root/out/build/$preset"
stage="$root/out/install-check/stage"
moved="$root/out/install-check/moved"
work="$root/out/install-check/work"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) exe=".exe"; bw="bitwuzla.exe" ;;
    *) exe=""; bw="bitwuzla" ;;
esac

failures=0

pass() { printf 'PASS  %s\n' "$1"; }
fail() { printf 'FAIL  %s\n' "$1"; failures=$((failures + 1)); }
note() { printf '      %s\n' "$1"; }

if [ ! -d "$build" ]; then
    echo "error: no build at $build; run cmake --build --preset $preset first" >&2
    exit 2
fi

echo "preset     $preset"
echo "build      $build"
echo

rm -rf "$root/out/install-check"
mkdir -p "$work"

# --------------------------------------------------------------------------
echo "== install"
if ! cmake --install "$build" --prefix "$stage" > "$work/install.log" 2>&1; then
    fail "cmake --install"
    tail -20 "$work/install.log"
    exit 1
fi
pass "cmake --install"

for expected in \
    "bin/quodlibet$exe" \
    "include/quodlibet/quodlibet.h" \
    "include/quodlibet/solver.h" \
    "include/quodlibet/plugin.h"
do
    if [ -e "$stage/$expected" ]; then
        pass "installed $expected"
    else
        fail "missing $expected"
    fi
done

if [ -e "$stage/bin/$bw" ]; then
    pass "installed bin/$bw beside the executables"
else
    fail "missing bin/$bw; the relocatable lookup has nothing to find"
fi

library=$(ls "$stage"/lib/*quodlibet* 2>/dev/null | head -1)
if [ -n "$library" ]; then
    pass "installed $(basename "$library")"
else
    fail "no quodlibet library under lib/"
fi

# Anything installed outside the three standard directories is worth naming:
# a prefix is shared with other packages and a stray directory at its root is
# a collision waiting to happen.
stray=$(cd "$stage" && find . -mindepth 1 -maxdepth 1 -type d \
    ! -name bin ! -name lib ! -name include 2>/dev/null)
if [ -z "$stray" ]; then
    pass "nothing installed outside bin, lib and include"
else
    fail "installed outside the standard directories: $(echo $stray)"
fi
echo

# --------------------------------------------------------------------------
echo "== relocate"
mv "$stage" "$moved"
pass "moved the prefix to a different path"
echo

# --------------------------------------------------------------------------
echo "== a consumer built against the installed artifacts"
consumer_src="$root/scripts/install/consumer.c"
installed_link=0
if [ -n "$library" ]; then
    library="$moved/lib/$(basename "$library")"
    if "${CC:-clang}" -std=c17 -I "$moved/include" "$consumer_src" \
        -o "$work/consumer-installed$exe" "$library" \
        > "$work/link-installed.log" 2>&1; then
        installed_link=1
        pass "links against the installed library alone"
    else
        fail "does not link against the installed library alone"
        note "the vendored dependencies are neither bundled into the archive"
        note "nor installed beside it, and no CMake package config names them"
        note "first undefined symbols:"
        grep -o "undefined symbol: [^ ]*" "$work/link-installed.log" \
            | head -5 | sed 's/^/      /'
    fi
fi
echo

# --------------------------------------------------------------------------
# The relocatable lookup is a property of the library, not of how a consumer
# was linked, so it is still worth answering when the link above failed. The
# probe is then built against the build tree and run from the moved prefix.
echo "== relocatable Bitwuzla lookup"
probe=""
if [ "$installed_link" -eq 1 ]; then
    probe="$work/consumer-installed$exe"
else
    vendored=$(find "$build/third_party" -name '*.lib' -o -name '*.a' 2>/dev/null)
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            system="-lws2_32 -liphlpapi -luserenv -ldbghelp -lole32 -loleaut32
                    -lshell32 -luuid -ladvapi32 -luser32 -lgdi32" ;;
        *) system="-lpthread -ldl -lm" ;;
    esac
    # The static archive, named explicitly. `ls` with several globs sorts the
    # combined list, so a build that also emits quodlibet.lib beside
    # quodlibet_static.lib would hand back the one carrying none of the
    # objects and the probe would fail to link for the wrong reason.
    core=""
    for candidate in \
        "$build/quodlibet_static.lib" \
        "$build/libquodlibet_static.a" \
        "$build/libquodlibet.a" \
        "$build/quodlibet.lib"
    do
        if [ -e "$candidate" ]; then
            core="$candidate"
            break
        fi
    done
    if [ -n "$core" ] && "${CC:-clang}" -std=c17 -I "$root/include" \
        "$consumer_src" -o "$work/consumer-build$exe" \
        $core $vendored $system > "$work/link-build.log" 2>&1; then
        probe="$work/consumer-build$exe"
        note "built against the build tree, because the installed link failed"
    else
        fail "could not build the lookup probe at all"
        tail -5 "$work/link-build.log" 2>/dev/null | sed 's/^/      /'
    fi
fi

if [ -n "$probe" ]; then
    cp "$probe" "$moved/bin/ql-install-probe$exe"

    if out=$("$moved/bin/ql-install-probe$exe" 2>&1); then
        pass "the probe runs from the moved prefix"
    else
        fail "the probe failed from the moved prefix"
    fi
    echo "$out" | sed 's/^/      /'

    # The decisive half. With the adjacent copy replaced by something that is
    # not Bitwuzla, a lookup that really uses the adjacent copy must fail. If
    # it still succeeds, it reached the build-time default instead and the
    # install is not relocatable.
    mv "$moved/bin/$bw" "$work/$bw.real"
    cp "$moved/bin/quodlibet$exe" "$moved/bin/$bw"
    if out=$("$moved/bin/ql-install-probe$exe" 2>&1); then
        fail "the lookup ignored the adjacent copy and used another path"
        echo "$out" | sed 's/^/      /'
    else
        pass "the lookup uses the adjacent copy, not the build-time default"
        echo "$out" | grep -v "^build-time default" | sed 's/^/      /'
    fi
    mv "$work/$bw.real" "$moved/bin/$bw"
fi
echo

echo "checks failed: $failures"
[ "$failures" -eq 0 ]
