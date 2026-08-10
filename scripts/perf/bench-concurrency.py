#!/usr/bin/env python3
"""Cancellation cost and worker scaling for the batch judgement path.

    PYTHONPATH=out/build/<preset>/bindings/python/package \
        python3 scripts/perf/bench-concurrency.py [pairs] [worker,worker,...]

scripts/perf/bench-batch.py already measures occupancy and speedup across
worker counts, and docs/perf/baseline.md records what it found. This measures
the axis that one does not: what the execution-budget guard costs when it is
armed, and how long a run takes to give up once its budget has expired.

Those are the same question cancellation asks. The public Python surface
exposes the budget rather than a cancel token, and the core withdraws a
budget-exhausted run to UNKNOWN by exactly the path a cancelled one takes, so
the budget is the observable handle on it.

Three measurements, in order of what they answer:

  arming    Does having a guard cost anything when it never fires? Same batch
            with no budget and with one far larger than the run needs.

  giving up How long after its deadline does a run actually stop? A budget is
            a promise about wall clock, and a guard checked only at stage
            boundaries keeps that promise only as coarsely as the stages are.

  scaling   Throughput across worker counts, as a cross-check against
            bench-batch.py on the same machine and commit.

Every row reports min, median and max rather than a median alone. On a
machine running other work the spread is frequently wider than the effect
being measured, and a lone median hides exactly that. A row whose spread
straddles the difference it is meant to show has not measured anything, and
saying so is the point of printing it.

    QL_BENCH_REPEATS=15 ...   samples per row (default 5)
    QL_BENCH_SECTIONS=giving-up,arming,scaling   which measurements to run

The commit is printed with the results because the batch path's internals are
actively changing.
"""

from __future__ import annotations

import os
import statistics
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import quodlibet

REPEATS = int(os.environ.get("QL_BENCH_REPEATS", "5"))


def spec(index: int) -> dict:
    """Equivalent by commutativity, with a per-index identity term so no two
    pairs share a digest and nothing downstream can collapse the batch."""
    return {
        "left_source": (
            f"int add(int x, int y){{ return x + y + {index} - {index}; }}"
        ),
        "left_function": "add",
        "right_source": (
            f"int sum(int a, int b){{ return b + a + {index} - {index}; }}"
        ),
        "right_function": "sum",
        "trust_smt_backend": True,
    }


def commit() -> str:
    try:
        return subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, check=True,
            cwd=os.path.dirname(os.path.abspath(__file__)),
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def judge(payload: dict, budget: dict | None) -> tuple[float, object]:
    """One judgement, timed from the caller's side. The caller's clock is the
    one a cancellation promise is made to."""
    extra = {} if budget is None else {"budget": budget}
    start = time.perf_counter()
    try:
        result = quodlibet.check(**payload, **extra)
    except quodlibet.QuodlibetError as error:
        result = error
    return time.perf_counter() - start, result


def timed_batch(specs: list[dict], workers: int,
                budget: dict | None) -> tuple[float, list]:
    """Return (batch wall seconds, results). Judgements run in a plain thread
    pool because the extension releases the GIL for each one."""
    results: list = [None] * len(specs)

    def run(item: tuple[int, dict]) -> None:
        index, payload = item
        _, results[index] = judge(payload, budget)

    start = time.perf_counter()
    if workers == 1:
        for item in enumerate(specs):
            run(item)
    else:
        with ThreadPoolExecutor(max_workers=workers) as pool:
            list(pool.map(run, enumerate(specs)))
    return time.perf_counter() - start, results


def spread(samples: list[float]) -> tuple[float, float, float]:
    """min, median, max. Reported together so a reader can see whether the
    difference between two rows survives the noise in either of them."""
    return (min(samples), statistics.median(samples), max(samples))


def proved(results: list) -> int:
    return sum(
        1 for item in results
        if not isinstance(item, Exception)
        and getattr(item, "verdict", None) == "proved-equivalent"
    )


