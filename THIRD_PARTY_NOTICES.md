# Third-party notices

This file records the dependency policy; the complete license texts are kept in
the corresponding vendored directories.

| Project | Pinned version | License | Use in Quodlibet |
| --- | --- | --- | --- |
| libuv | v1.52.1 | MIT | Cross-platform threading and plugin loading |
| yyjson | 0.12.0 | MIT | Configuration and interchange JSON |
| xxHash | v0.8.3 | BSD-2-Clause | Fast process-local hashing |
| BLAKE3 | 1.8.6 | CC0-1.0 or Apache-2.0 | Reproducible content identities |
| Tree-sitter | v0.26.12 | MIT | Incremental parsing runtime |
| tree-sitter-c | v0.24.2 | MIT | Generated C grammar |
| zf_log | v0.4.1 | MIT | Logging engine behind the public logging API |
| GoogleTest | v1.17.0 | BSD-3-Clause | Test-only framework |
| Bitwuzla | 0.9.1 | MIT | Canonical SMT backend on Windows and Linux x86-64 |

Planned optional adapters:

| Project | Pinned version | License | Intended use |
| --- | --- | --- | --- |
| Kissat | rel-4.0.4 | MIT | SAT and AIG-miter backend |

Adding or updating a dependency requires an explicit version, an upstream
archive checksum, a license review, and successful Windows and Linux CI.
