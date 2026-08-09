# Dependency decisions

Quodlibet vendors small, reusable native components instead of rebuilding
platform code or solver infrastructure. Every required upstream archive is
pinned and SHA-256 verified by `scripts/vendor.sh`. Configuration and ordinary
builds are offline.

## Core dependencies

### libuv 1.52.1

Used for Windows and POSIX threads, mutexes, condition variables, read-write
locks, CPU parallelism discovery, and dynamic-library loading. This replaces a
large platform abstraction layer while keeping the public API in C. Quodlibet
does not use libuv's event loop in the initial runtime.

Source: <https://github.com/libuv/libuv>

### yyjson 0.12.0

Used for pipeline configuration and, later, result interchange. It is a compact
C implementation with no required dependency. JSON is a control-plane format;
solver terms and large IR are not routed through JSON.

Source: <https://github.com/ibireme/yyjson>

### xxHash 0.8.3

Used only for fast transient hashes where collisions are resolved by an equality
check. It must not identify proofs, disk cache entries, or security-sensitive
objects.

Source: <https://github.com/Cyan4973/xxHash>

### BLAKE3 1.8.6

Used for stable artifact, cache, and evidence identities. The initial build uses
the official portable C implementation on every platform. SIMD enablement can
be added as a measured build option without changing digest values or the ABI.

Source: <https://github.com/BLAKE3-team/BLAKE3>

### zf_log 0.4.1

Used as the logging engine behind `include/quodlibet/log.h`. Quodlibet does not
implement its own logger. zf_log owns level gating, the per-call stack buffer,
message formatting, truncation, and single-callback delivery; Quodlibet owns
level and verbosity policy, category routing, the caller-supplied sink, and the
lock that keeps concurrent writers from interleaving inside that sink.

Selection compared the permissively licensed candidates that are usable from a
C17 static library:

| Candidate | License | Language | Disabled level | Thread safety | Windows | Size |
| --- | --- | --- | --- | --- | --- | --- |
| zf_log 0.4.1 | MIT | C99 | One load and compare of an `int`, arguments not evaluated | Stack buffer per call, one callback invocation per line | Yes | 2 files, ~2.3k lines |
| rxi/log.c | MIT | C99 | Function call, arguments evaluated | Caller must install a lock callback | Yes | 1 file, ~200 lines |
| yksz/c-logger | MIT | C99 | Function call, arguments evaluated | Internal mutex | Yes | 4 files, ~1k lines |
| zlog | LGPL-2.1 | C99 | Function call | Internal mutex | Partial | ~10k lines |
| spdlog | MIT | C++11 | Atomic load | Internal mutex | Yes | Header set, ~20k lines |

zf_log wins on the axes this repository weighs:

- License is MIT, so static distribution carries no relinking obligation. zlog
  is LGPL-2.1 and was rejected for that reason alone.
- It is pure C99, so a static consumer of `libquodlibet` never has to link a
  C++ runtime. spdlog is faster in some benchmarks but would impose that cost
  on every C consumer, including the planned CPython extension.
- The disabled-level path is a macro that compares a message level against an
  `int` before evaluating any argument. Measured cost is 0.071 ns per call on
  the reference machine; the method and the machine are recorded in
  `docs/runtime-services/logging.md`.
- Every message is composed in a caller-stack buffer and handed to the output
  callback exactly once, so concurrent writers cannot interleave a partial
  line inside the buffer. Serializing the sink itself is Quodlibet's job.
- Windows and Linux are first-class upstream targets.
- Two source files fit a repository that vendors small, pinned dependencies.

The library has no build target of its own. `src/log.c` sets the configuration
macros, then includes the pinned `zf_log.c` into its own translation unit. This
keeps the logging engine under Quodlibet's warning, visibility and sanitizer
flags, and it lets the message context, tag and source-location layouts be
rendered by Quodlibet callbacks that honour the caller's runtime verbosity.
Only symbols declared in `zf_log.h` are used.

