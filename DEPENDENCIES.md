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

## Planned optional proof backends

Other solver libraries remain adapters, not dependencies of `libquodlibet`.

- Kissat rel-4.0.4 is the initial SAT candidate for bit-blasted and AIG-miter
  obligations.
- cvc5 remains a later candidate for quantified obligations, CHC-adjacent
  workflows, and proof production where its feature set is needed.

Backend versions belong to each plugin's evidence metadata and cache key. A
backend may only return a proved verdict when the selected trust policy accepts
its certificate or trusted result path.

Sources: <https://github.com/arminbiere/kissat>, <https://github.com/cvc5/cvc5>

## Update policy

A dependency update requires:

1. an explicit upstream release or commit;
2. an HTTPS source URL and SHA-256 archive checksum;
3. license and notice review;
4. a clean offline configure after vendoring;
5. Windows and Linux builds and native tests;
6. an ABI review if a dependency's types cross a public boundary.

No third-party type currently appears in a public Quodlibet header.
