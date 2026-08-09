#include "quodlibet/problem.h"

#include <stdatomic.h>
#include <string.h>

#include "yyjson.h"

struct ql_problem {
    atomic_uint reference_count;
    ql_allocator allocator;
    ql_artifact *artifact;
    yyjson_doc *document;
    ql_problem_view_v1 view;
};

static void *problem_json_allocate(void *context, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    return allocator->allocate(allocator->user_data, size);
}

static void *problem_json_reallocate(void *context, void *pointer,
                                     size_t old_size, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    (void)old_size;
    return allocator->reallocate(allocator->user_data, pointer, size);
}

static void problem_json_deallocate(void *context, void *pointer) {
    ql_allocator *allocator = (ql_allocator *)context;
    allocator->deallocate(allocator->user_data, pointer);
}

static yyjson_alc problem_json_allocator(ql_allocator *allocator) {
    yyjson_alc result;
    result.malloc = problem_json_allocate;
    result.realloc = problem_json_reallocate;
    result.free = problem_json_deallocate;
    result.ctx = allocator;
    return result;
}

static int checked_add_size(size_t *total, size_t amount) {
    if (amount > SIZE_MAX - *total) {
        return 0;
    }
    *total += amount;
    return 1;
}

static int bytes_are_present(const char *bytes, size_t size) {
    return bytes != NULL && size != 0u;
}

static int bytes_have_no_nul(const char *bytes, size_t size) {
    return memchr(bytes, '\0', size) == NULL;
}

