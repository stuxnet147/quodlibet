#include "quodlibet/problem.h"

#include <stdatomic.h>
#include <string.h>

#include "quodlibet/precondition.h"
#include "quodlibet/signature.h"

#include "yyjson.h"

struct ql_problem {
    atomic_uint reference_count;
    ql_allocator allocator;
    ql_artifact *artifact;
    yyjson_doc *document;
    ql_problem_view_v1 view;
    /* Populated only for schema v2. */
    ql_problem_view_v2 view2;
    ql_problem_argument_binding_v1 *bindings;
    ql_artifact *left_signature_artifact;
    ql_artifact *right_signature_artifact;
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

/* --- Schema v2 binding helpers -------------------------------------------- */

typedef struct problem_signature {
    ql_artifact *artifact;
    ql_source_signature *signature;
    ql_source_signature_view_v1 view;
} problem_signature;

static void problem_signature_dispose(problem_signature *held) {
    ql_source_signature_release(held->signature);
    ql_artifact_release(held->artifact);
    memset(held, 0, sizeof(*held));
}

/* Rebuilds a signature artifact from embedded canonical bytes instead of
   re-serializing a parsed document, so the recorded digest is verified against
   the exact bytes the problem carries. */
static ql_status problem_signature_adopt(const ql_allocator *allocator,
                                         const char *bytes, size_t size,
                                         problem_signature *held,
                                         ql_error *error) {
    ql_status status;

    memset(held, 0, sizeof(*held));
    status = ql_artifact_create(allocator, QL_ARTIFACT_KIND_SOURCE_SIGNATURE,
                                QL_SOURCE_SIGNATURE_SCHEMA_VERSION, bytes,
                                size, &held->artifact, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_source_signature_open(allocator, held->artifact,
                                      &held->signature, error);
    if (status != QL_STATUS_OK) {
        problem_signature_dispose(held);
        return status;
    }
    held->view.struct_size = sizeof(held->view);
    status = ql_source_signature_get_view(held->signature, &held->view,
                                          error);
    if (status != QL_STATUS_OK) {
        problem_signature_dispose(held);
    }
    return status;
}

static ql_status problem_signature_borrow(const ql_allocator *allocator,
                                          const ql_artifact *artifact,
                                          const char *role,
                                          problem_signature *held,
                                          ql_error *error) {
    ql_status status;

    memset(held, 0, sizeof(*held));
    if (artifact == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "problem schema v2 requires a %s source signature", role);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = ql_source_signature_open(allocator, artifact, &held->signature,
                                      error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    ql_artifact_retain((ql_artifact *)(uintptr_t)artifact);
    held->artifact = (ql_artifact *)(uintptr_t)artifact;
    held->view.struct_size = sizeof(held->view);
    status = ql_source_signature_get_view(held->signature, &held->view,
                                          error);
    if (status != QL_STATUS_OK) {
        problem_signature_dispose(held);
    }
    return status;
}

static ql_status signature_names_function(const problem_signature *held,
                                          const char *name, size_t name_size,
                                          const char *role,
                                          ql_error *error) {
    if (held->view.function_name_size != name_size ||
        memcmp(held->view.function_name, name, name_size) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s source signature names a different function than the problem",
                     role);
        return QL_STATUS_TYPE_MISMATCH;
    }
    return QL_STATUS_OK;
}

/* The correspondence must be a total bijection between the two argument
   lists, and corresponding arguments must have identical source types. An
   unmapped or retyped argument would leave an input whose shared meaning is
   undefined, which no relation could be stated over. */
static ql_status validate_argument_bindings(
    const problem_signature *left, const problem_signature *right,
    const ql_problem_argument_binding_v1 *bindings, size_t binding_count,
    ql_error *error) {
    uint8_t left_seen[QL_SOURCE_SIGNATURE_MAX_ARGUMENTS];
    uint8_t right_seen[QL_SOURCE_SIGNATURE_MAX_ARGUMENTS];
    size_t index;
    ql_status status;

    if (left->view.argument_count != right->view.argument_count ||
        binding_count != left->view.argument_count) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "argument correspondence must be total: %zu bindings for %zu left and %zu right arguments",
                     binding_count, left->view.argument_count,
                     right->view.argument_count);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (binding_count != 0u && bindings == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "argument bindings are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(left_seen, 0, sizeof(left_seen));
    memset(right_seen, 0, sizeof(right_seen));
    for (index = 0u; index < binding_count; ++index) {
        ql_source_type_v1 left_type;
        ql_source_type_v1 right_type;

        if (bindings[index].struct_size < sizeof(bindings[index])) {
            ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                         "argument binding %zu has an invalid size", index);
            return QL_STATUS_ABI_MISMATCH;
        }
        if (bindings[index].left_index >= left->view.argument_count ||
            bindings[index].right_index >= right->view.argument_count ||
            left_seen[bindings[index].left_index] != 0u ||
            right_seen[bindings[index].right_index] != 0u) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "argument binding %zu is out of range or repeats an argument",
                         index);
            return QL_STATUS_INVALID_ARGUMENT;
        }
        left_seen[bindings[index].left_index] = 1u;
        right_seen[bindings[index].right_index] = 1u;
        status = ql_source_signature_argument_at(
            left->signature, bindings[index].left_index, &left_type, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_source_signature_argument_at(
            right->signature, bindings[index].right_index, &right_type,
            error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_source_type_compatible(&left_type, &right_type, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return ql_source_type_compatible(&left->view.return_type,
                                     &right->view.return_type, error);
}

/* A null precondition is `true` with a real signature-bound digest, so every
   v2 problem records one and no case falls back to an unbound meaning. */
static ql_status compute_precondition_digest(const ql_allocator *allocator,
                                             const problem_signature *left,
                                             const char *json,
                                             size_t json_size,
                                             ql_digest *digest,
                                             ql_error *error) {
    ql_signature_argument_v1 storage[QL_SOURCE_SIGNATURE_MAX_ARGUMENTS];
    ql_signature_view_v1 signature_view;
    ql_precondition_view_v1 precondition_view;
    ql_precondition *precondition = NULL;
    ql_status status;

    memset(&signature_view, 0, sizeof(signature_view));
    status = ql_source_signature_precondition_view(
        left->signature, &signature_view, storage,
        QL_SOURCE_SIGNATURE_MAX_ARGUMENTS, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_precondition_parse(allocator, json, json_size,
                                   &signature_view, &precondition, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(&precondition_view, 0, sizeof(precondition_view));
    precondition_view.struct_size = sizeof(precondition_view);
    status = ql_precondition_get_view(precondition, &precondition_view,
                                      error);
    if (status == QL_STATUS_OK) {
        *digest = precondition_view.digest;
    }
    ql_precondition_destroy(precondition);
    return status;
}

static int add_digest_json(yyjson_mut_doc *document, yyjson_mut_val *object,
                           const char *key, const ql_digest *digest) {
    char hex[QL_DIGEST_HEX_SIZE];
    ql_digest_hex(digest, hex);
    return yyjson_mut_obj_add_strncpy(document, object, key, hex,
                                      QL_DIGEST_HEX_SIZE - 1u);
}

static int hex_nibble(char ch, uint8_t *value) {
    if (ch >= '0' && ch <= '9') {
        *value = (uint8_t)(ch - '0');
        return 1;
    }
    if (ch >= 'a' && ch <= 'f') {
        *value = (uint8_t)(ch - 'a' + 10);
        return 1;
    }
    return 0;
}

static int read_digest_json(yyjson_val *object, const char *key,
                            ql_digest *digest) {
    yyjson_val *value = yyjson_obj_get(object, key);
    const char *text;
    size_t index;

    if (!yyjson_is_str(value) ||
        yyjson_get_len(value) != QL_DIGEST_HEX_SIZE - 1u) {
        return 0;
    }
    text = yyjson_get_str(value);
    for (index = 0u; index < QL_DIGEST_SIZE; ++index) {
        uint8_t high;
        uint8_t low;
        if (!hex_nibble(text[index * 2u], &high) ||
            !hex_nibble(text[index * 2u + 1u], &low)) {
            return 0;
        }
        digest->bytes[index] = (uint8_t)((high << 4) | low);
    }
    return 1;
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

void QL_CALL ql_problem_definition_v2_init(
    ql_problem_definition_v2 *definition) {
    if (definition == NULL) {
        return;
    }
    memset(definition, 0, sizeof(*definition));
    definition->struct_size = sizeof(*definition);
    definition->schema_version = QL_PROBLEM_SCHEMA_VERSION_2;
    ql_semantic_contract_init(&definition->contract);
}

static ql_status validate_definition_v2(
    const ql_problem_definition_v2 *definition, ql_error *error) {
    ql_problem_definition_v1 shared;

    if (definition == NULL || definition->struct_size < sizeof(*definition)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "problem definition v2 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (definition->schema_version != QL_PROBLEM_SCHEMA_VERSION_2) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "unsupported problem schema version %u",
                     definition->schema_version);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    /* The source, name, and contract rules are identical in both versions. */
    ql_problem_definition_init(&shared);
    shared.contract = definition->contract;
    shared.left_source = definition->left_source;
    shared.left_source_size = definition->left_source_size;
    shared.left_function_name = definition->left_function_name;
    shared.left_function_name_size = definition->left_function_name_size;
    shared.right_source = definition->right_source;
    shared.right_source_size = definition->right_source_size;
    shared.right_function_name = definition->right_function_name;
    shared.right_function_name_size = definition->right_function_name_size;
    return validate_definition(&shared, error);
}

static int add_function_json_v2(yyjson_mut_doc *document,
                                yyjson_mut_val *object, const char *name,
                                size_t name_size, const char *source,
                                size_t source_size,
                                const ql_artifact_view *signature_view) {
    return add_function_json(document, object, name, name_size, source,
                             source_size) &&
           yyjson_mut_obj_add_strncpy(document, object, "signature",
                                      (const char *)signature_view->data,
                                      signature_view->size) &&
           add_digest_json(document, object, "signature_digest",
                           &signature_view->digest);
}

static int add_bindings_json(yyjson_mut_doc *document, yyjson_mut_val *array,
                             const ql_problem_argument_binding_v1 *bindings,
                             size_t binding_count) {
    size_t index;
    for (index = 0u; index < binding_count; ++index) {
        yyjson_mut_val *entry = yyjson_mut_obj(document);
        if (entry == NULL ||
            !yyjson_mut_obj_add_uint(document, entry, "left",
                                     bindings[index].left_index) ||
            !yyjson_mut_obj_add_uint(document, entry, "right",
                                     bindings[index].right_index) ||
            !yyjson_mut_arr_append(array, entry)) {
            return 0;
        }
    }
    return 1;
}

ql_status QL_CALL ql_problem_artifact_create_v2(
    const ql_allocator *allocator,
    const ql_problem_definition_v2 *definition, ql_artifact **output,
    ql_error *error) {
    const ql_allocator *selected = allocator;
    ql_allocator allocator_copy;
    yyjson_alc json_allocator;
    yyjson_mut_doc *document = NULL;
    yyjson_mut_val *root;
    yyjson_mut_val *contract;
    yyjson_mut_val *left_object;
    yyjson_mut_val *right_object;
    yyjson_mut_val *bindings;
    yyjson_write_err write_error;
    problem_signature left = { 0 };
    problem_signature right = { 0 };
    ql_artifact_view left_view;
    ql_artifact_view right_view;
    ql_digest precondition_digest;
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
    status = validate_definition_v2(definition, error);
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
    status = problem_signature_borrow(selected, definition->left_signature,
                                      "left", &left, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = problem_signature_borrow(selected, definition->right_signature,
                                      "right", &right, error);
    if (status != QL_STATUS_OK) {
        problem_signature_dispose(&left);
        return status;
    }
    status = signature_names_function(&left, definition->left_function_name,
                                      definition->left_function_name_size,
                                      "left", error);
    if (status == QL_STATUS_OK) {
        status = signature_names_function(
            &right, definition->right_function_name,
            definition->right_function_name_size, "right", error);
    }
    if (status == QL_STATUS_OK) {
        status = validate_argument_bindings(&left, &right,
                                            definition->argument_bindings,
                                            definition->argument_binding_count,
                                            error);
    }
    if (status == QL_STATUS_OK) {
        status = compute_precondition_digest(
            selected, &left, definition->contract.precondition_json,
            definition->contract.precondition_json_size,
            &precondition_digest, error);
    }
    memset(&left_view, 0, sizeof(left_view));
    memset(&right_view, 0, sizeof(right_view));
    left_view.struct_size = sizeof(left_view);
    right_view.struct_size = sizeof(right_view);
    if (status == QL_STATUS_OK) {
        status = ql_artifact_get_view(left.artifact, &left_view, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_artifact_get_view(right.artifact, &right_view, error);
    }
    if (status != QL_STATUS_OK) {
        problem_signature_dispose(&left);
        problem_signature_dispose(&right);
        return status;
    }

    allocator_copy = *selected;
    json_allocator = problem_json_allocator(&allocator_copy);
    status = canonicalize_precondition(&definition->contract, &json_allocator,
                                       &precondition, &precondition_size,
                                       error);
    if (status != QL_STATUS_OK) {
        problem_signature_dispose(&left);
        problem_signature_dispose(&right);
        return status;
    }

    document = yyjson_mut_doc_new(&json_allocator);
    root = document != NULL ? yyjson_mut_obj(document) : NULL;
    contract = document != NULL ? yyjson_mut_obj(document) : NULL;
    left_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    right_object = document != NULL ? yyjson_mut_obj(document) : NULL;
    bindings = document != NULL ? yyjson_mut_arr(document) : NULL;
    if (document == NULL || root == NULL || contract == NULL ||
        left_object == NULL || right_object == NULL || bindings == NULL ||
        !yyjson_mut_obj_add_str(document, root, "kind",
                                QL_ARTIFACT_KIND_PROBLEM) ||
        !yyjson_mut_obj_add_uint(document, root, "schema_version",
                                 QL_PROBLEM_SCHEMA_VERSION_2) ||
        !add_contract_json(document, contract, &definition->contract,
                           precondition, precondition_size) ||
        !yyjson_mut_obj_add_val(document, root, "contract", contract) ||
        !add_function_json_v2(document, left_object,
                              definition->left_function_name,
                              definition->left_function_name_size,
                              definition->left_source,
                              definition->left_source_size, &left_view) ||
        !yyjson_mut_obj_add_val(document, root, "left", left_object) ||
        !add_function_json_v2(document, right_object,
                              definition->right_function_name,
                              definition->right_function_name_size,
                              definition->right_source,
                              definition->right_source_size, &right_view) ||
        !yyjson_mut_obj_add_val(document, root, "right", right_object) ||
        !add_bindings_json(document, bindings, definition->argument_bindings,
                           definition->argument_binding_count) ||
        !yyjson_mut_obj_add_val(document, root, "argument_bindings",
                                bindings) ||
        !add_digest_json(document, root, "precondition_digest",
                         &precondition_digest)) {
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
                                QL_PROBLEM_SCHEMA_VERSION_2, json, json_size,
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
    problem_signature_dispose(&left);
    problem_signature_dispose(&right);
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

/* Re-derives every v2 binding from the payload and compares it with the
   recorded digests, so an edited problem artifact cannot present a
   correspondence or precondition meaning it does not actually carry. */
static ql_status read_problem_bindings_v2(ql_problem *problem,
                                          ql_error *error) {
    yyjson_val *root = yyjson_doc_get_root(problem->document);
    yyjson_val *left_object = yyjson_obj_get(root, "left");
    yyjson_val *right_object = yyjson_obj_get(root, "right");
    yyjson_val *bindings = yyjson_obj_get(root, "argument_bindings");
    problem_signature left = { 0 };
    problem_signature right = { 0 };
    ql_digest recorded_left;
    ql_digest recorded_right;
    ql_digest recorded_precondition;
    ql_digest computed_precondition;
    ql_artifact_view left_view;
    ql_artifact_view right_view;
    const char *bytes;
    size_t size;
    size_t count;
    size_t index;
    ql_status status;

    if (!yyjson_is_arr(bindings) ||
        !read_digest_json(root, "precondition_digest",
                          &recorded_precondition) ||
        !read_digest_json(left_object, "signature_digest", &recorded_left) ||
        !read_digest_json(right_object, "signature_digest",
                          &recorded_right)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "problem JSON does not match schema version 2");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (!get_required_string(left_object, "signature", &bytes, &size)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "left source signature payload is missing");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    status = problem_signature_adopt(&problem->allocator, bytes, size, &left,
                                     error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (!get_required_string(right_object, "signature", &bytes, &size)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "right source signature payload is missing");
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto cleanup;
    }
    status = problem_signature_adopt(&problem->allocator, bytes, size, &right,
                                     error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    memset(&left_view, 0, sizeof(left_view));
    memset(&right_view, 0, sizeof(right_view));
    left_view.struct_size = sizeof(left_view);
    right_view.struct_size = sizeof(right_view);
    status = ql_artifact_get_view(left.artifact, &left_view, error);
    if (status == QL_STATUS_OK) {
        status = ql_artifact_get_view(right.artifact, &right_view, error);
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    if (ql_digest_equal(&left_view.digest, &recorded_left) == 0u ||
        ql_digest_equal(&right_view.digest, &recorded_right) == 0u) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "a recorded source-signature digest does not match its payload");
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto cleanup;
    }
    status = signature_names_function(&left, problem->view.left_function_name,
                                      problem->view.left_function_name_size,
                                      "left", error);
    if (status == QL_STATUS_OK) {
        status = signature_names_function(
            &right, problem->view.right_function_name,
            problem->view.right_function_name_size, "right", error);
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    count = yyjson_arr_size(bindings);
    if (count > QL_SOURCE_SIGNATURE_MAX_ARGUMENTS) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "problem records more than %u argument bindings",
                     QL_SOURCE_SIGNATURE_MAX_ARGUMENTS);
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto cleanup;
    }
    if (count != 0u) {
        problem->bindings = problem->allocator.allocate(
            problem->allocator.user_data,
            count * sizeof(*problem->bindings));
        if (problem->bindings == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            status = QL_STATUS_OUT_OF_MEMORY;
            goto cleanup;
        }
    }
    for (index = 0u; index < count; ++index) {
        yyjson_val *entry = yyjson_arr_get(bindings, index);
        uint64_t left_index;
        uint64_t right_index;
        if (!yyjson_is_obj(entry) || yyjson_obj_size(entry) != 2u ||
            !get_uint64_field(entry, "left", UINT32_MAX, &left_index) ||
            !get_uint64_field(entry, "right", UINT32_MAX, &right_index)) {
            ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                         "argument binding %zu is malformed", index);
            status = QL_STATUS_SCHEMA_MISMATCH;
            goto cleanup;
        }
        memset(&problem->bindings[index], 0, sizeof(problem->bindings[index]));
        problem->bindings[index].struct_size =
            sizeof(problem->bindings[index]);
        problem->bindings[index].left_index = (uint32_t)left_index;
        problem->bindings[index].right_index = (uint32_t)right_index;
    }
    status = validate_argument_bindings(&left, &right, problem->bindings,
                                        count, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = compute_precondition_digest(
        &problem->allocator, &left, problem->view.contract.precondition_json,
        problem->view.contract.precondition_json_size,
        &computed_precondition, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    if (ql_digest_equal(&computed_precondition, &recorded_precondition) ==
        0u) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "recorded typed-precondition digest does not match the contract and left signature");
        status = QL_STATUS_SCHEMA_MISMATCH;
        goto cleanup;
    }

    problem->view2.struct_size = sizeof(problem->view2);
    problem->view2.schema_version = problem->view.schema_version;
    problem->view2.contract = problem->view.contract;
    problem->view2.left_source = problem->view.left_source;
    problem->view2.left_source_size = problem->view.left_source_size;
    problem->view2.left_function_name = problem->view.left_function_name;
    problem->view2.left_function_name_size =
        problem->view.left_function_name_size;
    problem->view2.right_source = problem->view.right_source;
    problem->view2.right_source_size = problem->view.right_source_size;
    problem->view2.right_function_name = problem->view.right_function_name;
    problem->view2.right_function_name_size =
        problem->view.right_function_name_size;
    problem->view2.left_signature_digest = recorded_left;
    problem->view2.right_signature_digest = recorded_right;
    problem->view2.precondition_digest = recorded_precondition;
    problem->view2.argument_binding_count = count;
    problem->view2.artifact_digest = problem->view.artifact_digest;
    problem->left_signature_artifact = left.artifact;
    problem->right_signature_artifact = right.artifact;
    left.artifact = NULL;
    right.artifact = NULL;

cleanup:
    problem_signature_dispose(&left);
    problem_signature_dispose(&right);
    return status;
}

static ql_status read_problem_view(ql_problem *problem,
                                   const ql_artifact_view *artifact_view,
                                   ql_error *error) {
    yyjson_val *root = yyjson_doc_get_root(problem->document);
    yyjson_val *kind;
    yyjson_val *contract;
    yyjson_val *left;
    yyjson_val *right;
    const int is_v2 =
        artifact_view->schema_version == QL_PROBLEM_SCHEMA_VERSION_2;
    const size_t root_fields = is_v2 ? 7u : 5u;
    const size_t function_fields = is_v2 ? 4u : 2u;
    uint64_t schema_version;
    ql_status status;

    kind = yyjson_obj_get(root, "kind");
    contract = yyjson_obj_get(root, "contract");
    left = yyjson_obj_get(root, "left");
    right = yyjson_obj_get(root, "right");
    if (!yyjson_is_obj(root) || yyjson_obj_size(root) != root_fields ||
        !json_string_equals(kind, QL_ARTIFACT_KIND_PROBLEM) ||
        !get_uint64_field(root, "schema_version", UINT32_MAX,
                          &schema_version) ||
        schema_version != artifact_view->schema_version ||
        !read_contract_json(contract, &problem->view.contract) ||
        !yyjson_is_obj(left) || yyjson_obj_size(left) != function_fields ||
        !yyjson_is_obj(right) || yyjson_obj_size(right) != function_fields ||
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
                     "problem JSON does not match schema version %u",
                     artifact_view->schema_version);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    status = ql_semantic_contract_validate(&problem->view.contract, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    problem->view.struct_size = sizeof(problem->view);
    problem->view.schema_version = (uint32_t)schema_version;
    problem->view.artifact_digest = artifact_view->digest;
    if (!is_v2) {
        return QL_STATUS_OK;
    }
    return read_problem_bindings_v2(problem, error);
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
    if (artifact_view.schema_version != QL_PROBLEM_SCHEMA_VERSION &&
        artifact_view.schema_version != QL_PROBLEM_SCHEMA_VERSION_2) {
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
        ql_artifact_release(problem->left_signature_artifact);
        ql_artifact_release(problem->right_signature_artifact);
        selected->deallocate(selected->user_data, problem->bindings);
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
    ql_artifact_release(problem->left_signature_artifact);
    ql_artifact_release(problem->right_signature_artifact);
    allocator.deallocate(allocator.user_data, problem->bindings);
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

ql_status QL_CALL ql_problem_get_view_v2(const ql_problem *problem,
                                         ql_problem_view_v2 *view,
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
                     "problem view v2 structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (problem->view.schema_version != QL_PROBLEM_SCHEMA_VERSION_2) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "problem uses schema version %u and carries no signature binding",
                     problem->view.schema_version);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    *view = problem->view2;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_problem_argument_binding_at(
    const ql_problem *problem, size_t index,
    ql_problem_argument_binding_v1 *output, ql_error *error) {
    if (problem == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "problem and binding output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (problem->view.schema_version != QL_PROBLEM_SCHEMA_VERSION_2) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "problem uses schema version %u and records no argument correspondence",
                     problem->view.schema_version);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (index >= problem->view2.argument_binding_count) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "problem has no argument binding %zu", index);
        return QL_STATUS_NOT_FOUND;
    }
    *output = problem->bindings[index];
    ql_error_clear(error);
    return QL_STATUS_OK;
}

const ql_artifact *QL_CALL ql_problem_left_signature_artifact(
    const ql_problem *problem) {
    return problem == NULL ? NULL : problem->left_signature_artifact;
}

const ql_artifact *QL_CALL ql_problem_right_signature_artifact(
    const ql_problem *problem) {
    return problem == NULL ? NULL : problem->right_signature_artifact;
}

ql_status QL_CALL ql_problem_require_proof_binding(const ql_problem *problem,
                                                   ql_error *error) {
    if (problem == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "problem is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (problem->view.schema_version == QL_PROBLEM_SCHEMA_VERSION_2) {
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    if (problem->view.contract.precondition_json != NULL) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "problem schema v1 does not bind its precondition to either source signature, so the precondition is configuration and not proof evidence");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                 "problem schema v1 records no source-signature digests or argument correspondence, so no PROVED verdict is eligible");
    return QL_STATUS_SCHEMA_MISMATCH;
}
