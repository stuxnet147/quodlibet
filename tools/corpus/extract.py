#!/usr/bin/env python3
"""Turn asm2c corpus records into C translation units for coverage measurement.

The asm2c record stores a function body and the semantic context needed to read
it. Concatenating the context declarations with the target definition yields a
translation unit that needs no preprocessor and no external header, which is
exactly what Quodlibet's C frontend accepts as input.

A record is one (function, compiler, optimization) tuple, so the same C body
appears once per compiler and optimization level. Coverage is therefore reported
against two different denominators and this script preserves both:

  records  - every row the model trains on
  bodies   - distinct C texts, each carrying its record multiplicity

Usage:

    python tools/corpus/extract.py \
        --corpus D:/projects/machine-model/datasets/records-local \
        --split val \
        --out out/corpus/val \
        --limit 20000
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import random
import sys

# The context field is a marker-delimited document, not C. These lines announce
# which half follows and must not reach the parser.
CONTEXT_MARKERS = ("<TYPES>", "<CALLEES>", "<SIG>", "<GLOBALS>", "<VARS>")


def strip_markers(context: str) -> tuple[str, str]:
    """Split a context blob into declarations and the subject signature.

    The signature is returned separately because the target already repeats it.
    Emitting both would be a duplicate definition rather than a declaration.
    """
    declarations: list[str] = []
    signature: list[str] = []
    current = declarations
    for line in context.splitlines():
        stripped = line.strip()
        if stripped in CONTEXT_MARKERS:
            current = signature if stripped == "<SIG>" else declarations
            continue
        if stripped.startswith("<") and stripped.endswith(">") and " " not in stripped:
            # An unknown marker. Treat it as a declaration section boundary
            # rather than guessing that its payload is C.
            current = declarations
            continue
        current.append(line)
    return "\n".join(declarations).strip(), "\n".join(signature).strip()


def build_unit(record: dict) -> str | None:
    """Build one translation unit, or None when the record cannot form one."""
    context = record.get("context")
    target = record.get("target")
    if not isinstance(context, str) or not isinstance(target, str):
        return None
    if not target.strip():
        return None
    declarations, _signature = strip_markers(context)
    parts = []
    if declarations:
        parts.append(declarations)
    parts.append(target.strip())
    return "\n\n".join(parts) + "\n"


def iter_records(corpus: str, split: str):
    split_dir = os.path.join(corpus, split)
    names = sorted(n for n in os.listdir(split_dir) if n.endswith(".jsonl"))
    for name in names:
        path = os.path.join(split_dir, name)
        with io.open(path, encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                try:
                    yield json.loads(line)
                except json.JSONDecodeError:
                    continue


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", required=True,
                        help="records-local root holding train/val/test")
    parser.add_argument("--split", default="val", choices=("train", "val", "test"))
    parser.add_argument("--out", required=True, help="output directory")
    parser.add_argument("--limit", type=int, default=0,
                        help="stop after this many distinct bodies (0 = all)")
    parser.add_argument("--seed", type=int, default=20260810,
                        help="sampling seed, recorded in the manifest")
    parser.add_argument("--sample", type=float, default=1.0,
                        help="probability of keeping a distinct body")
    args = parser.parse_args(argv)

    os.makedirs(args.out, exist_ok=True)
    units_dir = os.path.join(args.out, "units")
    os.makedirs(units_dir, exist_ok=True)

    rng = random.Random(args.seed)
    seen: dict[str, dict] = {}
    records_total = 0
    records_unusable = 0

    for record in iter_records(args.corpus, args.split):
        records_total += 1
        unit = build_unit(record)
        if unit is None:
            records_unusable += 1
            continue
        digest = hashlib.blake2b(unit.encode("utf-8"), digest_size=16).hexdigest()
        entry = seen.get(digest)
        if entry is not None:
            entry["records"] += 1
            continue
        if args.limit and len(seen) >= args.limit:
            # Still count multiplicity for bodies already kept, but stop adding.
            continue
        if args.sample < 1.0 and rng.random() > args.sample:
            continue
        name = digest + ".c"
        with io.open(os.path.join(units_dir, name), "w",
                     encoding="utf-8", newline="\n") as handle:
            handle.write(unit)
        seen[digest] = {
            "file": "units/" + name,
            "digest": digest,
            "records": 1,
            "subject": record.get("subject"),
            "project": record.get("project"),
            "bytes": len(unit.encode("utf-8")),
        }

    manifest = {
        "schema_version": 1,
        "corpus": os.path.abspath(args.corpus).replace("\\", "/"),
        "split": args.split,
        "seed": args.seed,
        "sample": args.sample,
        "limit": args.limit,
        "records_total": records_total,
        "records_unusable": records_unusable,
        "bodies": len(seen),
        "records_covered": sum(e["records"] for e in seen.values()),
        "units": sorted(seen.values(), key=lambda e: e["digest"]),
    }
    manifest_path = os.path.join(args.out, "manifest.json")
    with io.open(manifest_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle, indent=2)
        handle.write("\n")

    print("records      %d" % records_total)
    print("unusable     %d" % records_unusable)
    print("bodies       %d" % len(seen))
    print("manifest     %s" % manifest_path.replace("\\", "/"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
