# Dependency decisions

Quodlibet vendors small, reusable native components instead of rebuilding
platform code or solver infrastructure. Every required source archive is pinned
and SHA-256 verified by `scripts/vendor.sh`. Configuration and ordinary builds
are offline.

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

## Planned optional proof backends

Solver libraries will be adapters, not dependencies of `libquodlibet`.

- Kissat rel-4.0.4 is the initial SAT candidate for bit-blasted and AIG-miter
  obligations.
- Bitwuzla 0.9.1 is the initial SMT candidate for bit vectors, arrays,
  floating-point terms, and uninterpreted functions.
- cvc5 remains a later candidate for quantified obligations, CHC-adjacent
  workflows, and proof production where its feature set is needed.

Backend versions belong to each plugin's evidence metadata and cache key. A
backend may only return a proved verdict when the selected trust policy accepts
its certificate or trusted result path.

Sources: <https://github.com/arminbiere/kissat>,
<https://github.com/bitwuzla/bitwuzla>, <https://github.com/cvc5/cvc5>

## Update policy

A dependency update requires:

1. an explicit upstream release or commit;
2. an HTTPS source URL and SHA-256 archive checksum;
3. license and notice review;
4. a clean offline configure after vendoring;
5. Windows and Linux builds and native tests;
6. an ABI review if a dependency's types cross a public boundary.

No third-party type currently appears in a public Quodlibet header.
