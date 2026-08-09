# Proof and Refutation Methods

## Purpose

Quodlibet does not have a privileged equivalence algorithm. A user selects one
method, several independent methods, or a pipeline in which one method prepares
an artifact for another. Method selection changes search strategy and supported
fragments, but never changes the problem's semantic contract.

The names and JSON examples below specify the intended method interfaces. They
do not imply that every named plugin is implemented or registered yet. Until a
method exists, pipeline validation must fail instead of substituting a different
method.

## Common logical contract

Every method consumes the same versioned `quodlibet.problem` identity. That
identity includes the two functions, the lowered precondition, the complete
`ql_semantic_contract_v1`, and the language and target profiles. A transformation
must carry the input problem digest and transformation evidence into its output.
It must not silently narrow inputs, observations, or undefined-behavior rules.

For an input state `x` satisfying precondition `P`, let `Beh(F, x)` be the set of
behaviors of function `F` after projection onto the selected observations. The
projection can contain:

- return value;
- final reachable memory, ordered writes, or the full memory trace;
- termination or divergence;
- traps, which are defined observable events rather than undefined behavior;
- ordered external calls, including callee identity and contract-selected
  arguments, results, and effects;
- volatile, atomic, and I/O event traces;
- definedness according to the selected UB policy.

`QL_RELATION_LEFT_REFINES_RIGHT` means
`Beh(left, x) subseteq Beh(right, x)` for all `x` satisfying `P`.
`QL_RELATION_RIGHT_REFINES_LEFT` reverses the operands. Equivalence means equality
of behavior sets. A method must record the direction it actually discharged.

The UB policies are interpreted as follows:

- `QL_UB_MUST_MATCH` requires equal defined input domains. Selected behaviors
  must satisfy the requested relation on that common domain.
- `QL_UB_LANGUAGE_REFINEMENT` models UB as permitting every behavior and applies
  the requested behavior-set relation. Thus a refining side cannot introduce UB
  on an input where the refined side is defined. A defined refining side remains
  admissible where the refined side has UB.
- `QL_UB_COMPARE_WHERE_BOTH_DEFINED` restricts comparison to the intersection of
  the defined domains. It is deliberately opt-in. Evidence must report the
  intersection condition because an empty intersection makes the claim vacuous.

No method may reinterpret ignored observations as assumptions. If memory is not
observed, for example, the functions may differ in memory behavior. That does
not grant either method permission to assume memory is immutable while proving
an observed return value.

## C and machine profile

The first source profile is `QL_C_DIALECT_ASM2C_GNU_V1`: the frozen
GCC/Clang-compatible subset used by the asm2c corpus, targeting x86-64 Linux SysV
LP64. Corpus provenance covers GCC and Clang, PIC and non-PIC, and O0 through O3.
Optimization level is validation provenance, not source semantics.

The corpus does not preserve exact compiler versions, an explicit `-std`, or the
complete compilation command. A method must therefore use Quodlibet's frozen
profile, not the current host compiler's default. Target feature groups include
x86-64 base, the SSSE3/SSE4/AVX/AVX2/POPCNT/LZCNT domain baseline, AVX-512,
crypto, bit manipulation, hardware random, TSX, and timing. Unsupported syntax,
intrinsics, instructions, or implementation-defined cases must be rejected or
produce `UNKNOWN`. They must never be assigned guessed semantics.

## Required semantic IR

All proving methods operate after preprocessing, name and type resolution,
implicit-conversion insertion, and lowering into a versioned typed IR. A
Tree-sitter concrete syntax tree is not proof input. The common IR must make the
following explicit:

- fixed-width integers, floats with a named floating-point model, pointers,
  aggregates, memory objects, and event traces;
- control-flow blocks, phi or block parameters, calls, returns, traps, and
  termination states;
- allocation identity, pointer provenance, byte offsets, alignment, object
  lifetime, readable and writable ranges, and alias constraints;
- loads, stores, volatile accesses, atomics and memory order, I/O, and external
  calls as ordered effects where requested;
- every modeled source of UB as a definedness predicate, separate from traps;
- the input precondition lowered and type-checked in the same semantics as the
  two functions;
- the exact observation projection and relation direction.

