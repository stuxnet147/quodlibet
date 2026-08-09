# Execution and memory budgets

A caller embedding Quodlibet decides how much time and memory a verification
attempt may consume. `include/quodlibet/budget.h` exposes that decision as five
independent axes, and it guarantees one thing above all: **exceeding a budget
is a run state, never a logical verdict.**

## Axes

`ql_budget_limits_v1` carries five limits. Zero means unlimited on that axis
alone; the axes never imply one another.

| Field | Meaning |
| --- | --- |
| `total_wall_clock_ns` | Whole run, measured from `ql_budget_start` |
| `node_wall_clock_ns` | Any one `QL_BUDGET_SCOPE_NODE` scope |
| `solver_wall_clock_ns` | Any one `QL_BUDGET_SCOPE_SOLVER` scope |
| `memory_bytes` | Live bytes held through the accounting allocator |
| `single_allocation_bytes` | Largest single request the allocator will serve |

Time is monotonic (`uv_hrtime`), not wall-clock timestamps, so a system clock
adjustment cannot extend or collapse a budget.

## Scopes

Per-node and per-solver-call axes need a start and an end, so they are carried
by explicit `ql_budget_scope` objects rather than by hidden thread state. A
scope belongs to the thread that opened it, which is what lets the scheduler
run several nodes in parallel under one budget.

```c
ql_budget_scope *node = NULL;
ql_budget_scope *solver = NULL;

ql_budget_scope_begin(budget, NULL, QL_BUDGET_SCOPE_NODE, "lower", &node, &e);
ql_budget_scope_begin(budget, node, QL_BUDGET_SCOPE_SOLVER, "bitwuzla",
                      &solver, &e);
```

A nested scope inherits its parents' deadlines:
`ql_budget_scope_remaining_ns` returns the tightest of the total axis, the
scope's own axis, and every ancestor's axis.

`ql_budget_scope_begin` refuses to open a scope under an already exhausted
budget, so a run that hit a limit cannot start more work.

## Combining with the solver deadline

`SOLVERS.md` already gives the solver adapter a deadline of its own:
`ql_solver_check_request_v1.timeout_ms` becomes Bitwuzla's `--time-limit`,
guarded by a libuv watchdog, and an expiry is classified as
`QL_SOLVER_UNKNOWN_TIMEOUT`, never as SAT or UNSAT.

These are not two competing deadlines. The budget axis is the ceiling and the
request field is how that ceiling reaches the backend:

> The effective solver deadline is the minimum of the caller's explicit
> `timeout_ms`, the remaining solver-scope budget, the remaining node-scope
> budget, and the remaining total budget. The caller computes it with
> `ql_budget_scope_remaining_ms` and writes the result into `timeout_ms`.

```c
uint64_t remaining = ql_budget_scope_remaining_ms(solver);
if (remaining != 0u &&
    (request.timeout_ms == 0u || remaining < request.timeout_ms)) {
    request.timeout_ms = remaining;
}
```

`ql_budget_scope_remaining_ms` rounds up while any time remains, because a
backend reads `timeout_ms == 0` as "no limit"; truncating a live
sub-millisecond budget to zero would silently remove the deadline. It returns
zero only when there is no time axis at all or when the deadline has already
passed, and the caller must check the budget before starting a solver at all in
the latter case.

For cancellation during a check, pass `ql_budget_scope_is_cancelled` and the
scope as `is_cancelled` and `cancel_state`. Both fields have the shape
`ql_solver_check_request_v1` and `ql_run_context_v1` already expect. A backend
timeout and a budget cancellation both land on `UNKNOWN`, which is exactly
where the soundness rules put them.

## Memory accounting

`ql_budget_allocator(budget)` returns a `ql_allocator` that wraps the budget's
backing allocator. It stores a header in front of every block, so a free knows
the size it is returning and the current total is exact rather than sampled.
The header is padded to `max_align_t`, so the pointer handed back keeps the
alignment guarantee the caller had from `malloc`.

