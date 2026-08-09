#ifndef QUODLIBET_BUDGET_H
#define QUODLIBET_BUDGET_H

#include "quodlibet/allocator.h"
#include "quodlibet/semantics.h"
#include "quodlibet/status.h"

#define QL_BUDGET_ABI_VERSION 1u
#define QL_BUDGET_UNLIMITED UINT64_C(0)
#define QL_BUDGET_MAX_SCOPE_NAME 63u

/* Set on ql_outcome_v1.flags by ql_budget_guard_outcome when a run that hit a
   limit is forced back to UNKNOWN. The high bit is used so that verdict flags
   defined elsewhere can grow upward from zero without colliding. */
#define QL_OUTCOME_FLAG_BUDGET_EXHAUSTED (UINT32_C(1) << 31)

QL_EXTERN_C_BEGIN

typedef struct ql_budget ql_budget;
typedef struct ql_budget_scope ql_budget_scope;

/* Exhaustion is a run state, never a logical verdict. Anything other than
   QL_BUDGET_STATE_ACTIVE disqualifies the run from PROVED_*, COUNTEREXAMPLE
   and BOUNDED_CLEAN alike. */
typedef enum ql_budget_state {
    QL_BUDGET_STATE_ACTIVE = 0,
    QL_BUDGET_STATE_TOTAL_TIME_EXHAUSTED,
    QL_BUDGET_STATE_NODE_TIME_EXHAUSTED,
    QL_BUDGET_STATE_SOLVER_TIME_EXHAUSTED,
    QL_BUDGET_STATE_MEMORY_EXHAUSTED,
    QL_BUDGET_STATE_CANCELLED
} ql_budget_state;

typedef enum ql_budget_scope_kind {
    QL_BUDGET_SCOPE_NODE = 0,
    QL_BUDGET_SCOPE_SOLVER = 1
} ql_budget_scope_kind;

/* Every axis is independent and zero means unlimited on that axis alone.
   Time axes are monotonic nanoseconds, not wall-clock timestamps. */
typedef struct ql_budget_limits_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t reserved32;
    uint64_t total_wall_clock_ns;
    uint64_t node_wall_clock_ns;
    uint64_t solver_wall_clock_ns;
    uint64_t memory_bytes;
    uint64_t single_allocation_bytes;
    uint64_t reserved[6];
} ql_budget_limits_v1;

typedef struct ql_budget_usage_v1 {
    size_t struct_size;
    uint32_t abi_version;
    ql_budget_state state;
    uint64_t elapsed_ns;
    uint64_t memory_current_bytes;
    uint64_t memory_peak_bytes;
    uint64_t live_allocation_count;
    uint64_t allocation_count;
    uint64_t deallocation_count;
    uint64_t denied_allocation_count;
    uint64_t scope_count;
    uint64_t reserved[4];
} ql_budget_usage_v1;

QL_API void QL_CALL ql_budget_limits_init(ql_budget_limits_v1 *limits);
QL_API void QL_CALL ql_budget_usage_init(ql_budget_usage_v1 *usage);
QL_API ql_status QL_CALL ql_budget_limits_validate(
    const ql_budget_limits_v1 *limits, ql_error *error);

/* The budget borrows `allocator` for its own bookkeeping and as the backing
   allocator of ql_budget_allocator(). A null allocator selects the default.
   The allocator must outlive the budget. */
QL_API ql_status QL_CALL ql_budget_create(const ql_allocator *allocator,
                                          const ql_budget_limits_v1 *limits,
                                          ql_budget **output, ql_error *error);
QL_API void QL_CALL ql_budget_destroy(ql_budget *budget);

/* Arms the total wall clock and clears the state and the counters. A budget
   that was never started has no elapsed time and no time limit in force. */
QL_API void QL_CALL ql_budget_start(ql_budget *budget);
QL_API void QL_CALL ql_budget_cancel(ql_budget *budget);

/* Accounting allocator. Allocations are refused the moment they would push the
   live total past the memory limit; the refusal depends only on the current
   usage and the requested size, never on host allocator behaviour. Exceeding
   the limit latches QL_BUDGET_STATE_MEMORY_EXHAUSTED, and that state is sticky
   so a run cannot quietly recover, but later in-limit allocations still
   succeed so unwinding code can finish. */
QL_API const ql_allocator *QL_CALL ql_budget_allocator(
    const ql_budget *budget);

QL_API ql_budget_state QL_CALL ql_budget_get_state(const ql_budget *budget);
QL_API const char *QL_CALL ql_budget_state_string(ql_budget_state state);
QL_API ql_status QL_CALL ql_budget_get_usage(const ql_budget *budget,
                                             ql_budget_usage_v1 *usage,
                                             ql_error *error);

/* Samples the clocks, latches an exhausted state if a limit passed, and maps
   it to a status: QL_STATUS_CANCELLED for a time axis or a cancellation,
   QL_STATUS_OUT_OF_MEMORY for the memory axis, QL_STATUS_OK while active. */
QL_API ql_status QL_CALL ql_budget_check(ql_budget *budget, ql_error *error);
QL_API uint32_t QL_CALL ql_budget_is_exhausted(const ql_budget *budget);

/* Cancellation predicate with the ql_solver_is_cancelled_v1 and
   ql_run_context_v1.is_cancelled shape. `state` is a const ql_budget *. */
QL_API uint32_t QL_CALL ql_budget_is_cancelled(const void *state);

/* Scopes carry the per-node and per-solver-call axes. `parent` may be null;
   a solver scope nested in a node scope inherits the node deadline. Scopes
   belong to the thread that opened them and must be ended on that thread. */
QL_API ql_status QL_CALL ql_budget_scope_begin(ql_budget *budget,
                                               ql_budget_scope *parent,
                                               ql_budget_scope_kind kind,
                                               const char *name,
                                               ql_budget_scope **output,
                                               ql_error *error);
QL_API void QL_CALL ql_budget_scope_end(ql_budget_scope *scope);
QL_API ql_status QL_CALL ql_budget_scope_check(ql_budget_scope *scope,
                                               ql_error *error);
QL_API uint32_t QL_CALL ql_budget_scope_is_cancelled(const void *state);
QL_API ql_budget *QL_CALL ql_budget_scope_budget(const ql_budget_scope *scope);

/* Nanoseconds left before the tightest deadline that applies here, or
   UINT64_MAX when no time axis applies. Zero means the deadline has passed.
   ql_budget_remaining_ms rounds up to at least one millisecond while time
   remains, so a caller never converts a live budget into an immediate
   timeout. */
QL_API uint64_t QL_CALL ql_budget_remaining_ns(const ql_budget *budget);
QL_API uint64_t QL_CALL ql_budget_scope_remaining_ns(
    const ql_budget_scope *scope);
QL_API uint64_t QL_CALL ql_budget_scope_remaining_ms(
    const ql_budget_scope *scope);

/* The gate between a budget and a verdict. A run that hit any limit cannot
   report PROVED_*, COUNTEREXAMPLE or BOUNDED_CLEAN: the outcome is rewritten
   to UNKNOWN with QL_OUTCOME_FLAG_BUDGET_EXHAUSTED set and checked_bound
   cleared, and the mapped error status is returned. Rewriting happens even
   when the caller ignores the status. */
QL_API ql_status QL_CALL ql_budget_guard_outcome(const ql_budget *budget,
                                                 ql_outcome_v1 *outcome,
                                                 ql_error *error);

QL_EXTERN_C_END

#endif