A method advertises a capability descriptor covering IR schema versions, type
and instruction fragments, loop and recursion support, memory models,
observation modes, UB policies, relations, target features, and whether it emits
checkable proof or counterexample evidence. `validate` must reject an unsupported
axis before execution. Until a dedicated unsupported status is added, the
method should return `QL_STATUS_TYPE_MISMATCH` with the first unsupported axis in
the diagnostic. Ignoring the axis and continuing is unsound.

## Verdict discipline

The distinction between proof and finite search is mandatory:

- `PROVED_EQUIVALENT`, `PROVED_LEFT_REFINES_RIGHT`, and
  `PROVED_RIGHT_REFINES_LEFT` require a sound argument over every input satisfying
  the precondition and every behavior admitted by the contract. Production
  acceptance also requires the advertised proof evidence to pass its checker.
- `COUNTEREXAMPLE` requires a concrete witness satisfying the precondition and a
  replay that demonstrates a contract-visible violation. A raw solver model or
  crash without semantic replay is only a candidate witness.
- `BOUNDED_CLEAN` means that no violating witness was found inside the recorded
  finite bounds. Bounds include all relevant dimensions, such as loop visits,
  recursion depth, path count, allocation size, symbolic bytes, and test count.
  It is never promoted to `PROVED` by elapsed time, method count, or agreement.
- `UNKNOWN` means that no sound logical conclusion was reached. Timeout,
  resource exhaustion, unsupported theory after execution, and incomplete
  saturation normally lead here. Transport and plugin failures remain
  `ql_status` failures rather than logical verdicts.

An exhaustive finite analysis may return `PROVED` only when evidence establishes
that its finite model covers the complete contract domain. Merely reaching a
user-supplied bound returns `BOUNDED_CLEAN`.

A caller-supplied verdict policy decides which of these verdicts it counts as
a pass and what it calls them. It never produces a verdict, and it cannot
weaken this discipline: the policy parser rejects any policy that would call
`BOUNDED_CLEAN` a proof, treat an unreplayed SAT model as a `COUNTEREXAMPLE`,
or promote a raw solver `unsat` without a checker or a stated trusted backend.
An execution that exhausted its budget is withdrawn to `UNKNOWN` before any
policy sees it. See `docs/runtime-services/policy.md` and
`docs/runtime-services/budget.md`.

## Method catalogue

### E-graph normalization

Recommended method name: `normalize.egraph`.

An e-graph represents equivalent IR expressions in shared equivalence classes,
applies guarded rewrites to saturation or a resource limit, and extracts one or
more low-cost forms. It is useful before SAT, SMT, or symbolic execution because
it can expose common structure and remove algebraic noise.

Required input is typed, effect-aware IR. Pure expressions may use ordinary
equality saturation. Memory, calls, volatile and atomic events, traps,
definedness predicates, and termination tokens require ordered effect nodes or
side conditions that prevent reordering. Pointer and integer rewrites must be
guarded by provenance, overflow, alignment, and UB facts from the IR.

Soundness comes from a checked rewrite library and a reconstructed rewrite proof
whose guards are discharged under the precondition. Extraction cost and rewrite
schedule do not affect soundness. Saturation is incomplete and resource-bounded,
so failure to merge the two roots is `UNKNOWN`, not inequivalence. Root merging
may yield `PROVED` only when the proof checker confirms that the roots denote the
full selected behavior, including effects and definedness. The default role is a
normalizer that emits IR plus transformation evidence, not a final oracle.

Important options include rewrite-set identity, node and iteration limits,
extraction cost model, proof reconstruction, and whether conditional rewrites
may invoke a side-condition solver. The rewrite-set digest belongs in evidence
and cache keys.

### SMT product program

Method name: `prove.smt-product`. This is the first implemented method. The
paragraphs below the implementation notes describe the general interface; the
implementation currently covers the loop-free fragment of that interface over
scalars and the flat memory model, and refuses the rest.

#### Implementation, schema v1

The method consumes one `quodlibet.problem` artifact and produces one
`quodlibet.outcome`. It requires problem schema v2 and rejects v1 in
`validate`, before execution: a v1 problem records no argument correspondence,
so there is no stated relation between the two input lists to encode.

