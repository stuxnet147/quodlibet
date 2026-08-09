#include "quodlibet/budget.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "uv.h"

/* Prefix stored in front of every accounted block. The union pads the header
   to the strictest fundamental alignment so the pointer handed back keeps the
   guarantee the caller had from malloc. */
typedef union ql_budget_header {
    struct {
        uint64_t size;
        uint64_t marker;
    } fields;
    max_align_t alignment;
} ql_budget_header;

#define QL_BUDGET_HEADER_MARKER UINT64_C(0x7175646c62676574) /* "qudlbget" */

struct ql_budget {
    ql_allocator base;
    ql_allocator accounted;
    ql_budget_limits_v1 limits;
    uint64_t started_ns;
    uint32_t started;
    _Atomic uint32_t state;
    _Atomic uint64_t memory_current;
    _Atomic uint64_t memory_peak;
    _Atomic uint64_t live_allocations;
    _Atomic uint64_t allocations;
    _Atomic uint64_t deallocations;
    _Atomic uint64_t denied_allocations;
    _Atomic uint64_t scopes;
};

struct ql_budget_scope {
    ql_budget *budget;
    ql_budget_scope *parent;
    ql_budget_scope_kind kind;
    uint64_t started_ns;
    uint64_t limit_ns;
    char name[QL_BUDGET_MAX_SCOPE_NAME + 1u];
};

static const ql_allocator *select_allocator(const ql_allocator *allocator) {
    return ql_allocator_is_valid(allocator) ? allocator
                                            : ql_default_allocator();
}

void QL_CALL ql_budget_limits_init(ql_budget_limits_v1 *limits) {
    if (limits == NULL) {
        return;
    }
    memset(limits, 0, sizeof(*limits));
    limits->struct_size = sizeof(*limits);
    limits->abi_version = QL_BUDGET_ABI_VERSION;
}

void QL_CALL ql_budget_usage_init(ql_budget_usage_v1 *usage) {
    if (usage == NULL) {
        return;
    }
    memset(usage, 0, sizeof(*usage));
    usage->struct_size = sizeof(*usage);
    usage->abi_version = QL_BUDGET_ABI_VERSION;
}

ql_status QL_CALL ql_budget_limits_validate(const ql_budget_limits_v1 *limits,
                                            ql_error *error) {
    if (limits == NULL || limits->struct_size < sizeof(*limits)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "budget limits v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (limits->abi_version != QL_BUDGET_ABI_VERSION) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "unsupported budget ABI version %u", limits->abi_version);
        return QL_STATUS_ABI_MISMATCH;
    }
    if (limits->single_allocation_bytes != QL_BUDGET_UNLIMITED &&
        limits->memory_bytes != QL_BUDGET_UNLIMITED &&
        limits->single_allocation_bytes > limits->memory_bytes) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "single allocation limit %llu exceeds the memory limit %llu",
                     (unsigned long long)limits->single_allocation_bytes,
                     (unsigned long long)limits->memory_bytes);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

const char *QL_CALL ql_budget_state_string(ql_budget_state state) {
    switch (state) {
    case QL_BUDGET_STATE_ACTIVE:
        return "active";
    case QL_BUDGET_STATE_TOTAL_TIME_EXHAUSTED:
        return "total-time-exhausted";
    case QL_BUDGET_STATE_NODE_TIME_EXHAUSTED:
        return "node-time-exhausted";
    case QL_BUDGET_STATE_SOLVER_TIME_EXHAUSTED:
        return "solver-time-exhausted";
    case QL_BUDGET_STATE_MEMORY_EXHAUSTED:
        return "memory-exhausted";
    case QL_BUDGET_STATE_CANCELLED:
        return "cancelled";
    default:
        return "invalid";
    }
}

/* The first exhaustion wins so the reported reason is the one that actually
   stopped the run. */
