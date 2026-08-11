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

#### The shipped `normalize.egraph` method

`src/egraph_method.c` registers `normalize.egraph` as a built-in. It is a
normalizer and never an oracle. `ql_registry_find_proof_method` does not find
it, and it emits a `quodlibet.ir` artifact rather than a verdict.

What it accepts today is narrower than the interface above, and every narrowing
is enforced rather than assumed.

- One `quodlibet.ir` input holding a single acyclic basic block. Anything else
  fails `validate` with `QL_STATUS_TYPE_MISMATCH` instead of passing through
  unnormalized.
- The input is re-verified with `ql_ir_verify` before a term is built.
- `reconstruct_proof` must be true. There is no unchecked mode to select.
- `rewrite_set` must be `pure-bitvector-v1`, the built-in catalogue.
- An unknown option name is an error, so a misspelt limit cannot silently keep
  its default.

Only bool and bit-vector values within `max_bit_width` enter the graph, and only
effect-free single-result instructions whose opcode has a pure term operator:
`IDENTITY`, `BOOL_NOT`, `BV_NOT`, `BV_NEG`, `ADD`, `SUB`, `MUL`, `BV_AND`,
`BV_OR`, `BV_XOR`, `EQ`, `NE`, and `SELECT`. `BV_NEG` and `NE` are expressed
through `BV_SUB` and a `BOOL_NOT` of `EQUAL` because the engine has no operator
of its own for them.

Everything else becomes an opaque leaf variable and is copied to the output
verbatim: comparisons, shifts, division and remainder, width casts, floating
point, memory, calls, and every effectful instruction. That is a loss of
normalization power, not of soundness. An opaque leaf denotes whatever the
instruction denotes, so the surrounding expression still normalizes around it.
Two textually identical such instructions stay distinct, because the leaf is
keyed by the value it defines rather than by the expression that produced it.

Extraction is not trusted. Every extracted term must be established equal to its
root by `src/egraph_check.c` replaying the merge log, every record in that log
must be `JUSTIFIED` with no assumed and no rejected merges, and the replacement
is kept only when the extracted term is available strictly earlier in the block
than the value it replaces. The output IR is verified again before it leaves the
method.

#### The shipped `prove.egraph` method

`src/proof_egraph.c` registers `prove.egraph` as a built-in proof method, the
solver-free rung between the concrete refuter and the two solver-backed
provers. It consumes one schema v2 problem, lowers both sides, and builds ONE
e-graph in which the argument correspondence maps each left parameter and its
right counterpart onto the same variable term. The graph is saturated with
the versioned catalogue, and the two return roots landing in one class is the
equivalence claim.

The verdict is issued only on the independent replay: every recorded merge
must be `JUSTIFIED` by `src/egraph_check.c`, none may be `ASSUMED`, and the
checker's own replayed union-find must merge the two roots. Only then does
the outcome carry `PROVED_*`, `QL_EVIDENCE_PROOF`, and `checked_proof: true`.
The trusted computing base is this file's IR-to-term conversion, the replay
checker, and the rule catalogue whose version and digest the evidence names;
the engine's rewrite matcher is not in it, exactly as CaDiCaL is not in the
AIG path's.

The accepted fragment is one where root equality IS the selected behavior:
one basic block per side ending in RETURN of a bool or bit-vector, every
instruction effect-free with a pure term operator, no UB_GUARD, ASSUME,
memory, calls, or traps, and a literal-true typed precondition (this method
has no solver to witness a narrower domain's inhabitation; all-inputs
equality of total functions is never vacuous). There are no opaque leaves: a
prover may not copy what it cannot model, because an unshared leaf can never
merge across sides. Under the ASM2C_GNU_V1 profile signed `+`, `-`, and `*`
lower with an overflow UB guard and therefore fall outside the fragment;
unsigned and bitwise arithmetic is the practical territory. Inside it both
sides are total, deterministic, and defined everywhere, so each behavior set
is a singleton and return equality discharges equivalence and both
refinement directions at once.