It lowers both functions from the problem's own sources, so the IR it reasons
about is derived from the problem rather than supplied alongside it. A
function the semantic C lowering reports as `UNKNOWN` yields an `UNKNOWN`
outcome with a diagnostic, never a narrower question.

The relational encoding flattens each acyclic control-flow graph into path
conditions, shares one symbolic input per corresponding argument, and names
every intermediate value with a nullary `define-fun`. It emits one shared
SMT-LIB prefix and two terminal assertions:

- `quodlibet_violation` holds exactly when the declared relation is broken;
- `quodlibet_domain` holds exactly when the comparison domain is inhabited.

Each observation axis is encoded explicitly. The return-value axis compares
whether a normal return happened before it compares the value, because a trap
or a divergence produces no return value at all. The volatile, atomic, I/O, and
external-call axes are discharged only after establishing that neither IR
carries the corresponding effect; an effect the encoding cannot state is a
`QL_STATUS_TYPE_MISMATCH` naming the axis, never a dropped obligation.
Definedness is governed by the UB policy, as this document specifies, so
`QL_OBSERVE_UNDEFINED_BEHAVIOR` adds no separate conjunct.

#### The flat memory model in the miter

Under the `ASM2C_GNU_V1` profile memory is a flat 64-bit address space of
bytes, which the query states as one `(Array (_ BitVec 64) (_ BitVec 8))` in
`QF_ABV`. A pointer takes the bit-vector sort of its width, since a pointer is
an address and nothing more in this profile. An access of `W` bytes is `W`
`select`s concatenated, or `W` nested `store`s, in little-endian order, which
is byte for byte the order `ql_ir_interp_run` uses; an access whose width is
not a whole number of bytes is refused rather than rounded.

Both sides share the initial memory constant and one base and size constant per
object, so the two functions run over the same storage without either side
describing it to the other. The object table follows from the pointer arguments
in source order on each side and is matched through the problem's argument
correspondence, so a correspondence that permutes the two argument lists still
binds the same object to the same pair of constants.

**Neither the model's standing constraints nor the access-definedness predicate
is restated here.** The lowering already emits them as ordinary IR: `ASSUME`
instructions for disjointness, the first-page floor, and the no-wrap bound, and
a `UB_GUARD` over ordinary arithmetic at each access. The miter encodes those
instructions like any others, so the query and `ql_ir_interp_run` cannot come
to disagree about which layouts are admissible or which accesses are defined.
The assumptions are conjoined into both terminal queries, so an `unsat`
violation is never vacuous through an unsatisfiable layout.

The memory observation is the final reachable state: the memory the reached
return carries, compared byte by byte inside the objects. It is stated with one
free address constant rather than a quantifier. In the violation query a free
constant is existential, which is exactly "some address differs"; in the same
query answered `unsat` it is universal, which is exactly "every address
agrees". One constant is therefore precise in both directions and the logic
stays quantifier-free. The comparison is gated on both sides terminating, since
a trapping or diverging run leaves no final memory. A contract that asks for
`QL_MEMORY_ORDERED_WRITES` or `QL_MEMORY_FULL_TRACE` is refused: this encoding
has no term for a write order.

A model may describe an object far too large to materialize for replay. The
query therefore also carries a **search-only** terminal assertion: the same
violation claim with every object size bounded. It is used solely to obtain a
replayable model after the unbounded query returned one that is too large.
`sat` on it is still a genuine violation and may be replayed; `unsat` on it
proves nothing whatsoever and is never promoted. The proof path reads only the
unbounded violation query.

Every UB policy conjoins the observation obligation with both sides being
defined. SMT-LIB totalizes division, remainder, and shift; that totalization
therefore never reaches an observation claim on an input whose C semantics are
undefined.

A `sat` answer is a candidate. The method decodes the model into typed inputs,
requires an exactly-width literal for every declared input, runs both
functions through the IR interpreter, re-evaluates the typed precondition
concretely, and re-derives the relation on the concrete results. Only a replay
that reproduces the violation becomes `COUNTEREXAMPLE`. A replay that does not
reproduce is logged as an encoding defect and reported as `UNKNOWN`; the
counterexample serializer refuses an unconfirmed witness outright.

