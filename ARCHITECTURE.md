# Architecture

## Core rule

Algorithms are plugins and data is an immutable artifact. The host knows how to
schedule methods and preserve evidence, but it does not assign proof meaning to
an arbitrary method result. This keeps AIG/SAT, e-graph, product-program,
CHC/PDR, bounded execution, and future language frontends composable without
putting their implementation details in the runtime.

## Data model

An artifact has four observable properties: a UTF-8 kind, a schema version,
immutable bytes, and a 256-bit BLAKE3 digest. Construction copies both the kind
and the bytes. Reference counting permits fan-out without copying payloads.

Standard kind names are reserved for problems, language-neutral IR, outcomes,
proofs, and counterexamples. Method-specific kinds may be added without
changing the host ABI.

The digest is domain-separated by the artifact format, kind, schema version,
and payload. It is suitable for persistent cache and evidence identities.
`ql_fast_hash` uses xxHash only for transient process-local tables and must not
be used as a proof or persistent-cache identity.

## Pipeline semantics

A pipeline is a directed acyclic graph with one method per node.

1. A node with no declared dependency receives the caller's initial artifact as
   its sole input.
2. A node with dependencies receives their output artifacts in the exact order
   listed in `depends_on`.
3. Every node produces exactly one artifact.
4. Nodes at the same topological level are submitted to the worker pool
   together. A level completes before the next begins.
5. A failure or cancellation prevents later levels from starting. All artifacts
   already produced by the run are released.
6. Nodes with no consumers are sinks. A successful run returns every sink in
   declaration order.

Level barriers are deliberately simple and deterministic. The scheduler API is
separate, so a later executor may release successors immediately or implement
work stealing without changing method or plugin ABI.

Pipeline JSON has schema version 1:

```json
{
  "schema_version": 1,
  "nodes": [
    {
      "name": "lower",
      "method": "frontend.c.lower",
      "options": { "dialect": "c17" }
    },
    {
      "name": "sat",
      "method": "proof.aig-sat",
      "depends_on": ["lower"]
    }
  ]
}
```

Node names are unique. Dependencies may refer forward or backward in the JSON;
the compiler detects missing nodes, invalid method arity, and cycles. An options
object is serialized to compact JSON and passed unchanged to the method's
creation callback.

## Method and plugin ABI

All ABI structures begin with `struct_size` and `abi_version`. Append-only
growth is therefore possible without guessing a compiler's structure layout.
Version 1 uses a fixed C calling convention and fixed-width integer fields.

A method has creation, optional input validation, execution, and destruction
callbacks. A new method instance is created per pipeline node per run. Inputs
are borrowed immutable artifacts; a successful method returns one owned
artifact. Methods poll the cancellation callback in their run context when they
perform long work.

Plugins export exactly one entry point named `quodlibet_plugin_init_v1`. The
entry point receives host services and returns a static plugin descriptor.
Plugins must create artifacts and allocate cross-boundary data with the supplied
host services. They must never ask the host to free memory allocated by a
different C runtime.

A plugin handle must outlive every active call into that plugin. Registry
mutation and plugin unload concurrent with pipeline execution are not supported.
Registry generations refresh method bindings after a sequential change, so
unloading a plugin before a later run yields `NOT_FOUND` rather than using a
stale descriptor.

Every loaded plugin must be unloaded before its registry is destroyed. Method
descriptors registered directly have borrowed lifetime and must outlive that
registry.

## Parallel runtime

The scheduler is a bounded libuv-thread pool with a FIFO task queue. Task groups
are one-shot barriers: submit the full group, then wait once. Destroying a
scheduler drains already queued tasks and joins every worker. A zero worker
count uses `uv_available_parallelism`.

Independent methods must not rely on invocation order. The current executor
selects the lowest declaration-order node error when several nodes in a level
fail, which keeps externally reported failures stable across schedules.

After an explicit successful `ql_pipeline_compile`, the same pipeline may run
concurrently because run-local artifacts and method instances are separate. The
registry must remain unchanged, and a custom allocator supplied to the pipeline
must itself be thread-safe. Pipeline construction and compilation are not
concurrent operations.

## Logical result contract

The core distinguishes these outcomes:

| Verdict | Meaning |
| --- | --- |
| `PROVED_EQUIVALENT` | A sound method discharged the declared equivalence contract |
| `PROVED_LEFT_REFINES_RIGHT` | The declared left-to-right refinement was proved |
| `PROVED_RIGHT_REFINES_LEFT` | The declared right-to-left refinement was proved |
| `COUNTEREXAMPLE` | A concrete witness violates the declared relation |
| `BOUNDED_CLEAN` | No witness was found only inside a recorded finite bound |
| `UNKNOWN` | No sound conclusion was reached |

`BOUNDED_CLEAN` is never promoted to a proof. Transport or method failure is a
`ql_status`, not a logical verdict.

The semantic contract separately records relation direction, undefined-behavior
policy, observable effects, the C/ABI profile, and an optional input
precondition. `ql_semantic_contract_init` selects the conservative default:
equivalence, matching definedness, and observation of return value, externally
reachable final memory, termination, traps, ordered external calls, volatile
accesses, atomics, and I/O. A caller must explicitly clear observation bits or
select weaker memory/call modes to ask a narrower question.

`LEFT_REFINES_RIGHT` means behavior-set inclusion `Beh(left, x) subseteq
Beh(right, x)` for every input satisfying the precondition. The reverse
relation swaps the operands. Under `UB_MUST_MATCH`, the defined input domains
must be equal. `UB_LANGUAGE_REFINEMENT` applies the selected behavior-set
inclusion and permits the refining side only behavior allowed by the refined
side. `UB_COMPARE_WHERE_BOTH_DEFINED` compares only the intersection of the two
defined domains; it is opt-in because an empty intersection would otherwise
make a result vacuous. Traps are defined observable events. Undefined behavior
is not a trap and is governed by the UB policy.

The first C profile is `ASM2C_GNU_V1`: GCC/Clang-compatible corpus syntax on
x86-64 Linux SysV LP64. Its provenance matrix is GCC and Clang, PIC and
non-PIC, and O0 through O3. The target-feature groups mirror the corpus:
x86-64 base, the synthetic SSSE3/SSE4/AVX/AVX2/POPCNT/LZCNT baseline, AVX-512,
crypto, bit manipulation, hardware random, TSX, and timing. Optimization level
is not source semantics and is therefore validation provenance rather than a
semantic switch.

The corpus records compiler family and optimization name, but not compiler
version, an explicit C `-std`, or the complete command line. Consequently this
profile freezes Quodlibet's own accepted subset instead of inheriting a moving
compiler default. Unsupported extensions must yield `UNKNOWN` or a frontend
error, never a proof under guessed semantics.

#### Corpus preamble typedefs

Record extraction keeps the type and callee context around a function and
drops the rest of the file, including the preamble every AnghaBench source
carries. One typedef from that preamble matters: `typedef long scalar_t__;`,
commented "Either arithmetic or pointer type". The profile therefore resolves
`scalar_t__` as the LP64 signed 64-bit integer the corpus compiler used.

This is not a profile-relative assumption in the way the memory model is. It
is the definition the compilation that produced the corpus actually used,
recoverable from the raw sources: a 4,000-file sample found that exact
spelling 3,998 times and no other spelling of it. A name absent from this
table stays unresolved and is refused, and a unit that declares the name
itself overrides the table.

#### Binary floating-point profile

The profile carries C `float` and `double` as IEC 60559 binary32 and binary64
values. `long double`, decimal floating types, complex values, and vector
types remain outside the lowering. Literals are rounded to their suffix-selected
format and stored as exact little-endian object bytes. Source signatures keep
the floating kind and width independently of the IR, just as they retain
integer signedness.

The accepted expression slice includes unary negation, the four basic
arithmetic operations, ordered comparisons with C's unordered `!=` behavior,
integer and boolean conversions, assignments, memory access, call arguments
and results, and the default `float`-to-`double` variadic promotion. A
floating-to-integer conversion emits an explicit definedness predicate for
the values whose truncation is representable; NaN and out-of-range values
therefore reach `UB_GUARD` rather than being assigned a host-dependent integer.

Concrete execution uses the host's default round-to-nearest floating
environment at the declared precision. The profile does not model dynamic
rounding modes, floating exception flags, trapping math, excess precision, or
contraction. The schema-v1 precondition expression language has no floating
terms and refuses a source signature with a floating argument. The current
product proof encoder also refuses floating IR. Thus a floating function can
be lowered, verified, and concretely replayed, but it cannot yet receive an
exact product-method proof verdict.