Source: <https://github.com/wonder-mice/zf_log>

### GoogleTest 1.17.0

Used only by C++17 test drivers and omitted when `QL_BUILD_TESTS=OFF`. The
production library remains C17; GoogleTest tests its public C surface and CTest
owns discovery and execution.

Source: <https://github.com/google/googletest>

### Tree-sitter 0.26.12 and tree-sitter-c 0.24.2

Used for the fast, error-tolerant C syntax frontend. The runtime and generated C
grammar are compiled directly into Quodlibet, while all Tree-sitter types remain
behind Quodlibet's public C API. A parser instance can be reused, but it must not
be used concurrently. Syntax acceptance is not a semantic-equivalence result:
preprocessing, name and type resolution, implicit conversions, control-flow
lowering, and undefined-behavior policy remain later frontend stages.

Sources: <https://github.com/tree-sitter/tree-sitter>,
<https://github.com/tree-sitter/tree-sitter-c>

## Solver backends

Quodlibet does not implement an SMT solver. Its canonical SMT backend is the
official Bitwuzla 0.9.1 distribution. The solver-neutral ABI and deterministic
SMT-LIB2 encoder are orchestration boundaries only: every satisfiability result
is produced by Bitwuzla and records the exact backend identity in its evidence.

### Bitwuzla 0.9.1

The checksum-locked official Linux and Windows x86-64 static release bundles
are vendored. Quodlibet invokes their command-line frontend through an argument
vector and pipes, without a shell. This keeps the C host ABI stable and avoids
mixing the MinGW C++ ABI used by the official Windows archive with Quodlibet's
MSVC-ABI Clang build. A compatible direct C API adapter may be added as an
optimization, but it must preserve the same public solver contract and result
classification.

The Linux executable still uses the host's glibc, libstdc++, libgcc, GMP 6 ABI
(`libgmp.so.10`) and MPFR 4 ABI (`libmpfr.so.6`). Its newest imported symbol
versions are `GLIBC_2.38` and `GLIBCXX_3.4.32`, so the bundled default targets
glibc-based x86-64 systems meeting those versions. Older glibc and musl systems
must set `QL_BITWUZLA_EXECUTABLE` to their own compatible Bitwuzla 0.9.1 build.
Configuration runs the selected binary and requires an exact `0.9.1` response,
so an incompatible runtime fails before compilation. The Windows executable is
used from its official self-contained release bundle.

Bitwuzla supports the bit-vector, array, floating-point and uninterpreted
function theories needed by the SMT product-program method. The vendored
binary is also the parser and solver for generated SMT-LIB2; Quodlibet does not
evaluate those terms itself.

Source: <https://github.com/bitwuzla/bitwuzla>

### CaDiCaL 2.2.1

The SAT backend for the AIG miter. It is invoked as an isolated process, like
Bitwuzla, and is never linked into `libquodlibet`: CaDiCaL is C++ and the core
is C17.

It is used specifically for its **native LRAT proof output** (`--lrat`). LRAT
records the antecedent clauses of every learned clause, so checking it is
replay rather than search, and the checker is small enough to audit. That is
what lets the AIG path report `checked_proof = true` where the SMT path cannot.
Enabling LRAT disables some of CaDiCaL's inprocessing, so the same instance can
be slower than an unproved run; a proof that no one can check is not the
cheaper option, it is a different result.

Kissat rel-4.0.4 was the recorded candidate and was **disqualified by
measurement**. Under the MSVC ABI its `watch` union assumes GNU bitfield
packing, its own assertion `sizeof (watch) == sizeof (unsigned)` fires, and an
`NDEBUG` build segfaults during solving; `-mno-ms-bitfields` is refused by the
Windows SDK headers. A MinGW build compiles and runs but **answers a trivially
unsatisfiable formula with `s SATISFIABLE`**. A checker catches a bad UNSAT
proof and nothing in the chain catches a wrong SAT answer, so a solver that
answers wrongly cannot enter a trust chain whose purpose is to shrink what is
trusted. The measurements and the reproducing formula are in
`docs/notes/sat-backend-and-proof-checker.md`.