When the query has objects, the replay also rebuilds them from the model: each
object's base and size, and its initial image from the model's array term. The
three standing constraints are re-checked on the rebuilt layout instead of
being assumed, and an object whose image is too large to materialize leaves the
replay undecided rather than being silently replaced by a smaller one. When the
contract observes memory, both runs write their final images and the replay
compares them, so a difference that lives only in memory is confirmed by the
same concrete path as a difference in a return value.

#### UNSAT promotion boundary

Bitwuzla 0.9.1 exposes no proof object, so no certificate exists for a checker
to validate. Quodlibet therefore takes the second admissible route: an
explicit, opt-in, recorded trust policy, selected by the `unsat_promotion`
option.

`"none"` is the default. A raw `unsat` is retained as solver evidence and the
verdict stays `UNKNOWN`.

`"trusted-backend"` permits a `PROVED_*` verdict only when all of the
following hold, and the outcome envelope records every one of them:

1. the problem passes `ql_problem_require_proof_binding`, so both source
   signature digests, the argument correspondence, and the typed-precondition
   digest are bound into one artifact identity;
2. the miter covers exactly the contract's relation direction, UB policy, and
   observation axes;
3. the domain query answered `sat`, so the `unsat` is not vacuous;
4. the **unbounded** violation query answered `unsat`; the search-only bounded
   variant carries no proof authority at all;
5. the backend is the pinned Bitwuzla, and its name, version, executable
   content digest, and query digest are recorded;
6. the caller selected the policy explicitly.

The envelope always records `checked_proof: false` for this backend. A proof
from this path rests on trusting Bitwuzla, not on a validated certificate, and
the result says so rather than letting a reader assume otherwise. An empty
comparison domain is reported and never promoted, for every UB policy and not
only for `QL_UB_COMPARE_WHERE_BOTH_DEFINED`.

The outcome's cache key binds the problem digest, method name and version, the
selected policy and limits, all three query digests, and the backend
executable digest, so no answer is reusable across a different backend,
policy, or query.

#### Interface



This method forms a relational product of the two CFGs, shares the same symbolic
input and initial memory, and asks whether any paired execution violates the
selected relation. It should align corresponding blocks when profitable but
must retain asynchronous product steps when control flow differs. Return values,
memory projection, definedness, termination, traps, and event traces become
explicit relational obligations.

Quantifier-free fixed-width, loop-free programs with a finite memory encoding
are decidable in principle. Solver resource limits can still produce `UNKNOWN`.
Unbounded loops, recursion, unbounded allocation, quantified memory properties,
and unrestricted external-call contracts require invariants, summaries, or
abstraction. Loop unrolling alone yields at most `BOUNDED_CLEAN` after an UNSAT
query. It yields `PROVED` only if the product is loop-free, the explored state
space is otherwise complete, or separately checked invariants close every
back-edge.

A SAT result is a candidate counterexample and must be concretized and replayed.
An UNSAT result supports `PROVED` only when the encoding covers the entire
contract and the solver emits evidence accepted by the configured checker. If
proof checking is unavailable, the result is solver evidence but must not cross
a deployment trust boundary that requires checked proof.

Important options include solver and logic, timeout, path alignment strategy,
memory encoding, floating-point theory, external-call summaries, invariant
source, proof format, and unroll bounds.

### AIG/SAT miter

Recommended method name: `prove.aig-sat`.

This method bit-blasts both functions into circuits, shares input wires, and
constructs a miter whose output is true exactly when the requested relation is
violated. It is the preferred exact backend for loop-free fixed-width integer
and bit-vector code after effects have been converted into finite state and
trace vectors.

For a fully finite encoding, SAT provides a candidate witness and UNSAT plus a
checked DRAT, LRAT, or equivalent certificate proves the miter unreachable.
This fragment is complete up to solver resources. Arrays, symbolic-sized
objects, unbounded traces, floating point without exact bit-blasting, external
calls without finite summaries, unbounded loops, and recursion fall outside the
exact fragment.

Finite loop unrolling or finite memory truncation must be recorded in the bound
vector and can yield only `BOUNDED_CLEAN`, unless a separate completeness
argument proves that the bound covers all executions. Counterexample bits must
be decoded into typed inputs and replayed against the semantic IR.

