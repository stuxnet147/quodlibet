#include "quodlibet/pipeline.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "yyjson.h"

typedef struct ql_pipeline_node {
    char *name;
    char *method_name;
    char *options_json;
    ql_node_id *dependencies;
    size_t dependency_count;
    const ql_method_v1 *method;
    uint32_t level;
    uint32_t is_sink;
} ql_pipeline_node;

struct ql_pipeline {
    ql_allocator allocator;
    ql_registry *registry;
    ql_pipeline_node *nodes;
    size_t count;
    size_t capacity;
    uint32_t compiled;
    uint32_t maximum_level;
    uint64_t registry_generation;
};

typedef struct ql_pipeline_result_item {
    char *node_name;
    ql_artifact *artifact;
} ql_pipeline_result_item;

struct ql_pipeline_result {
    ql_allocator allocator;
    ql_pipeline_result_item *items;
    size_t count;
};

typedef struct ql_pipeline_task {
    ql_pipeline *pipeline;
    ql_artifact **node_outputs;
    ql_artifact *initial_input;
    const ql_cancel_token *cancel_token;
    ql_budget *budget;
    size_t node_index;
    uint64_t run_id;
    ql_status status;
    ql_error error;
} ql_pipeline_task;

/* The run context carries one cancellation predicate, so the token and the
   budget are presented to a method through a single state. */
typedef struct ql_pipeline_cancel_state {
    const ql_cancel_token *token;
    ql_budget_scope *scope;
} ql_pipeline_cancel_state;

static atomic_uint_fast64_t next_run_id = UINT64_C(1);

static void destroy_node(ql_pipeline *pipeline, ql_pipeline_node *node) {
    pipeline->allocator.deallocate(pipeline->allocator.user_data,
                                   node->dependencies);
    pipeline->allocator.deallocate(pipeline->allocator.user_data,
                                   node->options_json);
    pipeline->allocator.deallocate(pipeline->allocator.user_data,
                                   node->method_name);
    pipeline->allocator.deallocate(pipeline->allocator.user_data, node->name);
    memset(node, 0, sizeof(*node));
}

