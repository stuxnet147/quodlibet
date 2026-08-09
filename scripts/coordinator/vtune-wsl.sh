#!/usr/bin/env bash
# Compatibility shim. The measurement scripts now live in scripts/perf/ and
# derive the worktree path instead of hard-coding the main checkout under
# /mnt/d, so they work from any workstream worktree.
#
#   vtune-wsl.sh                          default: coverage over val units
#   QL_PERF_WORKLOAD="./out/build/linux-clang/quodlibet methods" vtune-wsl.sh
#
# The caller runs this in the background and enforces the 120-second bound;
# background timeouts are not enforced by the harness, so a run past the bound
# is killed by hand (TaskStop). docs/perf/baseline.md holds the method.
set -eu
cd "$(dirname "$0")/../.."
exec ./scripts/perf/wsl.sh scripts/perf/vtune-hotspots.sh "$@"