Important options include SAT solver, circuit simplification passes, memory and
trace bounds, integer multiplier strategy, certificate format, proof checker,
and a maximum bit budget.

#### The circuit layer

`src/aig.c` is the and-inverter graph the miter is built in. It knows nothing
about the IR; it is a circuit library with a CNF exit, and it is tested against
ordinary C arithmetic rather than against the lowering that will use it.

Inversion lives on the edge, not in a node, so the negations a bit-level
encoding produces in bulk cost nothing. Every construction goes through one
`ql_aig_and` that folds constants, collapses the four one-operand identities,
and structurally hashes the result with its operands in a normal order. Two
syntactically identical subcircuits therefore become one node. That matters for
a miter specifically: the two functions share their input wires, so the parts
of them that agree collapse into each other before a solver is ever started.

The bit-vector layer is a ripple-carry adder, a shift-and-add multiplier
truncated to the operand width, a restoring divider that carries one extra
remainder bit, barrel shifters, and the four ordered comparisons, with signed
comparison expressed as the unsigned one on sign-flipped operands.

**Partial operations are totalized here, not decided here.** Division by zero
yields the SMT-LIB result, an all-ones quotient and the dividend as the
remainder; a shift at or beyond the width yields zero, zero, or the sign bit;
signed division overflow wraps. This matches what the SMT encoding already
does, and for the same reason: the circuit must be total, and whether the C
program was allowed to perform the operation is the UB guard's question. A
circuit that answered it would be answering it twice, in two places that could
disagree.

CNF export is Tseitin over the cone of influence of one root, so a part of the
graph the root does not reach costs the solver nothing. The DIMACS bytes are
deterministic, which is what lets a query digest identify the question. A root
that folded to a constant is reported as trivially true or trivially false
rather than sent to a solver; a trivial answer is still an answer this encoding
produced and never a checked proof.

### Bounded symbolic execution

Recommended method name: `search.bounded-symbolic`.

Bounded symbolic execution forks or merges symbolic states along CFG paths and
uses a constraint solver to search for an observed mismatch. It is effective on
branch-heavy code and supplies focused counterexamples without bit-blasting the
entire product eagerly.

The state must contain memory objects, pointer provenance, definedness, event
trace prefixes, and termination or trap state. Both sides start from the same
precondition-constrained inputs. Search bounds must include loop and recursion
depth, total states or paths, symbolic memory size, and trace length.

A replayed witness yields `COUNTEREXAMPLE`. Exhausting only the configured bounds
yields `BOUNDED_CLEAN`. It may yield `PROVED` for a loop-free finite CFG only when
coverage evidence shows every feasible path and complete solver answers for all
path obligations. State merging and subsumption must preserve the selected
observations and UB policy. Solver `unknown`, a dropped state, or a heuristic
path cap prevents proof.

Important options include search order, state merging, subsumption, per-query
and global timeouts, loop and recursion bounds, state and path caps, memory and
trace bounds, and counterexample minimization.

### Concrete differential refutation

Recommended method name: `refute.concrete-differential`.

This method generates or mutates concrete inputs satisfying the precondition,
executes both functions in an instrumented sandbox, and compares the selected
observations. It may use corpus examples, coverage guidance, property-based
generators, solver-produced seeds, or counterexamples from other methods.

The runner must make the target ABI and CPU feature assumptions explicit and
capture return values, reachable memory, termination, traps, external calls,
volatile and atomic events, and I/O as selected. Native execution alone cannot
reliably identify all C UB, so a witness is accepted only when replay under the
semantic IR or a validated instrumentation path establishes its definedness and
the contract-visible mismatch.

This is a refutation method. A validated witness yields `COUNTEREXAMPLE`. A
finite number of matching tests may yield `BOUNDED_CLEAN` only when the tests
actually exhaust a stated bound, with seed, generator, test count, size ranges,
timeout, and coverage metrics recorded. It never yields `PROVED`, even with
complete observed branch coverage. Nondeterministic external state must be
modeled, recorded, or excluded by the precondition.

Important options include generator, seed, test and size budgets, runner backend,
sanitizers, timeout, coverage guidance, CPU profile, deterministic external-call
stubs, and witness minimization.