Failure to merge -- saturation completing without connecting the roots, or
stopping on a limit -- is `UNKNOWN` and never a counterexample, because
equality saturation is incomplete and cannot refute anything.

#### The rewrite rule catalogue

A merge record carries only the rule name that fired. The premises that make
that rule sound live in a versioned catalogue keyed by that name, reachable
through `ql_egraph_rule_lookup` and enumerable through
`ql_egraph_rule_catalogue_at`. The catalogue is the rewrite-set identity:
`ql_egraph_rule_catalogue_digest` is the BLAKE3 digest of its canonical
serialization, and that digest is what evidence and cache keys carry.

Each descriptor states the shape of the rewrite, the operators the rule name
may have fired on, which operands must already share a class, which constant
must witness a side condition, and the width range the rule accepts. Three
condition bits carry the premises that a source language can invalidate.

- `WIDTH_AGNOSTIC` says the rule holds at every width the sort admits.
- `SIGN_AGNOSTIC` says the rule holds under both readings of its operands.
- `TOTAL_ARITHMETIC` says the rule holds only because this engine's bit-vector
  arithmetic is total modulo `2^width`. A frontend whose source language makes
  the same operation undefined on overflow still owes an argument for those
  merges, and the evidence names exactly which ones.

The e-graph itself is a pure term engine with no undefined-behaviour, effect,
or poison semantics, so these bits are a record of what the engine assumed,
not a claim that the frontend discharged it.

#### The independent replay checker

`src/egraph_check.c` shares no code with the engine. It takes a term table and
a merge log as data, rebuilds its own union-find, and walks the records in
sequence. For each record it asks whether the named rule justifies that merge
in the state the earlier records produced, recomputing every side condition
itself rather than trusting the reason string. This is the same separation
`src/ir_verify.c` keeps from the IR builder.

Each record gets one of three verdicts.

- `JUSTIFIED`. The rule and the replayed state establish the merge.
- `ASSUMED`. A trusted axiom. Nothing in the log justifies it and every
  downstream result is relative to it, so assumed and justified are counted
  separately and `all_merges_justified` is 0 whenever an axiom is present.
- `REJECTED`. The merge does not follow. This is a defect in the engine, the
  evidence, or the catalogue. It is never resolved by trusting the engine.

The checker refuses a log whose rewrite catalogue version or digest it does not
carry, because it would otherwise discharge side conditions from a different
rule set than the one that fired. A rejected merge is still applied to the
replay state so later records are checked against the derivation the engine
actually had; stopping at the first defect would hide the rest.

`ql_egraph_check_terms_equal` answers the equivalence the replay established,
not the one the engine reports. A caller that wants to act on an e-graph
`PROVED_EQUAL` asks the report, not the engine.

### SMT product program

Method name: `prove.smt-product`. This is the first implemented method. The
paragraphs below the implementation notes describe the general interface; the
implementation covers the loop-free fragment over scalars and the flat memory
model, plus a deliberately narrow structural scalar-loop fast path. It refuses
or returns `UNKNOWN` for the rest.

#### Implementation, method version 2

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

Method version 2 adds loop canonicalization, relational induction queries, and
their query digests to the evidence and cache identity. The outcome artifact
schema and the independently versioned loop telemetry structure remain
append-only; a loop-free outcome reports that the loop path was not applicable.

Each observation axis is encoded explicitly. The return-value axis compares
whether a normal return happened before it compares the value, because a trap
or a divergence produces no return value at all. The volatile, atomic, I/O, and
external-call axes are discharged only after establishing that neither IR
carries the corresponding effect; an effect the encoding cannot state is a
`QL_STATUS_TYPE_MISMATCH` naming the axis, never a dropped obligation.
Definedness is governed by the UB policy, as this document specifies, so
`QL_OBSERVE_UNDEFINED_BEHAVIOR` adds no separate conjunct.

#### Structural relational loop induction