#### An uninitialised aggregate starts from a shared unknown image

An array or record local with no initialiser gets an object whose initial
bytes are unknown, supplied by whatever runs the module, exactly as a caller's
pointer region is. ISO C calls those bytes indeterminate and permits two
executions to see different ones; the profile does not. It gives both sides of
a comparison the same image and quantifies over it.

That is the same modelling decision already made for every other object in
this model, and it is the right one for the profile's source material: two
translations of one function run on one machine state, so the frame really is
shared. It is stated here because it is an assumption rather than a fact about
C, and because a proof that rests on it is a proof about programs paired that
way. An address-taken *scalar* local with no initialiser is generally still
refused. The narrow exception is a direct `&local` argument to a declared
external callee. That CALL returns both the scalar value it supplied and a
predicate saying whether it wrote the whole local. The lowering selects the
supplied value only on that predicate and carries the local's definedness as
`old_defined || wrote`, so a callee that does not write still makes a later
read undefined. Arbitrary pointer escape remains refused.

#### Record arguments and returns at external calls are packed values

A record passed by value to a declared external callee is represented by its
complete target-layout object image, including padding bytes and the active
union representation. The lowering reads that image in address order and
packs it into little-endian 64-bit `CALL` operands. Unused high bytes of the
final operand are zero. It does not pass the caller's object address, so the
callee cannot mutate that object through a by-value argument.

The packed operands are ordinary call values. Concrete callbacks receive the
same byte image, and product-call congruence can cancel calls only when these
values and the other observed call components match. A direct declared call
that returns a record similarly receives one little-endian 64-bit `CALL`
result per eight bytes of the complete target-layout image. The lowering
writes every result chunk bytewise into a fixed temporary object for that
syntactic call site, then treats the temporary's address as the record value.
This preserves padding and union bytes across a record initializer and direct
member access. Every chunk participates in product-call congruence.

An indirect record return remains outside the slice because the object
prepass cannot bind a statically named callee to a return temporary. Record
parameters or returns on the selected source function use the separate
boundary rule below.

#### Small selected-function records use one object-image value

A record of at most 32 bytes passed to or returned from the selected source
function is one bit-vector containing its complete little-endian object
image. The limit is the concrete interpreter's existing value capacity. A
parameter image is copied into a function-owned object before the body runs,
so modifying the parameter never modifies caller storage. A returned record
is read from its object after its complete-image definedness guard and packed
back into the same carrier. Padding and union bytes participate in both
directions.

This is currently a lowering, verifier, and concrete-execution contract.
Source-signature schema v1 cannot describe a record value, so a pipeline that
requires `ql_source_signature_bind_ir` still refuses this boundary. Records
larger than 32 bytes remain `UNKNOWN`; no bytes are truncated.

#### Declared corpus function designators are opaque tokens

A declared `FUN_<decimal>` used as a value has the function-pointer type its
prototype states and the deterministic token `0xffff000000000000 + decimal`.
The mapping is injective over the admitted 32-bit suffix range and never uses
zero. The value can be compared, selected, passed to a declared callee, or
used where the existing function-pointer slice permits it. It cannot be read
as data storage.

This rule applies only when the unit contains the prototype. A missing
declaration is still `UNKNOWN`, and the selected function's own definition is
not turned into an external token. In particular, recursive `FUN_0` calls do
not silently acquire uninterpreted external-call semantics.

#### Corpus globals carry no promised initial value

A file-scope object with no initialiser is, in ISO C, a tentative definition
initialised to zero. The profile does not read the corpus that way. A raw
AnghaBench source states its globals in a synthesised
`/* Variables and functions */` section, one bare `int NAME ;` per global the
extracted function names, standing in for a definition that lives in another
translation unit. The value that definition gave it is not in the corpus, so
the profile leaves such an object's initial contents unknown rather than
assuming a zero the program never promised. A declaration that does state a
value is honoured, and the lowering writes it before the body runs.

#### Mutable block-scope static objects carry persistent input state

A mutable local declared `static` also has static storage duration, but its
initializer is not replayed at every function call. The function model starts
at an arbitrary invocation, so the object's incoming bytes are a caller-
supplied persistent image shared by both sides of a comparison. The body reads
and updates that memory object, and final-memory observation exposes the new
image. This represents effects left by earlier invocations without inventing
an invocation count. Immutable static objects may still state their invariant
initializer bytes because no defined execution can change them.