#### The implemented runner

`src/proof_diff.c` implements this method over the loop-free scalar slice.
There is no instrumented native sandbox: both sides are executed by the IR
interpreter, which is the same semantic path a solver model must survive in
`prove.smt-product`. That removes the ABI and CPU-feature assumptions a native
runner would have to state, and it removes the question of whether native
execution silently accepted C undefined behaviour, because the interpreter
models definedness explicitly.

The runner builds the same relational product query the SMT method builds. It
solves nothing with it. The query is what states the shared input list, the
argument correspondence, and the exact observation axes, and what refuses
memory, effects, and non-scalar types instead of narrowing them. A generated
input tuple is serialised in solver-model syntax and handed to
`ql_replay_decode_model` and `ql_replay_execute`, so the generated witness and a
Bitwuzla witness travel through one decoder and one relation evaluator. A second
decoder could disagree with the first; there is not one.

The generator is deterministic: splitmix64 seeded by the recorded seed, with the
first twelve tuples assigning every input the same boundary pattern (zero, one,
all ones, the sign bit, the signed extremes, the alternating patterns, the byte
boundary, the half-width bit) before the random phase begins. The seed, the test
count, and the query digests are part of the outcome's cache key, so a different
sample is a different search and never reuses another sample's answer.

**Finding nothing is `UNKNOWN`, not `BOUNDED_CLEAN`.** `BOUNDED_CLEAN` states
that a bound was exhausted. Boundary-and-random sampling over a 64-bit input
space exhausts no bound; it covers an unmeasured fraction of one. Reporting a
passing test count as a clean bounded result would name a guarantee the search
does not provide, so the outcome records the generator, the seed, the executed
and conclusive test counts, and how many inputs the precondition rejected, all
on an `UNKNOWN` verdict. The advertised capability does not include
`QL_PROOF_RESULT_BOUNDED` under any option.

A mismatch found this way needs no separate replay stage. It was produced by
running both functions concretely and re-evaluating the typed precondition and
the relation on the concrete results, which is exactly what replay confirmation
means, so the outcome records `replay_confirmed` and emits the same
`quodlibet.counterexample` artifact the SMT path emits. `checked_proof` is
always false because this method never claims a proof.

### CHC/PDR

Recommended method name: `prove.chc-pdr`.

This method encodes the relational product as constrained Horn clauses and uses
property-directed reachability or another CHC engine to infer inductive
invariants. Its primary role is unbounded loops, recursion, and CFG pairs for
which finite unrolling is not a proof.

The encoding needs relational control locations, typed program state, memory and
event abstractions, definedness predicates, precondition clauses, transition
clauses, and a bad-state clause for the selected relation. Asynchronous products
or stuttering steps are necessary when the functions iterate at different
rates. External calls require relational summaries with explicit frame and
effect conditions.

CHC/PDR is sound when the clause encoding is sound and a checker validates the
returned inductive invariant: it contains all initial states, is closed under
every transition, and excludes every bad state. In that case it may return the
requested `PROVED` verdict for unbounded executions. The method is incomplete:
the required invariant may not be expressible in its abstract domain, nonlinear
arithmetic and heap properties may defeat inference, and resource limits may be
reached. Such cases return `UNKNOWN`, not `BOUNDED_CLEAN`, unless the method also
performed and reports a separate bounded search.

A reachable bad-state trace is only a candidate counterexample until concretized
and replayed. Abstractions may over-approximate behavior and produce spurious
traces, but they must never omit concrete behavior in a proof. Important options
include CHC engine, arithmetic and heap abstractions, predicate templates,
product alignment, supplied summaries or invariants, widening policy, timeout,
and invariant proof format.

## User-selected pipeline

The user selects methods and method-specific options in the ordinary pipeline
JSON. This example normalizes once, runs five independent searches in parallel
where dependencies permit, and combines their evidence. The method names are
the target interface described above and require corresponding plugins.

