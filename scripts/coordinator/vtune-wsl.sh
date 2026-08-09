#!/usr/bin/env bash
# One bounded VTune hotspots collection on WSL, native-disk workload.
#
#   vtune-wsl.sh                          default: coverage over val units
#   vtune-wsl.sh ./out/build/linux-clang/quodlibet methods
#
# The caller runs this in the background and enforces the 120-second bound;
# background timeouts are not enforced by the harness, so a run past the bound
# is killed by hand (TaskStop). docs/perf/baseline.md holds the method.
set -eu
cd "$(dirname "$0")/../.."
. scripts/coordinator/env.sh

if [ "$#" -gt 0 ]; then
    workload="$*"
else
    # Refresh the native-disk unit copy; profiling through /mnt 9p inflates
    # file I/O to a third of CPU time and buries the engine (measured).
    env MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' wsl.exe -d "$QL_WSL_DISTRO" -- \
        bash -lc "rm -rf /tmp/qlunits && cp -r /mnt/d/projects/machine-model/python/quodlibet/out/corpus/val/units /tmp/qlunits && ls /tmp/qlunits | sed 's|^|/tmp/qlunits/|' > /tmp/qlunits.txt"
    workload="./out/build/linux-clang/quodlibet coverage /tmp/qlunits.txt"
fi

env MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' wsl.exe -d "$QL_WSL_DISTRO" -- \
    bash -lc "cd /mnt/d/projects/machine-model/python/quodlibet && rm -rf /tmp/vt-out && $QL_VTUNE_WSL -collect hotspots -result-dir /tmp/vt-out -- $workload 2>&1 | sed -n '/Top Hotspots/,/Collection and Platform/p' | head -20; $QL_VTUNE_WSL -report summary -r /tmp/vt-out 2>/dev/null | grep -E 'Elapsed|CPU Time' | head -3"
