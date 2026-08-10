# The parallel judgement ceiling

`stages.md` established that a judgement is 98.8% solver, and left one thing
open: at eight or more workers the pool's occupancy fell to under 30% while the
work outside the SMT method stayed under 6 ms, so the idle was not our
per-judgement C code and its cause was not measured. This is that measurement.

Three findings, in the order they were established.

1. **The occupancy collapse was entirely the tail.** Four pairs out of 279.
2. **The GIL is not involved.** The binding releases it around a judgement.
3. **What is left of the non-linearity is ours, not Bitwuzla's.** A judgement
   hashes 18 MB and forks twice before Bitwuzla does any work, and both of
   those scale worse with worker count than the solving does.

The third is a soundness control, not an accident, so it was measured and then
left alone. The thinning that looked available turns out not to be: four hashes
per judgement is the minimum that brackets both execs, shown by counterexample
below. What the control costs is recorded here as its price, not as a backlog
item.

## 1. The occupancy collapse was the tail

`stages.md` recorded that at twelve workers a single judgement was 96% of the
batch wall. That was the whole story, and dropping the outliers proves it.
`bench-stages.py --max-ms 200` excludes pairs whose serial judgement ran longer
than 200 ms. **Four of 279 qualify.** With those four gone:

| workers | batch wall | ms/judgement | speedup | occupancy |
| --- | --- | --- | --- | --- |
| 1 | 7.000s | 25.46 | 1.00x | 99.9% |
| 2 | 3.550s | 12.91 | 1.97x | 99.0% |
| 4 | 1.917s | 6.97 | 3.65x | 97.4% |
| 8 | 1.150s | 4.13 | 6.16x | 95.0% |
| 12 | 0.968s | **3.52** | **7.23x** | 94.3% |
| 16 | 1.039s | 3.78 | 6.74x | 93.9% |

Minima over three passes; occupancy from the pass whose wall is quoted.
Occupancy is measured in-thread, `sum(per-judgement) / (wall * workers)`.

**Occupancy never leaves the nineties.** The same code that looked like it was
idling 70% of its capacity is busy 94% of the time once four pairs are removed
from the sample. Nothing was fixed between the two measurements.

The peak is at twelve workers, which is this machine's physical core count, and
sixteen is worse than twelve. `baseline.md` found the same shape on synthetic
pairs: this workload saturates at physical cores and SMT siblings do not add.

**Both numbers are true and they answer different questions.** 2.7x is what a
caller gets on this corpus, because this corpus has four judgements that take
five to seven seconds and a batch cannot finish before its slowest member.
7.23x is what the parallel machinery delivers. Quoting either alone misleads:
the first blames the engine for the corpus, the second hides a real effect a
caller will meet.

The lever for the first is scheduling, not parallelism. `check_batch` submits
in input order; longest-first would let the tail overlap the rest instead of
trailing it. That is the batch API owner's call, and this document only
establishes that it is the tail that bounds the batch.

## 2. The GIL is not involved

`bindings/python/src/quodlibet_module.c:337` and `:481` wrap the judgement in
`Py_BEGIN_ALLOW_THREADS` / `Py_END_ALLOW_THREADS`, so the interpreter lock is
released for the whole C call. The occupancy above says the same thing from the
outside: threads that were waiting on the GIL could not sit at 94% occupancy
with wall-clock speedup climbing to 7.23x.

## 3. What is left is our own fixed cost

`stages.md` timed the solver round trip as one bucket. It is now split three
ways: the integrity hashing this engine does, the spawn the kernel does, and
the run Bitwuzla does. Medians per judgement, full corpus.

| workers | integrity hash | process spawn | bitwuzla run | method total |
| --- | --- | --- | --- | --- |
| 1 | 4.004 | 1.570 | 7.836 | 14.166 |
| 4 | 4.425 | 2.628 | 8.665 | 17.058 |
| 8 | 6.459 | 3.293 | 9.115 | 21.471 |
| 12 | 9.235 | 4.848 | 11.217 | 29.447 |
| 16 | 14.066 | 6.995 | 13.396 | 43.950 |
| **1 to 16** | **3.51x** | **4.46x** | **1.71x** | **3.10x** |

**The thing that scales worst is not the solver.** Bitwuzla's own median run
grows 1.71x between one worker and sixteen, which is ordinary oversubscription
on twelve physical cores. Our two fixed costs grow 3.5x and 4.5x.

**At sixteen workers the integrity hash (14.07 ms) is larger than Bitwuzla's
median run (13.40 ms).** Even at one worker, hashing plus spawning is 39% of a
median judgement; at sixteen it is 47%.

### Where the 18 MB comes from

A judgement asks the solver twice: once whether the miter is satisfiable, once
whether the comparison domain is inhabited. An UNSAT over an empty domain
proves nothing, so both are required.

Each `check-sat` verifies the Bitwuzla snapshot's digest twice, before the exec
and after it (`src/solver.c`, `verify_snapshot_digest`). The snapshot is
4,490,928 bytes. **Four hashes per judgement, about 18 MB.**

