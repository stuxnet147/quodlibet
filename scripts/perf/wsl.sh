#!/usr/bin/env bash
# Run a perf script inside WSL against this worktree, whatever drive it is on.
#
#   scripts/perf/wsl.sh scripts/perf/bench-coverage.sh val 5
#
# The coordinator's original vtune wrapper hard-coded the main worktree path
# under /mnt/d. Workstream worktrees live elsewhere, so the path is derived
# here instead of written down.
set -eu

cd "$(dirname "$0")/../.."
win_root=$(pwd -W 2>/dev/null || pwd)
# C:/x/y -> /mnt/c/x/y
wsl_root=$(printf '%s' "$win_root" |
    sed -E 's|^([A-Za-z]):|/mnt/\L\1|; s|\\|/|g')

distro="${QL_WSL_DISTRO:-Ubuntu-24.04}"
script="$1"
shift

env MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' wsl.exe -d "$distro" -- \
    bash -c "cd '$wsl_root' && ./$script $*"
