#!/usr/bin/env python3
"""Replay recorded SMT-LIB queries against Bitwuzla under option variants.

    QL_STAGE_QUERY_LOG=/tmp/qlq.tsv QL_STAGE_QUERY_DIR=/tmp/qlq \
        PYTHONPATH=out/build/linux-stage/bindings/python/package \
        python3 scripts/perf/bench-stages.py --corpus out/corpus/val --workers 1

    python3 scripts/perf/bench-solver-options.py \
        --queries /tmp/qlq --log /tmp/qlq.tsv \
        --bitwuzla third_party/bitwuzla-linux-x86_64/bin/bitwuzla

docs/perf/stages.md put 98.8% of a judgement inside Bitwuzla and
docs/perf/parallel.md ruled the two fixed costs around it untouchable. What is
left is the solving itself, and the only lever on it that does not touch the
engine is which options it is invoked with.

A variant counts only if every query returns the same answer it returned under
the shipped options. That check is the point: an option that turns an UNSAT
into an unknown has not made anything faster, it has made the pipeline stop
proving things. `--bv-solver prop` and `preprop` are excluded for that reason
rather than measured; they are local search and cannot answer UNSAT.

Times are minima over `--repeat` runs. A single run of a two-second query
moves by hundreds of milliseconds on a loaded machine, which is more than most
of the differences being looked for.
"""

from __future__ import annotations

import argparse
import os
import random
import statistics
import subprocess
import sys
import time

# Every variant here is answer-preserving by construction: a different SAT
# engine, a different rewrite level, or a preprocessing pass being on or off.
# None of them makes the procedure incomplete.
VARIANTS = {
    "baseline": [],
    "sat=kissat": ["--sat-solver", "kissat"],
    "sat=cms": ["--sat-solver", "cms"],
    "sat=gimsatul": ["--sat-solver", "gimsatul"],
    "rwl=0": ["--rewrite-level", "0"],
    "rwl=1": ["--rewrite-level", "1"],
    "no-abstraction": ["--abstraction", "false"],
    "abstraction-bvadd": ["--abstraction-bvadd", "true"],
    "abstraction-ite": ["--abstraction-ite", "true"],
    "abs-eager-refine": ["--abstraction-eager-refine", "true"],
    "pp-contr-ands": ["--pp-contr-ands", "true"],
    "pp-elim-bvudiv": ["--pp-elim-bvudiv", "true"],
    "pp-subst-diseq": ["--pp-variable-subst-norm-diseq", "true"],
}


def load_queries(directory: str, log: str, heavy: int, sample: int,
                 seed: int) -> list[tuple[str, str, float]]:
    """Slowest `heavy` queries plus a random `sample` of the rest.

    The tail and the body have to be reported separately: the tail is where
    the wall clock is and the body is what a typical judgement pays, and an
    option can help one and hurt the other.
    """
    best: dict[str, float] = {}
    for line in open(log, encoding="utf-8"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) != 4:
            continue
        digest, _bytes, nanos, _answer = parts
        millis = int(nanos) / 1e6
        if digest not in best or millis < best[digest]:
            best[digest] = millis
    present = []
    for digest, millis in best.items():
        path = os.path.join(directory, digest + ".smt2")
        if os.path.exists(path):
            present.append((digest, path, millis))
    present.sort(key=lambda row: -row[2])
    picked = present[:heavy]
    rest = present[heavy:]
    random.Random(seed).shuffle(rest)
    return picked + rest[:sample]


def run(bitwuzla: str, path: str, extra: list[str], timeout_ms: int):
    """Returns (answer, seconds). The adapter's own flags come first so the
    variant is the only difference."""
    with open(path, "rb") as handle:
        query = handle.read()
    arguments = [bitwuzla, "--lang", "smt2", "--bv-output-format", "16"]
    if b"(get-model)" in query:
        arguments.append("--produce-models")
    arguments += ["--time-limit", str(timeout_ms)]
    arguments += extra
    began = time.perf_counter()
    finished = subprocess.run(arguments, input=query, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, check=False)
    elapsed = time.perf_counter() - began
    first = finished.stdout.split(b"\n", 1)[0].strip().decode("ascii", "replace")
    return (first or "?"), elapsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--queries", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--bitwuzla", required=True)
    parser.add_argument("--heavy", type=int, default=6)
    parser.add_argument("--sample", type=int, default=20)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    parser.add_argument("--seed", type=int, default=20260810)
    parser.add_argument("--only", default="",
                        help="comma separated variant names")
    args = parser.parse_args()

    queries = load_queries(args.queries, args.log, args.heavy, args.sample,
                           args.seed)
    if not queries:
        print("error: no queries found", file=sys.stderr)
        return 1
    names = ([n for n in args.only.split(",") if n] if args.only
             else list(VARIANTS))
    if "baseline" not in names:
        names.insert(0, "baseline")

    print(f"queries   {len(queries)} "
          f"({args.heavy} heaviest, {len(queries) - args.heavy} sampled)")
    print(f"repeat    {args.repeat}, minimum reported")
    print()

    baseline_answers: dict[str, str] = {}
    baseline_times: dict[str, float] = {}
    print(f"{'variant':<20} {'heavy s':>9} {'body s':>9} {'total s':>9} "
          f"{'vs base':>9}  answers")
    for name in names:
        extra = VARIANTS[name]
        answers, times = {}, {}
        for digest, path, _ in queries:
            best = None
            answer = "?"
            for _ in range(args.repeat):
                answer, seconds = run(args.bitwuzla, path, extra,
                                      args.timeout_ms)
                best = seconds if best is None else min(best, seconds)
            answers[digest] = answer
            times[digest] = best or 0.0
        heavy = sum(times[d] for d, _, _ in queries[:args.heavy])
        body = sum(times[d] for d, _, _ in queries[args.heavy:])
        total = heavy + body
        if name == "baseline":
            baseline_answers = answers
            baseline_times = times
            verdict = "reference"
        else:
            changed = [d for d in answers
                       if answers[d] != baseline_answers.get(d)]
            verdict = ("SAME" if not changed
                       else f"CHANGED on {len(changed)} query")
        ratio = (total / sum(baseline_times.values())
                 if baseline_times else 1.0)
        print(f"{name:<20} {heavy:>9.3f} {body:>9.3f} {total:>9.3f} "
              f"{ratio:>8.2f}x  {verdict}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
