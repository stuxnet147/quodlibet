#include "quodlibet/scheduler.h"

#include <stdatomic.h>
#include <string.h>

#include "uv.h"

typedef struct ql_task {
    ql_task_fn function;
    void *user_data;
    struct ql_task_group *group;
    struct ql_task *next;
} ql_task;

struct ql_task_group {
    ql_allocator allocator;
    uv_mutex_t mutex;
    uv_cond_t condition;
    size_t pending;
    ql_status status;
    ql_error first_error;
};

struct ql_scheduler {
    ql_allocator allocator;
    uv_mutex_t mutex;
    uv_cond_t condition;
    uv_thread_t *workers;
    size_t worker_count;
    ql_task *head;
    ql_task *tail;
    uint32_t stopping;
};

struct ql_cancel_token {
    ql_allocator allocator;
    atomic_uint requested;
};

static void finish_task(ql_task_group *group, ql_status status,
                        const ql_error *error) {
    uv_mutex_lock(&group->mutex);
    if (status != QL_STATUS_OK && group->status == QL_STATUS_OK) {
        group->status = status;
        if (error != NULL && error->code != QL_STATUS_OK) {
            group->first_error = *error;
        } else {
            ql_error_set(&group->first_error, status, NULL);
        }
    }
    --group->pending;
    if (group->pending == 0u) {
        uv_cond_broadcast(&group->condition);
    }
    uv_mutex_unlock(&group->mutex);
}

static void worker_main(void *user_data) {
    ql_scheduler *scheduler = user_data;

    for (;;) {
        ql_task *task;
        ql_error error;
        ql_status status;

        uv_mutex_lock(&scheduler->mutex);
        while (scheduler->head == NULL && !scheduler->stopping) {
            uv_cond_wait(&scheduler->condition, &scheduler->mutex);
        }
        if (scheduler->head == NULL && scheduler->stopping) {
            uv_mutex_unlock(&scheduler->mutex);
            return;
        }
        task = scheduler->head;
        scheduler->head = task->next;
        if (scheduler->head == NULL) {
            scheduler->tail = NULL;
        }
        uv_mutex_unlock(&scheduler->mutex);

        ql_error_clear(&error);
        status = task->function(task->user_data, &error);
        finish_task(task->group, status, &error);
        scheduler->allocator.deallocate(scheduler->allocator.user_data, task);
    }
}

ql_status QL_CALL ql_scheduler_create(
    const ql_allocator *allocator, size_t worker_count, ql_scheduler **output,
    ql_error *error) {
    const ql_allocator *selected = allocator;
    ql_scheduler *scheduler;
    size_t started = 0u;
    int uv_status;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "scheduler output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (selected == NULL) {
        selected = ql_default_allocator();
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (worker_count == 0u) {
        worker_count = (size_t)uv_available_parallelism();
        if (worker_count == 0u) {
            worker_count = 1u;
        }
    }
    if (worker_count > SIZE_MAX / sizeof(uv_thread_t)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "worker count is too large");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    scheduler = selected->allocate(selected->user_data, sizeof(*scheduler));
    if (scheduler == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->allocator = *selected;
    scheduler->worker_count = worker_count;
    scheduler->workers = selected->allocate(
        selected->user_data, worker_count * sizeof(*scheduler->workers));
    if (scheduler->workers == NULL) {
        selected->deallocate(selected->user_data, scheduler);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    uv_status = uv_mutex_init(&scheduler->mutex);
    if (uv_status != 0) {
        selected->deallocate(selected->user_data, scheduler->workers);
        selected->deallocate(selected->user_data, scheduler);
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not initialize scheduler mutex: %s",
                     uv_strerror(uv_status));
        return QL_STATUS_INTERNAL_ERROR;
    }
    uv_status = uv_cond_init(&scheduler->condition);
    if (uv_status != 0) {
        uv_mutex_destroy(&scheduler->mutex);
        selected->deallocate(selected->user_data, scheduler->workers);
        selected->deallocate(selected->user_data, scheduler);
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not initialize scheduler condition: %s",
                     uv_strerror(uv_status));
        return QL_STATUS_INTERNAL_ERROR;
    }
    while (started < worker_count) {
        uv_status = uv_thread_create(&scheduler->workers[started], worker_main,
                                     scheduler);
        if (uv_status != 0) {
            size_t index;
            uv_mutex_lock(&scheduler->mutex);
            scheduler->stopping = 1u;
            uv_cond_broadcast(&scheduler->condition);
            uv_mutex_unlock(&scheduler->mutex);
            for (index = 0u; index < started; ++index) {
                (void)uv_thread_join(&scheduler->workers[index]);
            }
            uv_cond_destroy(&scheduler->condition);
            uv_mutex_destroy(&scheduler->mutex);
            selected->deallocate(selected->user_data, scheduler->workers);
            selected->deallocate(selected->user_data, scheduler);
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "could not create worker thread: %s",
                         uv_strerror(uv_status));
            return QL_STATUS_INTERNAL_ERROR;
        }
        ++started;
    }
    *output = scheduler;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_scheduler_destroy(ql_scheduler *scheduler) {
    size_t index;
    ql_allocator allocator;

    if (scheduler == NULL) {
        return;
    }
    allocator = scheduler->allocator;
    uv_mutex_lock(&scheduler->mutex);
    scheduler->stopping = 1u;
    uv_cond_broadcast(&scheduler->condition);
    uv_mutex_unlock(&scheduler->mutex);
    for (index = 0u; index < scheduler->worker_count; ++index) {
        (void)uv_thread_join(&scheduler->workers[index]);
    }
    uv_cond_destroy(&scheduler->condition);
    uv_mutex_destroy(&scheduler->mutex);
    allocator.deallocate(allocator.user_data, scheduler->workers);
    allocator.deallocate(allocator.user_data, scheduler);
}