#### A one-dimensional VLA binds its object size at declaration

An automatic one-dimensional variable length array introduces its local object
before the body, like fixed local storage, but leaves that descriptor's size
unconstrained until execution reaches the declaration. The bound expression is
evaluated exactly once there. A `UB_GUARD` requires a positive bound and a byte
count that does not overflow the 64-bit object-size domain, then an assumption
binds the descriptor size to that byte count. Subscript bounds and
`sizeof(array)` consequently observe the same declaration-time size.

The executor must supply a memory image with that computed size. A non-positive
or overflowing source bound is undefined behavior, whereas a mismatching image
violates the module's input assumptions. VLA declarations inside loops would
need a fresh object lifetime on every iteration, which the current lowering
does not represent. Multidimensional arrays, static-storage VLAs, and
initialized VLAs also remain outside this profile.

#### A loop may exit through a direct forward label

Loop headers carry scalar, definedness, memory, and call-trace state through
SSA PHIs. A `goto` inside a `for`, `while`, or `do` loop may leave that loop for
a later label that is a direct child of the function body. The edge records
only the function-scope state visible at its target, while the loop's other
paths continue through their ordinary backedge PHIs. At the label, those exit
states merge with any live lexical fallthrough.

A forward label nested in structured control flow is also admitted when its
pending edges need only function-scope state. A terminated compound keeps
scanning for such a live label, including a chain of adjacent labels. Entering
a nested scope with live automatic declarations, entering a loop or switch
from outside its structured entry, and bypassing a function-scope declaration
remain explicit UNKNOWN cases.

#### A backward label is a cyclic SSA header

Before body lowering, a syntactic backward goto marks its target label. The
label first merges ordinary fallthrough and forward incoming edges, then
branches through a dedicated cyclic header. That header carries the same
scalar value, definedness, memory, and call-trace PHIs as a structured loop.
Each backward goto appends its predecessor state to those PHIs, including when
one label has several backedges.

Only variables visible at label entry participate in the cycle. A goto that
needs nested automatic state, bypasses an initialization, or enters a loop or
switch from outside remains `UNKNOWN`. A loop-carried pointer must also retain
compatible object authority across every backedge.

#### Data pointers retain up to three levels of indirection

The lowering preserves up to three declarator and typedef stars rather than
flattening them into one pointer-width carrier. Each dereference removes one
level, loads the nested pointer value when another level remains, and admits
an auxiliary authority region only when that loaded address is actually
accessed. Taking the address of a two-level local consequently produces a
three-level pointer to that local's stack object. Source signatures retain the
same pointer depth even though every pointer has the target ABI's 64-bit
representation.

Four or more levels remain an explicit resource boundary. Function pointers
keep their narrower call-specific limits and are not made into data pointers
by this extension.

#### Memory model

`ASM2C_GNU_V1` gives memory a flat 64-bit address space. A pointer is an
address and carries no provenance beyond it, which is what the profile's
source material actually is: C recovered from x86-64 SysV object code, where
casting a pointer to an integer and back, and computing `(char *)p + n`, are
ordinary. Storage is described by object descriptors, each a base address and
a size, and the model holds three standing constraints:

- two descriptors either name the exact same live object or occupy disjoint
  byte ranges; partial overlap is forbidden;
- every object lies strictly above the first page, so address zero belongs to
  no object and dereferencing null is always undefined;
- an object's range does not wrap the address space.

Pointer arguments, globals, strings, and local storage introduce descriptors
before the body. An access through pointer bits read from memory, returned by
a call, or cast from an integer introduces one auxiliary descriptor at that
syntactic access site, because the source signature cannot name its target
object. This inventory is finite even when the CFG is cyclic. A pointer that
is assigned by a loop is widened before its header PHI; every access through
that PHI receives one fixed auxiliary authority region whose contiguous span
must contain every address reached at that syntax site across all iterations.
It is not a fresh descriptor per iteration. The lowering admits at most 128
descriptors and reports `UNKNOWN` above that explicit bound.
An auxiliary descriptor may exactly alias an earlier descriptor. This covers
a derived pointer back into an existing object without admitting partial
overlaps. Loading, comparing, or returning pointer bits without accessing
their target introduces no descriptor and no new domain assumption.

