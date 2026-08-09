# Solver boundary

Quodlibet has a replaceable solver ABI, but its initial canonical backend is
Bitwuzla 0.9.1. Quodlibet does not contain an SMT or SAT evaluator. The small
SMT-LIB builder in `solver.h` only serializes deterministic interchange bytes;
all satisfiability decisions come from the pinned Bitwuzla executable.

## Canonical Bitwuzla backend

The repository pins the official Bitwuzla 0.9.1 x86-64 bundles. The Windows
bundle has a MinGW C++ ABI and cannot be linked safely into the current native
Windows Clang/MSVC-ABI library. The canonical adapter therefore isolates the
solver behind a child-process boundary on both Windows and Linux. This also
keeps the public Quodlibet ABI in C and separates a direct solver crash from
the host process.

Without an explicit override, the adapter first looks beside the running host
executable for `bitwuzla.exe` on Windows or `bitwuzla` on Linux. This is the
installed and relocatable layout. It falls back to the build definition
`QL_BITWUZLA_EXECUTABLE` only when that adjacent file is absent. A caller may
override both for testing with the exact options object below. Relative paths
and additional fields are rejected.

When `QL_ENABLE_BITWUZLA=OFF`, the descriptor remains discoverable but reports
`QL_SOLVER_UNAVAILABLE`, and instance creation returns `QL_STATUS_NOT_FOUND`.

```json
{"executable":"D:/absolute/path/to/bitwuzla.exe"}
```

Creation resolves the selected source path but never runs it directly. The
adapter first copies the selected file into a unique private temporary
directory. Its name contains the creator PID and a random suffix so concurrent
hosts can distinguish their own snapshot bookkeeping. It hashes, runs
`--version` on, and later solves with only that per-instance snapshot,
accepting only the exact `0.9.1` version line. The original path is never
executed after selection. This removes the identity gap caused by replacing an
override between creation and a later check. Linux lowers both the completed
snapshot file and its directory to mode `0500`. Windows applies the platform's
standard read-only file permission through libuv. This is not a Windows ACL
sandbox, and the PID in the name is not a security boundary.

The BLAKE3 digest in every result is the digest of the actual snapshot bytes,
so a same-version patched override has a distinct identity. The adapter hashes
the snapshot immediately before and after the version probe, and again
immediately before and after each check. A mismatch discards the process result
and returns an integrity error. These comparisons detect ordinary replacement
or modification across an execution, but they are not a seal against a hostile
program that restores its original bytes before the post-execution hash. The
`--version` probe has a five-second internal deadline and the same hard stdout
and stderr ceilings as normal checks. Checks use `uv_spawn`, explicit argv
entries and pipes.

Snapshot cleanup is best-effort. Creation failures attempt cleanup, and the
void instance destructor attempts to make the snapshot writable again before
removing its file and otherwise-empty directory. The API cannot report a
cleanup failure, so a sharing violation, open executable, or unexpected
directory entry may leave a temporary artifact behind.

There is no command shell. The adapter sends a controlled transcript ending in
`check-sat`, optionally `get-model`, and `exit`. It accepts a logical answer
only when stdout begins with an exact `sat`, `unsat`, or `unknown` line. Other
stdout and stderr are retained as diagnostics.

Timeouts are passed to Bitwuzla as `--time-limit` in milliseconds. A libuv
watchdog terminates a process that exceeds the limit plus a short transport
grace period, then escalates to a force kill if graceful termination is not
observed. The same watchdog polls the caller's cancellation callback.
Cancellation and timeout are logical `UNKNOWN` results, not SAT or UNSAT.
An already-cancelled request is detected synchronously before spawning a child.
A normal Bitwuzla `unknown` response is conservatively classified as backend
unknown; only an adapter watchdog expiration is classified as timeout.

If the direct child exits before stdout and stderr reach EOF, the adapter gives
the pipes a bounded 250 millisecond drain period. It then closes inherited
pipes locally and returns a transport error instead of allowing a descendant
that retained a pipe to keep the event loop alive indefinitely.

Termination targets only the direct child. `PROCESS_ISOLATION` means an
out-of-process adapter boundary, not descendant containment, a Windows Job
Object, a POSIX process sandbox, or hostile-binary isolation. A malicious
solver running as the same user, self-modification that is restored between
digest checks, and termination of descendants are outside this backend's
security boundary. Deployments that accept untrusted solver executables must
add an OS sandbox and process-tree containment outside Quodlibet.