That measured 4.004 ms at one worker, which is 4.49 GB/s. That is BLAKE3's
AVX2 compute rate on a warm page cache, so **the cost is the hashing, not the
file I/O.** A bigger read buffer or an mmap would not move it; `blake3` here is
already built with the vendored assembly (`baseline.md`, optimisation 3).

The spawn is two `fork`/`exec` of that same 4.5 MB binary per judgement, 1.57 ms
serially. `baseline.md` measured spawn in isolation at 0.92 ms and concluded it
parallelises well; in a full judgement under load it reaches 7.0 ms at sixteen
workers, so that earlier conclusion holds only at low width.

## What the hash covers, exactly

Not the query, and not any solver state. `verify_snapshot_digest`
(`src/solver.c:1707`) hashes `state->executable` — the session's private
snapshot of the Bitwuzla binary — and compares it against
`state->executable_digest`, recorded when the session installed that snapshot.
**It is the binary's identity, and it is invariant for the life of the
session.** The query has its own digest, taken in memory over the SMT-LIB text
at `src/solver.c:2260`; that one is cheap and is not part of the 18 MB.

The invariance is the reason the check exists, not a reason to do it less.
`execve` loads the binary from disk on every spawn, so what has to be
established is not "is this value still the value we computed" but "does the
file the kernel is about to load still hold the recorded bytes". A cached
answer cannot establish that; only reading the file can.

## Four hashes is the minimum, and that is a proof, not a preference

The control is a bracket per exec: a verification immediately before the exec
and one immediately after it. A judgement runs two execs, so it holds four
verifications:

```text
H1  exec1  H2   ...   H3  exec2  H4   -> verdict returned
```

The adoption test for thinning this to three is result equivalence: **every
binary tampering the four-hash scheme rejects must also be rejected by the
three-hash scheme.** It is not, and the counterexample is short.

Dropping `H3` — the merge that looked redundant, because no exec happens
between `H2` and `H3` — loses exactly the tamperings that are observable at
`H3` and at neither `H2` nor `H4`. That set is not empty: a file that is
modified after `H2` and restored before `H4` is caught today by `H3` and would
not be caught without it. **That window contains `exec2`.** The scheme would
keep hashing four megabytes twice per judgement and stop catching the case the
second exec is exposed to.

Dropping `H2` instead is worse: `H2` is `exec1`'s after-hash, which is the
verification that protects the answer `exec1` already produced.

So neither merge preserves the rejected set, and four is the minimum for
bracket-per-exec over two execs. **Option (b) is not adopted**, on its own
stated criterion.

What the bracket does and does not claim is worth writing down, because it
bounds what anyone can promise from the cheaper variants too. The bracket
catches any tampering that persists across the after-hash. An attacker who can
write the file and time a revert inside the window between the after-hash and
the next read is not stopped by four hashes either. The control is integrity
against corruption and against tampering that persists, and the reduction
argument has to be evaluated against that, which is what the counterexample
above does.

## Decision

Recorded so the next reader does not reopen it.

| option | judgement median at 12 workers | outcome |
| --- | --- | --- |
| (a) leave it at four | 29.4 ms | **adopted.** The numbers in this document are the honest price of the control |
| (b) merge to three | ~26 ms | **rejected.** Result equivalence fails; see above |
| (c) verify once per session | ~22 ms | **rejected by the coordinator.** A binary swapped between judgements would contaminate answers already returned, and nothing would catch it |

The 12-worker figures are subtraction from the measured hash cost and are a
lower bound: removing memory traffic would also relieve the contention that
inflates the other stages, which subtraction does not capture. They are listed
so the price is legible, not because either row is available.

**Process reuse or a solver pool is not on this list either.** Process
isolation is what keeps one query from seeing another's state, and `AGENTS.md`
keeps Bitwuzla as the solver rather than something driven in-process. Removing
the spawn means removing the isolation, so the 1.57 ms serial and 7.0 ms at
sixteen workers stay.

## Reproduce

Build the instrumented tree as `stages.md` describes, then:

```sh
# Full corpus: what a caller gets, tail included.
PYTHONPATH=out/build/linux-stage/bindings/python/package \
    python3 scripts/perf/bench-stages.py \
    --corpus out/corpus/val --workers 1,2,4,8,12,16

# Tail excluded: what the parallel machinery delivers.
PYTHONPATH=out/build/linux-stage/bindings/python/package \
    python3 scripts/perf/bench-stages.py \
    --corpus out/corpus/val --max-ms 200 --workers 1,2,4,8,12,16
```

Report minima over at least three passes. A single pass put the 16-worker
speedup anywhere between 6.1x and 6.7x, which is wide enough to invent a trend
that is not there.

## What this changes

- **`stages.md`'s open question is closed.** The idle between judgements was
  the tail, not contention and not our code.
- **The parallel path itself is sound and scales to 7.23x on twelve physical
  cores**, with occupancy above 93% throughout. There is no lock to remove and
  no GIL to release.
- **The remaining non-linearity is a fixed per-judgement cost that is ours**:
  18 MB of hashing and two forks, together 39% of a median judgement serially
  and 47% at sixteen workers.
- **That cost is a soundness control and it stays.** Four hashes is the
  minimum that brackets both execs, shown above by counterexample rather than
  asserted. The numbers here are its price, not a backlog item.