An access of `W` bytes at address `a` is defined exactly when `[a, a + W)`
lies inside one live object and `a` is naturally aligned for `W`. Both the
concrete interpreter and the SMT encoding answer that question the same way,
because the predicate is emitted into the IR as ordinary bit-vector arithmetic
guarded by `UB_GUARD` rather than restated in each backend. The interpreter
additionally knows the partiality of `LOAD` and `STORE` on its own, so a guard
too weak to cover an access is reported instead of silently passing.

**This model admits programs ISO C leaves undefined.** Comparing or
subtracting pointers into different objects is defined here, and object
identity does not constrain arithmetic. Verdicts are therefore relative to
this profile, not to ISO C provenance. A stricter provenance model belongs in
a separate profile rather than as a change to this one, and results carry the
profile precisely so that the distinction survives.

### Input precondition schema

A null precondition means `true`. Otherwise the contract carries UTF-8 JSON
with this envelope:

```json
{
  "schema_version": 1,
  "expression": {
    "op": "and",
    "args": [
      {
        "op": "ule",
        "left": { "op": "arg", "index": 1 },
        "right": {
          "op": "int", "signed": false, "width": 64, "value": "4096"
        }
      },
      {
        "op": "valid_range",
        "range": {
          "pointer": { "op": "arg", "index": 0 },
          "offset": {
            "op": "int", "signed": true, "width": 64, "value": "0"
          },
          "bytes": { "op": "arg", "index": 1 }
        },
        "read": true,
        "write": true,
        "alignment": 4,
        "nullable": false,
        "alias_group": 1
      }
    ]
  }
}
```

The common expression vocabulary implements boolean connectives, typed integer
comparisons and modular arithmetic, `valid_range`, `aligned`, and `disjoint`.
A valid pointer range names the pointer argument, signed byte offset, symbolic
or constant byte extent, read/write permission, alignment, nullability, and an
alias group. `ql_precondition_parse` type-checks these nodes against a versioned
signature and binds canonical JSON plus the signature to one digest.

`quodlibet.problem` schema v1 predates that resolved signature and does not
bind the typed-precondition digest to either C function. A non-null
precondition in a v1 problem is therefore storage and configuration only; it is
not sufficient evidence for a `PROVED_*` verdict.

Schema v2 supplies that binding. It embeds both resolved
`quodlibet.source-signature` payloads with their digests, a total bijective
argument correspondence between them, and the typed-precondition digest
computed against the left signature. Opening a v2 problem rebuilds each
signature artifact from the embedded bytes, re-derives the correspondence and
the precondition digest, and rejects the artifact when any recorded digest
disagrees. `ql_problem_require_proof_binding` is the gate a proof method must
pass before emitting any `PROVED_*` verdict; a v1 problem never passes it,
whether or not it carries a precondition.

A source signature is a separately versioned artifact because IR bit-vector
types are signless: argument and return signedness, pointer depth, address
space, and the ABI profile exist nowhere else.

IR schema v1 defines explicit types for bit vectors, floats, pointers, memory,
and event traces, plus effect bits for memory, calls, volatile access, atomics,
I/O, and undefined behavior. Its declared CFG kind is either acyclic or cyclic;
both are typed SSA artifacts with stable little-endian serialization and a
content digest. Source integer signedness is carried by operation choice, not
by bit-vector types, so source signatures must remain a separate artifact.

## Trust boundary and next layers

The runtime currently trusts a plugin's claimed verdict. A production proof
path should make the trusted base smaller:

1. The Tree-sitter C syntax layer rejects or exposes recovered syntax; a later
   semantic frontend emits a versioned problem and explicit-semantics IR
   artifact.
2. Normalizers emit transformation evidence.
3. Proof and refutation methods race or compose through the DAG.
4. A checker validates SAT certificates, rewrite proofs, or relational
   invariants before the host accepts a proved verdict.
5. Content-addressed caching stores only artifacts whose schema and contract
   identities are part of the key.

The first useful vertical slice should be restricted, loop-free C lowered to
bit vectors and memory, an AIG/SAT miter plugin, a concrete counterexample
replayer, and a small certificate-checking boundary.

Tree-sitter is deliberately outside the proof boundary. Its concrete syntax
tree preserves source structure for fast filtering and later lowering, but a
syntax tree alone proves nothing about preprocessing, C types, observable
behavior, or function equivalence.