The frontend already lowers `for`, `while`, and `do` to cyclic typed SSA.
`for` and `while` share a pre-test form and `do` keeps its post-test guard. The
proof path does not compare those source constructs. It computes dominators,
finds edges whose targets dominate their sources, merges natural backedges by
header, and records each loop's header, preheader, latches, exits, guard,
nesting, and leading loop-carried PHIs. A remaining cycle that is not explained
by those dominance backedges is outside this fast path.

Loop pairing uses that canonical view and the loop-carried state rather than
source locations or block identifiers. Reducible single-entry scalar loops are
the general target. Nested loops, mismatched loop counts, ambiguous guards,
memory or event-trace state, calls, and unsupported effects do not get silently
abstracted by the general relational encoder. They reach the fallback boundary
unless the exact-self rule below applies.

There are two exact-self exceptions to that scalar rule. First, a clean scalar
self-pair may represent its complete header tuple with exactly sorted shared
transition symbols. Congruence is sound only after exact IR digest and
instruction-by-instruction structural matching. This shared-transition
induction is distinct from the actual left/right expression encoder used for
non-identical programs.

Second, an exact whole-IR self-pair that reaches the guard, nesting, UB, or
effect fallback can use a reflexivity rule. Its terminal is the disequality of
the identical 256-bit IR artifact digests, bound with the canonicalization
digest.
It deliberately claims no loop invariant. A concrete interpreter execution
with one selected zero, one, or all-ones scalar pattern, explicit object bounds
and images, and a deterministic zero or object-base callee interpretation must
produce a defined outcome before the domain is considered inhabited. The
execution is only an existential domain witness,
not the universal proof. UB, assumption rejection, unsupported semantics, or
the step limit leaves this rule unavailable.

For paired PHIs the candidate vocabulary is modular bit-vector equality,
constant offset, and affine relation:

```text
left == right
left == right + B
left == A * right + B
```

The recurrence analysis independently recognizes identity, fixed addition or
subtraction, affine multiply-add, and fixed-stride pointer updates. It records
justified bit-vector candidate coefficients, while the first structural query
selects the equality relation for paired header state. Pointer candidates retain
their recognized stride as telemetry, but method version 2 does not synthesize
a pointer-offset invariant or promote pointer state.

The induction terminal is one combined disjunction of four possible failures:

1. Base: the invariant does not hold at the paired loop entries.
2. Guard alignment: under the invariant, the two continue predicates disagree.
3. Step: under the invariant and paired continue condition, one symbolic
   transition through a latch does not re-establish the invariant.
4. Exit: under the invariant and paired exit condition, the shared canonical
   exit projection disagrees.

The builder first checks every latch and exit count and the exact canonical IR
transition structure. Only an exact structural match may share the guard,
transition, and exit symbols between sides; non-identical transitions do not
gain a congruence assumption. An `UNSAT` answer then discharges all four
obligations for an arbitrary number of iterations through that matched
transition system. The query never executes the loop, constructs `N` copies of
its body, or depends on a loop bound. A `SAT` answer only rejects this invariant
candidate. It is not a program counterexample and is never sent to the ordinary
counterexample replay path as one.

The analyzer counts affine closed-form opportunities, but method version 2
does not expose a summary proof terminal. A summary that is disconnected from
a common iteration count and the actual exit observable would be unsound.
Consequently summary attempts and proved counts remain zero. An induction
`SAT`, an unsupported summary connection, or an unsupported loop shape reaches
the CHC/PDR boundary. The in-process fallback is still recorded as reached
but not attempted and this method's result is `UNKNOWN`; the registered
`prove.chc-pdr` method (below) is how that boundary is attempted, over the
same serialized prefix.

The promotion gate requires the problem and contract binding, literal-true
typed precondition, and explicit `trusted-backend` selection. Actual or shared-
transition induction additionally requires a scalar whole IR with no
assumptions, UB guards, effects, memory, traces, or pointers, a satisfiable
encoded domain, and induction `UNSAT`. Exact whole-IR reflexivity instead
requires exact artifact and structural identity, a concrete defined domain
witness, and reflexivity `UNSAT`. Other analyzed programs remain `UNKNOWN`.
As on the loop-free SMT path, promoted Bitwuzla evidence always records
`checked_proof: false`.

