#!/usr/bin/env bash
# Run corpus coverage and print the summary plus the first-blocker table.
#
#   coverage-val.sh          val split (1,050 bodies, the fast loop)
#   coverage-val.sh train    train split (29,893 bodies)
set -eu
cd "$(dirname "$0")/../.."
. scripts/coordinator/env.sh

split="${1:-val}"
exe="out/build/${PRESET:-windows-clang}/quodlibet.exe"
outdir="out/corpus/$split"

if [ ! -f "$outdir/manifest.json" ]; then
    echo "== extracting $split units"
    python tools/corpus/extract.py --corpus "$QL_CORPUS" --split "$split" --out "$outdir"
fi
if [ ! -f "$outdir/units.txt" ]; then
    ls "$outdir"/units/*.c | sed "s|^|$(pwd -W 2>/dev/null || pwd)/|" > "$outdir/units.txt"
fi

"$exe" coverage "$outdir/units.txt" "$outdir/detail.tsv" | python -c "
import sys, json
d = json.load(sys.stdin)
defs = d['definitions'] or 1
print(json.dumps(d, indent=2))
print()
print('lowered: %d/%d (%.2f%%)' % (d['definitions_lowered'], defs,
                                   100.0 * d['definitions_lowered'] / defs))
"
echo
echo "== first blocker distribution"
cut -f4 "$outdir/detail.tsv" | sort | uniq -c | sort -rn
