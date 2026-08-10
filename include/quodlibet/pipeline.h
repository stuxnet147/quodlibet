#ifndef QUODLIBET_PIPELINE_H
#define QUODLIBET_PIPELINE_H

#include "quodlibet/budget.h"
#include "quodlibet/registry.h"
#include "quodlibet/scheduler.h"

QL_EXTERN_C_BEGIN

typedef uint32_t ql_node_id;
#define QL_INVALID_NODE_ID UINT32_MAX

typedef struct ql_pipeline ql_pipeline;
typedef struct ql_pipeline_result ql_pipeline_result;

QL_API ql_status QL_CALL ql_pipeline_create(
    ql_registry *registry, const ql_allocator *allocator, ql_pipeline **output,
    ql_error *error);
QL_API void QL_CALL ql_pipeline_destroy(ql_pipeline *pipeline);
QL_API ql_status QL_CALL ql_pipeline_add_node(
    ql_pipeline *pipeline, const char *name, const char *method_name,
    const char *options_json, const ql_node_id *dependencies,
    size_t dependency_count, ql_node_id *node_id, ql_error *error);
QL_API ql_node_id QL_CALL ql_pipeline_find_node(const ql_pipeline *pipeline,
                                               const char *name);
QL_API ql_status QL_CALL ql_pipeline_compile(ql_pipeline *pipeline,
                                             ql_error *error);
QL_API ql_status QL_CALL ql_pipeline_from_json(
    ql_registry *registry, const ql_allocator *allocator, const char *json,
    size_t json_size, ql_pipeline **output, ql_error *error);

/* Lend a solver session to every node this pipeline runs, so methods that
   would each establish a backend share one the caller already established.
   Typed as void* because pipeline.h does not depend on solver.h; pass a
   ql_solver_session*. Null clears it.

   The session is borrowed and must outlive the runs that use it. A session is
   not thread safe, so a pipeline carrying one must not be run concurrently
   from more than one thread: one session, one pipeline, one worker. */
QL_API ql_status QL_CALL ql_pipeline_set_solver_session(
    ql_pipeline *pipeline, void *session, ql_error *error);

QL_API ql_status QL_CALL ql_pipeline_run(
    ql_pipeline *pipeline, ql_scheduler *scheduler, ql_artifact *input,
    const ql_cancel_token *cancel_token, ql_pipeline_result **output,
    ql_error *error);

/* Same run, under an execution budget. Each node runs inside its own
   QL_BUDGET_SCOPE_NODE scope and sees the budget through the run context's
   cancellation predicate. When any axis is exhausted the run stops, releases
   every artifact produced so far, and returns QL_STATUS_CANCELLED for a time
   axis or QL_STATUS_OUT_OF_MEMORY for the memory axis; no partial result is
   handed back. A null budget behaves exactly like ql_pipeline_run. */
QL_API ql_status QL_CALL ql_pipeline_run_with_budget(
    ql_pipeline *pipeline, ql_scheduler *scheduler, ql_artifact *input,
    const ql_cancel_token *cancel_token, ql_budget *budget,
    ql_pipeline_result **output, ql_error *error);
QL_API void QL_CALL ql_pipeline_result_destroy(ql_pipeline_result *result);
QL_API size_t QL_CALL ql_pipeline_result_count(
    const ql_pipeline_result *result);
QL_API const char *QL_CALL ql_pipeline_result_node_name(
    const ql_pipeline_result *result, size_t index);
QL_API const ql_artifact *QL_CALL ql_pipeline_result_artifact(
    const ql_pipeline_result *result, size_t index);

QL_EXTERN_C_END

#endif