#### The flat memory model in the miter

Under the `ASM2C_GNU_V1` profile memory is a flat 64-bit address space of
bytes, which the query states as one `(Array (_ BitVec 64) (_ BitVec 8))` in
`QF_ABV`. A pointer takes the bit-vector sort of its width, since a pointer is
an address and nothing more in this profile. An access of `W` bytes is `W`
`select`s concatenated, or `W` nested `store`s, in little-endian order, which
is byte for byte the order `ql_ir_interp_run` uses; an access whose width is
not a whole number of bytes is refused rather than rounded.

Both sides share the initial memory constant and one base and size constant per
object descriptor, so the two functions run over the same storage without
either side describing it to the other. Pointer-argument descriptors come
first and are matched through the problem's argument correspondence. Globals,
strings, local storage, and auxiliary descriptors for accesses through
otherwise unbound pointer bits follow in lowering order and match by position.
The two sides must therefore declare the same number of these internal
descriptors; otherwise the
method returns `UNKNOWN` rather than guessing an identity relation. Auxiliary
descriptors may name an earlier region exactly, while partial overlap remains
forbidden.

**Neither the model's standing constraints nor the access-definedness predicate
is restated here.** The lowering already emits them as ordinary IR: `ASSUME`
instructions for exact-alias-or-disjoint regions, the first-page floor, and the
no-wrap bound, and a `UB_GUARD` over ordinary arithmetic at each access. The miter encodes those
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
3. the domain query answered `sat`, or exact whole-IR reflexivity recorded a
   concrete defined witness, so the `unsat` is not vacuous;
4. the **unbounded** violation query answered `unsat`; the search-only bounded
   variant carries no proof authority at all;
5. the backend is the pinned Bitwuzla, and its name, version, executable
   content digest, and query digest are recorded;
6. the caller selected the policy explicitly.

For the structural loop branch, item 4 is either the unbounded combined
induction terminal or the exact-reflexivity terminal. Base, guard, Step, Exit,
canonical-loop, reflexivity, and concrete-witness digests are recorded as
applicable. Affine summary candidates have no terminal. A finite unroll query
is never substituted for either proof rule.

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
The method-version-2 structural path also closes supported paired scalar loops
with a separately recorded inductive invariant, and exact self-pairs with the
whole-IR reflexivity rule described above.
Recursion, general loop memory, unbounded allocation, quantified memory
properties, and unrestricted external-call contracts still require a broader
invariant, summary, or abstraction. Loop unrolling alone yields at most
`BOUNDED_CLEAN` after an UNSAT query. It yields `PROVED` only if the product is
loop-free, the explored state space is otherwise complete, or separately
checked invariants close every backedge.

A SAT result is a candidate counterexample and must be concretized and replayed.
An UNSAT result supports `PROVED` only when the encoding covers the entire
contract and the solver emits evidence accepted by the configured checker. If
proof checking is unavailable, the result is solver evidence but must not cross
a deployment trust boundary that requires checked proof.

Important options include solver and logic, timeout, path alignment strategy,
memory encoding, floating-point theory, external-call summaries, invariant
source, and proof format. The shipped structural loop path has no unroll-bound
option because its proof query is independent of iteration count.

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

#### Where the miter comes from

The AIG path does **not** walk the IR. `src/product.c` already encodes the
whole contract once: block reachability, PHI, the observation axes, the UB
policy and its totalization, the typed precondition, the relation direction,
and the refusal of memory, effects, and non-scalar types. Rebuilding that at
bit level would put the C semantics in two independent encoders, and a
`checked_proof` that rests on the second one is only worth anything if the two
never drift. A verified proof of the wrong question is worse than an unverified
proof of the right one, because it carries authority.

So `src/proof_aigsat.c` bit-blasts the SMT-LIB bytes `ql_smt2_builder` already
produced for Bitwuzla. The two backends answer the same question by
construction, and the outcome records the same query digest for both, so the
evidence itself states that they did.

