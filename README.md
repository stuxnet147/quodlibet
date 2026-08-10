# quodlibet

Quodlibet 0.1.0 is a native C17 function-equivalence engine and a framework
for composing proof and refutation methods. It has an end-to-end path for the
currently supported, loop-free subset of the `ASM2C_GNU_V1` C profile:

1. parse and resolve two C functions;
2. lower them to typed SSA IR with explicit definedness and effects;
3. bind their signatures, argument correspondence, precondition, and semantic
   contract into one content-addressed problem;
4. prove the requested relation or find a concrete counterexample;
5. return the verdict together with the evidence and trust boundary that
   justify it.

Quodlibet is not yet a general C equivalence checker. Unsupported syntax or
semantics produce an explicit `UNKNOWN` result instead of being approximated
as a narrower problem. Loops and several important C features remain outside
the implemented proof slice.

## What is implemented

### Native host and semantic core

- immutable, typed artifacts with persistent BLAKE3 identities;
- a versioned C ABI and plugin boundary that do not transfer CRT-owned memory;
- a method registry, portable dynamic loading, and JSON DAG pipelines;
- parallel execution of independent DAG nodes on a native worker pool;
- cancellation, deterministic sink ordering, task groups, and cleanup on
  failure;
- configurable relation, UB policy, observations, target profile, and typed
  input preconditions;
- caller-controlled wall-clock, node, solver, total-memory, and
  single-allocation budgets;
- runtime logging with independent level, category, timestamp, source, and
  thread-id controls;
- caller-defined verdict policies whose parser and evaluator cannot weaken the
  core soundness rules.

### C frontend and IR

- a reusable Tree-sitter C syntax parser with no Tree-sitter types in the
  public API;
- restricted-C eligibility, function selection, type inventory, and reusable
  parser/tree entry points for corpus processing;
- source-signature artifacts that preserve widths, signedness, pointer depth,
  address space, and ABI profile;
- immutable typed SSA IR with control flow, PHI nodes, memory objects, calls,
  traps, termination, effects, UB guards, and initial memory images;
- an independent IR verifier and a concrete interpreter;
- differential tests that compare lowered execution with compiled C;
- a flat 64-bit little-endian memory model for the `ASM2C_GNU_V1` profile,
  including aligned in-object access checks and disjoint live objects.

The current lowering handles a useful loop-free slice that includes fixed-width
integer and `_Bool` expressions, conversions, short-circuit control flow,
conditionals, assignments, increments, pointers, selected arrays and records,
locals, selected globals, and calls to declared callees. The exact accepted
surface is intentionally narrower than the syntax frontend.

### Proof, refutation, and evidence

The builtin registry currently contains:

| Method | Role | Positive evidence |
| --- | --- | --- |
| `builtin.identity` | Pipeline and plugin plumbing | No logical claim |
| `prove.smt-product` | Relational SMT product over loop-free IR and flat memory | Explicit trusted-Bitwuzla policy, recorded as `checked_proof=false` |
| `prove.aig-sat` | Bit-blasted scalar product using CaDiCaL | LRAT certificate accepted by the independent `lrat-check`, recorded as `checked_proof=true` |
| `refute.concrete-differential` | Deterministic boundary and random input search | Replay-confirmed counterexample only |

Both exact paths use the same product-query encoding. A SAT assignment is only
a candidate until the shared concrete replay path reproduces the violation.
The SMT method defaults to retaining raw Bitwuzla `UNSAT` as evidence while
returning `UNKNOWN`; promotion requires the caller to select the recorded
`trusted-backend` policy. The AIG/SAT method promotes `UNSAT` only after its
LRAT certificate is independently checked and the comparison domain is shown
to be inhabited.

The core also provides:

- a pure bit-vector e-graph, a versioned rewrite-rule catalogue, and an
  independent merge-log replay checker;
- evidence-based combination of method results, with no majority voting;
- a persistent artifact and evidence store whose records and cache-key inputs
  are revalidated on load;
- deterministic cache keys that bind the problem, contract, method version,
  options, backend, queries, and trust policy.

