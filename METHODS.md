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

Recommended method name: `prove.smt-product`.

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

This is a refutation method. A validated witness yields `COUNTEREXAMPLE`.
Any finite number of matching tests yields `BOUNDED_CLEAN` with seed, generator,
test count, size ranges, timeout, and coverage metrics. It never yields
`PROVED`, even with complete observed branch coverage. Nondeterministic external
state must be modeled, recorded, or excluded by the precondition.

Important options include generator, seed, test and size budgets, runner backend,
sanitizers, timeout, coverage guidance, CPU profile, deterministic external-call
stubs, and witness minimization.

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
        "solver": "z3",
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