`ql_budget_usage_v1` reports current bytes, peak bytes, live allocation count,
allocation and deallocation counts, and the number of refusals.

The refusal rule is exact: an allocation fails when the current total plus the
request plus the header would exceed `memory_bytes`, or when the request alone
exceeds `single_allocation_bytes`. **The decision is a function of the current
usage and the requested size only.** The same allocation sequence therefore
fails at the same allocation every time, which is what
`Budget.MemoryLimitFailsDeterministicallyAtTheSameAllocation` fixes by running
the sequence three times and comparing the accepted count.

Crossing a limit latches `QL_BUDGET_STATE_MEMORY_EXHAUSTED`, and that state is
sticky so the run cannot quietly recover and produce a verdict. Later in-limit
allocations still succeed, because unwinding code has to be able to build an
error before it returns.

Because the counter is exact, the budget is also a precise leak detector for
anything allocated through it. `PipelineBudgetLeak` builds an entire registry,
scheduler, pipeline and input on the accounting allocator, aborts the run on
the node axis, and asserts that the live allocation count and current bytes
return to their pre-run values, then to zero after teardown.

## The gate between a budget and a verdict

```c
ql_status status = ql_budget_guard_outcome(budget, &outcome, &error);
```

If any axis was exhausted, or the budget was cancelled, the outcome is
rewritten to `QL_VERDICT_UNKNOWN`, `checked_bound` is cleared,
`QL_OUTCOME_FLAG_BUDGET_EXHAUSTED` is set on `flags`, and the mapped error
status is returned. The rewrite happens whether or not the caller reads the
status.

`QL_VERDICT_BOUNDED_CLEAN` is withdrawn along with `PROVED_*` and
`COUNTEREXAMPLE`. A bounded-clean result is a claim that a stated bound was
searched without a counterexample; a run cut short by a limit did not finish
searching its bound, so it cannot make that claim either.
`Budget.ExhaustionCannotBecomeALogicalVerdict` fixes this for all five
promotable verdicts.

## Status mapping

No new `ql_status` value was added. Budget failures reuse the codes every
existing call path already propagates and cleans up after:

| State | Status |
| --- | --- |
| `QL_BUDGET_STATE_ACTIVE` | `QL_STATUS_OK` |
| `..._TOTAL_TIME_EXHAUSTED`, `..._NODE_TIME_EXHAUSTED`, `..._SOLVER_TIME_EXHAUSTED`, `..._CANCELLED` | `QL_STATUS_CANCELLED` |
| `..._MEMORY_EXHAUSTED` | `QL_STATUS_OUT_OF_MEMORY` |

`ql_budget_get_state` and `ql_budget_state_string` recover the precise reason.
The first exhaustion wins, so the reported reason is the one that actually
stopped the run.

## Pipeline integration

`ql_pipeline_run_with_budget` is `ql_pipeline_run` plus a budget; passing null
is exactly the old behaviour. Each node runs inside its own
`QL_BUDGET_SCOPE_NODE` scope named after the node, the budget is checked before
each dependency level starts, and the run context's cancellation predicate
reports either the cancel token or the budget. A node that overruns its axis
releases its own output before returning.

When the run stops on a budget, `ql_pipeline_run_with_budget` releases every
artifact produced so far and hands back no partial result.

## Verification

| Configuration | Result |
| --- | --- |
| `windows-clang` | 171/171 |
| `windows-clang` with `-DQL_ENABLE_LOGGING=OFF` | 171/171 |
| `linux-clang` | 171/171 |
| Linux clang 18 with `-fsanitize=address,undefined` (LeakSanitizer on, Bitwuzla on) | 171/171 |
| Windows clang 22 with `-fsanitize=address,undefined` (Bitwuzla off) | 168/171, the three Bitwuzla cases skipped by that configuration |

The Linux sanitizer run is the one that carries leak coverage: LeakSanitizer is
not available in the Windows ASan runtime.

```sh
cmake -S . -B out/build/linux-clang-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
cmake --build out/build/linux-clang-asan --parallel
ctest --test-dir out/build/linux-clang-asan
```
