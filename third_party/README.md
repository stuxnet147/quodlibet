# Vendored dependencies

Quodlibet configures and builds without network access. Run
`./scripts/vendor.sh` once to download the exact, checksum-locked upstream
archives into this directory. The script never replaces an existing directory.

Required sources:

| Directory | Version | Purpose |
| --- | --- | --- |
| `libuv` | 1.52.1 | Portable threads, synchronization, CPU discovery, dynamic libraries |
| `yyjson` | 0.12.0 | Pipeline and result JSON |
| `xxhash` | 0.8.3 | Fast non-cryptographic hashes for transient tables |
| `blake3` | 1.8.6 | Stable artifact and cache identities |
| `tree-sitter` | 0.26.12 | Error-tolerant incremental parsing runtime |
| `tree-sitter-c` | 0.24.2 | Generated C grammar |
| `googletest` | 1.17.0 | GoogleTest unit tests, excluded when tests are disabled |

The source URL and SHA-256 checksum are recorded both in `scripts/vendor.sh`
and in each generated `.quodlibet-vendor` marker. Upstream license files remain
inside their respective source directories.

Solver backends are deliberately not core dependencies. Kissat 4.0.4 and
Bitwuzla 0.9.1 are the first planned adapters. They will be optional plugins so
that the host remains small and different proof methods can be combined without
forcing one solver stack on every build.