def measure_arming(specs: list[dict], ladder: list[int]) -> None:
    """A budget large enough never to fire must not change what the run
    costs. If arming the guard were expensive, every caller that wants a
    deadline would be paying for it on every judgement."""
    print("arming: a budget that never fires, against none at all")
    print(f"{'workers':>8} {'no budget min/med/max s':>26} "
          f"{'armed min/med/max s':>26} {'separated':>10}")
    for workers in ladder:
        bare = []
        armed = []
        for _ in range(REPEATS):
            wall, results = timed_batch(specs, workers, None)
            bare.append(wall)
            assert proved(results) == len(specs), "a bare run did not prove"
            wall, results = timed_batch(specs, workers,
                                        {"total_ms": 600_000})
            armed.append(wall)
            assert proved(results) == len(specs), "an armed run did not prove"
        bare_lo, bare_mid, bare_hi = spread(bare)
        armed_lo, armed_mid, armed_hi = spread(armed)
        # Only claim a difference when the two ranges do not overlap at all.
        separated = "yes" if bare_hi < armed_lo or armed_hi < bare_lo else "no"
        print(f"{workers:>8} "
              f"{bare_lo:>8.3f}{bare_mid:>9.3f}{bare_hi:>9.3f} "
              f"{armed_lo:>8.3f}{armed_mid:>9.3f}{armed_hi:>9.3f} "
              f"{separated:>10}")
    print()
    print("'separated' is no when the two ranges overlap, which means this run")
    print("did not resolve an arming cost rather than that there is none.")
    print()


def measure_giving_up(payload: dict) -> None:
    """How long after its deadline a run actually stops.

    The budget is checked where the pipeline can act on it, not inside a
    solver call, so the gap between the deadline and the return is bounded by
    whatever stage was running. That gap is the real cancellation latency and
    it is what a scorer with a per-pair deadline will see."""
    print("giving up: requested deadline against observed return")
    print(f"{'budget':>10} {'min s':>9} {'median s':>9} {'max s':>9} "
          f"{'verdict':>18} {'state':>22}")
    for label, budget in (
        ("1 ns", {"total_ns": 1}),
        ("1 ms", {"total_ms": 1}),
        ("10 ms", {"total_ms": 10}),
        ("100 ms", {"total_ms": 100}),
        ("500 ms", {"total_ms": 500}),
        ("none", None),
    ):
        samples = []
        verdict = ""
        state = ""
        for _ in range(REPEATS):
            elapsed, result = judge(payload, budget)
            samples.append(elapsed)
            if isinstance(result, Exception):
                verdict, state = "error", str(result)[:24]
            else:
                verdict = result.verdict
                state = result.evidence.budget_state
        low, mid, high = spread(samples)
        print(f"{label:>10} {low:>9.4f} {mid:>9.4f} {high:>9.4f} "
              f"{verdict:>18} {state:>22}")
    print()
    print("A run that overshoots its budget is still withdrawn to UNKNOWN, so")
    print("the overshoot costs wall clock and never soundness. The number to")
    print("watch is how far past the deadline the return lands.")
    print()


def measure_scaling(specs: list[dict], ladder: list[int]) -> None:
    """Cross-check against bench-batch.py on this machine and commit."""
    print("scaling: throughput across worker counts")
    print(f"{'workers':>8} {'min s':>9} {'median s':>9} {'max s':>9} "
          f"{'pairs/s':>9} {'speedup':>9}")
    base = None
    for workers in ladder:
        samples = []
        for _ in range(REPEATS):
            wall, results = timed_batch(specs, workers, None)
            assert proved(results) == len(specs), "a run did not prove"
            samples.append(wall)
        low, mid, high = spread(samples)
        if base is None:
            base = mid
        print(f"{workers:>8} {low:>9.3f} {mid:>9.3f} {high:>9.3f} "
              f"{len(specs) / mid:>9.2f} {base / mid:>8.2f}x")
    print()


def main() -> int:
    pairs = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    if len(sys.argv) > 2:
        ladder = [int(value) for value in sys.argv[2].split(",")]
    else:
        ladder = [1, 2, 4]

    backend = quodlibet.backend_info()
    if not backend.get("available"):
        print("the pinned SMT backend is unavailable; nothing to measure",
              file=sys.stderr)
        return 1
    print(f"commit     {commit()}")
    print(f"cpus       {os.cpu_count()}")
    print(f"backend    {backend['name']} {backend['version']}")
    print(f"pairs      {pairs}")
    print(f"repeats    {REPEATS} (median reported)")
    print()

    sections = os.environ.get("QL_BENCH_SECTIONS",
                              "giving-up,arming,scaling").split(",")
    specs = [spec(index) for index in range(pairs)]
    if "giving-up" in sections:
        measure_giving_up(specs[0])
    if "arming" in sections:
        measure_arming(specs, ladder)
    if "scaling" in sections:
        measure_scaling(specs, ladder)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
