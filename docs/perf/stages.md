# Where one judgement's time goes

`baseline.md` could say how long a judgement takes and not where the time went
inside one. This fills that hole: a stage breakdown of a single judgement, with
medians, p90s, the sample size, the method, and the commands to reproduce it.

**The answer is one line. A judgement is the solver. Everything the engine does
itself — parsing, frontend acceptance, IR lowering, the miter, the SMT-LIB
text — is 0.4 ms of a 64 ms judgement.**

That matters because `baseline.md`'s call attribution names `emit_store` and
`ql_ir_builder_destroy` as the next thing to optimise. Those are the right
names for the `coverage` tool, which is parse-and-lower and nothing else. They
are rounding error inside a judgement. The two workloads do not share a
priority list, and this document is the reason to stop treating them as if
they do.

## Method

A profiler cannot answer this on this host. `baseline.md` records the reason:
the VTune collector hangs on any workload that forks a child, and every
judgement forks Bitwuzla. Both collection types hang, so it is the child
process and not the collection type.

So the stage boundaries carry a monotonic clock instead, in `src/stage_timer.h`,
compiled only when `QL_STAGE_TIMING` is defined. In a normal build every macro
expands to nothing: no clock call, no branch, no storage. The hooks sit at
stage boundaries only, never inside a loop, so a judgement makes a fixed
handful of clock calls no matter how large the C is.

The buckets nest where the implementation nests. The frontend bucket contains
its own parse and the lowering bucket contains its own parse, because the hook
is at the call site rather than inside the parser. `scripts/perf/bench-stages.py`
does the subtraction.

One line per judgement is appended to the file named by `QL_STAGE_TIMING_FILE`.
With the variable unset an instrumented binary writes nothing.

### The sample

244 judgements, from the val split.

`tools/corpus/extract.py` yields 1,050 distinct bodies. A judgement needs two
functions and a corpus record carries one, so each pair here compares a body
with itself. That keeps the real size and shape of the C on both sides, which
is what the parse, the lowering and the miter scale with. Of the 1,049 usable
bodies, **244 lower and reach `proved-equivalent`**; the rest use C this
lowering does not accept, come back UNKNOWN without the SMT method ever
running, and are excluded — leaving them in would divide the stage totals by a
denominator that includes judgements the stages never ran for.

**The solver column is a floor.** Two identical sides make an easy miter. A
judgement of two genuinely different but equivalent functions gives the solver
more work and every other stage the same work, so the solver's share can only
go up from what is recorded here. Every other stage's number is
representative; the solver's is a lower bound.

## The breakdown

244 judgements, one worker, one session held across the batch, WSL2
Ubuntu-24.04, 12 physical cores.

| stage | median ms | p90 ms | mean ms | share of mean |
| --- | --- | --- | --- | --- |
| problem decode | 0.014 | 0.031 | 0.017 | 0.0% |
| parse (tree-sitter, twice) | 0.134 | 0.276 | 0.168 | 0.3% |
| frontend acceptance | 0.040 | 0.076 | 0.050 | 0.1% |
| IR lowering | 0.115 | 0.376 | 0.196 | 0.3% |
| IR open | 0.019 | 0.066 | 0.038 | 0.1% |
| miter encoding | 0.062 | 0.227 | 0.111 | 0.2% |
| SMT-LIB serialisation | 0.008 | 0.030 | 0.016 | 0.0% |
| solver setup and teardown | 0.048 | 0.182 | 0.093 | 0.1% |
| **solver round trips** | **12.842** | **45.776** | **62.156** | **98.8%** |
| outcome JSON | 0.015 | 0.018 | 0.015 | 0.0% |
| residual, not instrumented | 0.038 | | 0.064 | 0.1% |
| method total | 13.334 | 47.054 | 62.923 | 100% |

Run to run across five serial passes the method median moved between 13.3 and
15.2 ms and the solver mean between 62.2 and 65.8 ms. Nothing else moved
enough to change a digit above.

### The total reconciles

The instrumented total has to add up to what a caller actually waits for, or
the breakdown is describing something other than a judgement.

| | ms |
| --- | --- |
| method total, mean | 62.923 |
| outside the method (binding, pipeline, problem artifact) | 1.253 |
| **sum** | **64.176** |
| measured wall per judgement | **64.176** |