The accepted grammar is closed: `set-logic`, `declare-const`, nullary
`define-fun`, and `assert`; the `Bool` and `(_ BitVec N)` sorts; the operators
the encoder emits and no others. An Array sort is refused, because the first
cut is scalar. Anything outside the grammar is `QL_STATUS_TYPE_MISMATCH` and
becomes `UNKNOWN`; nothing is guessed at. This is a front end for one producer,
not a general SMT-LIB parser, and it is the only place in the AIG path that
reads bytes it did not write, so it carries a fuzz target
(`tests/fuzz/fuzz_smt2_blaster.c`) whose deterministic corpus also runs on
every CTest.

Two tests hold the architecture up. A round-trip test blasts what the builder
writes and checks the circuit against the operators' ordinary meanings. An
agreement test blasts the real product query and requires that the miter call
an input a violation exactly when concretely running both functions through the
IR interpreter does, over boundary and pseudo-random inputs, including
division, remainder, shifts, and a typed precondition. A disagreement is a bug
in the blaster or in the encoder and fails loudly; it is never averaged away.

CNF export is Tseitin over the cone of influence of one root, so a part of the
graph the root does not reach costs the solver nothing. The DIMACS bytes are
deterministic, which is what lets a query digest identify the question. A root
that folded to a constant is reported as trivially true or trivially false
rather than sent to a solver; a trivial answer is still an answer this encoding
produced and never a checked proof.

#### The method

`src/proof_aigsat.c` carries `prove.aig-sat` beside the blaster. It consumes
the same `quodlibet.problem` schema v2 artifact as `prove.smt-product`, lowers
both sides the same way, and builds the same `ql_product_query`. From there it
blasts, solves, and refuses to believe anybody.

The flow has exactly five gates, and `checked_proof: true` is on the far side
of all of them.

1. The violation query is blasted from the product query's own bytes and
   Tseitin-encoded. A root the folding collapsed to a constant is reported as
   trivially true or trivially false and never dressed up as a solver result.
2. CaDiCaL runs on the DIMACS with `--lrat --no-binary`. A binary certificate
   would defeat the point of a checker small enough to audit.
3. `s SATISFIABLE` goes to the replay path. The DIMACS assignment becomes a
   `quodlibet.solver-model` artifact and passes through `ql_replay_decode_model`
   and `ql_replay_execute` -- the same decoder and evaluator a Bitwuzla model
   passes through. Only a replay that reproduces the violation yields
   `COUNTEREXAMPLE`. A solver that says satisfiable and a replay that says
   otherwise is an encoding defect, logged as one, and reported as `UNKNOWN`.
4. `s UNSATISFIABLE` goes to `lrat-check` over the original CNF. The approval
   must be a whole line: `NOT VERIFIED` contains `VERIFIED`, and a substring
   search there would read a refusal as an approval.
5. A verified certificate still proves nothing about an empty domain, so the
   `[prefix, domain]` query is blasted and solved too, and its satisfying
   assignment is **evaluated against the domain circuit by this process**
   rather than believed. That inhabitance claim has no replay behind it, so it
   is the one place where a SAT answer would otherwise be taken on trust.
   Finally `ql_problem_require_proof_binding` must hold.

Only then does the outcome carry `PROVED_*`, `QL_EVIDENCE_PROOF`, and
`checked_proof: true`. The trusted computing base is `lrat-check.c` plus this
repository's own encoder and evaluator. CaDiCaL is not in it: a wrong UNSAT is
caught by the checker, and a wrong SAT is caught by the replay.

A folded-false root is deliberately **not** promoted. It is this process's own
arithmetic with no certificate behind it, and believing it would be believing
exactly the thing the method was built to stop believing.

The envelope records both executable digests, the CNF digest, the certificate
digest, and the same `prefix`, `violation`, and `domain` digests the SMT path
records for the same problem. Two backends, one question, and the envelope says
so rather than leaving a reader to take it on faith.

