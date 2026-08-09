# Vendored dependencies

Quodlibet configures and builds without network access. Run
`./scripts/vendor.sh` once to download the exact, checksum-locked upstream
archives into this directory. The script never replaces an existing directory.

Required sources and release artifacts:

| Directory | Version | Purpose |
| --- | --- | --- |
| `libuv` | 1.52.1 | Portable threads, synchronization, CPU discovery, dynamic libraries |
| `yyjson` | 0.12.0 | Pipeline and result JSON |
| `xxhash` | 0.8.3 | Fast non-cryptographic hashes for transient tables |
| `blake3` | 1.8.6 | Stable artifact and cache identities |
| `tree-sitter` | 0.26.12 | Error-tolerant incremental parsing runtime |
| `tree-sitter-c` | 0.24.2 | Generated C grammar |
| `googletest` | 1.17.0 | GoogleTest unit tests, excluded when tests are disabled |
| `bitwuzla-linux-x86_64` | 0.9.1 | Canonical SMT backend for Linux x86-64 |
| `bitwuzla-windows-x86_64` | 0.9.1 | Canonical SMT backend for Windows x86-64 |

The source URL and archive SHA-256 checksum are recorded both in
`scripts/vendor.sh` and in each generated `.quodlibet-vendor` marker. The two
Bitwuzla executable payloads have an additional checksum. Upstream license
files remain inside their respective directories.

Bitwuzla 0.9.1 is the canonical SMT solver. Quodlibet vendors the official
static release bundles and invokes the matching executable without a shell.
The host does not contain an SMT implementation. Other proof backends remain
plugins so users can compose methods without changing this solver boundary.
The bundled Linux executable requires `GLIBC_2.38`, `GLIBCXX_3.4.32`,
`libgcc_s.so.1`, `libgmp.so.10`, and `libmpfr.so.6`. Older glibc and musl hosts
must provide a compatible Bitwuzla 0.9.1 through `QL_BITWUZLA_EXECUTABLE`.
CMake verifies that the selected executable runs and reports exactly 0.9.1.