The typed `memory_limit_mb` request field maps to Bitwuzla's per-check
`--memory-limit` setting. Zero leaves the setting unset. This is a
backend-native limit in MiB, not an operating-system RSS hard cap, so callers
requiring process-wide containment must add an OS-level sandbox or job limit.

The v1 transport has hard byte ceilings: 64 MiB for the complete SMT-LIB
transcript and terminal query, 16 MiB for stdout, and 4 MiB for stderr. Crossing
a ceiling terminates the child and returns an error instead of growing memory
without bound. A check request may select smaller non-zero stdout and stderr
ceilings for tighter per-query resource control.

Each check uses a fresh Bitwuzla process and replays the deterministic
transcript. `push` and `pop` remain available at the Quodlibet instance layer,
so incremental clients can share their lowering and transcript while process
state never leaks across checks.

## Capability contract

The Bitwuzla descriptor advertises these initial capabilities:

- `QF_BV`, `QF_ABV`, `QF_FP`, `QF_BVFP`, and `QF_AUFBV`;
- bit-vector widths from 1 through the Quodlibet `uint32_t` limit;
- arrays, floating point, incremental transcripts, models, cancellation,
  timeout, backend-native memory limits, and process isolation;
- no proof-artifact capability.

Every request is checked against the descriptor before execution. Requesting a
proof artifact from Bitwuzla is therefore rejected before a process starts.
Future native C API adapters may reuse the same descriptor and result ABI, but
they must not weaken the version check, cancellation contract, or proof trust
boundary.
They are a non-canonical optimization until their platform ABI and dependency
closure are independently validated.

## Result and evidence discipline

`SAT` is a candidate model. A Quodlibet equivalence method must decode and
replay it against the semantic IR before emitting a counterexample verdict.

`UNSAT` from Bitwuzla records backend name, exact version, executable content
digest, transport, query digest, exit status, and the fact that a proof is
unavailable. This metadata is not a proof. Bitwuzla 0.9.1 exposes models, unsat
cores, and interpolation, but does not expose a proof object through its
documented C interface or CLI.

The function `ql_solver_checked_proof_binding_validate` performs only
structural validation of caller-supplied metadata. It checks all of the
following:

1. an `UNSAT` result carrying raw solver-proof bytes;
2. non-empty claimed checker identity metadata;
3. matching query and raw-proof digests;
4. a checked `quodlibet.proof` artifact and checker identity.

Successful binding validation does not prove that the named checker ran, that
it accepted the artifact, or that it is trusted. The public structure is filled
by a caller and grants no verdict authority. Trusted proof-method code must
invoke and authenticate a checker independently before emitting any `PROVED`
verdict. Consequently, canonical Bitwuzla `UNSAT` cannot by itself become
`PROVED_EQUIVALENT` or either refinement verdict. It may be retained as solver
evidence, used in a bounded result, or combined with a future independently
checked certificate path.

## Deterministic SMT-LIB subset

The v1 builder emits one command per line in call order. It currently provides
logic selection, Boolean and bit-vector constants, nullary `define-fun`
bindings for Boolean and bit-vector terms, and Boolean assertions. It also
declares and defines `(Array (_ BitVec I) (_ BitVec E))` symbols, which is how
a flat byte-addressed memory enters a query; `select` and `store` appear only
inside definition bodies, so the builder writes the sort and nothing else. The
definition forms exist so a relational encoding can name each intermediate
value once instead of substituting it into a single term that grows with the
product of two control-flow graphs. A definition body is serialized verbatim;
Bitwuzla remains the only component that parses or sort-checks it. Raw
assertion artifacts pass a top-level command whitelist before reaching the
backend. Solver-driving commands such as `check-sat`, `get-model`, `reset`,
`push`, `pop`, and `exit` cannot be injected through that artifact boundary.
Raw `set-option` and `set-info` are also rejected, preventing output-channel or
filesystem options from escaping the process protocol. Safe options belong in
typed Quodlibet request fields.
The whitelist is not an SMT parser and makes no satisfiability decision;
Bitwuzla performs syntax, sort, theory, and satisfiability checking.

Solver identity, exact options, the canonical SMT-LIB bytes, semantic problem
digest, timeout and memory-limit policy, output ceilings, and requested
artifacts must all participate in the calling proof method's cache and evidence
identity.
