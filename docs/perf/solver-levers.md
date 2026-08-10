# Sound levers on the solver cost

**Status: unfinished. Levers 1 and 2 are measured, lever 3 was stopped mid
screening on a user instruction to close the session.** What is written here
is what was actually measured. The open part is marked open, with what it
would take to resume.

`stages.md` put 98.8% of a judgement inside Bitwuzla. `parallel.md` ruled the
two fixed costs around the solving untouchable: the integrity hashing is a
soundness control and four hashes is the minimum that brackets both execs, and
the spawn is the process isolation itself. What is left is the solving, and
there are exactly three levers on it that do not break either contract.

## Lever 1: the e-graph is not on this path

**There is no on/off to measure. The e-graph never runs during a judgement.**

Nothing in `src/proof_smt.c`, `src/product.c` or
`bindings/python/src/ql_check.c` calls any `ql_egraph_*` entry point, and the
only method `check()` registers is the SMT product method. Grep across the
tree finds the e-graph referenced from two files: `src/egraph.c`, which is the
engine, and `src/egraph_check.c`, which is a *checker* for e-graph proofs and
says so in its own header comment — "Nothing in this file calls the e-graph
engine."

So the question "does e-graph simplification make the solver's queries easier"
cannot be answered by toggling anything. Answering it means first wiring the
e-graph into the miter path, which is a design change and not a measurement,
and which belongs to whoever owns that path rather than to this workstream.
Recorded here so the lever is not re-opened as if it were a switch.

## Lever 2: query size is what drives solver time, and the tail is size

Measured. This one has an answer.

A new per-query hook (`QL_STAGE_QUERY_LOG`, `QL_STAGE_QUERY_DIR`, both behind
`QL_STAGE_TIMING`) records size, wall time and answer for every `check-sat`,
and keeps the query text so a slow one can be replayed. Over the val corpus,
1,118 queries.

**A judgement runs exactly two `check-sat` calls.** The answers come back 560
`unsat` and 558 `sat`, which is the shape the pipeline predicts: one violation
query expected UNSAT, one domain query expected SAT, per judgement. There is no
third query hiding anywhere.

| | median | p90 | p99 | max |
| --- | --- | --- | --- | --- |
| solve time | 4.37 ms | 28.91 ms | 164.21 ms | **3,388 ms** |
| query size | 14,495 B | 69,017 B | | **673,741 B** |

**Pearson correlation between query bytes and solve time is 0.836.**

The eight slowest queries are all 673 KB, which is **46x the median size**, and
they are the two queries of one corpus pair, seen twice because the harness
makes a classification pass and then a measured pass:

| ms | bytes | answer |
| --- | --- | --- |
| 3,388 | 673,726 | sat |
| 3,310 | 673,726 | sat |
| 3,096 | 673,726 | sat |
| 2,958 | 673,726 | sat |
| 2,074 | 673,741 | unsat |
| 2,009 | 673,741 | unsat |
| 1,913 | 673,741 | unsat |
| 1,846 | 673,741 | unsat |

**So the tail `parallel.md` found is not hard logic, it is the biggest miter.**
The four pairs that dominate the batch wall are the four pairs whose product
program serialises to the most SMT-LIB. That is a statement about the encoding,
not about Bitwuzla.

**The code-side win is therefore miter size, which is W1's.** The routing
recommendation, with what is and is not known:

- **What**: reduce the SMT-LIB a product query emits for the largest pairs.
  The correlation says time follows bytes at 0.836 across three orders of
  magnitude of size, so a smaller encoding of the same relation should move the
  tail.
- **Why here**: `stages.md` already showed the miter *construction* is 0.06 ms,
  so this is not about making `ql_product_query_build` faster. It is about what
  it emits.
- **Expected ceiling, stated as a ceiling**: unknown, and this workstream
  cannot bound it. 0.836 is a correlation over a corpus, not a model of what a
  smaller encoding of these particular functions would cost to solve. Anyone
  acting on this should re-measure on the specific pairs rather than scale the
  correlation.
- **Not attempted here**: the miter and the product encoding are W1's, and the
  domain query's structure is entangled with the proof pipeline (W6).

