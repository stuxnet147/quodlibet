# Logging

Quodlibet is a library that runs inside somebody else's program. Its logging
service is therefore silent by default, writes nothing to the caller's stdout
or stderr on its own, and lets the caller decide the level, the verbosity and
where records go.

The public surface is `include/quodlibet/log.h`. The engine is the pinned
zf_log 0.4.1 release; `DEPENDENCIES.md` records why that library was selected
and how it is built.

## Model

A record is admitted when its level passes the threshold for its category, and
it is emitted when a sink is installed. Nothing else gates it.

```
QL_LOGI("solver", "check %s", name)
  -> QL_LOG_IS_ENABLED  : one load of ql_log_threshold, one compare
  -> ql_log_write       : per-category level, sink presence, format, capture
  -> engine             : compose the line into a per-call stack buffer
  -> sink               : one call under a mutex, with the structured record
```

### Levels

`trace`, `debug`, `info`, `warn`, `error`, `fatal`, and `off` as a threshold
that admits nothing. `ql_log_set_level` sets the global threshold and
`ql_log_set_category_level` overrides it for one exact category name. Passing a
null category to the latter clears every override.

`ql_log_threshold` is exported so that `QL_LOG_IS_ENABLED` is a load and a
compare in the caller's own translation unit. It always holds the most verbose
level any rule can admit, so the cheap check never hides a record that a
category override would have allowed; the exact per-category decision is made
inside `ql_log_write`.

### Verbosity axes

Each axis is an independent bit in `ql_log_config_v1.details`:

| Bit | Effect on the line | Effect on the record |
| --- | --- | --- |
| `QL_LOG_DETAIL_TIMESTAMP` | ISO-8601 stamp, `Z` suffix under `QL_LOG_CLOCK_UTC` | `timestamp_seconds`, `timestamp_nanoseconds` |
| `QL_LOG_DETAIL_THREAD_ID` | operating-system thread id | `thread_id` |
| `QL_LOG_DETAIL_PROCESS_ID` | process id | `process_id` |
| `QL_LOG_DETAIL_LEVEL` | level name | always present in `level` |
| `QL_LOG_DETAIL_CATEGORY` | category | `category` |
| `QL_LOG_DETAIL_SOURCE_FILE` | file base name | `file` |
| `QL_LOG_DETAIL_SOURCE_LINE` | line number | `line_number` |
| `QL_LOG_DETAIL_SOURCE_FUNCTION` | function name | `function` |

A disabled axis is absent from both the composed line and the structured
record, so a text sink and a structured sink always agree about how much was
disclosed. `timestamp_precision` selects seconds, milliseconds, microseconds or
nanoseconds independently of the timestamp bit.

### Sinks

`ql_log_set_sink` copies the caller's `ql_log_sink_v1`. Installing a new sink or
calling `ql_log_reset` runs the previous sink's optional `close` exactly once.
Every pointer in `ql_log_record_v1` is valid only for the duration of the sink
call.

Sink calls are serialized by a mutex, so concurrent writers never interleave
inside a sink. A sink that logs would reenter the logger, so a thread-local
guard drops those records and counts them in
`ql_log_statistics_v1.dropped_reentrant` rather than deadlocking or splicing
one line into another.

### Builds without the engine

`QL_ENABLE_LOGGING=0` compiles the engine out. The whole API stays callable and
returns the same statuses, `ql_log_available()` returns zero, no record is ever
emitted, and a sink handed to `ql_log_set_sink` is closed immediately so the
caller is not left believing its callback owns a live subscription.

```sh
cmake -S . -B out/build/windows-clang-nolog -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-DQL_ENABLE_LOGGING=0"
cmake --build out/build/windows-clang-nolog --parallel
ctest --test-dir out/build/windows-clang-nolog
```

## Disabled-level overhead

The requirement is a measured cost per call for a call site whose level is
turned off, not an estimate.

### Method

`tests/test_log_overhead.cpp` times two loops that differ by exactly one thing:
the presence of `QL_LOGT("bench", "iteration %d payload %s", ..., "unused")`.

- Each loop iteration contains 16 steps, and every step begins with an inline
  assembly barrier (`"" : "+r"(accumulator) : : "memory"`). The memory clobber
  forces each guard to reload `ql_log_threshold`, so the compiler can neither
  hoist the load out of the loop nor share it between the 16 call sites. This
  is the worst case for a disabled call.
- 1,000,000 iterations, 16 call sites each, 9 repetitions, plus one untimed
  warm-up pass per loop. The reported figure is the minimum across repetitions,
  which is the least noise-contaminated sample.
- Overhead per call is `(with_call - baseline) / (iterations * 16)`.
- The test also asserts that `ql_log_statistics_v1` counted nothing, which
  proves the guard rejected every call before `ql_log_write` was entered.

Reproduce:

```sh
ctest --preset windows-clang -R quodlibet.LogOverhead -V
```

### Result

Measured on 2026-08-10.

| Item | Value |
| --- | --- |
| Machine | Intel Core Ultra 9 285K, 24 cores, 3.7 GHz base |
| OS | Windows 11 Pro 10.0.26200 |
| Compiler | clang 22.1.2, target x86_64-pc-windows-msvc |
| Build | `windows-clang` preset, RelWithDebInfo, `-O2 -DNDEBUG` |
| Baseline step | 0.0334 ns |
| Step with a disabled call site | 0.1045 ns |
| **Disabled call overhead** | **0.071 ns per call** |
| Spread over three consecutive runs | 0.0709 to 0.0714 ns per call |
| Same measurement in a `QL_ENABLE_LOGGING=0` build | 0.067 ns per call |

At the measured clock this is roughly a third of a cycle per call site: the
guard is a load of a level-one-resident global, a compare and a
predicted-not-taken branch, and the out-of-order engine absorbs most of it into
surrounding work. The two build configurations agree because the guard is the
same code either way; `QL_ENABLE_LOGGING=0` removes the engine behind it, not
the check.

The CTest case asserts only a loose ceiling of 25 ns per call. It is there to
catch a regression that turns the guard into real work, not to pin a
machine-specific number. When the number above is refreshed, refresh the
machine, OS, compiler and build rows with it.