```json
{
  "schema_version": 1,
  "nodes": [
    {
      "name": "lower",
      "method": "frontend.c.semantic-lower",
      "options": {
        "dialect": "asm2c-gnu-v1",
        "target": "x86_64-linux-sysv-lp64",
        "require_error_free_tree": true
      }
    },
    {
      "name": "normalize",
      "method": "normalize.egraph",
      "depends_on": ["lower"],
      "options": {
        "rewrite_set": "c-bitvector-memory-v1",
        "node_limit": 200000,
        "iteration_limit": 20,
        "reconstruct_proof": true
      }
    },
    {
      "name": "sat",
      "method": "prove.aig-sat",
      "depends_on": ["normalize"],
      "options": {
        "solver": "cadical",
        "certificate": "lrat",
        "max_bits": 2000000
      }
    },
    {
      "name": "smt",
      "method": "prove.smt-product",
      "depends_on": ["lower"],
      "options": {
        "solver": "bitwuzla-0.9.1",
        "timeout_ms": 30000,
        "memory_encoding": "arrays",
        "unroll": 32
      }
    },
    {
      "name": "symbolic",
      "method": "search.bounded-symbolic",
      "depends_on": ["lower"],
      "options": {
        "loop_bound": 64,
        "recursion_bound": 8,
        "max_states": 50000,
        "merge_states": true
      }
    },
    {
      "name": "differential",
      "method": "refute.concrete-differential",
      "depends_on": ["lower"],
      "options": {
        "generator": "coverage-guided",
        "seed": 381775,
        "tests": 100000,
        "timeout_ms_per_test": 100
      }
    },
    {
      "name": "pdr",
      "method": "prove.chc-pdr",
      "depends_on": ["lower"],
      "options": {
        "engine": "spacer",
        "heap_abstraction": "finite-regions",
        "timeout_ms": 60000,
        "emit_invariant": true
      }
    },
    {
      "name": "verdict",
      "method": "verdict.combine",
      "depends_on": ["sat", "smt", "symbolic", "differential", "pdr"],
      "options": {
        "require_checked_proofs": true,
        "require_replayed_counterexamples": true,
        "on_conflict": "inconsistent",
        "bounded_clean": "preserve-bound-vector"
      }
    }
  ]
}
```

Users may omit any branch, change its options, use only one method, or arrange
multiple normalizers and provers. No omitted branch becomes an implicit default.
The semantic contract belongs to the input problem rather than node options so
all branches answer exactly the same question.

## Parallel combination rules

Parallel methods produce evidence, not votes. `verdict.combine` follows these
rules:

1. It accepts only artifacts that name the same problem digest, semantic-contract
   digest, IR semantics version, relation, and observation projection. Otherwise
   combination fails with a schema or type error.
2. It validates proof certificates and replays concrete witnesses according to
   configured trust policy before accepting their logical verdicts. Method name,
   solver brand, runtime, and majority agreement have no logical weight.
3. One checked proof is sufficient for the exact relation it establishes. Two
   checked opposite refinements may establish equivalence only when their
   precondition, UB policy, behavior universe, and observation projection are
   identical and the combiner applies this rule explicitly.
4. One replayed counterexample refutes the requested relation. A valid proof and
   a valid counterexample for the same contract indicate an encoding, checker,
   or trust-boundary defect. The result is `inconsistent` and must not be resolved
   by priority or voting.
5. `UNKNOWN` is neutral. Several `UNKNOWN` results remain `UNKNOWN`.
6. `BOUNDED_CLEAN` is preserved with its full bound vector. Bounds with different
   dimensions are not collapsed into one scalar, and results may be dominated or
   joined only when their covered state sets are formally comparable. Any number
   of bounded-clean results remains bounded.
7. E-graph normalization evidence composes transitively with downstream proof or
   counterexample evidence. If its proof is absent or invalid, no downstream
   result may be transferred back to the original problem.
8. Cancellation after a decisive result is a runtime optimization. It must not
   change which evidence is considered valid. When several valid artifacts are
   retained, deterministic ordering uses pipeline declaration order and artifact
   digest, not worker completion time.
9. Cache keys include the problem and contract digests, method implementation and
   options digests, semantic IR version, rewrite or solver profile, bounds, and
   checker policy. A cached result from a weaker observation or different UB
   policy is not reusable as an answer to a stronger contract.

This combination model permits fast refuters, bounded searches, normalizers, and
unbounded provers to cooperate without promoting any single method to a built-in
source of truth.