The residual inside the method is 0.064 ms, 0.1%. Nothing large is unaccounted
for.

### A judgement makes two solver round trips, not one

A `proved-equivalent` verdict asks twice: once whether the miter is
satisfiable, and once whether the comparison domain is inhabited. An UNSAT over
an empty domain proves nothing, so the second question is what makes the first
one mean something. Both are in the solver row.

### The parse happens twice and it does not matter here

`lower_side` in `src/proof_smt.c` calls `ql_c_frontend_analyze` and then
`ql_c_lower_selected_function`, and each parses the source. That is the same
duplication `baseline.md` measured at 46% of the `coverage` tool, and the
`_with_tree` API that removes it is already in the tree. In a judgement the
whole parse is 0.168 ms of 64 ms. **Applying that optimisation here would win
0.13% and is not worth a change to the judgement path.** It is recorded so that
nobody re-derives the 46% figure and expects it to apply.

### Miter and SMT-LIB serialisation, split

The encoder calls the SMT-LIB builder per instruction, so the two are
interleaved rather than sequential phases. They are separable at exactly one
point: `ql_smt2_builder_build` and the `ql_artifact_create` calls next to it
render the accumulated encoding into text and hash it. Those are timed
separately, and the rest of `ql_product_query_build` is the encoding.

Serialisation is 0.008 ms against 0.062 ms of encoding. Splitting them any
further would mean a clock call per builder call, which would cost more than
the stage it measures.

## Measuring this did not cost the canonical path anything

Two claims, both measured rather than argued from the macro definitions.

**The canonical build is unchanged.** Three builds, alternated five times so
machine drift cannot decide the answer: A is main before the instrumentation
existed, B is this branch built normally, C is this branch with
`QL_STAGE_TIMING` on.

| build | min ms/judgement | median ms/judgement |
| --- | --- | --- |
| A, pre-instrumentation main, canonical | 62.623 | 65.309 |
| B, this branch, canonical | 62.157 | 63.709 |
| C, this branch, instrumented | 62.956 | 63.452 |

The three are inside each other's run-to-run spread, and the ordering flips
between rounds. **On a judgement workload this comparison cannot resolve the
instrumentation at all**, because the solver's own variance is larger than
every non-solver stage put together. Saying "no overhead" from this table alone
would be reading noise.

**So it was measured where it could show.** The `coverage` path has no solver,
and runs the frontend and the lowering once each per unit — the two hooks in
W1-owned files fire 2,100 times in 300 ms. Alternated, minimum of five per
turn, three turns.

| build | min wall, 1,050 units | per unit | digest |
| --- | --- | --- | --- |
| B, canonical | 299 ms | 0.2848 ms | 3681642204 |
| C, instrumented | 296 ms | 0.2819 ms | 3681642204 |

The digests are identical, so the measurement target did not move. The
instrumented build comes out 1% *faster*, which is the spread, not a speedup.
**The hooks are below the resolution of the tightest measurement available.**
In the canonical build the macros expand to nothing, so there is no code to be
slower.

## Does the parallel region actually run in parallel

Yes, and the ceiling is not what occupancy alone suggests. Same 244 pairs, one
session per worker, minima over three passes.

| workers | batch wall | ms/judgement | speedup | in-thread mean | solver mean | occupancy |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 16.358s | 67.0 | 1.00x | 67.0 | 65.0 | 100.0% |
| 2 | 8.814s | 36.1 | 1.86x | 74.7 | 72.8 | 99.8% |
| 4 | 6.347s | 26.0 | 2.58x | 73.5 | 71.4 | 69.6% |
| 8 | 6.231s | 25.5 | 2.63x | 80.9 | 78.1 | 39.6% |
| 12 | 5.995s | 24.6 | **2.73x** | 94.2 | 89.1 | 31.5% |
| 16 | 6.158s | 25.2 | 2.66x | 111.8 | 102.4 | 27.7% |

Occupancy here is measured in-thread: each worker timed its own `check()` call,
so it stays meaningful at every worker count. It is
`sum(per-judgement) / (wall * workers)`.

**Two workers is nearly linear and occupancy is 100%.** The region is genuinely
parallel; nothing is serialised behind a lock at that width.

