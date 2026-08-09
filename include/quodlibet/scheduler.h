#ifndef QUODLIBET_SCHEDULER_H
#define QUODLIBET_SCHEDULER_H

#include "quodlibet/allocator.h"
#include "quodlibet/status.h"

QL_EXTERN_C_BEGIN

typedef struct ql_scheduler ql_scheduler;
typedef struct ql_task_group ql_task_group;
typedef struct ql_cancel_token ql_cancel_token;

typedef ql_status (QL_CALL *ql_task_fn)(void *user_data, ql_error *error);

QL_API ql_status QL_CALL ql_scheduler_create(
    const ql_allocator *allocator, size_t worker_count, ql_scheduler **output,
    ql_error *error);
QL_API void QL_CALL ql_scheduler_destroy(ql_scheduler *scheduler);
QL_API size_t QL_CALL ql_scheduler_worker_count(
    const ql_scheduler *scheduler);

QL_API ql_status QL_CALL ql_task_group_create(
    const ql_allocator *allocator, ql_task_group **output, ql_error *error);
QL_API void QL_CALL ql_task_group_destroy(ql_task_group *group);
QL_API ql_status QL_CALL ql_scheduler_submit(
    ql_scheduler *scheduler, ql_task_group *group, ql_task_fn function,
    void *user_data, ql_error *error);
QL_API ql_status QL_CALL ql_task_group_wait(ql_task_group *group,
                                           ql_error *error);

QL_API ql_status QL_CALL ql_cancel_token_create(
    const ql_allocator *allocator, ql_cancel_token **output, ql_error *error);
QL_API void QL_CALL ql_cancel_token_destroy(ql_cancel_token *token);
QL_API void QL_CALL ql_cancel_token_request(ql_cancel_token *token);
QL_API uint32_t QL_CALL ql_cancel_token_is_requested(
    const ql_cancel_token *token);

QL_EXTERN_C_END

#endif