CaDiCaL was probed the same way before it was pinned: trivial SAT, the trivial
UNSAT formula Kissat got wrong, and a mid-size pigeonhole instance with LRAT
emission and checking, on Windows and Linux. All passed.

Source: <https://github.com/arminbiere/cadical>

### drat-trim (`lrat-check` only), commit `2e3b2dc`

Only `lrat-check.c`, roughly 500 lines of C, is compiled. **This one file is
the entire trusted base of a `checked_proof = true` verdict.** It reads the
original CNF and the LRAT proof and replays the propagations the proof names;
it searches for nothing and it is small enough to read.

`drat-trim.c` is present because the checker's source arrives with its licence
and its siblings under one pinned identity, and it is deliberately **not
built**. The earlier plan used it to elaborate Kissat's DRAT into LRAT; CaDiCaL
emits LRAT directly, so that step and its process left the chain.

The checker is a replaceable boundary. The method takes its executable as an
option and the outcome envelope records which checker validated what, so
`cake_lpr`, whose machine code is verified in HOL4, can be adopted later
without rewriting the method.

Source: <https://github.com/marijnheule/drat-trim>

## Planned optional proof backends

Other solver libraries remain adapters, not dependencies of `libquodlibet`.

- cvc5 remains a later candidate for quantified obligations, CHC-adjacent
  workflows, and proof production where its feature set is needed.

Backend versions belong to each plugin's evidence metadata and cache key. A
backend may only return a proved verdict when the selected trust policy accepts
its certificate or trusted result path.

Source: <https://github.com/cvc5/cvc5>

## Python bindings

`bindings/python/` is a CPython C extension over the same public headers the
core is built from. It is deliberately not an FFI: `ctypes` and `cffi`
re-declare the ABI at run time from a Python-side description that nothing
validates, so a struct that grew a field or an enum whose values moved becomes
silent memory corruption rather than a diagnostic. A compiled extension
consumes `include/quodlibet/*.h` through the C compiler, which turns the same
drift into a build error.

The bindings add **no new vendored dependency**. Their entire runtime is
CPython itself plus the statically linked core.

| Dependency | Version | Role | License | Why |
|---|---|---|---|---|
| CPython stable ABI (abi3) | `Py_LIMITED_API` 0x030B0000, that is 3.11 | The extension's only runtime interface | PSF-2.0 | One built module serves 3.11 and every later minor version, so a Python upgrade in a training environment does not force a rebuild. 3.11 is the floor because it is the oldest release still receiving security fixes when this was written. |
| scikit-build-core | `>= 0.10`, build-time only | PEP 517 backend that drives the existing CMake build | Apache-2.0 | The core already builds with CMake. Any other backend would need a second description of the same build. It is never installed with the wheel. |
| ninja | `>= 1.11`, build-time only, Windows | Generator for the Windows `pip install` path | Apache-2.0 | The Windows default generator selects MSVC, which rejects the C11 atomics the runtime uses. This matches the repository's `windows-clang` preset. |

pybind11 was rejected: it is a C++ library, while the core is C17 and every
public header is C. A pure C extension keeps one language across the boundary
and adds no header-only C++ dependency to the build.

The core is linked **statically** into the extension, so an installed wheel is
one self-contained file with no companion shared library on a loader path. The
pinned Bitwuzla executable is not bundled into the wheel; the core finds it
through the path compiled into it, and `check(solver_executable=...)` overrides
that for a deployment where the wheel and the solver are shipped apart.

## Update policy

A dependency update requires:

1. an explicit upstream release or commit;
2. an HTTPS source URL and SHA-256 archive checksum;
3. license and notice review;
4. a clean offline configure after vendoring;
5. Windows and Linux builds and native tests;
6. an ABI review if a dependency's types cross a public boundary.

No third-party type currently appears in a public Quodlibet header.