`tests/test_proof_aigsat.cpp` pins the agreement between the two backends over
a small corpus. A disagreement is not resolved by majority -- there is no
majority of two -- and it fails the test loudly, because it is evidence of a
defect in one backend or in the blaster.

### Bounded symbolic execution

Recommended method name: `search.bounded-symbolic`.

#### The implemented method

`src/proof_bounded.c` registers `search.bounded-symbolic` as a built-in over
the product encoder's scalar and flat-memory fragment. It does not maintain
its own symbolic-state engine. Each side's lowered IR is rebuilt by
`src/unroll.c` into an acyclic graph covering every execution that traverses
at most `unroll_bound` retreating edges -- the copy-based unrolling routes
each retreating edge to the next copy and, in the last copy, to one cut block
whose body is `ASSUME(false)`. The product encoding conditions every ASSUME
on its block being reached, so inputs beyond the bound leave the comparison
domain on both terminal queries instead of being given invented behavior. The
rebuilt graph is re-validated by the ordinary IR reader; a shape whose SSA
the copy-based renaming cannot order is refused as `UNKNOWN`, never emitted
wrong.

The unrolled pair then flows through `ql_product_query_build` and the pinned
Bitwuzla backend exactly as `prove.smt-product`'s loop-free path does, and
the verdict discipline is the bounded one:

- a SAT model is decoded by the shared replay decoder and executed against
  the **original cyclic IR**, so a confirmed `COUNTEREXAMPLE` is a statement
  about the real functions and not about the unrolling;
- UNSAT is `BOUNDED_CLEAN` with the bound vector recorded (`unroll_bound`,
  whether any cut was reachable, and the emitted block counts). It is never
  promoted, and the capability never advertises proof soundness under any
  option;
- a backend timeout, cancellation, or an unreplayable model is `UNKNOWN`
  with a diagnostic.

The bound is a total budget of retreating-edge traversals per side, not a
per-loop trip count: two sequential loops or a nested pair draw on the same
budget. `bound_cut_used = 0` in the outcome records that neither side's
graph reached the bound, so the search was exhaustive over the encoded
fragment; the verdict still stays `BOUNDED_CLEAN`, because promotion is a
combiner and policy decision, not this method's.

#### Interface

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

Method name: `prove.chc-pdr`.

#### The implemented method

`src/proof_chcpdr.c` registers `prove.chc-pdr` as a built-in. It runs
IC3/PDR over the transition system `src/loop_proof.c` already serializes for
the induction fast path: shared input symbols, one current-state constant
per loop-carried PHI on each side, and entry, guard, next-state, and exit
expressions defined over them. When the fast path's fixed relation
vocabulary cannot cover a pair, the serializer now still emits that prefix
and records `chc_pdr_available` on the fallback disposition, which is
exactly the territory this method exists for; it also accepts the
QUERY_READY prefixes whose single induction query failed.

Frames of lemmas are strengthened by blocking counterexamples-to-induction
cube by cube, with literal-drop generalization and forward propagation.
The lemma vocabulary is concrete-value cubes, the analyzer's own
equality/offset/affine candidates, an entry anchor (a variable the latch
never updates stays at its entry expression), and entry-anchored sums and
differences of state pairs. The last two are what close a pair like "count
up" against "count down": its invariant `i + j = n` names an input and is
outside the fast path's constant-coefficient vocabulary. Every seed still
passes the ordinary initiation and consecution queries before any frame
carries it, so an unsound heuristic cannot become an unsound lemma.

The safety property is the fast path's own: reachable synchronized states
never disagree on the continue guards, and a state where both sides exit
never disagrees on the exit observable. A fixpoint is re-verified with
fresh initiation, consecution, and exclusion queries, the comparison domain
must answer `sat`, and promotion additionally requires the whole-IR
non-vacuity flags, the positional binding, the literal-true precondition,
and the explicit `trusted-backend` policy -- the same gate shape as the SMT
product's, with `checked_proof: false` because Bitwuzla answers every
clause query. The verified invariant is recorded lemma by lemma in the
outcome over the prefix's own state symbols, so an external checker can
re-discharge all three obligations against the same prefix.

