#!/usr/bin/env python3
"""Worker occupancy and throughput for the batch judgement path.

    PYTHONPATH=out/build/<preset>/bindings/python/package \
        python3 scripts/perf/bench-batch.py [pairs] [worker,worker,...]

G6 asks whether the part that should run in parallel actually does. A VTune
threading report answers that from the outside; this answers it from the
inside, and it runs anywhere the extension imports, which the profiler does
not. The two are not redundant: this measures what the batch API delivers,
the profiler measures where a thread that is not delivering has gone.

Occupancy here is sum(per-judgement wall) / (batch wall * workers). It is the
fraction of the requested parallel capacity that was actually busy judging.
Speedup is reported against the same batch at one worker, on the same
process, so the extension load and the backend probe are already paid.

Occupancy and speedup answer different questions and the pair is the point.
High occupancy with flat speedup means the threads were busy and got less
done each: contention inside the judgement, not an idle pool. That is why
mean per-judgement latency is printed next to them.

Each pair is textually distinct so that nothing downstream can collapse the
batch into one judgement and call it fast.
"""

from __future__ import annotations

import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import quodlibet


def spec(index: int) -> dict:
    """Equivalent by commutativity, with a per-index identity term so no two
    pairs share a digest."""
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


def timed_batch(specs: list[dict], workers: int) -> tuple[float, float, list]:
    """Return (batch wall seconds, summed judgement seconds, results).

    Times each judgement inside the worker rather than trusting the pool, so
    a queue that never handed work out shows up as idle capacity instead of
    disappearing into the wall clock.
    """
    busy = 0.0
    results: list = [None] * len(specs)

    def run(item: tuple[int, dict]) -> float:
        index, payload = item
        start = time.perf_counter()
        try:
            results[index] = quodlibet.check(**payload)
        except quodlibet.QuodlibetError as error:
            results[index] = error
        return time.perf_counter() - start

    start = time.perf_counter()
    if workers == 1:
        for item in enumerate(specs):
            busy += run(item)
    else:
        with ThreadPoolExecutor(max_workers=workers) as pool:
            busy = sum(pool.map(run, enumerate(specs)))
    wall = time.perf_counter() - start
    return wall, busy, results


def main() -> int:
    pairs = int(sys.argv[1]) if len(sys.argv) > 1 else 48
    if len(sys.argv) > 2:
        ladder = [int(w) for w in sys.argv[2].split(",")]
    else:
        cpus = os.cpu_count() or 1
        ladder = [w for w in (1, 2, 4, 8, 16) if w <= cpus]

    info = quodlibet.backend_info()
    if not info["available"]:
        print("error: this build has no Bitwuzla backend", file=sys.stderr)
        return 1

    specs = [spec(i) for i in range(pairs)]

    # Warm the extension, the backend probe and the page cache once, so the
    # first rung of the ladder does not pay for all of them.
    timed_batch(specs[: min(4, pairs)], 1)

    print(f"backend   {info['name']} {info['version']}")
    print(f"cpus      {os.cpu_count()}")
    print(f"pairs     {pairs}")
    print()
    print(f"{'workers':>7} {'wall s':>9} {'pairs/s':>9} {'ms/pair':>9} "
          f"{'speedup':>8} {'occupancy':>10}")

    baseline = None
    for workers in ladder:
        wall, busy, results = timed_batch(specs, workers)
        verdicts = {getattr(r, "verdict", type(r).__name__) for r in results}
        if verdicts != {"proved-equivalent"}:
            print(f"error: batch returned {verdicts}", file=sys.stderr)
            return 1
        if baseline is None:
            baseline = wall
        occupancy = busy / (wall * workers)
        print(f"{workers:>7} {wall:>9.3f} {pairs / wall:>9.1f} "
              f"{1000 * wall / pairs:>9.2f} {baseline / wall:>7.2f}x "
              f"{100 * occupancy:>9.1f}%")

    print()
    print("occupancy = sum(per-judgement wall) / (batch wall * workers).")
    print("Low occupancy at high worker counts means the capacity was")
    print("requested and not used, which is a scheduling or contention")
    print("finding, not a speed finding.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
