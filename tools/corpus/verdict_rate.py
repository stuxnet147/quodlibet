#!/usr/bin/env python3
"""Of the corpus bodies the lowering accepts, how many reach a verdict?

`quodlibet coverage` answers how much C the frontend and the lowering accept.
This answers the next question: of the IR that comes out, how much can the
miter actually state well enough to decide.

Each body is paired with itself, so the answer is about what the miter can say
about that IR and not about whether two different programs happen to agree. A
self-pair that does not reach `proved-equivalent` names the layer that stopped
it, which is the useful output: it points at the next obstacle rather than at a
percentage.

Usage:

    python tools/corpus/extract.py --corpus <records> --split val \
        --out out/corpus/val
    python - <<'PY'
    import json
    m = json.load(open("out/corpus/val/manifest.json"))
    open("out/corpus/val/units.txt", "w").write(
        "\\n".join("out/corpus/val/" + u["file"] for u in m["units"]) + "\\n")
    PY
    quodlibet coverage out/corpus/val/units.txt out/corpus/val/detail.tsv
    PYTHONPATH=<build>/bindings/python/package \\
        python tools/corpus/verdict_rate.py out/corpus/val/detail.tsv
"""

from __future__ import annotations

import collections
import csv
import json
import re
import sys

import quodlibet

OBSERVATIONS = ["return-value", "memory", "termination", "traps"]


def signature_line(source: str, name: str) -> str:
    """The declaration line, used only to label a row in the report."""
    for line in source.splitlines():
        if re.search(r"\b" + re.escape(name) + r"\s*\(", line):
            return line.strip()
    return ""


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    rows = []
    with open(argv[1], newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle, delimiter="\t"):
            if len(row) >= 3 and row[2] == "lowered":
                rows.append((row[0], row[1]))

    counts: collections.Counter[str] = collections.Counter()
    reasons: collections.Counter[str] = collections.Counter()
    bodies = []
    for path, name in rows:
        with open(path, encoding="utf-8") as handle:
            source = handle.read()
        entry = {
            "unit": path.replace("\\", "/").rsplit("/", 1)[-1],
            "function": name,
            "signature": signature_line(source, name),
        }
        entry["pointer"] = "*" in entry["signature"]
        try:
            result = quodlibet.check(
                source, name, source, name,
                observations=OBSERVATIONS,
                trust_smt_backend=True,
                solver_timeout_ms=20000,
            )
            entry["verdict"] = str(result.verdict)
            entry["diagnostic"] = result.diagnostic
        except Exception as error:  # noqa: BLE001
            # A refusal before the run starts is a boundary of some layer, not
            # a statement about the function. It is counted separately.
            entry["verdict"] = "refused"
            entry["diagnostic"] = str(error)
        counts[entry["verdict"]] += 1
        if entry["verdict"] != "proved-equivalent":
            reasons[entry["diagnostic"]] += 1
        bodies.append(entry)

    decided = counts["proved-equivalent"]
    print(json.dumps({
        "lowered_bodies": len(rows),
        "bodies_with_a_verdict": decided,
        "ratio": 0.0 if not rows else round(decided / len(rows), 4),
        "verdicts": dict(counts),
        "pointer_bodies": sum(1 for e in bodies if e["pointer"]),
        "pointer_bodies_with_a_verdict": sum(
            1 for e in bodies
            if e["pointer"] and e["verdict"] == "proved-equivalent"),
        "blocking_reasons": dict(reasons),
        "detail": bodies,
    }, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
