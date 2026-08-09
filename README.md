# quodlibet

Quodlibet is a native C17 foundation for composing function-equivalence
methods. It is not yet an end-to-end equivalence checker: this milestone
establishes the portable host, semantic data, solver boundary, and plugin model
on which checked proof pipelines can be assembled.

The current foundation provides:

- immutable, typed and content-addressed artifacts;
- a versioned C plugin ABI that does not transfer CRT-owned memory;
- a method registry and a portable dynamic loader;
- composable DAG pipelines loaded from JSON;
- parallel execution of independent DAG nodes on a native worker pool;
- cancellation, task groups, deterministic sink ordering, and error cleanup;
- explicit semantic-contract and verdict vocabulary;
- user-selectable observations, UB/refinement rules, target profiles, and
  versioned input preconditions;
- a reusable Tree-sitter C syntax parser with opaque tree traversal;
- an immutable, content-addressed problem artifact containing both functions
  and their complete semantic contract;
- restricted-C eligibility and function/type inventory without pretending that
  syntax analysis is semantic lowering;
- a typed, immutable SSA IR with explicit effects, UB guards, traps,
  termination, memory, and event traces;
- a loop-free integer and `_Bool` C lowering slice with explicit conversions,
  control flow, PHI nodes, and definedness predicates;
- a typed and canonical input-precondition AST for integer constraints, valid
  pointer ranges, alignment, and disjointness;
- a pure Bool/bit-vector e-graph with bounded saturation and replayable merge
  evidence;
- a solver-neutral ABI backed by the pinned Bitwuzla 0.9.1 process adapter,
  with real SAT/model and UNSAT-metadata results;
- capability checks for user-selected e-graph, SMT, AIG/SAT, bounded execution,
  concrete differential, CHC/PDR, and extension methods;
- deterministic BLAKE3 cache identities and method-independent evidence
  envelopes;
- pinned, checksum-verified dependencies in `third_party`;
- native tests exercised on Windows and Linux.

## Build

Prerequisites are CMake 3.21 or newer, Ninja, a C17 compiler, a C++17 compiler
for GoogleTest, Git Bash on Windows, and a POSIX shell on Linux. Dependency
downloads require `curl`, `tar`, `unzip`, and `sha256sum` only once.

```sh
cd D:/projects/machine-model/python/quodlibet
./scripts/vendor.sh
./scripts/check.sh
```

The first command is idempotent for an unchanged dependency lock. CMake never
downloads source code during configuration.

The bundled Linux Bitwuzla executable targets glibc-based x86-64 systems with
`GLIBC_2.38` and `GLIBCXX_3.4.32`. On older glibc or musl systems, configure
with `-DQL_BITWUZLA_EXECUTABLE=/absolute/path/to/bitwuzla-0.9.1`.

Equivalent explicit commands are:

```sh
cmake --preset windows-clang
cmake --build --preset windows-clang --parallel
ctest --preset windows-clang
```

On Linux, replace `windows-clang` with `linux-clang`.

## CLI

The CLI currently exposes foundation diagnostics and pipeline validation:

```sh
out/build/windows-clang/quodlibet.exe version
out/build/windows-clang/quodlibet.exe methods
out/build/windows-clang/quodlibet.exe parse-c input.c
out/build/windows-clang/quodlibet.exe validate examples/branching.json
```

The example is a fan-out pipeline. `normalize` receives the initial artifact;
`prove` and `refute` consume its output and may run in parallel. Both terminal
artifacts are returned to the caller.

## Embedding

Include the umbrella header and link the `quodlibet::quodlibet` CMake target:

```c
#include <quodlibet/quodlibet.h>

ql_registry *registry = NULL;
ql_scheduler *scheduler = NULL;
ql_error error;

if (ql_registry_create(NULL, &registry, &error) != QL_STATUS_OK) {
    return 1;
}
if (ql_register_builtin_methods(registry, &error) != QL_STATUS_OK) {
    ql_registry_destroy(registry);
    return 1;
}
if (ql_scheduler_create(NULL, 0, &scheduler, &error) != QL_STATUS_OK) {
    ql_registry_destroy(registry);
    return 1;
}

/* Load methods, build or parse a pipeline, create a problem artifact, run. */

ql_scheduler_destroy(scheduler);
ql_registry_destroy(registry);
```

A worker count of zero selects the host's available parallelism. The public
headers are under `include/quodlibet`.

## C syntax frontend

The public `c_syntax.h` API owns no source text and exposes no Tree-sitter type.
Create one `ql_c_parser` per worker and reuse it across independent inputs. A
successful call may return a recovered tree containing `ERROR` or missing
nodes, so call `ql_c_syntax_tree_has_errors` before accepting the syntax. The
cursor API traverses both named and anonymous nodes and exposes byte and UTF-8
row/column ranges.

This layer only establishes C syntax. It does not run a preprocessor or prove
source semantics. The separate `c_frontend` eligibility layer and `c_lower`
vertical slice currently handle loop-free integer and `_Bool` functions; loops,
pointers, calls, volatile and atomic operations return an explicit unsupported
result. There is still no product-program equivalence method, counterexample
replayer, or complete checked-proof path. See `ARCHITECTURE.md` for the exact
boundaries.

## Semantic contract

Initialize the public contract and then narrow only the axes the application
intends to ignore:

```c
ql_semantic_contract_v1 contract;
ql_error error;

ql_semantic_contract_init(&contract);
contract.relation = QL_RELATION_LEFT_REFINES_RIGHT;
contract.ub_policy = QL_UB_LANGUAGE_REFINEMENT;
contract.observations &= ~QL_OBSERVE_EXTERNAL_CALLS;
contract.external_call_observation = QL_EXTERNAL_CALLS_IGNORE;

if (ql_semantic_contract_validate(&contract, &error) != QL_STATUS_OK) {
    /* Reject the request before any proof method runs. */
}
```

The default profile follows the canonical asm2c corpus: GCC and Clang families,
PIC and non-PIC x86-64 Linux SysV code, with target feature groups selectable by
the caller. Exact compiler versions and an explicit C standard were not stored
in the corpus, so Quodlibet does not silently claim either. Input assumptions,
including symbolic readable/writable pointer ranges, are supplied through the
versioned `precondition_json` field described in `ARCHITECTURE.md`. The typed
precondition parser is implemented, but binding its signature digest to both
lowered C functions and the problem artifact remains required before a method
may claim `PROVED_*` for a non-null precondition.

## Proof method selection

No proof algorithm is privileged by the host. A pipeline node selects a method
by its registered name and supplies method-specific options. Before execution,
the proof capability API can reject a method that does not support the
problem's relation, UB policy, observations, precondition, or requested result
kind. `BOUNDED_CLEAN` remains a finite-search result and is never promoted to a
proof.

See `METHODS.md` for the method catalogue, soundness boundaries, evidence
requirements, and pipeline examples.