static void latch_state(ql_budget *budget, ql_budget_state state) {
    uint32_t expected = (uint32_t)QL_BUDGET_STATE_ACTIVE;

    (void)atomic_compare_exchange_strong_explicit(
        &budget->state, &expected, (uint32_t)state, memory_order_acq_rel,
        memory_order_acquire);
}

static ql_budget_state load_state(const ql_budget *budget) {
    const ql_budget *readable = budget;
    return (ql_budget_state)atomic_load_explicit(
        &((ql_budget *)(uintptr_t)readable)->state, memory_order_acquire);
}

static ql_status state_status(ql_budget_state state) {
    switch (state) {
    case QL_BUDGET_STATE_ACTIVE:
        return QL_STATUS_OK;
    case QL_BUDGET_STATE_MEMORY_EXHAUSTED:
        return QL_STATUS_OUT_OF_MEMORY;
    default:
        return QL_STATUS_CANCELLED;
    }
}

static void set_state_error(ql_error *error, ql_budget_state state) {
    ql_error_set(error, state_status(state), "execution budget %s",
                 ql_budget_state_string(state));
}

static uint64_t elapsed_ns(const ql_budget *budget) {
    uint64_t now;

    if (budget->started == 0u) {
        return 0u;
    }
    now = uv_hrtime();
    return now > budget->started_ns ? now - budget->started_ns : 0u;
}