size_t QL_CALL ql_scheduler_worker_count(const ql_scheduler *scheduler) {
    return scheduler != NULL ? scheduler->worker_count : 0u;
}

ql_status QL_CALL ql_task_group_create(
    const ql_allocator *allocator, ql_task_group **output, ql_error *error) {
    const ql_allocator *selected = allocator;
    ql_task_group *group;
    int uv_status;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "task group output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (selected == NULL) {
        selected = ql_default_allocator();
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    group = selected->allocate(selected->user_data, sizeof(*group));
    if (group == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(group, 0, sizeof(*group));
    group->allocator = *selected;
    group->status = QL_STATUS_OK;
    ql_error_clear(&group->first_error);
    uv_status = uv_mutex_init(&group->mutex);
    if (uv_status != 0) {
        selected->deallocate(selected->user_data, group);
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not initialize task group mutex: %s",
                     uv_strerror(uv_status));
        return QL_STATUS_INTERNAL_ERROR;
    }
    uv_status = uv_cond_init(&group->condition);
    if (uv_status != 0) {
        uv_mutex_destroy(&group->mutex);
        selected->deallocate(selected->user_data, group);
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not initialize task group condition: %s",
                     uv_strerror(uv_status));
        return QL_STATUS_INTERNAL_ERROR;
    }
    *output = group;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_task_group_destroy(ql_task_group *group) {
    ql_allocator allocator;

    if (group == NULL) {
        return;
    }
    (void)ql_task_group_wait(group, NULL);
    allocator = group->allocator;
    uv_cond_destroy(&group->condition);
    uv_mutex_destroy(&group->mutex);
    allocator.deallocate(allocator.user_data, group);
}

ql_status QL_CALL ql_scheduler_submit(
    ql_scheduler *scheduler, ql_task_group *group, ql_task_fn function,
    void *user_data, ql_error *error) {
    ql_task *task;

    if (scheduler == NULL || group == NULL || function == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "scheduler, task group, and function are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    task = scheduler->allocator.allocate(scheduler->allocator.user_data,
                                         sizeof(*task));
    if (task == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    task->function = function;
    task->user_data = user_data;
    task->group = group;
    task->next = NULL;

    uv_mutex_lock(&scheduler->mutex);
    if (scheduler->stopping) {
        uv_mutex_unlock(&scheduler->mutex);
        scheduler->allocator.deallocate(scheduler->allocator.user_data, task);
        ql_error_set(error, QL_STATUS_CANCELLED,
                     "scheduler is stopping");
        return QL_STATUS_CANCELLED;
    }
    uv_mutex_lock(&group->mutex);
    ++group->pending;
    uv_mutex_unlock(&group->mutex);
    if (scheduler->tail != NULL) {
        scheduler->tail->next = task;
    } else {
        scheduler->head = task;
    }
    scheduler->tail = task;
    uv_cond_signal(&scheduler->condition);
    uv_mutex_unlock(&scheduler->mutex);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_task_group_wait(ql_task_group *group, ql_error *error) {
    ql_status status;

    if (group == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "task group is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    uv_mutex_lock(&group->mutex);
    while (group->pending != 0u) {
        uv_cond_wait(&group->condition, &group->mutex);
    }
    status = group->status;
    if (error != NULL) {
        *error = group->first_error;
    }
    uv_mutex_unlock(&group->mutex);
    return status;
}

ql_status QL_CALL ql_cancel_token_create(
    const ql_allocator *allocator, ql_cancel_token **output, ql_error *error) {
    const ql_allocator *selected = allocator;
    ql_cancel_token *token;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "cancellation token output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (selected == NULL) {
        selected = ql_default_allocator();
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    token = selected->allocate(selected->user_data, sizeof(*token));
    if (token == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    token->allocator = *selected;
    atomic_init(&token->requested, 0u);
    *output = token;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_cancel_token_destroy(ql_cancel_token *token) {
    ql_allocator allocator;

    if (token == NULL) {
        return;
    }
    allocator = token->allocator;
    allocator.deallocate(allocator.user_data, token);
}

void QL_CALL ql_cancel_token_request(ql_cancel_token *token) {
    if (token != NULL) {
        atomic_store_explicit(&token->requested, 1u, memory_order_release);
    }
}

uint32_t QL_CALL ql_cancel_token_is_requested(const ql_cancel_token *token) {
    return token != NULL
               ? atomic_load_explicit(&token->requested, memory_order_acquire)
               : 0u;
}
