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

#### Memory model

`ASM2C_GNU_V1` gives memory a flat 64-bit address space. A pointer is an
address and carries no provenance beyond it, which is what the profile's
source material actually is: C recovered from x86-64 SysV object code, where
casting a pointer to an integer and back, and computing `(char *)p + n`, are
ordinary. Storage is described by objects, each a base address and a size,
and the model holds three standing constraints:

- distinct live objects occupy disjoint byte ranges;
- every object lies strictly above the first page, so address zero belongs to
  no object and dereferencing null is always undefined;
- an object's range does not wrap the address space.

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
I/O, and undefined behavior. It is an acyclic typed SSA artifact with stable
little-endian serialization and a content digest. Source integer signedness is
carried by operation choice, not by bit-vector types, so source signatures must
remain a separate artifact.

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