ql_status QL_CALL ql_pipeline_create(
    ql_registry *registry, const ql_allocator *allocator, ql_pipeline **output,
    ql_error *error) {
    const ql_allocator *selected = allocator;
    ql_pipeline *pipeline;

    if (registry == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "registry and pipeline output are required");
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
    pipeline = selected->allocate(selected->user_data, sizeof(*pipeline));
    if (pipeline == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->allocator = *selected;
    pipeline->registry = registry;
    *output = pipeline;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_pipeline_destroy(ql_pipeline *pipeline) {
    size_t index;
    ql_allocator allocator;

    if (pipeline == NULL) {
        return;
    }
    allocator = pipeline->allocator;
    for (index = 0u; index < pipeline->count; ++index) {
        destroy_node(pipeline, &pipeline->nodes[index]);
    }
    allocator.deallocate(allocator.user_data, pipeline->nodes);
    allocator.deallocate(allocator.user_data, pipeline);
}

ql_node_id QL_CALL ql_pipeline_find_node(const ql_pipeline *pipeline,
                                         const char *name) {
    size_t index;

    if (pipeline == NULL || name == NULL) {
        return QL_INVALID_NODE_ID;
    }
    for (index = 0u; index < pipeline->count; ++index) {
        if (strcmp(pipeline->nodes[index].name, name) == 0) {
            return (ql_node_id)index;
        }
    }
    return QL_INVALID_NODE_ID;
}

ql_status QL_CALL ql_pipeline_add_node(
    ql_pipeline *pipeline, const char *name, const char *method_name,
    const char *options_json, const ql_node_id *dependencies,
    size_t dependency_count, ql_node_id *node_id, ql_error *error) {
    ql_pipeline_node node;
    ql_pipeline_node *nodes;
    size_t capacity;

    if (pipeline == NULL || name == NULL || name[0] == '\0' ||
        method_name == NULL || method_name[0] == '\0' ||
        (dependency_count != 0u && dependencies == NULL) ||
        pipeline->count >= UINT32_MAX) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "pipeline node requires valid names and dependencies");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (ql_pipeline_find_node(pipeline, name) != QL_INVALID_NODE_ID) {
        ql_error_set(error, QL_STATUS_ALREADY_EXISTS,
                     "pipeline node '%s' already exists", name);
        return QL_STATUS_ALREADY_EXISTS;
    }
    memset(&node, 0, sizeof(node));
    node.name = ql_internal_strdup(&pipeline->allocator, name);
    node.method_name = ql_internal_strdup(&pipeline->allocator, method_name);
    node.options_json = ql_internal_strdup(
        &pipeline->allocator, options_json != NULL ? options_json : "{}");
    if (node.name == NULL || node.method_name == NULL ||
        node.options_json == NULL) {
        destroy_node(pipeline, &node);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    if (dependency_count != 0u) {
        if (dependency_count > SIZE_MAX / sizeof(*node.dependencies)) {
            destroy_node(pipeline, &node);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                         "dependency count overflow");
            return QL_STATUS_OUT_OF_MEMORY;
        }
        node.dependencies = pipeline->allocator.allocate(
            pipeline->allocator.user_data,
            dependency_count * sizeof(*node.dependencies));
        if (node.dependencies == NULL) {
            destroy_node(pipeline, &node);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        memcpy(node.dependencies, dependencies,
               dependency_count * sizeof(*node.dependencies));
        node.dependency_count = dependency_count;
    }
    if (pipeline->count == pipeline->capacity) {
        capacity = pipeline->capacity == 0u ? 8u : pipeline->capacity * 2u;
        if (capacity < pipeline->capacity ||
            capacity > SIZE_MAX / sizeof(*nodes)) {
            destroy_node(pipeline, &node);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                         "pipeline capacity overflow");
            return QL_STATUS_OUT_OF_MEMORY;
        }
        nodes = pipeline->allocator.reallocate(
            pipeline->allocator.user_data, pipeline->nodes,
            capacity * sizeof(*nodes));
        if (nodes == NULL) {
            destroy_node(pipeline, &node);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        pipeline->nodes = nodes;
        pipeline->capacity = capacity;
    }
    pipeline->nodes[pipeline->count] = node;
    if (node_id != NULL) {
        *node_id = (ql_node_id)pipeline->count;
    }
    ++pipeline->count;
    pipeline->compiled = 0u;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_pipeline_compile(ql_pipeline *pipeline,
                                      ql_error *error) {
    size_t *indegrees = NULL;
    ql_node_id *queue = NULL;
    size_t queue_read = 0u;
    size_t queue_write = 0u;
    size_t index;
    size_t processed = 0u;
    ql_status status = QL_STATUS_OK;

    if (pipeline == NULL || pipeline->count == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a non-empty pipeline is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    pipeline->compiled = 0u;
    pipeline->maximum_level = 0u;
    indegrees = pipeline->allocator.allocate(
        pipeline->allocator.user_data, pipeline->count * sizeof(*indegrees));
    queue = pipeline->allocator.allocate(
        pipeline->allocator.user_data, pipeline->count * sizeof(*queue));
    if (indegrees == NULL || queue == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        status = QL_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }

    for (index = 0u; index < pipeline->count; ++index) {
        pipeline->nodes[index].is_sink = 1u;
    }

    for (index = 0u; index < pipeline->count; ++index) {
        ql_pipeline_node *node = &pipeline->nodes[index];
        size_t dependency_index;
        size_t effective_inputs = node->dependency_count == 0u
                                      ? 1u
                                      : node->dependency_count;

        node->method = ql_registry_find(pipeline->registry, node->method_name);
        node->level = 0u;
        if (node->method == NULL) {
            ql_error_set(error, QL_STATUS_NOT_FOUND,
                         "node '%s' references unknown method '%s'", node->name,
                         node->method_name);
            status = QL_STATUS_NOT_FOUND;
            goto cleanup;
        }
        if (effective_inputs < node->method->minimum_inputs ||
            effective_inputs > node->method->maximum_inputs) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "node '%s' gives method '%s' %zu inputs; expected %zu through %zu",
                         node->name, node->method_name, effective_inputs,
                         node->method->minimum_inputs,
                         node->method->maximum_inputs);
            status = QL_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        for (dependency_index = 0u;
             dependency_index < node->dependency_count; ++dependency_index) {
            ql_node_id dependency = node->dependencies[dependency_index];
            if ((size_t)dependency >= pipeline->count) {
                ql_error_set(error, QL_STATUS_NOT_FOUND,
                             "node '%s' has missing dependency id %u",
                             node->name, dependency);
                status = QL_STATUS_NOT_FOUND;
                goto cleanup;
            }
            if ((size_t)dependency == index) {
                ql_error_set(error, QL_STATUS_CYCLE,
                             "node '%s' depends on itself", node->name);
                status = QL_STATUS_CYCLE;
                goto cleanup;
            }
            pipeline->nodes[dependency].is_sink = 0u;
        }
        indegrees[index] = node->dependency_count;
        if (indegrees[index] == 0u) {
            queue[queue_write++] = (ql_node_id)index;
        }
    }

    while (queue_read < queue_write) {
        ql_node_id current = queue[queue_read++];
        size_t candidate;
        ++processed;
        for (candidate = 0u; candidate < pipeline->count; ++candidate) {
            ql_pipeline_node *node = &pipeline->nodes[candidate];
            size_t dependency_index;
            for (dependency_index = 0u;
                 dependency_index < node->dependency_count;
                 ++dependency_index) {
                if (node->dependencies[dependency_index] == current) {
                    uint32_t next_level =
                        pipeline->nodes[current].level + 1u;
                    if (node->level < next_level) {
                        node->level = next_level;
                    }
                    --indegrees[candidate];
                    if (indegrees[candidate] == 0u) {
                        queue[queue_write++] = (ql_node_id)candidate;
                    }
                }
            }
        }
    }
    if (processed != pipeline->count) {
        ql_error_set(error, QL_STATUS_CYCLE,
                     "pipeline contains a dependency cycle");
        status = QL_STATUS_CYCLE;
        goto cleanup;
    }
    for (index = 0u; index < pipeline->count; ++index) {
        if (pipeline->nodes[index].level > pipeline->maximum_level) {
            pipeline->maximum_level = pipeline->nodes[index].level;
        }
    }
    pipeline->compiled = 1u;
    pipeline->registry_generation =
        ql_internal_registry_generation(pipeline->registry);
    ql_error_clear(error);

cleanup:
    pipeline->allocator.deallocate(pipeline->allocator.user_data, queue);
    pipeline->allocator.deallocate(pipeline->allocator.user_data, indegrees);
    return status;
}

static ql_status set_json_dependencies(ql_pipeline *pipeline, yyjson_val *nodes,
                                       ql_error *error) {
    size_t index;
    size_t maximum;
    yyjson_val *value;

    yyjson_arr_foreach(nodes, index, maximum, value) {
        yyjson_val *depends_on = yyjson_obj_get(value, "depends_on");
        size_t dependency_count;
        ql_node_id *dependencies;
        size_t dependency_index;
        size_t dependency_maximum;
        yyjson_val *dependency_value;

        if (depends_on == NULL) {
            continue;
        }
        if (!yyjson_is_arr(depends_on)) {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "node %zu field 'depends_on' must be an array", index);
            return QL_STATUS_PARSE_ERROR;
        }
        dependency_count = yyjson_arr_size(depends_on);
        if (dependency_count == 0u) {
            continue;
        }
        if (dependency_count > SIZE_MAX / sizeof(*dependencies)) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                         "dependency count overflow");
            return QL_STATUS_OUT_OF_MEMORY;
        }
        dependencies = pipeline->allocator.allocate(
            pipeline->allocator.user_data,
            dependency_count * sizeof(*dependencies));
        if (dependencies == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        yyjson_arr_foreach(depends_on, dependency_index, dependency_maximum,
                           dependency_value) {
            const char *dependency_name;
            ql_node_id dependency;
            if (!yyjson_is_str(dependency_value)) {
                pipeline->allocator.deallocate(pipeline->allocator.user_data,
                                               dependencies);
                ql_error_set(error, QL_STATUS_PARSE_ERROR,
                             "node %zu dependency %zu must be a string", index,
                             dependency_index);
                return QL_STATUS_PARSE_ERROR;
            }
            dependency_name = yyjson_get_str(dependency_value);
            if (strlen(dependency_name) != yyjson_get_len(dependency_value)) {
                pipeline->allocator.deallocate(pipeline->allocator.user_data,
                                               dependencies);
                ql_error_set(error, QL_STATUS_PARSE_ERROR,
                             "node %zu dependency %zu contains a null byte",
                             index, dependency_index);
                return QL_STATUS_PARSE_ERROR;
            }
            dependency = ql_pipeline_find_node(pipeline, dependency_name);
            if (dependency == QL_INVALID_NODE_ID) {
                pipeline->allocator.deallocate(pipeline->allocator.user_data,
                                               dependencies);
                ql_error_set(error, QL_STATUS_NOT_FOUND,
                             "node '%s' depends on unknown node '%s'",
                             pipeline->nodes[index].name, dependency_name);
                return QL_STATUS_NOT_FOUND;
            }
            dependencies[dependency_index] = dependency;
        }
        pipeline->nodes[index].dependencies = dependencies;
        pipeline->nodes[index].dependency_count = dependency_count;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_pipeline_from_json(
    ql_registry *registry, const ql_allocator *allocator, const char *json,
    size_t json_size, ql_pipeline **output, ql_error *error) {
    yyjson_read_err read_error;
    yyjson_doc *document = NULL;
    yyjson_val *root;
    yyjson_val *schema;
    yyjson_val *nodes;
    yyjson_val *value;
    size_t index;
    size_t maximum;
    ql_pipeline *pipeline = NULL;
    ql_status status;

    if (registry == NULL || json == NULL || json_size == 0u || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "registry, JSON data, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    memset(&read_error, 0, sizeof(read_error));
    document = yyjson_read_opts((char *)(uintptr_t)json, json_size, 0u, NULL,
                                &read_error);
    if (document == NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "invalid pipeline JSON at byte %zu: %s", read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return QL_STATUS_PARSE_ERROR;
    }
    root = yyjson_doc_get_root(document);
    schema = yyjson_obj_get(root, "schema_version");
    nodes = yyjson_obj_get(root, "nodes");
    if (!yyjson_is_obj(root) || !yyjson_is_uint(schema) ||
        yyjson_get_uint(schema) != QL_PIPELINE_SCHEMA_VERSION ||
        !yyjson_is_arr(nodes) || yyjson_arr_size(nodes) == 0u) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "pipeline requires schema_version %u and a non-empty nodes array",
                     QL_PIPELINE_SCHEMA_VERSION);
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto cleanup;
    }
    status = ql_pipeline_create(registry, allocator, &pipeline, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    yyjson_arr_foreach(nodes, index, maximum, value) {
        yyjson_val *name_value;
        yyjson_val *method_value;
        yyjson_val *options_value;
        const char *name;
        const char *method;
        const char *options = "{}";
        char *serialized_options = NULL;

        if (!yyjson_is_obj(value)) {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "pipeline node %zu must be an object", index);
            status = QL_STATUS_PARSE_ERROR;
            goto cleanup;
        }
        name_value = yyjson_obj_get(value, "name");
        method_value = yyjson_obj_get(value, "method");
        options_value = yyjson_obj_get(value, "options");
        if (!yyjson_is_str(name_value) || !yyjson_is_str(method_value) ||
            (options_value != NULL && !yyjson_is_obj(options_value))) {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "pipeline node %zu requires string fields 'name' and 'method'; 'options' is an optional object",
                         index);
            status = QL_STATUS_PARSE_ERROR;
            goto cleanup;
        }
        name = yyjson_get_str(name_value);
        method = yyjson_get_str(method_value);
        if (strlen(name) != yyjson_get_len(name_value) ||
            strlen(method) != yyjson_get_len(method_value)) {
            ql_error_set(error, QL_STATUS_PARSE_ERROR,
                         "pipeline node %zu name or method contains a null byte",
                         index);
            status = QL_STATUS_PARSE_ERROR;
            goto cleanup;
        }
        if (options_value != NULL) {
            serialized_options = yyjson_val_write(options_value, 0u, NULL);
            if (serialized_options == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                             "could not serialize options for node '%s'", name);
                status = QL_STATUS_OUT_OF_MEMORY;
                goto cleanup;
            }
            options = serialized_options;
        }
        status = ql_pipeline_add_node(pipeline, name, method, options, NULL, 0u,
                                      NULL, error);
        free(serialized_options);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
    }
    status = set_json_dependencies(pipeline, nodes, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = ql_pipeline_compile(pipeline, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    *output = pipeline;
    pipeline = NULL;

cleanup:
    ql_pipeline_destroy(pipeline);
    yyjson_doc_free(document);
    return status;
}

static uint32_t QL_CALL cancellation_requested(const void *state) {
    const ql_pipeline_cancel_state *combined = state;

    if (ql_cancel_token_is_requested(combined->token)) {
        return 1u;
    }
    return ql_budget_scope_is_cancelled(combined->scope);
}

static ql_status QL_CALL run_pipeline_node(void *user_data, ql_error *error) {
    ql_pipeline_task *task = user_data;
    ql_pipeline_node *node = &task->pipeline->nodes[task->node_index];
    ql_artifact **inputs = NULL;
    ql_artifact *root_input[1];
    size_t input_count;
    void *instance = NULL;
    ql_run_context_v1 context;
    ql_pipeline_cancel_state cancel_state;
    ql_budget_scope *scope = NULL;
    ql_status status;
    size_t index;
    uint32_t instance_ready = node->method->create == NULL;

    ql_error_clear(&task->error);
    task->status = QL_STATUS_OK;
    if (ql_cancel_token_is_requested(task->cancel_token)) {
        ql_error_set(&task->error, QL_STATUS_CANCELLED,
                     "pipeline run was cancelled before node '%s'", node->name);
        task->status = QL_STATUS_CANCELLED;
        *error = task->error;
        return task->status;
    }
    if (task->budget != NULL) {
        status = ql_budget_scope_begin(task->budget, NULL,
                                       QL_BUDGET_SCOPE_NODE, node->name,
                                       &scope, &task->error);
        if (status != QL_STATUS_OK) {
            task->status = status;
            *error = task->error;
            return task->status;
        }
    }
    if (node->dependency_count == 0u) {
        root_input[0] = task->initial_input;
        inputs = root_input;
        input_count = 1u;
    } else {
        input_count = node->dependency_count;
        inputs = task->pipeline->allocator.allocate(
            task->pipeline->allocator.user_data,
            input_count * sizeof(*inputs));
        if (inputs == NULL) {
            ql_error_set(&task->error, QL_STATUS_OUT_OF_MEMORY, NULL);
            task->status = QL_STATUS_OUT_OF_MEMORY;
            *error = task->error;
            return task->status;
        }
        for (index = 0u; index < input_count; ++index) {
            inputs[index] =
                task->node_outputs[node->dependencies[index]];
            if (inputs[index] == NULL) {
                ql_error_set(&task->error, QL_STATUS_INTERNAL_ERROR,
                             "dependency output for node '%s' is missing",
                             node->name);
                task->status = QL_STATUS_INTERNAL_ERROR;
                goto cleanup;
            }
        }
    }
    if (node->method->create != NULL) {
        status = node->method->create(ql_default_host(), node->options_json,
                                      &instance, &task->error);
        if (status != QL_STATUS_OK) {
            task->status = status;
            goto cleanup;
        }
        instance_ready = 1u;
    }
    if (node->method->validate != NULL) {
        status = node->method->validate(instance, inputs, input_count,
                                        &task->error);
        if (status != QL_STATUS_OK) {
            task->status = status;
            goto cleanup;
        }
    }
    cancel_state.token = task->cancel_token;
    cancel_state.scope = scope;
    memset(&context, 0, sizeof(context));
    context.struct_size = sizeof(context);
    context.abi_version = QL_ABI_VERSION;
    context.host = ql_default_host();
    context.cancel_state = &cancel_state;
    context.is_cancelled = cancellation_requested;
    context.run_id = task->run_id;
    status = node->method->run(instance, &context, inputs, input_count,
                               &task->node_outputs[task->node_index],
                               &task->error);
    if (status != QL_STATUS_OK) {
        task->status = status;
        goto cleanup;
    }
    /* A node that overran its own axis does not get to keep its output. */
    status = ql_budget_scope_check(scope, &task->error);
    if (status != QL_STATUS_OK) {
        task->status = status;
        ql_artifact_release(task->node_outputs[task->node_index]);
        task->node_outputs[task->node_index] = NULL;
        goto cleanup;
    }
    if (task->node_outputs[task->node_index] == NULL) {
        ql_error_set(&task->error, QL_STATUS_METHOD_ERROR,
                     "method '%s' succeeded without producing an artifact",
                     node->method_name);
        task->status = QL_STATUS_METHOD_ERROR;
        goto cleanup;
    }
    if (node->method->output_kind != NULL) {
        ql_artifact_view view = { sizeof(ql_artifact_view), NULL, 0u, NULL, 0u,
                                  { { 0u } } };
        status = ql_artifact_get_view(
            task->node_outputs[task->node_index], &view, &task->error);
        if (status != QL_STATUS_OK ||
            strcmp(view.kind, node->method->output_kind) != 0) {
            if (status == QL_STATUS_OK) {
                ql_error_set(&task->error, QL_STATUS_TYPE_MISMATCH,
                             "method '%s' promised artifact kind '%s' but produced '%s'",
                             node->method_name, node->method->output_kind,
                             view.kind);
            }
            task->status = status == QL_STATUS_OK ? QL_STATUS_TYPE_MISMATCH
                                                  : status;
            ql_artifact_release(task->node_outputs[task->node_index]);
            task->node_outputs[task->node_index] = NULL;
            goto cleanup;
        }
    }

cleanup:
    if (instance_ready && node->method->destroy != NULL) {
        node->method->destroy(instance);
    }
    ql_budget_scope_end(scope);
    if (node->dependency_count != 0u) {
        task->pipeline->allocator.deallocate(
            task->pipeline->allocator.user_data, inputs);
    }
    if (task->status != QL_STATUS_OK && task->error.message[0] == '\0') {
        ql_error_set(&task->error, task->status,
                     "method '%s' failed", node->method_name);
    }
    *error = task->error;
    return task->status;
}

static void release_node_outputs(ql_pipeline *pipeline,
                                 ql_artifact **node_outputs) {
    size_t index;
    if (node_outputs == NULL) {
        return;
    }
    for (index = 0u; index < pipeline->count; ++index) {
        ql_artifact_release(node_outputs[index]);
    }
    pipeline->allocator.deallocate(pipeline->allocator.user_data,
                                   node_outputs);
}

static ql_status build_result(ql_pipeline *pipeline,
                              ql_artifact **node_outputs,
                              ql_pipeline_result **output, ql_error *error) {
    ql_pipeline_result *result;
    size_t sink_count = 0u;
    size_t index;
    size_t output_index = 0u;

    for (index = 0u; index < pipeline->count; ++index) {
        if (pipeline->nodes[index].is_sink) {
            ++sink_count;
        }
    }
    result = pipeline->allocator.allocate(pipeline->allocator.user_data,
                                          sizeof(*result));
    if (result == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(result, 0, sizeof(*result));
    result->allocator = pipeline->allocator;
    result->items = pipeline->allocator.allocate(
        pipeline->allocator.user_data, sink_count * sizeof(*result->items));
    if (result->items == NULL) {
        pipeline->allocator.deallocate(pipeline->allocator.user_data, result);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(result->items, 0, sink_count * sizeof(*result->items));
    result->count = sink_count;
    for (index = 0u; index < pipeline->count; ++index) {
        if (!pipeline->nodes[index].is_sink) {
            continue;
        }
        result->items[output_index].node_name = ql_internal_strdup(
            &pipeline->allocator, pipeline->nodes[index].name);
        if (result->items[output_index].node_name == NULL) {
            ql_pipeline_result_destroy(result);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        result->items[output_index].artifact = node_outputs[index];
        ql_artifact_retain(node_outputs[index]);
        ++output_index;
    }
    *output = result;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_pipeline_run(
    ql_pipeline *pipeline, ql_scheduler *scheduler, ql_artifact *input,
    const ql_cancel_token *cancel_token, ql_pipeline_result **output,
    ql_error *error) {
    return ql_pipeline_run_with_budget(pipeline, scheduler, input,
                                       cancel_token, NULL, output, error);
}

ql_status QL_CALL ql_pipeline_run_with_budget(
    ql_pipeline *pipeline, ql_scheduler *scheduler, ql_artifact *input,
    const ql_cancel_token *cancel_token, ql_budget *budget,
    ql_pipeline_result **output, ql_error *error) {
    ql_artifact **node_outputs = NULL;
    uint32_t level;
    uint64_t run_id;
    ql_status status = QL_STATUS_OK;

    if (pipeline == NULL || scheduler == NULL || input == NULL ||
        output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "compiled pipeline, scheduler, input, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    /* Refresh method pointers after sequential registry changes. Registry
       mutation concurrent with compile or run is outside the API contract. */
    if (!pipeline->compiled ||
        pipeline->registry_generation !=
            ql_internal_registry_generation(pipeline->registry)) {
        status = ql_pipeline_compile(pipeline, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    node_outputs = pipeline->allocator.allocate(
        pipeline->allocator.user_data,
        pipeline->count * sizeof(*node_outputs));
    if (node_outputs == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(node_outputs, 0, pipeline->count * sizeof(*node_outputs));
    run_id = atomic_fetch_add_explicit(&next_run_id, UINT64_C(1),
                                       memory_order_relaxed);

    for (level = 0u; level <= pipeline->maximum_level; ++level) {
        ql_task_group *group = NULL;
        ql_pipeline_task *tasks = NULL;
        size_t task_count = 0u;
        size_t task_index = 0u;
        size_t node_index;
        ql_error group_error;

        if (ql_cancel_token_is_requested(cancel_token)) {
            ql_error_set(error, QL_STATUS_CANCELLED,
                         "pipeline run was cancelled");
            status = QL_STATUS_CANCELLED;
            break;
        }
        status = ql_budget_check(budget, error);
        if (status != QL_STATUS_OK) {
            break;
        }
        for (node_index = 0u; node_index < pipeline->count; ++node_index) {
            if (pipeline->nodes[node_index].level == level) {
                ++task_count;
            }
        }
        tasks = pipeline->allocator.allocate(
            pipeline->allocator.user_data, task_count * sizeof(*tasks));
        if (tasks == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            status = QL_STATUS_OUT_OF_MEMORY;
            break;
        }
        memset(tasks, 0, task_count * sizeof(*tasks));
        status = ql_task_group_create(&pipeline->allocator, &group, error);
        if (status != QL_STATUS_OK) {
            pipeline->allocator.deallocate(pipeline->allocator.user_data,
                                           tasks);
            break;
        }
        for (node_index = 0u; node_index < pipeline->count; ++node_index) {
            if (pipeline->nodes[node_index].level != level) {
                continue;
            }
            tasks[task_index].pipeline = pipeline;
            tasks[task_index].node_outputs = node_outputs;
            tasks[task_index].initial_input = input;
            tasks[task_index].cancel_token = cancel_token;
            tasks[task_index].budget = budget;
            tasks[task_index].node_index = node_index;
            tasks[task_index].run_id = run_id;
            tasks[task_index].status = QL_STATUS_OK;
            ql_error_clear(&tasks[task_index].error);
            status = ql_scheduler_submit(scheduler, group, run_pipeline_node,
                                         &tasks[task_index], error);
            if (status != QL_STATUS_OK) {
                break;
            }
            ++task_index;
        }
        ql_error_clear(&group_error);
        (void)ql_task_group_wait(group, &group_error);
        if (status == QL_STATUS_OK) {
            for (node_index = 0u; node_index < task_index; ++node_index) {
                if (tasks[node_index].status != QL_STATUS_OK) {
                    status = tasks[node_index].status;
                    if (error != NULL) {
                        *error = tasks[node_index].error;
                    }
                    break;
                }
            }
        }
        ql_task_group_destroy(group);
        pipeline->allocator.deallocate(pipeline->allocator.user_data, tasks);
        if (status != QL_STATUS_OK) {
            break;
        }
    }
    if (status == QL_STATUS_OK) {
        status = build_result(pipeline, node_outputs, output, error);
    }
    release_node_outputs(pipeline, node_outputs);
    return status;
}

void QL_CALL ql_pipeline_result_destroy(ql_pipeline_result *result) {
    size_t index;
    ql_allocator allocator;

    if (result == NULL) {
        return;
    }
    allocator = result->allocator;
    for (index = 0u; index < result->count; ++index) {
        ql_artifact_release(result->items[index].artifact);
        allocator.deallocate(allocator.user_data,
                             result->items[index].node_name);
    }
    allocator.deallocate(allocator.user_data, result->items);
    allocator.deallocate(allocator.user_data, result);
}

size_t QL_CALL ql_pipeline_result_count(const ql_pipeline_result *result) {
    return result != NULL ? result->count : 0u;
}

const char *QL_CALL ql_pipeline_result_node_name(
    const ql_pipeline_result *result, size_t index) {
    return result != NULL && index < result->count
               ? result->items[index].node_name
               : NULL;
}

const ql_artifact *QL_CALL ql_pipeline_result_artifact(
    const ql_pipeline_result *result, size_t index) {
    return result != NULL && index < result->count
               ? result->items[index].artifact
               : NULL;
}
