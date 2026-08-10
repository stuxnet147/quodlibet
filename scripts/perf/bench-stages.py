#!/usr/bin/env python3
"""Stage breakdown of one judgement, from a build that has QL_STAGE_TIMING on.

    cmake -S . -B out/build/linux-stage -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS=-DQL_STAGE_TIMING
    cmake --build out/build/linux-stage --parallel
    PYTHONPATH=out/build/linux-stage/bindings/python/package \
        python3 scripts/perf/bench-stages.py --corpus out/corpus/val

docs/perf/baseline.md could say how long a judgement takes and not where the
time went inside one, because VTune hangs on this host for any workload that
forks a child and every judgement forks Bitwuzla. src/stage_timer.h puts a
monotonic clock at the stage boundaries instead. This script drives the
sample, reads the per-judgement records the instrumented build appends, and
reports the distribution.

Medians and p90 are reported per stage rather than means. One judgement that
hits a slow solver round trip moves a mean and not a median, and the question
here is where a typical judgement spends its time.

The buckets nest on purpose: the frontend bucket contains its own parse and
the lowering bucket contains its own parse, because the hook sits at the call
site rather than inside the parser. This script does the subtraction.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

# Slot order must match the enum in src/stage_timer.h.
SLOTS = [
    "problem",
    "frontend_incl",
    "parse_frontend",
    "lower_incl",
    "parse_lower",
    "ir_open",
    "product",
    "smt2",
    "solver_setup",
    "solver_check",
    "solver_digest",
    "solver_spawn",
    "solver_run",
    "replay",
    "outcome",
    "total",
]

# What gets reported, after the nesting is subtracted out.
REPORT = [
    ("problem decode", lambda r: r["problem"]),
    ("parse (tree-sitter, x2)", lambda r: r["parse_frontend"] + r["parse_lower"]),
    ("frontend accept", lambda r: r["frontend_incl"] - r["parse_frontend"]),
    ("IR lowering", lambda r: r["lower_incl"] - r["parse_lower"]),
    ("IR open", lambda r: r["ir_open"]),
    ("miter encoding", lambda r: r["product"] - r["smt2"]),
    ("SMT-LIB serialise", lambda r: r["smt2"]),
    ("solver setup/teardown", lambda r: r["solver_setup"]),
    ("solver: integrity hash", lambda r: r["solver_digest"]),
    ("solver: process spawn", lambda r: r["solver_spawn"]),
    ("solver: bitwuzla run", lambda r: r["solver_run"]),
    ("solver: rest of trip",
     lambda r: r["solver_check"] - r["solver_digest"]
     - r["solver_spawn"] - r["solver_run"]),
    ("counterexample replay", lambda r: r["replay"]),
    ("outcome JSON", lambda r: r["outcome"]),
]


def load_pairs(corpus: str, limit: int) -> list[dict]:
    """Self-pairs over corpus units.

    A corpus record carries one function, and a judgement needs two. Comparing
    a body with itself keeps the real size and shape of the C on both sides,
    which is what the parse, the lowering and the miter cost scale with. It
    does make the miter easy for the solver, so the solver column here is a
    floor for that stage and is labelled as one in docs/perf/stages.md.
    """
    with open(os.path.join(corpus, "manifest.json"), encoding="utf-8") as handle:
        manifest = json.load(handle)
    pairs = []
    for entry in manifest["units"]:
        path = os.path.join(corpus, entry["file"].replace("/", os.sep))
        try:
            with open(path, encoding="utf-8") as handle:
                source = handle.read()
        except OSError:
            continue
        # The corpus is anonymised: the body a record carries is always
        # defined as FUN_0, and manifest["subject"] is the original name from
        # the upstream project, which does not appear in the text.
        if "FUN_0(" not in source:
            continue
        pairs.append({
            "digest": entry["digest"],
            "bytes": entry["bytes"],
            "left_source": source,
            "left_function": "FUN_0",
            "right_source": source,
            "right_function": "FUN_0",
            "trust_smt_backend": True,
        })
        if limit and len(pairs) >= limit:
            break
    return pairs


def synthetic_pairs(count: int) -> list[dict]:
    """The pairs docs/perf/baseline.md's ladder uses, so the stage totals here
    can be lined up against the per-judgement numbers already recorded."""
    return [{
        "digest": f"synthetic-{index}",
        "bytes": 0,
        "left_source": (
            f"int add(int x, int y){{ return x + y + {index} - {index}; }}"
        ),
        "left_function": "add",
        "right_source": (
            f"int sum(int a, int b){{ return b + a + {index} - {index}; }}"
        ),
        "right_function": "sum",
        "trust_smt_backend": True,
    } for index in range(count)]


def read_records(path: str) -> list[dict]:
    records = []
    if not os.path.exists(path):
        # No record means the method never ran: every judgement was refused
        # before it, or the sample produced none.
        return records
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            fields = line.split("\t")
            if len(fields) != len(SLOTS) + 1:
                continue
            record = {"worker": fields[0]}
            for name, raw in zip(SLOTS, fields[1:]):
                record[name] = int(raw)
            records.append(record)
    return records


def run(quodlibet, pairs: list[dict], workers: int, timing_path: str):
    """Returns (records, wall seconds, verdicts).

    One session per worker thread, opened before the clock starts. A session
    per judgement would put the backend installation back into every judgement
    and swamp the stages this is trying to separate; docs/perf/baseline.md
    measured that cost at 12.3 ms per judgement.
    """
    if os.path.exists(timing_path):
        os.remove(timing_path)
    verdicts: list = [None] * len(pairs)
    durations: list = [0.0] * len(pairs)
    local = threading.local()
    sessions = []
    sessions_lock = threading.Lock()

    def session_for_thread():
        session = getattr(local, "session", None)
        if session is None:
            session = quodlibet.SolverSession()
            local.session = session
            with sessions_lock:
                sessions.append(session)
        return session

    def judge(item):
        index, payload = item
        spec = {k: v for k, v in payload.items()
                if k not in ("digest", "bytes")}
        session = session_for_thread()
        began = time.perf_counter()
        try:
            result = quodlibet.check(session=session, **spec)
            verdicts[index] = result.verdict
        except Exception as error:                      # noqa: BLE001
            verdicts[index] = f"error: {error}"
        durations[index] = time.perf_counter() - began

    try:
        if workers == 1:
            session_for_thread()                        # opened off the clock
            start = time.perf_counter()
            for item in enumerate(pairs):
                judge(item)
            wall = time.perf_counter() - start
        else:
            with ThreadPoolExecutor(max_workers=workers) as pool:
                # Force every worker to exist and hold a session first.
                list(pool.map(lambda _: session_for_thread(),
                              range(workers * 4)))
                if os.path.exists(timing_path):
                    os.remove(timing_path)
                start = time.perf_counter()
                list(pool.map(judge, enumerate(pairs)))
                wall = time.perf_counter() - start
    finally:
        for session in sessions:
            session.close()
    return read_records(timing_path), wall, verdicts, durations


def summarise(records: list[dict], wall: float, label: str,
              workers: int, durations: list) -> None:
    if not records:
        # A build without QL_STAGE_TIMING still answers the A/B question:
        # what one judgement costs when nothing is instrumented.
        print(f"\n== {label}   wall={wall:.3f}s   "
              f"(no stage records: this build has no QL_STAGE_TIMING)")
        return
    count = len(records)
    print(f"\n== {label}   n={count}   wall={wall:.3f}s")
    print(f"{'stage':<24} {'median ms':>10} {'p90 ms':>10} {'mean ms':>10} "
          f"{'share':>8}")
    totals = sorted(r["total"] for r in records)
    median_total = statistics.median(totals) / 1e6
    mean_total = statistics.fmean(totals) / 1e6
    accounted_median = 0.0
    accounted_mean = 0.0
    for name, pick in REPORT:
        values = sorted(pick(r) / 1e6 for r in records)
        median = statistics.median(values)
        p90 = values[min(len(values) - 1, int(0.9 * len(values)))]
        mean = statistics.fmean(values)
        accounted_median += median
        accounted_mean += mean
        if median == 0.0 and p90 == 0.0 and mean == 0.0:
            continue
        print(f"{name:<24} {median:>10.3f} {p90:>10.3f} {mean:>10.3f} "
              f"{100 * mean / mean_total:>7.1f}%")
    print(f"{'residual (untimed)':<24} "
          f"{median_total - accounted_median:>10.3f} {'':>10} "
          f"{mean_total - accounted_mean:>10.3f} "
          f"{100 * (mean_total - accounted_mean) / mean_total:>7.1f}%")
    p90_total = totals[min(count - 1, int(0.9 * count))] / 1e6
    print(f"{'method total':<24} {median_total:>10.3f} {p90_total:>10.3f} "
          f"{mean_total:>10.3f} {100.0:>7.1f}%")
    per_check = 1000 * wall / count
    print(f"{'wall per judgement':<24} {'':>10} {'':>10} "
          f"{per_check:>10.3f}")
    # In-thread, so this stays meaningful at every worker count: the same
    # thread measured the whole check() call and the method inside it. The
    # wall-divided-by-count figure above cannot do that once work overlaps.
    if durations:
        in_thread = sorted(1000.0 * d for d in durations)
        mean_in_thread = statistics.fmean(in_thread)
        print(f"{'in-thread per check()':<24} "
              f"{statistics.median(in_thread):>10.3f} "
              f"{in_thread[min(len(in_thread) - 1, int(0.9 * len(in_thread)))]:>10.3f} "
              f"{mean_in_thread:>10.3f}")
        print(f"{'outside the method':<24} {'':>10} {'':>10} "
              f"{mean_in_thread - mean_total:>10.3f} "
              f"{100 * (mean_in_thread - mean_total) / mean_in_thread:>7.1f}%"
              " of in-thread")
        # A batch cannot finish before its slowest member. When the slowest
        # single judgement approaches the batch wall, idle workers at the end
        # are the tail, not contention, and no amount of capacity fixes it.
        print(f"{'slowest single check()':<24} {'':>10} {'':>10} "
              f"{in_thread[-1]:>10.3f} "
              f"{100 * in_thread[-1] / (1000 * wall):>7.1f}% of batch wall")
    occupancy = (statistics.fmean(durations) * count / (wall * workers)
                 if durations else 0.0)
    print(f"{'occupancy':<24} {'':>10} {'':>10} "
          f"{100 * occupancy:>9.1f}%")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", help="directory from tools/corpus/extract.py")
    parser.add_argument("--synthetic", type=int, default=0,
                        help="use N synthetic pairs instead of the corpus")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--repeat", type=int, default=1,
                        help="passes over the sample, all pooled")
    parser.add_argument("--workers", default="1",
                        help="comma separated worker counts")
    parser.add_argument("--timing-file", default="/tmp/ql-stages.tsv")
    parser.add_argument("--max-ms", type=float, default=0.0,
                        help="drop pairs whose serial judgement took longer "
                             "than this, to measure throughput scaling "
                             "separately from the tail")
    args = parser.parse_args()

    os.environ["QL_STAGE_TIMING_FILE"] = args.timing_file
    import quodlibet                                     # noqa: PLC0415

    info = quodlibet.backend_info()
    if not info["available"]:
        print("error: this build has no Bitwuzla backend", file=sys.stderr)
        return 1

    if args.synthetic:
        pairs = synthetic_pairs(args.synthetic)
        source_label = f"synthetic x{args.synthetic}"
    elif args.corpus:
        pairs = load_pairs(args.corpus, args.limit)
        source_label = f"{args.corpus}"
    else:
        print("error: pass --corpus or --synthetic", file=sys.stderr)
        return 1
    if not pairs:
        print("error: no pairs", file=sys.stderr)
        return 1

    print(f"backend   {info['name']} {info['version']}")
    print(f"sample    {source_label}, {len(pairs)} candidate pairs")

    # Classify first, then measure only what the method actually judges.
    # Most corpus bodies use C this lowering does not accept and come back
    # UNKNOWN without ever reaching the SMT method, so leaving them in would
    # divide the stage totals by a denominator that includes judgements the
    # stages never ran for. This pass also serves as the warm-up.
    _, _, verdicts, first_durations = run(
        quodlibet, pairs, 1, args.timing_file)
    kept = [p for p, v in zip(pairs, verdicts) if v == "proved-equivalent"]
    print(f"judged    {len(kept)} of {len(pairs)} reach a verdict of "
          f"proved-equivalent; the rest are UNKNOWN (unsupported C)")
    if args.max_ms:
        # A batch cannot finish before its slowest member, so on a sample with
        # a heavy tail the batch wall measures the tail and not the scaling.
        # Dropping the outliers is how the two questions get separated; both
        # numbers are reported in docs/perf so neither stands alone.
        before = len(kept)
        kept = [p for p, v, d in zip(pairs, verdicts, first_durations)
                if v == "proved-equivalent" and 1000.0 * d <= args.max_ms]
        print(f"tail cut  {before - len(kept)} of {before} judged pairs took "
              f"longer than {args.max_ms:.0f} ms serially and are excluded")
    if not kept:
        print("error: no judged pairs", file=sys.stderr)
        return 1
    pairs = kept

    pool = pairs * args.repeat
    for workers in [int(w) for w in args.workers.split(",")]:
        records, wall, verdicts, durations = run(
            quodlibet, pool, workers, args.timing_file)
        good = sum(1 for v in verdicts if v == "proved-equivalent")
        other = {v for v in verdicts if v != "proved-equivalent"}
        summarise(records, wall, f"workers={workers}", workers,
                  [d for d, v in zip(durations, verdicts)
                   if v == "proved-equivalent"])
        print(f"wall per judgement (all judged pairs): "
              f"{1000 * wall / len(pool):.3f} ms")
        print(f"verdicts  proved-equivalent {good}/{len(verdicts)}"
              + (f", other {sorted(other)[:3]}" if other else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