The dumped queries are the artefact to hand over: with
`QL_STAGE_QUERY_DIR` set, every query is written as `<digest>.smt2`, so the
four heavy pairs can be inspected and re-measured directly without rerunning
the corpus.

## Lever 3: Bitwuzla options — OPEN, stopped during screening

**This section is deliberately incomplete. No option was adopted, and no
variant has a measured result to report.**

### What was ruled out before measuring, and why

`--bv-solver prop` and `--bv-solver preprop` are propagation-based local
search. Local search can find a satisfying assignment; it cannot establish
unsatisfiability. Half of every judgement is a violation query whose useful
answer is UNSAT, so these would not make the pipeline faster, they would make
it stop proving. They were excluded rather than benchmarked. `--bv-solver`
stays `bitblast`.

The same reasoning excludes anything that trades completeness for speed. A
faster `unknown` is not a faster judgement.

### What was being screened

All of these are answer-preserving by construction — a different SAT engine, a
different rewrite level, or a preprocessing or abstraction pass toggled — so
none of them makes the decision procedure incomplete:

`--sat-solver kissat`, `--sat-solver cms`, `--sat-solver gimsatul` (the
shipped default is `cadical`); `--rewrite-level 0` and `1` (default 2);
`--abstraction false`; `--abstraction-bvadd true`; `--abstraction-ite true`;
`--abstraction-eager-refine true`; `--pp-contr-ands true`;
`--pp-elim-bvudiv true`; `--pp-variable-subst-norm-diseq true`.

The harness is `scripts/perf/bench-solver-options.py`. It replays the recorded
queries — the six slowest and a fixed random sample of the rest, reported as
two separate columns because an option can help the tail and hurt the body —
and requires every query to return the same answer as the shipped options
before a variant is allowed to count at all.

### How far it got

**Not far enough to report anything.** The screening pass was still running
when the session was closed and produced no output, so there is no partial
ranking here, not even a provisional one. Nothing about which options help,
hurt, or do nothing has been measured.

### To resume

1. Regenerate the query corpus, which is not committed:
   ```sh
   rm -rf /tmp/qlq && mkdir -p /tmp/qlq && rm -f /tmp/qlq.tsv
   QL_STAGE_QUERY_LOG=/tmp/qlq.tsv QL_STAGE_QUERY_DIR=/tmp/qlq \
       PYTHONPATH=out/build/linux-stage/bindings/python/package \
       python3 scripts/perf/bench-stages.py --corpus out/corpus/val --workers 1
   ```
2. Screen once, cheaply, to eliminate the variants that are clearly not worth
   repeating. Budget it: the six heaviest queries are two to three seconds
   each, so one pass over thirteen variants is tens of minutes.
   ```sh
   python3 scripts/perf/bench-solver-options.py \
       --queries /tmp/qlq --log /tmp/qlq.tsv \
       --bitwuzla third_party/bitwuzla-linux-x86_64/bin/bitwuzla \
       --heavy 6 --sample 20 --repeat 1
   ```
   Consider `--heavy 2` for the first pass; the heavy column is most of the
   runtime and its ordering is usually clear with two.
3. Re-measure only the survivors with `--repeat 3`. This workstream has
   already been bitten once by trusting a single pass: the parallel speedup
   read 6.1x and 7.2x on consecutive runs of identical code.
4. A variant that survives both is still not adoptable until the full corpus
   is re-judged with it and every verdict is unchanged and the verifier still
   passes. Answer equality on the replayed subset is a screen, not the
   differential.
5. If a winner is adopted, it goes in the argument vector in
   `bitwuzla_check` (`src/solver.c`), which is the adapter lane, and the
   before/after belongs in this file.

## What is closed and what is not

- **Lever 1 is closed as unmeasurable**: there is no e-graph on the judgement
  path to turn off.
- **Lever 2 is closed as measured**: query size drives solve time at 0.836
  correlation, the tail is the largest miters, and the code-side lever is miter
  size, which is W1's. This workstream did not touch it.
- **Lever 3 is open.** No option has been measured. Nothing has been adopted,
  and the shipped invocation is unchanged.
- Because lever 3 is unfinished, **whether the adapter lane still has a sound
  lever left is not yet known.** That question is what resuming answers.