**Per judgement the work inflates, and the inflation is the solver.** In-thread
time goes 67.0 to 111.8 ms between one worker and sixteen. The solver row
accounts for 65.0 to 102.4 of that, so **83% of the growth is the solver**.
Our own stages inflate more in relative terms — the parse is 4x and the
lowering 6x by sixteen workers — but they start at 0.17 and 0.20 ms, so all of
that growth together is under 2 ms.

**Work outside the method stays small at every width**: 1.27 ms at one worker,
5.71 ms at sixteen. Whatever the pool is doing when it is not judging, it is
not our per-judgement C code.

### The occupancy collapse is the tail, not contention

Occupancy falling to 28% looks like contention and is not.

| workers | batch wall | slowest single judgement | share of wall |
| --- | --- | --- | --- |
| 1 | 16.284s | 5.048s | 31.0% |
| 4 | 6.708s | 5.921s | 88.3% |
| 12 | 6.356s | 6.104s | **96.0%** |
| 16 | 6.971s | 6.739s | **96.7%** |

**One judgement in the 244 takes five to seven seconds, and a batch cannot
finish before its slowest member.** From four workers up, that single query is
almost the entire wall: every other worker has run out of work and is waiting
for it. That is why occupancy falls, why speedup stops near 2.7x, and why more
capacity does not help.

The distribution says the same thing from the other side. The solver's median
is 12.8 ms and its p90 is 45.8 ms, and its mean, 62.2 ms, is above its p90.
**A mean above the p90 means a handful of queries carry the batch.**

Two consequences, neither of which is a change to the batch API:

- **Throughput numbers from this corpus are tail-bound.** `baseline.md`'s
  ladder reached 465 pairs/s on synthetic pairs whose solver time is uniform.
  The same code on real bodies stops at 2.7x, because real bodies have a tail.
  Both numbers are correct about different inputs.
- **The one lever that would help is scheduling, not parallelism**: start the
  long judgements first, or cap them, so the tail is not what the batch waits
  on at the end. `check_batch` currently submits in input order. Whether that
  is worth doing is a decision for the owner of the batch API; this document
  only establishes that the tail, not contention, is what bounds the batch.

The slowest judgement also gets slower with more workers, 5.05s to 6.74s, which
is the oversubscription the per-judgement inflation above already shows.

## Reproduce

Extract the corpus and build both trees. The instrumented build is a separate
build directory; the preset list is untouched.

```sh
python3 tools/corpus/extract.py \
    --corpus "$QL_CORPUS" --split val --out out/corpus/val

cmake --preset linux-clang
cmake --build --preset linux-clang --parallel

cmake -S . -B out/build/linux-stage -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS=-DQL_STAGE_TIMING
cmake --build out/build/linux-stage --parallel
```

The breakdown, and the worker ladder:

```sh
PYTHONPATH=out/build/linux-stage/bindings/python/package \
    python3 scripts/perf/bench-stages.py --corpus out/corpus/val --workers 1

PYTHONPATH=out/build/linux-stage/bindings/python/package \
    python3 scripts/perf/bench-stages.py \
    --corpus out/corpus/val --workers 1,2,4,8,12,16
```

`--synthetic N` runs the same breakdown over the pairs `baseline.md`'s ladder
uses, which is how the two documents' numbers are lined up.

The overhead check is `scripts/perf/bench-coverage.sh` pointed at each build in
turn, alternating:

```sh
QL_PERF_EXE=out/build/linux-clang/quodlibet ./scripts/perf/bench-coverage.sh val 5
QL_PERF_EXE=out/build/linux-stage/quodlibet ./scripts/perf/bench-coverage.sh val 5
```

Run the two alternately and compare minima. Running one five times and then the
other five times lets machine drift decide the answer; `baseline.md` records
where that flipped a sign.

## What this changes

- **The judgement path has no compiler-side hot spot.** Parsing, lowering and
  the miter are 0.6% of a judgement combined. Optimising them changes the
  `coverage` tool and nothing a caller of `check()` will notice.
- **The duplicate parse is not worth removing from the judgement path.**
  0.13%.
- **Batch throughput on real corpus bodies is bounded by one slow query**, not
  by contention and not by the batch API.
- **The remaining lever inside a judgement is the solver**: the query the miter
  hands to Bitwuzla, and how many round trips a verdict needs. That is the SMT
  encoding, not the engine's own bookkeeping.
