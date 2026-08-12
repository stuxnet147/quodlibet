"""How much of the corpus the pipeline can actually answer about.

Lowering acceptance says a body became IR. It does not say the registered
method chain can decide the requested relation, and the two numbers have never
been the same. This measures the second number through the Python API, including
the SMT product, bounded symbolic search, and CHC/PDR follow-up.

The probe is a body against itself. An identical pair is the easiest question
there is, so a self-pair that comes back UNKNOWN is a body the pipeline cannot
decide at all, and one that comes back proved is a body it can. Anything else
is a bug and is reported separately.

    python scripts/perf/decidable.py out/corpus/val/units.txt [limit] [workers]
"""

from __future__ import annotations

import collections
import pathlib
import re
import sys

preset = "windows-clang" if sys.platform == "win32" else "linux-clang"
package_dir = pathlib.Path(f"out/build/{preset}/bindings/python/package")
if package_dir.is_dir():
    sys.path.insert(0, str(package_dir.resolve()))

import quodlibet  # noqa: E402

SELECTED = re.compile(
    r"^\s*(?:static\s+)?[A-Za-z_][^\n;{}()]*?\b(FUN_\d+)\s*\(", re.M
)


def selected_function(source: str) -> str | None:
    """The corpus names the selected function FUN_0 and its callees FUN_n."""
    names = SELECTED.findall(source)
    return "FUN_0" if "FUN_0" in names else None


def listed_path(text: str) -> pathlib.Path:
    """Resolve a corpus listing written on Windows when running under WSL."""
    path = pathlib.Path(text)
    if path.is_file() or sys.platform == "win32":
        return path
    drive_path = re.fullmatch(r"([A-Za-z]):[\\/](.*)", text)
    if drive_path is None:
        return path
    relative = pathlib.PurePosixPath(drive_path.group(2).replace("\\", "/"))
    return pathlib.Path("/mnt", drive_path.group(1).lower(), relative)


def main() -> int:
    listing = pathlib.Path(sys.argv[1])
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    paths = [
        listed_path(line.strip())
        for line in listing.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    if limit:
        paths = paths[:limit]

    specs = []
    for path in paths:
        try:
            source = path.read_text(encoding="utf-8")
        except OSError:
            continue
        name = selected_function(source)
        if name is None:
            continue
        specs.append(
            quodlibet.CheckSpec(
                left_source=source,
                left_function=name,
                right_source=source,
                right_function=name,
                trust_smt_backend=True,
            )
        )

    workers = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    results = quodlibet.check_batch(specs, workers=workers)
    verdicts: collections.Counter[str] = collections.Counter()
    methods: collections.Counter[str] = collections.Counter()
    reasons: collections.Counter[str] = collections.Counter()
    for result in results:
        if isinstance(result, Exception):
            verdicts["error"] += 1
            reasons[str(result)[:90]] += 1
            continue
        verdicts[result.verdict] += 1
        methods[result.decided_by or "none"] += 1
        if result.verdict != "proved-equivalent":
            reasons[(result.diagnostic or "")[:90]] += 1

    total = sum(verdicts.values())
    decided = verdicts.get("proved-equivalent", 0)
    print(f"probed:  {total}")
    print(
        f"decided: {decided}"
        + (f"  ({decided * 100.0 / total:.2f}%)" if total else "")
    )
    for verdict, count in verdicts.most_common():
        print(f"  {count:6d}  {verdict}")
    print("decided by:")
    for method, count in methods.most_common():
        print(f"  {count:6d}  {method}")
    print("why not:")
    for reason, count in reasons.most_common(12):
        print(f"  {count:6d}  {reason}")
    return 0


sys.exit(main())
