#ifndef QUODLIBET_METHOD_H
#define QUODLIBET_METHOD_H

#include "quodlibet/artifact.h"
#include "quodlibet/log.h"

QL_EXTERN_C_BEGIN

/* ql_log_level now lives in quodlibet/log.h so that the plugin host callback
   and the public logging service share one scale. The numeric values of these
   original spellings are unchanged. */
#define QL_LOG_TRACE QL_LOG_LEVEL_TRACE
#define QL_LOG_DEBUG QL_LOG_LEVEL_DEBUG
#define QL_LOG_INFO QL_LOG_LEVEL_INFO
#define QL_LOG_WARNING QL_LOG_LEVEL_WARN
#define QL_LOG_ERROR QL_LOG_LEVEL_ERROR

typedef struct ql_host_v1 {
    size_t struct_size;
    uint32_t abi_version;
    ql_allocator allocator;
    ql_status (QL_CALL *artifact_create)(
        const ql_allocator *allocator, const char *kind,
        uint32_t schema_version, const void *data, size_t size,
        ql_artifact **output, ql_error *error);
    void (QL_CALL *artifact_retain)(ql_artifact *artifact);
    void (QL_CALL *artifact_release)(ql_artifact *artifact);
    void (QL_CALL *log)(ql_log_level level, const char *component,
                        const char *message);
    void *reserved[8];
} ql_host_v1;

typedef struct ql_run_context_v1 {
    size_t struct_size;
    uint32_t abi_version;
    const ql_host_v1 *host;
    const void *cancel_state;
    uint32_t (QL_CALL *is_cancelled)(const void *cancel_state);
    uint64_t run_id;
    /* Appended in place of one reserved slot, so the structure is the same
       size and no existing field moved. A solver session the caller already
       established, for methods that would otherwise build one per run; null
       means build one. Typed as void* because method.h does not depend on
       solver.h; methods that use it cast to ql_solver_session*. Read it only
       when struct_size reaches past it. */
    void *solver_session;
    /* Nanoseconds left before the tightest deadline that applies to this run,
       read through the same state `is_cancelled` uses. UINT64_MAX when no time
       axis applies; zero when the deadline has already passed.

       `is_cancelled` alone tells a method only that time is up, which it can
       act on no sooner than its next poll. A method that hands work to
       something it cannot poll -- a solver subprocess, say -- needs the
       remaining time in advance so it can pass a deadline down instead of
       discovering the overrun afterwards. Without it a one-millisecond budget
       and a hundred-millisecond budget cost the same, because both are only
       noticed when the stage ends (docs/perf/concurrency.md).

       Appended in place of one reserved slot: same structure size, no existing
       field moved. Null when the host supplies no clock. Read it only when
       struct_size reaches past it. */
    uint64_t (QL_CALL *remaining_ns)(const void *cancel_state);
    void *reserved[6];
} ql_run_context_v1;

typedef enum ql_method_flag {
    QL_METHOD_DETERMINISTIC = UINT64_C(1) << 0,
    QL_METHOD_PROOF_PRODUCER = UINT64_C(1) << 1,
    QL_METHOD_COUNTEREXAMPLE_PRODUCER = UINT64_C(1) << 2,
    QL_METHOD_CACHEABLE = UINT64_C(1) << 3
} ql_method_flag;

typedef struct ql_method_v1 {
    size_t struct_size;
    uint32_t abi_version;
    const char *name;
    const char *description;
    const char *output_kind;
    uint64_t flags;
    size_t minimum_inputs;
    size_t maximum_inputs;
    ql_status (QL_CALL *create)(const ql_host_v1 *host,
                                const char *options_json, void **instance,
                                ql_error *error);
    ql_status (QL_CALL *validate)(void *instance,
                                  ql_artifact *const *inputs,
                                  size_t input_count, ql_error *error);
    ql_status (QL_CALL *run)(void *instance,
                             const ql_run_context_v1 *context,
                             ql_artifact *const *inputs,
                             size_t input_count, ql_artifact **output,
                             ql_error *error);
    void (QL_CALL *destroy)(void *instance);
    void *reserved[8];
} ql_method_v1;

QL_API const ql_host_v1 *QL_CALL ql_default_host(void);

QL_EXTERN_C_END

#endif