static void account_peak(ql_budget *budget, uint64_t current) {
    uint64_t peak = atomic_load_explicit(&budget->memory_peak,
                                         memory_order_relaxed);

    while (current > peak) {
        if (atomic_compare_exchange_weak_explicit(&budget->memory_peak, &peak,
                                                  current,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            return;
        }
    }
}

/* Reserves `bytes` against the memory limit. Returns zero and latches the
   exhausted state when the reservation would cross the limit. */
static uint32_t reserve_memory(ql_budget *budget, uint64_t bytes) {
    uint64_t limit = budget->limits.memory_bytes;
    uint64_t current =
        atomic_load_explicit(&budget->memory_current, memory_order_relaxed);
    uint64_t updated;

    for (;;) {
        if (bytes > UINT64_MAX - current) {
            atomic_fetch_add_explicit(&budget->denied_allocations, 1u,
                                      memory_order_relaxed);
            latch_state(budget, QL_BUDGET_STATE_MEMORY_EXHAUSTED);
            return 0u;
        }
        updated = current + bytes;
        if (limit != QL_BUDGET_UNLIMITED && updated > limit) {
            atomic_fetch_add_explicit(&budget->denied_allocations, 1u,
                                      memory_order_relaxed);
            latch_state(budget, QL_BUDGET_STATE_MEMORY_EXHAUSTED);
            return 0u;
        }
        if (atomic_compare_exchange_weak_explicit(&budget->memory_current,
                                                  &current, updated,
                                                  memory_order_acq_rel,
                                                  memory_order_relaxed)) {
            account_peak(budget, updated);
            return 1u;
        }
    }
}

static void release_memory(ql_budget *budget, uint64_t bytes) {
    if (bytes == 0u) {
        return;
    }
    (void)atomic_fetch_sub_explicit(&budget->memory_current, bytes,
                                    memory_order_acq_rel);
}

static uint32_t single_allocation_allowed(ql_budget *budget, size_t size) {
    if (budget->limits.single_allocation_bytes == QL_BUDGET_UNLIMITED) {
        return 1u;
    }
    if ((uint64_t)size <= budget->limits.single_allocation_bytes) {
        return 1u;
    }
    atomic_fetch_add_explicit(&budget->denied_allocations, 1u,
                              memory_order_relaxed);
    latch_state(budget, QL_BUDGET_STATE_MEMORY_EXHAUSTED);
    return 0u;
}

static void *QL_CALL budget_allocate(void *user_data, size_t size) {
    ql_budget *budget = user_data;
    size_t payload = size == 0u ? 1u : size;
    size_t total;
    ql_budget_header *header;

    if (payload > SIZE_MAX - sizeof(ql_budget_header)) {
        atomic_fetch_add_explicit(&budget->denied_allocations, 1u,
                                  memory_order_relaxed);
        latch_state(budget, QL_BUDGET_STATE_MEMORY_EXHAUSTED);
        return NULL;
    }
    if (!single_allocation_allowed(budget, payload)) {
        return NULL;
    }
    total = payload + sizeof(ql_budget_header);
    if (!reserve_memory(budget, (uint64_t)total)) {
        return NULL;
    }
    header = budget->base.allocate(budget->base.user_data, total);
    if (header == NULL) {
        release_memory(budget, (uint64_t)total);
        return NULL;
    }
    header->fields.size = (uint64_t)total;
    header->fields.marker = QL_BUDGET_HEADER_MARKER;
    atomic_fetch_add_explicit(&budget->allocations, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&budget->live_allocations, 1u,
                              memory_order_relaxed);
    return (char *)header + sizeof(ql_budget_header);
}

static ql_budget_header *header_of(void *pointer) {
    return (ql_budget_header *)(void *)((char *)pointer -
                                        sizeof(ql_budget_header));
}

static void QL_CALL budget_deallocate(void *user_data, void *pointer) {
    ql_budget *budget = user_data;
    ql_budget_header *header;
    uint64_t total;

    if (pointer == NULL) {
        return;
    }
    header = header_of(pointer);
    if (header->fields.marker != QL_BUDGET_HEADER_MARKER) {
        /* Not ours. Freeing it through the base allocator would corrupt the
           heap, so leave it alone and record the accounting break. */
        latch_state(budget, QL_BUDGET_STATE_MEMORY_EXHAUSTED);
        return;
    }
    total = header->fields.size;
    header->fields.marker = 0u;
    budget->base.deallocate(budget->base.user_data, header);
    release_memory(budget, total);
    atomic_fetch_add_explicit(&budget->deallocations, 1u,
                              memory_order_relaxed);
    (void)atomic_fetch_sub_explicit(&budget->live_allocations, 1u,
                                    memory_order_relaxed);
}

static void *QL_CALL budget_reallocate(void *user_data, void *pointer,
                                       size_t size) {
    ql_budget *budget = user_data;
    size_t payload = size == 0u ? 1u : size;
    size_t total;
    uint64_t old_total;
    ql_budget_header *header;
    ql_budget_header *moved;

    if (pointer == NULL) {
        return budget_allocate(user_data, size);
    }
    header = header_of(pointer);
    if (header->fields.marker != QL_BUDGET_HEADER_MARKER) {
        latch_state(budget, QL_BUDGET_STATE_MEMORY_EXHAUSTED);
        return NULL;
    }
    if (payload > SIZE_MAX - sizeof(ql_budget_header)) {
        atomic_fetch_add_explicit(&budget->denied_allocations, 1u,
                                  memory_order_relaxed);
        latch_state(budget, QL_BUDGET_STATE_MEMORY_EXHAUSTED);
        return NULL;
    }
    if (!single_allocation_allowed(budget, payload)) {
        return NULL;
    }
    old_total = header->fields.size;
    total = payload + sizeof(ql_budget_header);
    if ((uint64_t)total > old_total) {
        /* Reserve the growth before touching the heap so a refusal leaves the
           original block and the accounting untouched. */
        if (!reserve_memory(budget, (uint64_t)total - old_total)) {
            return NULL;
        }
    }
    moved = budget->base.reallocate(budget->base.user_data, header, total);
    if (moved == NULL) {
        if ((uint64_t)total > old_total) {
            release_memory(budget, (uint64_t)total - old_total);
        }
        return NULL;
    }
    if ((uint64_t)total < old_total) {
        release_memory(budget, old_total - (uint64_t)total);
    }
    moved->fields.size = (uint64_t)total;
    moved->fields.marker = QL_BUDGET_HEADER_MARKER;
    atomic_fetch_add_explicit(&budget->allocations, 1u, memory_order_relaxed);
    return (char *)moved + sizeof(ql_budget_header);
}

ql_status QL_CALL ql_budget_create(const ql_allocator *allocator,
                                   const ql_budget_limits_v1 *limits,
                                   ql_budget **output, ql_error *error) {
    const ql_allocator *base = select_allocator(allocator);
    ql_budget *budget;
    ql_status status;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "budget output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = ql_budget_limits_validate(limits, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    budget = base->allocate(base->user_data, sizeof(*budget));
    if (budget == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(budget, 0, sizeof(*budget));
    budget->base = *base;
    budget->limits = *limits;
    budget->limits.struct_size = sizeof(budget->limits);
    budget->accounted.user_data = budget;
    budget->accounted.allocate = budget_allocate;
    budget->accounted.reallocate = budget_reallocate;
    budget->accounted.deallocate = budget_deallocate;
    atomic_store_explicit(&budget->state, (uint32_t)QL_BUDGET_STATE_ACTIVE,
                          memory_order_release);
    *output = budget;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_budget_destroy(ql_budget *budget) {
    ql_allocator base;

    if (budget == NULL) {
        return;
    }
    base = budget->base;
    base.deallocate(base.user_data, budget);
}

void QL_CALL ql_budget_start(ql_budget *budget) {
    if (budget == NULL) {
        return;
    }
    budget->started_ns = uv_hrtime();
    budget->started = 1u;
    atomic_store_explicit(&budget->state, (uint32_t)QL_BUDGET_STATE_ACTIVE,
                          memory_order_release);
    atomic_store_explicit(&budget->memory_peak,
                          atomic_load_explicit(&budget->memory_current,
                                               memory_order_relaxed),
                          memory_order_relaxed);
    atomic_store_explicit(&budget->allocations, 0u, memory_order_relaxed);
    atomic_store_explicit(&budget->deallocations, 0u, memory_order_relaxed);
    atomic_store_explicit(&budget->denied_allocations, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&budget->scopes, 0u, memory_order_relaxed);
}

void QL_CALL ql_budget_cancel(ql_budget *budget) {
    if (budget == NULL) {
        return;
    }
    latch_state(budget, QL_BUDGET_STATE_CANCELLED);
}

const ql_allocator *QL_CALL ql_budget_allocator(const ql_budget *budget) {
    return budget != NULL ? &budget->accounted : NULL;
}

ql_budget_state QL_CALL ql_budget_get_state(const ql_budget *budget) {
    return budget != NULL ? load_state(budget) : QL_BUDGET_STATE_ACTIVE;
}

uint32_t QL_CALL ql_budget_is_exhausted(const ql_budget *budget) {
    return budget != NULL && load_state(budget) != QL_BUDGET_STATE_ACTIVE ? 1u
                                                                          : 0u;
}

ql_status QL_CALL ql_budget_get_usage(const ql_budget *budget,
                                      ql_budget_usage_v1 *usage,
                                      ql_error *error) {
    ql_budget *mutable_budget = (ql_budget *)(uintptr_t)budget;

    if (budget == NULL || usage == NULL ||
        usage->struct_size < sizeof(*usage)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "budget and a budget usage v1 output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    usage->abi_version = QL_BUDGET_ABI_VERSION;
    usage->state = load_state(budget);
    usage->elapsed_ns = elapsed_ns(budget);
    usage->memory_current_bytes = atomic_load_explicit(
        &mutable_budget->memory_current, memory_order_relaxed);
    usage->memory_peak_bytes = atomic_load_explicit(
        &mutable_budget->memory_peak, memory_order_relaxed);
    usage->live_allocation_count = atomic_load_explicit(
        &mutable_budget->live_allocations, memory_order_relaxed);
    usage->allocation_count = atomic_load_explicit(
        &mutable_budget->allocations, memory_order_relaxed);
    usage->deallocation_count = atomic_load_explicit(
        &mutable_budget->deallocations, memory_order_relaxed);
    usage->denied_allocation_count = atomic_load_explicit(
        &mutable_budget->denied_allocations, memory_order_relaxed);
    usage->scope_count =
        atomic_load_explicit(&mutable_budget->scopes, memory_order_relaxed);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_budget_check(ql_budget *budget, ql_error *error) {
    ql_budget_state state;

    if (budget == NULL) {
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    state = load_state(budget);
    if (state == QL_BUDGET_STATE_ACTIVE &&
        budget->limits.total_wall_clock_ns != QL_BUDGET_UNLIMITED &&
        budget->started != 0u &&
        elapsed_ns(budget) >= budget->limits.total_wall_clock_ns) {
        latch_state(budget, QL_BUDGET_STATE_TOTAL_TIME_EXHAUSTED);
        state = load_state(budget);
    }
    if (state == QL_BUDGET_STATE_ACTIVE) {
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    set_state_error(error, state);
    return state_status(state);
}

uint32_t QL_CALL ql_budget_is_cancelled(const void *state) {
    const ql_budget *budget = state;

    if (budget == NULL) {
        return 0u;
    }
    (void)ql_budget_check((ql_budget *)(uintptr_t)budget, NULL);
    return ql_budget_is_exhausted(budget);
}

uint64_t QL_CALL ql_budget_remaining_ns(const ql_budget *budget) {
    uint64_t elapsed;

    if (budget == NULL ||
        budget->limits.total_wall_clock_ns == QL_BUDGET_UNLIMITED ||
        budget->started == 0u) {
        return UINT64_MAX;
    }
    if (load_state(budget) != QL_BUDGET_STATE_ACTIVE) {
        return 0u;
    }
    elapsed = elapsed_ns(budget);
    return elapsed >= budget->limits.total_wall_clock_ns
               ? 0u
               : budget->limits.total_wall_clock_ns - elapsed;
}

static uint64_t scope_limit_for(const ql_budget *budget,
                                ql_budget_scope_kind kind) {
    return kind == QL_BUDGET_SCOPE_SOLVER
               ? budget->limits.solver_wall_clock_ns
               : budget->limits.node_wall_clock_ns;
}

ql_status QL_CALL ql_budget_scope_begin(ql_budget *budget,
                                        ql_budget_scope *parent,
                                        ql_budget_scope_kind kind,
                                        const char *name,
                                        ql_budget_scope **output,
                                        ql_error *error) {
    ql_budget_scope *scope;
    size_t length;
    ql_status status;

    if (budget == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "budget and a scope output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (kind != QL_BUDGET_SCOPE_NODE && kind != QL_BUDGET_SCOPE_SOLVER) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "budget scope kind %d is out of range", (int)kind);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (parent != NULL && parent->budget != budget) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "parent scope belongs to a different budget");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    /* A run that already hit a limit does not get to open more work. */
    status = ql_budget_check(budget, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    scope = budget->base.allocate(budget->base.user_data, sizeof(*scope));
    if (scope == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(scope, 0, sizeof(*scope));
    scope->budget = budget;
    scope->parent = parent;
    scope->kind = kind;
    scope->started_ns = uv_hrtime();
    scope->limit_ns = scope_limit_for(budget, kind);
    if (name != NULL) {
        length = strlen(name);
        if (length > QL_BUDGET_MAX_SCOPE_NAME) {
            length = QL_BUDGET_MAX_SCOPE_NAME;
        }
        memcpy(scope->name, name, length);
        scope->name[length] = '\0';
    }
    atomic_fetch_add_explicit(&budget->scopes, 1u, memory_order_relaxed);
    *output = scope;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_budget_scope_end(ql_budget_scope *scope) {
    ql_budget *budget;

    if (scope == NULL) {
        return;
    }
    budget = scope->budget;
    budget->base.deallocate(budget->base.user_data, scope);
}

ql_budget *QL_CALL ql_budget_scope_budget(const ql_budget_scope *scope) {
    return scope != NULL ? scope->budget : NULL;
}

static ql_budget_state scope_expired_state(const ql_budget_scope *scope) {
    uint64_t now;

    if (scope->limit_ns == QL_BUDGET_UNLIMITED) {
        return QL_BUDGET_STATE_ACTIVE;
    }
    now = uv_hrtime();
    if (now <= scope->started_ns ||
        now - scope->started_ns < scope->limit_ns) {
        return QL_BUDGET_STATE_ACTIVE;
    }
    return scope->kind == QL_BUDGET_SCOPE_SOLVER
               ? QL_BUDGET_STATE_SOLVER_TIME_EXHAUSTED
               : QL_BUDGET_STATE_NODE_TIME_EXHAUSTED;
}

ql_status QL_CALL ql_budget_scope_check(ql_budget_scope *scope,
                                        ql_error *error) {
    const ql_budget_scope *cursor;
    ql_budget_state expired;
    ql_status status;

    if (scope == NULL) {
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    status = ql_budget_check(scope->budget, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (cursor = scope; cursor != NULL; cursor = cursor->parent) {
        expired = scope_expired_state(cursor);
        if (expired != QL_BUDGET_STATE_ACTIVE) {
            latch_state(scope->budget, expired);
            set_state_error(error, load_state(scope->budget));
            return state_status(load_state(scope->budget));
        }
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

uint32_t QL_CALL ql_budget_scope_is_cancelled(const void *state) {
    ql_budget_scope *scope = (ql_budget_scope *)(uintptr_t)state;

    if (scope == NULL) {
        return 0u;
    }
    (void)ql_budget_scope_check(scope, NULL);
    return ql_budget_is_exhausted(scope->budget);
}

static uint64_t scope_axis_remaining(const ql_budget_scope *scope) {
    uint64_t now;
    uint64_t used;

    if (scope->limit_ns == QL_BUDGET_UNLIMITED) {
        return UINT64_MAX;
    }
    now = uv_hrtime();
    used = now > scope->started_ns ? now - scope->started_ns : 0u;
    return used >= scope->limit_ns ? 0u : scope->limit_ns - used;
}

uint64_t QL_CALL ql_budget_scope_remaining_ns(const ql_budget_scope *scope) {
    const ql_budget_scope *cursor;
    uint64_t remaining;
    uint64_t candidate;

    if (scope == NULL) {
        return UINT64_MAX;
    }
    remaining = ql_budget_remaining_ns(scope->budget);
    for (cursor = scope; cursor != NULL; cursor = cursor->parent) {
        candidate = scope_axis_remaining(cursor);
        if (candidate < remaining) {
            remaining = candidate;
        }
    }
    return remaining;
}

uint64_t QL_CALL ql_budget_scope_remaining_ms(const ql_budget_scope *scope) {
    uint64_t remaining = ql_budget_scope_remaining_ns(scope);

    if (remaining == UINT64_MAX) {
        return 0u;
    }
    if (remaining == 0u) {
        return 0u;
    }
    /* Round up. Truncating a live sub-millisecond budget to zero would read as
       "no limit" to a backend that treats zero as unset. */
    return (remaining + UINT64_C(999999)) / UINT64_C(1000000);
}

ql_status QL_CALL ql_budget_guard_outcome(const ql_budget *budget,
                                          ql_outcome_v1 *outcome,
                                          ql_error *error) {
    ql_budget_state state;

    if (outcome == NULL || outcome->struct_size < sizeof(*outcome)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "outcome v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (budget == NULL) {
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    (void)ql_budget_check((ql_budget *)(uintptr_t)budget, NULL);
    state = load_state(budget);
    if (state == QL_BUDGET_STATE_ACTIVE) {
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    /* A run that hit a limit observed only part of the problem. Every verdict
       that claims something about the whole problem, including BOUNDED_CLEAN,
       is withdrawn. */
    outcome->verdict = QL_VERDICT_UNKNOWN;
    outcome->checked_bound = 0u;
    outcome->flags |= QL_OUTCOME_FLAG_BUDGET_EXHAUSTED;
    set_state_error(error, state);
    return state_status(state);
}