`BOUNDED_CLEAN` is never promoted to a proof. An unreplayed solver model is
never reported as a counterexample. A budget failure withdraws any logical
claim to `UNKNOWN` before a caller policy is applied.

## Current coverage and limits

The latest checked corpus report uses 29,893 distinct training C bodies from
`D:/projects/machine-model/datasets/records-local`:

| Stage | Current result |
| --- | ---: |
| Tree-sitter parse without recovery nodes | 29,880 / 29,893, 99.96% |
| Restricted-C frontend eligibility | 29,822 / 29,880, 99.81% |
| Semantic IR lowering | 8,541 / 29,880, 28.58% |
| Verdicts on lowerable validation self-pairs | 337 / 349, 96.6% |

The lowering number is the relevant completeness limit. The largest remaining
groups include pointers loaded from memory whose object provenance cannot yet
be recovered, `sizeof`, loops, uninitialized-read analysis, additional record
and function types, variadic calls, and unsupported control flow. Preprocessor
directives are detected but not expanded, so callers must supply preprocessed
source. Volatile and atomic behavior is represented in the contract and IR
vocabulary but is not yet lowered from general C.

CHC/PDR and bounded symbolic execution are documented future methods, not
registered implementations. The AIG/SAT path is currently scalar and refuses
array sorts. The Python `check` API uses the SMT product path; other registered
methods are available through the native C API and pipelines.

The detailed measurement and remaining diagnostic distribution are in
[`docs/coverage/coverage-20260812b.md`](docs/coverage/coverage-20260812b.md).
The bounded libFuzzer campaign ran all nine current targets for one minute each
under ASan and UBSan with zero crashes after fixing the defect found by the
first pass. See [`docs/fuzz/campaign-20260810.md`](docs/fuzz/campaign-20260810.md).

## Build and test

The native build requires CMake 3.21 or newer, Ninja, Clang with C17 and C++17
support, and Git Bash on Windows or a POSIX shell on Linux. The tests use
GoogleTest. Dependency preparation requires `curl`, `tar`, `unzip`, and
`sha256sum` on the first run.

On Windows, run from Git Bash:

```sh
cd D:/projects/machine-model/python/quodlibet
./scripts/vendor.sh
./scripts/check.sh windows-clang
```

On Linux:

```sh
./scripts/vendor.sh
./scripts/check.sh linux-clang
```

`vendor.sh` is idempotent while the pinned dependency versions and checksums
are unchanged. CMake does not download dependencies during configuration.
`check.sh` configures, builds, and runs CTest. The equivalent explicit commands
on Windows are:

```sh
cmake --preset windows-clang
cmake --build --preset windows-clang --parallel
ctest --preset windows-clang
```

Useful build options are:

| Option | Default | Purpose |
| --- | --- | --- |
| `QL_BUILD_SHARED` | `OFF` | Build a shared core library instead of the default static library |
| `QL_BUILD_TESTS` | `ON` | Build the native GoogleTest suite |
| `QL_ENABLE_BITWUZLA` | `ON` | Enable the canonical SMT process backend |
| `QL_ENABLE_SAT` | `ON` | Build the pinned CaDiCaL backend and LRAT checker |
| `QL_ENABLE_LOGGING` | `ON` | Compile the logging backend |
| `QL_BUILD_FUZZERS` | `OFF` | Build Clang/libFuzzer targets |
| `QL_BITWUZLA_EXECUTABLE` | vendored executable | Override the Bitwuzla 0.9.1 path |

The bundled Linux Bitwuzla executable targets glibc-based x86-64 systems with
`GLIBC_2.38` and `GLIBCXX_3.4.32`. On an older glibc or musl system, provide a
compatible Bitwuzla 0.9.1 executable through `QL_BITWUZLA_EXECUTABLE`.
With `QL_ENABLE_BITWUZLA=OFF`, the transport-independent core still builds and
the solver descriptor remains discoverable as unavailable.

Sanitizer and fuzz configurations are available as `linux-sanitize` and
`linux-fuzz` presets. The fuzz targets require Clang on Linux.

## CLI

The CLI exposes diagnostics, method discovery, syntax validation, pipeline
validation, and corpus coverage measurement:

```sh
out/build/windows-clang/quodlibet.exe version
out/build/windows-clang/quodlibet.exe methods
out/build/windows-clang/quodlibet.exe parse-c input.c
out/build/windows-clang/quodlibet.exe validate examples/branching.json
out/build/windows-clang/quodlibet.exe coverage units.txt detail.tsv
```

On Linux, use `out/build/linux-clang/quodlibet`. The current CLI does not have
a direct pair-check command. End-to-end judgments are exposed by the Python
binding and the native method APIs.

## Python binding

The package is a CPython C extension, not `ctypes` or `cffi`. It is built for
the CPython 3.11 stable ABI, releases the GIL for the whole judgment, and links
the native core into a self-contained extension module. Building it requires
CMake 3.26 or newer and CPython 3.11 or newer.

From the repository root:

```sh
python -m pip install ./bindings/python
```

The distribution name is `quodlibet-engine`; the import name is `quodlibet`.
It should not be installed into the same environment as the unrelated Quod
Libet music-player package.

```python
import quodlibet

with quodlibet.SolverSession() as session:
    result = quodlibet.check(
        "int f(int x) { return x + x; }", "f",
        "int g(int x) { return 2 * x; }", "g",
        trust_smt_backend=True,
        budget={"total_ms": 5000, "solver_ms": 3000},
        session=session,
    )

print(result.verdict)
print(result.evidence.checked_proof)
```

`trust_smt_backend=False` is the default. In that mode an SMT `UNSAT` remains
`UNKNOWN` with solver evidence. Passing `True` is an explicit trust decision;
the result still reports `checked_proof=False`. Counterexamples are returned
only after replay.

For a training or scoring loop, use `quodlibet.check_batch(specs, workers=0)`.
It gives each worker its own reusable `SolverSession`, runs judgments in real
parallel threads while the GIL is released, and returns an exception in place
of the failed item without discarding the rest of the batch. See
[`bindings/python/README.md`](bindings/python/README.md) for the complete API.

## Native embedding and installation

Public headers are installed under `include/quodlibet`. In a build tree,
include the umbrella header and link the CMake target:

```c
#include <quodlibet/quodlibet.h>
```

```cmake
target_link_libraries(my_target PRIVATE quodlibet::quodlibet)
```

For an installed package:

```sh
cmake --install out/build/windows-clang --prefix out/install/windows
```

```cmake
find_package(quodlibet REQUIRED)
target_link_libraries(my_target PRIVATE quodlibet::quodlibet)
```

The default static install folds the vendored libraries into Quodlibet. The
CLI and Bitwuzla executable are installed together, and the solver lookup first
checks beside the running host executable so the prefix can be moved. Windows
and Linux install and relocation checks are implemented by
`scripts/check-install.sh`. Shared-library installation remains a separately
measured configuration.

## Semantic and trust contracts

The semantic contract records relation direction, UB policy, observations,
memory and call observation modes, the frozen C/ABI profile, and the typed
precondition. Problem schema v2 binds both source signatures, a total argument
correspondence, and the precondition digest. Schema v1 problems are accepted as
data but cannot cross the proof-binding gate.

The default `ASM2C_GNU_V1` profile models GCC and Clang corpus syntax on x86-64
Linux SysV LP64. It uses a flat 64-bit address-space model chosen for recovered
machine-oriented C. This model is deliberately not ISO C pointer provenance,
and every verdict carries the profile identity.

For exact definitions and trust boundaries, read:

- [`ARCHITECTURE.md`](ARCHITECTURE.md) for artifacts, pipelines, the semantic
  contract, and the memory model;
- [`METHODS.md`](METHODS.md) for implemented and planned methods, evidence, and
  verdict combination;
- [`SOLVERS.md`](SOLVERS.md) for Bitwuzla process isolation, resource limits,
  identity checks, and the raw-UNSAT boundary;
- [`DEPENDENCIES.md`](DEPENDENCIES.md) for pinned versions, checksums, licenses,
  and backend choices;
- [`GOAL.md`](GOAL.md) and [`todo.md`](todo.md) for the current completion
  criteria and open work.