A reachable bad state is never concretized here: the diagnostic routes the
caller to `search.bounded-symbolic` or `refute.concrete-differential`, and
the verdict stays `UNKNOWN`. Frame, lemma, query, and time budgets also
leave `UNKNOWN`. The SMT product's own in-process fallback telemetry still
records `fallback_attempted = false`; running this method, standalone or as
a pipeline branch beside `prove.smt-product`, is how the boundary is
attempted.

#### Interface

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

### The implemented combiner

`src/combine.c` implements rules 1 through 8. Rule 9 is the cache key, which
`ql_cache_key_compute` owns.

The shape of the API is chosen so the unsound moves are not expressible. There
is no count of agreeing methods anywhere in the interface or the
implementation, no method priority table, and no way to ask for a proof and a
counterexample to be reconciled.

`ql_combine_evaluate` takes a request naming the contract and a set of inputs.
Each input restates the contract it answered, so an answer to a different
question is refused with a status error rather than combined; the message names
the axis that differs. Each input then carries its `ql_policy_evidence_v1`, the
same facts the single-result policy boundary consumes, and the combiner
reapplies that discipline rather than trusting what the method reported. A
`PROVED_*` verdict survives only with a checked proof or a backend the request
names as trusted, a `COUNTEREXAMPLE` only with a replayed witness, and an
exhausted budget withdraws whatever was reported. Everything withdrawn becomes
`UNKNOWN`, never a weaker positive claim.

Every input gets a finding stating its disposition and, when the verdict was
withdrawn, which evidence was missing. A caller can always say why a given
method did or did not count.

Two cases deserve naming. A result produced on top of a normalization declares
`depends_on`, and if that normalization's own proof is absent or invalid the
dependent result is withdrawn, because there is nothing to transfer it back
along. And when a checked proof of the requested relation meets a replayed
counterexample, the result is `inconsistent` with both inputs named and a
verdict of `UNKNOWN`. That case is the point of the whole design: it is
evidence of a defect, and priority, ordering, and counting are all refused as
ways to make it go away.

Bounded results keep their whole bound vector. Two bounds on different
dimensions are carried side by side, never joined into a scalar, because the
covered state sets are not comparable. Any number of bounded results still
combines to `BOUNDED_CLEAN`.

Ordering among equally valid results is declaration order, then evidence
digest, then input position. Worker completion time is recorded on the input
and never consulted.

### The persistent store

`src/cache.c` keeps artifacts and evidence envelopes on disk under the
BLAKE3 cache key. The caller names the directory; the module never picks one.

Every record carries the digest of its content and a digest over its whole
encoding, and both are verified on load. The key it was stored under is
written into the record and compared with the key being looked up, so a file
that ends up in the wrong place cannot answer for another question. An
evidence record additionally stores the identity the envelope was built from
and recomputes the cache key on load rather than trusting the one the file was
filed under. A record that fails any of these checks is refused, and a refusal
is counted separately from a miss: a store handing out wrong answers must not
look like an empty one.

Storing the same content under an existing key succeeds. Storing different
content under a key that already holds some is `ALREADY_EXISTS`, never an
overwrite. Two different answers filed under one key means the key is missing
an axis, and that is exactly the defect rule 9 exists to prevent.

### Cache key completeness

`ql_cache_key_compute` builds the key from five things: the artifact digest,
the semantic-problem digest, the method name, the method version, and the
method's canonical options. Every axis a run depends on has to reach one of
those five.

The semantic contract reaches the key through the problem artifact, whose
digest binds the relation, the UB policy, the observation projection and both
observation modes, the dialect and target profile, the compiler and codegen
sets, the target features, the precondition, and both sources. The
`tests/test_cache_key.cpp` suite varies each of those one at a time and
requires both that the key moves and that the store then keeps the two answers
apart.

The solver backend has no field of its own. It must ride in the method version
or in the canonical options, and the same suite states that requirement from
both sides: it fixes that a backend named in the options separates the keys,
and it demonstrates the collision that follows when the backend is named
nowhere.