static ql_status validate_definition(
    const ql_problem_definition_v1 *definition, ql_error *error) {
    size_t aggregate_size = 1024u;

    if (definition == NULL ||
        definition->struct_size < sizeof(*definition)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "problem definition v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (definition->schema_version != QL_PROBLEM_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "unsupported problem schema version %u",
                     definition->schema_version);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (!bytes_are_present(definition->left_source,
                           definition->left_source_size) ||
        !bytes_are_present(definition->right_source,
                           definition->right_source_size) ||
        !bytes_are_present(definition->left_function_name,
                           definition->left_function_name_size) ||
        !bytes_are_present(definition->right_function_name,
                           definition->right_function_name_size)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "both C sources and function names are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((definition->contract.precondition_json == NULL) !=
        (definition->contract.precondition_json_size == 0u)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "precondition JSON pointer and size disagree");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (!checked_add_size(&aggregate_size, definition->left_source_size) ||
        !checked_add_size(&aggregate_size, definition->right_source_size) ||
        !checked_add_size(&aggregate_size,
                          definition->left_function_name_size) ||
        !checked_add_size(&aggregate_size,
                          definition->right_function_name_size) ||
        !checked_add_size(&aggregate_size,
                          definition->contract.precondition_json_size)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "problem text sizes overflow size_t");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (!bytes_have_no_nul(definition->left_source,
                           definition->left_source_size) ||
        !bytes_have_no_nul(definition->right_source,
                           definition->right_source_size) ||
        !bytes_have_no_nul(definition->left_function_name,
                           definition->left_function_name_size) ||
        !bytes_have_no_nul(definition->right_function_name,
                           definition->right_function_name_size)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C sources and function names cannot contain NUL bytes");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return ql_semantic_contract_validate(&definition->contract, error);
}

static ql_status canonicalize_precondition(
    const ql_semantic_contract_v1 *contract, const yyjson_alc *allocator,
    char **json, size_t *json_size, ql_error *error) {
    yyjson_doc *document;
    yyjson_read_err read_error;
    yyjson_write_err write_error;

    *json = NULL;
    *json_size = 0u;
    if (contract->precondition_json == NULL) {
        return QL_STATUS_OK;
    }
    if (yyjson_read_max_memory_usage(contract->precondition_json_size, 0u) ==
        0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "precondition JSON size overflows parser limits");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    document = yyjson_read_opts((char *)(uintptr_t)contract->precondition_json,
                                contract->precondition_json_size, 0u,
                                allocator, &read_error);
    if (document == NULL) {
        ql_status status =
            read_error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
                ? QL_STATUS_OUT_OF_MEMORY
                : QL_STATUS_PARSE_ERROR;
        ql_error_set(error, status, "invalid precondition JSON at byte %zu: %s",
                     read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        return status;
    }
    *json = yyjson_write_opts(document, 0u, allocator, json_size,
                              &write_error);
    yyjson_doc_free(document);
    if (*json == NULL) {
        ql_status status =
            write_error.code == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION
                ? QL_STATUS_OUT_OF_MEMORY
                : QL_STATUS_PARSE_ERROR;
        ql_error_set(error, status, "cannot serialize precondition JSON: %s",
                     write_error.msg != NULL ? write_error.msg
                                             : "JSON write error");
        return status;
    }
    return QL_STATUS_OK;
}

static int add_contract_json(yyjson_mut_doc *document, yyjson_mut_val *object,
                             const ql_semantic_contract_v1 *contract,
                             const char *precondition,
                             size_t precondition_size) {
    if (!yyjson_mut_obj_add_uint(document, object, "schema_version",
                                contract->schema_version) ||
        !yyjson_mut_obj_add_uint(document, object, "relation",
                                (uint64_t)contract->relation) ||
        !yyjson_mut_obj_add_uint(document, object, "ub_policy",
                                (uint64_t)contract->ub_policy) ||
        !yyjson_mut_obj_add_uint(document, object, "observations",
                                contract->observations) ||
        !yyjson_mut_obj_add_uint(document, object, "memory_observation",
                                (uint64_t)contract->memory_observation) ||
        !yyjson_mut_obj_add_uint(
            document, object, "external_call_observation",
            (uint64_t)contract->external_call_observation) ||
        !yyjson_mut_obj_add_uint(document, object, "c_dialect",
                                (uint64_t)contract->c_dialect) ||
        !yyjson_mut_obj_add_uint(document, object, "target_abi",
                                (uint64_t)contract->target_abi) ||
        !yyjson_mut_obj_add_uint(document, object, "compiler_families",
                                contract->compiler_families) ||
        !yyjson_mut_obj_add_uint(document, object, "codegen_modes",
                                contract->codegen_modes) ||
        !yyjson_mut_obj_add_uint(document, object, "target_features",
                                contract->target_features)) {
        return 0;
    }
    if (precondition == NULL) {
        return yyjson_mut_obj_add_null(document, object, "precondition_json");
    }
    return yyjson_mut_obj_add_strncpy(document, object, "precondition_json",
                                      precondition, precondition_size);
}

static int add_function_json(yyjson_mut_doc *document, yyjson_mut_val *object,
                             const char *name, size_t name_size,
                             const char *source, size_t source_size) {
    return yyjson_mut_obj_add_strncpy(document, object, "function_name", name,
                                      name_size) &&
           yyjson_mut_obj_add_strncpy(document, object, "source", source,
                                      source_size);
}

void QL_CALL ql_problem_definition_init(
    ql_problem_definition_v1 *definition) {
    if (definition == NULL) {
        return;
    }
    memset(definition, 0, sizeof(*definition));
    definition->struct_size = sizeof(*definition);
    definition->schema_version = QL_PROBLEM_SCHEMA_VERSION;
    ql_semantic_contract_init(&definition->contract);
}

ql_status QL_CALL ql_problem_artifact_create(
    const ql_allocator *allocator,
    const ql_problem_definition_v1 *definition, ql_artifact **output,
    ql_error *error) {
    const ql_allocator *selected = allocator;
    ql_allocator allocator_copy;
    yyjson_alc json_allocator;
    yyjson_mut_doc *document = NULL;
    yyjson_mut_val *root;
    yyjson_mut_val *contract;
    yyjson_mut_val *left;
    yyjson_mut_val *right;
    yyjson_write_err write_error;
    char *precondition = NULL;
    size_t precondition_size = 0u;
    char *json = NULL;
    size_t json_size = 0u;
    ql_status status;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "problem artifact output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = validate_definition(definition, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (selected == NULL) {
        selected = ql_default_allocator();
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    allocator_copy = *selected;
    json_allocator = problem_json_allocator(&allocator_copy);
    status = canonicalize_precondition(&definition->contract,
                                       &json_allocator, &precondition,
                                       &precondition_size, error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    document = yyjson_mut_doc_new(&json_allocator);
    root = document != NULL ? yyjson_mut_obj(document) : NULL;
    contract = document != NULL ? yyjson_mut_obj(document) : NULL;
    left = document != NULL ? yyjson_mut_obj(document) : NULL;
    right = document != NULL ? yyjson_mut_obj(document) : NULL;
    if (document == NULL || root == NULL || contract == NULL || left == NULL ||
        right == NULL ||
        !yyjson_mut_obj_add_str(document, root, "kind",
                                QL_ARTIFACT_KIND_PROBLEM) ||
        !yyjson_mut_obj_add_uint(document, root, "schema_version",
                                QL_PROBLEM_SCHEMA_VERSION) ||
        !add_contract_json(document, contract, &definition->contract,
                           precondition, precondition_size) ||
        !yyjson_mut_obj_add_val(document, root, "contract", contract) ||
        !add_function_json(document, left, definition->left_function_name,
                           definition->left_function_name_size,
                           definition->left_source,
                           definition->left_source_size) ||
        !yyjson_mut_obj_add_val(document, root, "left", left) ||
        !add_function_json(document, right, definition->right_function_name,
                           definition->right_function_name_size,
                           definition->right_source,
                           definition->right_source_size) ||
        !yyjson_mut_obj_add_val(document, root, "right", right)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    yyjson_mut_doc_set_root(document, root);
    json = yyjson_mut_write_opts(document, 0u, &json_allocator, &json_size,
                                 &write_error);
    if (json == NULL) {
        status = write_error.code == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION
                     ? QL_STATUS_OUT_OF_MEMORY
                     : QL_STATUS_PARSE_ERROR;
        ql_error_set(error, status, "cannot serialize problem JSON: %s",
                     write_error.msg != NULL ? write_error.msg
                                             : "JSON write error");
        goto cleanup;
    }
    status = ql_artifact_create(selected, QL_ARTIFACT_KIND_PROBLEM,
                                QL_PROBLEM_SCHEMA_VERSION, json, json_size,
                                output, error);

cleanup:
    if (json != NULL) {
        json_allocator.free(json_allocator.ctx, json);
    }
    if (document != NULL) {
        yyjson_mut_doc_free(document);
    }
    if (precondition != NULL) {
        json_allocator.free(json_allocator.ctx, precondition);
    }
    return status;
}

static int json_string_equals(yyjson_val *value, const char *expected) {
    size_t expected_size = strlen(expected);
    return yyjson_is_str(value) && yyjson_get_len(value) == expected_size &&
           memcmp(yyjson_get_str(value), expected, expected_size) == 0;
}

static int get_uint64_field(yyjson_val *object, const char *name,
                            uint64_t maximum, uint64_t *output) {
    yyjson_val *value = yyjson_obj_get(object, name);
    uint64_t number;
    if (!yyjson_is_uint(value)) {
        return 0;
    }
    number = yyjson_get_uint(value);
    if (number > maximum) {
        return 0;
    }
    *output = number;
    return 1;
}

static int get_required_string(yyjson_val *object, const char *name,
                               const char **bytes, size_t *size) {
    yyjson_val *value = yyjson_obj_get(object, name);
    if (!yyjson_is_str(value) || yyjson_get_len(value) == 0u) {
        return 0;
    }
    *bytes = yyjson_get_str(value);
    *size = yyjson_get_len(value);
    return memchr(*bytes, '\0', *size) == NULL;
}

static int read_contract_json(yyjson_val *object,
                              ql_semantic_contract_v1 *contract) {
    yyjson_val *precondition;
    uint64_t value;

    if (!yyjson_is_obj(object) || yyjson_obj_size(object) != 12u) {
        return 0;
    }
    memset(contract, 0, sizeof(*contract));
    contract->struct_size = sizeof(*contract);
    if (!get_uint64_field(object, "schema_version", UINT32_MAX, &value)) {
        return 0;
    }
    contract->schema_version = (uint32_t)value;
    if (!get_uint64_field(object, "relation",
                          QL_RELATION_RIGHT_REFINES_LEFT, &value)) {
        return 0;
    }
    contract->relation = (ql_relation)value;
    if (!get_uint64_field(object, "ub_policy",
                          QL_UB_COMPARE_WHERE_BOTH_DEFINED, &value)) {
        return 0;
    }
    contract->ub_policy = (ql_ub_policy)value;
    if (!get_uint64_field(object, "observations", UINT64_MAX, &value)) {
        return 0;
    }
    contract->observations = value;
    if (!get_uint64_field(object, "memory_observation", QL_MEMORY_FULL_TRACE,
                          &value)) {
        return 0;
    }
    contract->memory_observation = (ql_memory_observation)value;
    if (!get_uint64_field(object, "external_call_observation",
                          QL_EXTERNAL_CALLS_ORDERED_TRACE, &value)) {
        return 0;
    }
    contract->external_call_observation =
        (ql_external_call_observation)value;
    if (!get_uint64_field(object, "c_dialect", UINT32_MAX, &value)) {
        return 0;
    }
    contract->c_dialect = (ql_c_dialect_profile)value;
    if (!get_uint64_field(object, "target_abi", UINT32_MAX, &value)) {
        return 0;
    }
    contract->target_abi = (ql_target_abi)value;
    if (!get_uint64_field(object, "compiler_families", UINT32_MAX, &value)) {
        return 0;
    }
    contract->compiler_families = (uint32_t)value;
    if (!get_uint64_field(object, "codegen_modes", UINT32_MAX, &value)) {
        return 0;
    }
    contract->codegen_modes = (uint32_t)value;
    if (!get_uint64_field(object, "target_features", UINT64_MAX, &value)) {
        return 0;
    }
    contract->target_features = value;

    precondition = yyjson_obj_get(object, "precondition_json");
    if (yyjson_is_null(precondition)) {
        return 1;
    }
    if (!yyjson_is_str(precondition) || yyjson_get_len(precondition) == 0u) {
        return 0;
    }
    contract->precondition_json = yyjson_get_str(precondition);
    contract->precondition_json_size = yyjson_get_len(precondition);
    return memchr(contract->precondition_json, '\0',
                  contract->precondition_json_size) == NULL;
}

static ql_status read_problem_view(ql_problem *problem,
                                   const ql_artifact_view *artifact_view,
                                   ql_error *error) {
    yyjson_val *root = yyjson_doc_get_root(problem->document);
    yyjson_val *kind;
    yyjson_val *contract;
    yyjson_val *left;
    yyjson_val *right;
    uint64_t schema_version;
    ql_status status;

    kind = yyjson_obj_get(root, "kind");
    contract = yyjson_obj_get(root, "contract");
    left = yyjson_obj_get(root, "left");
    right = yyjson_obj_get(root, "right");
    if (!yyjson_is_obj(root) || yyjson_obj_size(root) != 5u ||
        !json_string_equals(kind, QL_ARTIFACT_KIND_PROBLEM) ||
        !get_uint64_field(root, "schema_version", UINT32_MAX,
                          &schema_version) ||
        schema_version != QL_PROBLEM_SCHEMA_VERSION ||
        !read_contract_json(contract, &problem->view.contract) ||
        !yyjson_is_obj(left) || yyjson_obj_size(left) != 2u ||
        !yyjson_is_obj(right) || yyjson_obj_size(right) != 2u ||
        !get_required_string(left, "function_name",
                             &problem->view.left_function_name,
                             &problem->view.left_function_name_size) ||
        !get_required_string(left, "source", &problem->view.left_source,
                             &problem->view.left_source_size) ||
        !get_required_string(right, "function_name",
                             &problem->view.right_function_name,
                             &problem->view.right_function_name_size) ||
        !get_required_string(right, "source", &problem->view.right_source,
                             &problem->view.right_source_size)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "problem JSON does not match schema version 1");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    status = ql_semantic_contract_validate(&problem->view.contract, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    problem->view.struct_size = sizeof(problem->view);
    problem->view.schema_version = (uint32_t)schema_version;
    problem->view.artifact_digest = artifact_view->digest;
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_problem_open(const ql_allocator *allocator,
                                  const ql_artifact *artifact,
                                  ql_problem **output, ql_error *error) {
    const ql_allocator *selected = allocator;
    ql_artifact_view artifact_view;
    ql_problem *problem;
    yyjson_alc json_allocator;
    yyjson_read_err read_error;
    ql_status status;

    if (output == NULL || artifact == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "artifact and problem output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    memset(&artifact_view, 0, sizeof(artifact_view));
    artifact_view.struct_size = sizeof(artifact_view);
    status = ql_artifact_get_view(artifact, &artifact_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(artifact_view.kind, QL_ARTIFACT_KIND_PROBLEM) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "artifact is not a quodlibet.problem");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (artifact_view.schema_version != QL_PROBLEM_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "unsupported problem artifact schema version %u",
                     artifact_view.schema_version);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (artifact_view.size == 0u ||
        yyjson_read_max_memory_usage(artifact_view.size, 0u) == 0u) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "problem artifact JSON size is invalid");
        return QL_STATUS_PARSE_ERROR;
    }
    if (selected == NULL) {
        selected = ql_default_allocator();
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    problem = selected->allocate(selected->user_data, sizeof(*problem));
    if (problem == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(problem, 0, sizeof(*problem));
    problem->allocator = *selected;
    json_allocator = problem_json_allocator(&problem->allocator);
    problem->document = yyjson_read_opts(
        (char *)(uintptr_t)artifact_view.data, artifact_view.size, 0u,
        &json_allocator, &read_error);
    if (problem->document == NULL) {
        status = read_error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
                     ? QL_STATUS_OUT_OF_MEMORY
                     : QL_STATUS_PARSE_ERROR;
        ql_error_set(error, status, "invalid problem JSON at byte %zu: %s",
                     read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        selected->deallocate(selected->user_data, problem);
        return status;
    }
    status = read_problem_view(problem, &artifact_view, error);
    if (status != QL_STATUS_OK) {
        yyjson_doc_free(problem->document);
        selected->deallocate(selected->user_data, problem);
        return status;
    }
    ql_artifact_retain((ql_artifact *)(uintptr_t)artifact);
    problem->artifact = (ql_artifact *)(uintptr_t)artifact;
    atomic_init(&problem->reference_count, 1u);
    *output = problem;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_problem_retain(ql_problem *problem) {
    if (problem != NULL) {
        (void)atomic_fetch_add_explicit(&problem->reference_count, 1u,
                                        memory_order_relaxed);
    }
}

void QL_CALL ql_problem_release(ql_problem *problem) {
    ql_allocator allocator;
    if (problem == NULL ||
        atomic_fetch_sub_explicit(&problem->reference_count, 1u,
                                  memory_order_acq_rel) != 1u) {
        return;
    }
    allocator = problem->allocator;
    yyjson_doc_free(problem->document);
    ql_artifact_release(problem->artifact);
    allocator.deallocate(allocator.user_data, problem);
}

ql_status QL_CALL ql_problem_get_view(const ql_problem *problem,
                                      ql_problem_view_v1 *view,
                                      ql_error *error) {
    size_t caller_size;
    if (problem == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "problem and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    caller_size = view->struct_size;
    if (caller_size != 0u && caller_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "problem view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    *view = problem->view;
    ql_error_clear(error);
    return QL_STATUS_OK;
}
