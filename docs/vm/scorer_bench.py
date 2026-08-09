"""Scorer-path benchmark: N pairs through check_batch, per-pair latency.

The pairs are synthetic scalar functions, not corpus bodies: the corpus pairs
that lower today are few, and what this stage verifies is the scorer loop
itself (throughput, stability, latency distribution) on the target machine.
The mix is 60 equivalent pairs, 20 inequivalent, 20 unknown-by-construction,
so all three verdicts and the replay path are exercised.
"""
import statistics
import sys
import time

import quodlibet

BITWUZLA = "/opt/quodlibet/quodlibet/third_party/bitwuzla-linux-x86_64/bin/bitwuzla"
QUICK = "--quick" in sys.argv
PAIRS = 24 if QUICK else 100
WORKERS = 8


def spec(index):
    kind = index % 5
    if kind < 3:  # equivalent pair
        return dict(
            left_source="int f(int a){return (a+%d)*2;}" % index,
            left_function="f",
            right_source="int g(int a){return 2*a+%d;}" % (2 * index),
            right_function="g",
            relation="equivalence", ub_policy="must-match",
            trust_smt_backend=True, solver_executable=BITWUZLA,
        )
    if kind == 3:  # counterexample pair
        return dict(
            left_source="int f(int a){return a+%d;}" % index,
            left_function="f",
            right_source="int g(int a){return a+%d;}" % (index + 1),
            right_function="g",
            relation="equivalence", ub_policy="must-match",
            trust_smt_backend=True, solver_executable=BITWUZLA,
        )
    # unknown by construction: a loop is outside the lowered slice
    return dict(
        left_source="int f(int a){int s=0;while(a>0){s+=a;a-=1;}return s;}",
        left_function="f",
        right_source="int g(int a){return a;}",
        right_function="g",
        relation="equivalence", ub_policy="must-match",
        trust_smt_backend=True, solver_executable=BITWUZLA,
    )


latencies = []
verdicts = {}
start = time.perf_counter()
for base in range(0, PAIRS, WORKERS):
    batch = [spec(i) for i in range(base, min(base + WORKERS, PAIRS))]
    t0 = time.perf_counter()
    results = quodlibet.check_batch(batch, workers=WORKERS)
    dt = time.perf_counter() - t0
    latencies.extend([dt / len(batch)] * len(batch))
    for r in results:
        v = getattr(r, "verdict", "error:" + str(r)[:40])
        verdicts[v] = verdicts.get(v, 0) + 1
wall = time.perf_counter() - start

lat_sorted = sorted(latencies)
print("pairs      %d" % PAIRS)
print("wall       %.2fs  (%.1f pairs/s at workers=%d)" % (wall, PAIRS / wall, WORKERS))
print("per-pair   median %.0fms  p90 %.0fms" % (
    1000 * statistics.median(lat_sorted),
    1000 * lat_sorted[int(len(lat_sorted) * 0.9) - 1]))
print("verdicts   %s" % dict(sorted(verdicts.items())))
