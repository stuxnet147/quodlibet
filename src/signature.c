#include "quodlibet/signature.h"

#include <stdatomic.h>
#include <string.h>

#include "yyjson.h"

/* The precondition vocabulary and this artifact must agree on argument kinds
   or a typed precondition could be checked against a different meaning. */
_Static_assert((int)QL_SOURCE_TYPE_BOOL == (int)QL_SIGNATURE_ARGUMENT_BOOL,
               "source and precondition bool kinds must agree");
_Static_assert((int)QL_SOURCE_TYPE_SIGNED_INTEGER ==
                   (int)QL_SIGNATURE_ARGUMENT_SIGNED_INTEGER,
               "source and precondition signed kinds must agree");
_Static_assert((int)QL_SOURCE_TYPE_UNSIGNED_INTEGER ==
                   (int)QL_SIGNATURE_ARGUMENT_UNSIGNED_INTEGER,
               "source and precondition unsigned kinds must agree");
_Static_assert((int)QL_SOURCE_TYPE_POINTER ==
                   (int)QL_SIGNATURE_ARGUMENT_POINTER,
               "source and precondition pointer kinds must agree");
_Static_assert((int)QL_SOURCE_TYPE_QUALIFIER_CONST ==
                   (int)QL_C_TYPE_QUALIFIER_CONST,
               "source and frontend qualifier bits must agree");
_Static_assert((int)QL_SOURCE_TYPE_QUALIFIER_ATOMIC ==
                   (int)QL_C_TYPE_QUALIFIER_ATOMIC,
               "source and frontend qualifier bits must agree");

struct ql_source_signature {
    atomic_uint reference_count;
    ql_allocator allocator;
    ql_artifact *artifact;
    yyjson_doc *document;
    ql_source_signature_view_v1 view;
    ql_source_type_v1 *arguments;
};

static void *signature_json_allocate(void *context, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    return allocator->allocate(allocator->user_data, size);
}

static void *signature_json_reallocate(void *context, void *pointer,
                                       size_t old_size, size_t size) {
    ql_allocator *allocator = (ql_allocator *)context;
    (void)old_size;
    return allocator->reallocate(allocator->user_data, pointer, size);
}

static void signature_json_deallocate(void *context, void *pointer) {
    ql_allocator *allocator = (ql_allocator *)context;
    allocator->deallocate(allocator->user_data, pointer);
}

static yyjson_alc signature_json_allocator(ql_allocator *allocator) {
    yyjson_alc result;
    result.malloc = signature_json_allocate;
    result.realloc = signature_json_reallocate;
    result.free = signature_json_deallocate;
    result.ctx = allocator;
    return result;
}

static const ql_allocator *select_allocator(const ql_allocator *allocator) {
    return allocator == NULL ? ql_default_allocator() : allocator;
}

static int valid_pointer_width(uint32_t width) {
    return width == 16u || width == 32u || width == 64u;
}

static ql_status validate_type(const ql_source_type_v1 *type,
                               uint32_t pointer_width, const char *role,
                               ql_error *error) {
    if (type->struct_size < sizeof(*type)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "source %s type structure is too small", role);
        return QL_STATUS_ABI_MISMATCH;
    }
    if ((type->qualifiers & ~(uint32_t)QL_SOURCE_TYPE_QUALIFIER_ALL) != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source %s type has unknown qualifier bits", role);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    switch (type->kind) {
    case QL_SOURCE_TYPE_VOID:
        if (type->bit_width != 0u || type->address_space != 0u ||
            type->pointer_depth != 0u) {
            break;
        }
        return QL_STATUS_OK;
    case QL_SOURCE_TYPE_BOOL:
        if (type->bit_width != 1u || type->address_space != 0u ||
            type->pointer_depth != 0u) {
            break;
        }
        return QL_STATUS_OK;
    case QL_SOURCE_TYPE_SIGNED_INTEGER:
    case QL_SOURCE_TYPE_UNSIGNED_INTEGER:
        if (type->bit_width == 0u ||
            type->bit_width > QL_PRECONDITION_MAX_INTEGER_BITS ||
            (type->bit_width % 8u) != 0u || type->address_space != 0u ||
            type->pointer_depth != 0u) {
            break;
        }
        return QL_STATUS_OK;
    case QL_SOURCE_TYPE_POINTER:
        if (type->bit_width != pointer_width || type->pointer_depth == 0u) {
            break;
        }
        return QL_STATUS_OK;
    default:
        break;
    }
    ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                 "source %s type is not a valid schema v1 type", role);
    return QL_STATUS_INVALID_ARGUMENT;
}

static ql_status validate_definition(
    const ql_source_signature_definition_v1 *definition, ql_error *error) {
    size_t index;
    ql_status status;

    if (definition == NULL || definition->struct_size < sizeof(*definition)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source-signature definition v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (definition->schema_version != QL_SOURCE_SIGNATURE_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "unsupported source-signature schema version %u",
                     definition->schema_version);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (definition->c_dialect != QL_C_DIALECT_ASM2C_GNU_V1 ||
        definition->target_abi != QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source signature requires a known C dialect and target ABI");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (!valid_pointer_width(definition->pointer_width)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source-signature pointer width %u is not supported",
                     definition->pointer_width);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (definition->function_name == NULL ||
        definition->function_name_size == 0u ||
        memchr(definition->function_name, '\0',
               definition->function_name_size) != NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source signature requires a NUL-free function name");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (definition->argument_count > QL_SOURCE_SIGNATURE_MAX_ARGUMENTS ||
        (definition->argument_count != 0u && definition->arguments == NULL)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source signature accepts at most %u arguments",
                     QL_SOURCE_SIGNATURE_MAX_ARGUMENTS);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_type(&definition->return_type,
                          definition->pointer_width, "return", error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < definition->argument_count; ++index) {
        status = validate_type(&definition->arguments[index],
                              definition->pointer_width, "argument", error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (definition->arguments[index].kind == QL_SOURCE_TYPE_VOID) {
            ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                         "argument %zu cannot have void type", index);
            return QL_STATUS_INVALID_ARGUMENT;
        }
    }
    return QL_STATUS_OK;
}

static int add_type_json(yyjson_mut_doc *document, yyjson_mut_val *parent,
                         const char *key, const ql_source_type_v1 *type) {
    yyjson_mut_val *object = yyjson_mut_obj(document);
    if (object == NULL ||
        !yyjson_mut_obj_add_uint(document, object, "kind",
                                 (uint64_t)type->kind) ||
        !yyjson_mut_obj_add_uint(document, object, "bit_width",
                                 type->bit_width) ||
        !yyjson_mut_obj_add_uint(document, object, "address_space",
                                 type->address_space) ||
        !yyjson_mut_obj_add_uint(document, object, "qualifiers",
                                 type->qualifiers) ||
        !yyjson_mut_obj_add_uint(document, object, "pointer_depth",
                                 type->pointer_depth)) {
        return 0;
    }
    if (key == NULL) {
        return yyjson_mut_arr_append(parent, object);
    }
    return yyjson_mut_obj_add_val(document, parent, key, object);
}

void QL_CALL ql_source_type_init(ql_source_type_v1 *type,
                                 ql_source_type_kind kind) {
    if (type == NULL) {
        return;
    }
    memset(type, 0, sizeof(*type));
    type->struct_size = sizeof(*type);
    type->kind = kind;
    if (kind == QL_SOURCE_TYPE_BOOL) {
        type->bit_width = 1u;
    }
}

void QL_CALL ql_source_signature_definition_init(
    ql_source_signature_definition_v1 *definition) {
    if (definition == NULL) {
        return;
    }
    memset(definition, 0, sizeof(*definition));
    definition->struct_size = sizeof(*definition);
    definition->schema_version = QL_SOURCE_SIGNATURE_SCHEMA_VERSION;
    definition->c_dialect = QL_C_DIALECT_ASM2C_GNU_V1;
    definition->target_abi = QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64;
    definition->pointer_width = 64u;
    ql_source_type_init(&definition->return_type, QL_SOURCE_TYPE_VOID);
}

ql_status QL_CALL ql_source_signature_artifact_create(
    const ql_allocator *allocator,
    const ql_source_signature_definition_v1 *definition,
    ql_artifact **output, ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_allocator allocator_copy;
    yyjson_alc json_allocator;
    yyjson_mut_doc *document = NULL;
    yyjson_mut_val *root;
    yyjson_mut_val *arguments;
    yyjson_write_err write_error;
    char *json = NULL;
    size_t json_size = 0u;
    size_t index;
    ql_status status;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source-signature artifact output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    status = validate_definition(definition, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    allocator_copy = *selected;
    json_allocator = signature_json_allocator(&allocator_copy);

    document = yyjson_mut_doc_new(&json_allocator);
    root = document != NULL ? yyjson_mut_obj(document) : NULL;
    arguments = document != NULL ? yyjson_mut_arr(document) : NULL;
    if (document == NULL || root == NULL || arguments == NULL ||
        !yyjson_mut_obj_add_str(document, root, "kind",
                                QL_ARTIFACT_KIND_SOURCE_SIGNATURE) ||
        !yyjson_mut_obj_add_uint(document, root, "schema_version",
                                 QL_SOURCE_SIGNATURE_SCHEMA_VERSION) ||
        !yyjson_mut_obj_add_uint(document, root, "c_dialect",
                                 (uint64_t)definition->c_dialect) ||
        !yyjson_mut_obj_add_uint(document, root, "target_abi",
                                 (uint64_t)definition->target_abi) ||
        !yyjson_mut_obj_add_uint(document, root, "pointer_width",
                                 definition->pointer_width) ||
        !yyjson_mut_obj_add_strncpy(document, root, "function_name",
                                    definition->function_name,
                                    definition->function_name_size) ||
        !add_type_json(document, root, "return_type",
                       &definition->return_type)) {
        status = QL_STATUS_OUT_OF_MEMORY;
        ql_error_set(error, status, NULL);
        goto cleanup;
    }
    for (index = 0u; index < definition->argument_count; ++index) {
        if (!add_type_json(document, arguments, NULL,
                           &definition->arguments[index])) {
            status = QL_STATUS_OUT_OF_MEMORY;
            ql_error_set(error, status, NULL);
            goto cleanup;
        }
    }
    if (!yyjson_mut_obj_add_val(document, root, "arguments", arguments)) {
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
        ql_error_set(error, status,
                     "cannot serialize source-signature JSON: %s",
                     write_error.msg != NULL ? write_error.msg
                                             : "JSON write error");
        goto cleanup;
    }
    status = ql_artifact_create(selected, QL_ARTIFACT_KIND_SOURCE_SIGNATURE,
                                QL_SOURCE_SIGNATURE_SCHEMA_VERSION, json,
                                json_size, output, error);

cleanup:
    if (json != NULL) {
        json_allocator.free(json_allocator.ctx, json);
    }
    if (document != NULL) {
        yyjson_mut_doc_free(document);
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

static int read_type_json(yyjson_val *object, ql_source_type_v1 *type) {
    uint64_t value;

    if (!yyjson_is_obj(object) || yyjson_obj_size(object) != 5u) {
        return 0;
    }
    memset(type, 0, sizeof(*type));
    type->struct_size = sizeof(*type);
    if (!get_uint64_field(object, "kind", (uint64_t)QL_SOURCE_TYPE_POINTER,
                          &value)) {
        return 0;
    }
    type->kind = (ql_source_type_kind)value;
    if (!get_uint64_field(object, "bit_width", UINT32_MAX, &value)) {
        return 0;
    }
    type->bit_width = (uint32_t)value;
    if (!get_uint64_field(object, "address_space", UINT32_MAX, &value)) {
        return 0;
    }
    type->address_space = (uint32_t)value;
    if (!get_uint64_field(object, "qualifiers", UINT32_MAX, &value)) {
        return 0;
    }
    type->qualifiers = (uint32_t)value;
    if (!get_uint64_field(object, "pointer_depth", UINT32_MAX, &value)) {
        return 0;
    }
    type->pointer_depth = (uint32_t)value;
    return 1;
}

static ql_status read_signature_view(ql_source_signature *signature,
                                     const ql_artifact_view *artifact_view,
                                     ql_error *error) {
    yyjson_val *root = yyjson_doc_get_root(signature->document);
    yyjson_val *arguments;
    yyjson_val *name;
    uint64_t value;
    size_t count;
    size_t index;
    ql_status status;

    arguments = yyjson_obj_get(root, "arguments");
    name = yyjson_obj_get(root, "function_name");
    if (!yyjson_is_obj(root) || yyjson_obj_size(root) != 8u ||
        !json_string_equals(yyjson_obj_get(root, "kind"),
                            QL_ARTIFACT_KIND_SOURCE_SIGNATURE) ||
        !get_uint64_field(root, "schema_version", UINT32_MAX, &value) ||
        value != QL_SOURCE_SIGNATURE_SCHEMA_VERSION || !yyjson_is_str(name) ||
        yyjson_get_len(name) == 0u ||
        memchr(yyjson_get_str(name), '\0', yyjson_get_len(name)) != NULL ||
        !yyjson_is_arr(arguments) ||
        !read_type_json(yyjson_obj_get(root, "return_type"),
                        &signature->view.return_type)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "source-signature JSON does not match schema version 1");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    signature->view.schema_version = QL_SOURCE_SIGNATURE_SCHEMA_VERSION;
    signature->view.function_name = yyjson_get_str(name);
    signature->view.function_name_size = yyjson_get_len(name);
    if (!get_uint64_field(root, "c_dialect", UINT32_MAX, &value)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "source-signature C dialect is invalid");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    signature->view.c_dialect = (ql_c_dialect_profile)value;
    if (!get_uint64_field(root, "target_abi", UINT32_MAX, &value)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "source-signature target ABI is invalid");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    signature->view.target_abi = (ql_target_abi)value;
    if (!get_uint64_field(root, "pointer_width", UINT32_MAX, &value)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "source-signature pointer width is invalid");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    signature->view.pointer_width = (uint32_t)value;
    if (signature->view.c_dialect != QL_C_DIALECT_ASM2C_GNU_V1 ||
        signature->view.target_abi !=
            QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64 ||
        !valid_pointer_width(signature->view.pointer_width)) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "source-signature ABI profile is not supported");
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    status = validate_type(&signature->view.return_type,
                          signature->view.pointer_width, "return", error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    count = yyjson_arr_size(arguments);
    if (count > QL_SOURCE_SIGNATURE_MAX_ARGUMENTS) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "source signature declares more than %u arguments",
                     QL_SOURCE_SIGNATURE_MAX_ARGUMENTS);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (count != 0u) {
        signature->arguments = signature->allocator.allocate(
            signature->allocator.user_data,
            count * sizeof(*signature->arguments));
        if (signature->arguments == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
    }
    for (index = 0u; index < count; ++index) {
        if (!read_type_json(yyjson_arr_get(arguments, index),
                            &signature->arguments[index])) {
            ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                         "source-signature argument %zu is malformed", index);
            return QL_STATUS_SCHEMA_MISMATCH;
        }
        status = validate_type(&signature->arguments[index],
                              signature->view.pointer_width, "argument",
                              error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (signature->arguments[index].kind == QL_SOURCE_TYPE_VOID) {
            ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                         "source-signature argument %zu has void type", index);
            return QL_STATUS_SCHEMA_MISMATCH;
        }
    }
    signature->view.struct_size = sizeof(signature->view);
    signature->view.argument_count = count;
    signature->view.artifact_digest = artifact_view->digest;
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_source_signature_open(const ql_allocator *allocator,
                                           const ql_artifact *artifact,
                                           ql_source_signature **output,
                                           ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_artifact_view artifact_view;
    ql_source_signature *signature;
    yyjson_alc json_allocator;
    yyjson_read_err read_error;
    ql_status status;

    if (output == NULL || artifact == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "artifact and source-signature output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    memset(&artifact_view, 0, sizeof(artifact_view));
    artifact_view.struct_size = sizeof(artifact_view);
    status = ql_artifact_get_view(artifact, &artifact_view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (strcmp(artifact_view.kind, QL_ARTIFACT_KIND_SOURCE_SIGNATURE) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "artifact is not a quodlibet.source-signature");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (artifact_view.schema_version !=
        QL_SOURCE_SIGNATURE_SCHEMA_VERSION) {
        ql_error_set(error, QL_STATUS_SCHEMA_MISMATCH,
                     "unsupported source-signature artifact schema version %u",
                     artifact_view.schema_version);
        return QL_STATUS_SCHEMA_MISMATCH;
    }
    if (artifact_view.size == 0u ||
        yyjson_read_max_memory_usage(artifact_view.size, 0u) == 0u) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "source-signature artifact JSON size is invalid");
        return QL_STATUS_PARSE_ERROR;
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    signature = selected->allocate(selected->user_data, sizeof(*signature));
    if (signature == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(signature, 0, sizeof(*signature));
    signature->allocator = *selected;
    json_allocator = signature_json_allocator(&signature->allocator);
    signature->document = yyjson_read_opts(
        (char *)(uintptr_t)artifact_view.data, artifact_view.size, 0u,
        &json_allocator, &read_error);
    if (signature->document == NULL) {
        status = read_error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
                     ? QL_STATUS_OUT_OF_MEMORY
                     : QL_STATUS_PARSE_ERROR;
        ql_error_set(error, status,
                     "invalid source-signature JSON at byte %zu: %s",
                     read_error.pos,
                     read_error.msg != NULL ? read_error.msg : "parse error");
        selected->deallocate(selected->user_data, signature);
        return status;
    }
    status = read_signature_view(signature, &artifact_view, error);
    if (status != QL_STATUS_OK) {
        selected->deallocate(selected->user_data, signature->arguments);
        yyjson_doc_free(signature->document);
        selected->deallocate(selected->user_data, signature);
        return status;
    }
    ql_artifact_retain((ql_artifact *)(uintptr_t)artifact);
    signature->artifact = (ql_artifact *)(uintptr_t)artifact;
    atomic_init(&signature->reference_count, 1u);
    *output = signature;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_source_signature_retain(ql_source_signature *signature) {
    if (signature != NULL) {
        (void)atomic_fetch_add_explicit(&signature->reference_count, 1u,
                                        memory_order_relaxed);
    }
}

void QL_CALL ql_source_signature_release(ql_source_signature *signature) {
    ql_allocator allocator;
    if (signature == NULL ||
        atomic_fetch_sub_explicit(&signature->reference_count, 1u,
                                  memory_order_acq_rel) != 1u) {
        return;
    }
    allocator = signature->allocator;
    allocator.deallocate(allocator.user_data, signature->arguments);
    yyjson_doc_free(signature->document);
    ql_artifact_release(signature->artifact);
    allocator.deallocate(allocator.user_data, signature);
}

ql_status QL_CALL ql_source_signature_get_view(
    const ql_source_signature *signature,
    ql_source_signature_view_v1 *view, ql_error *error) {
    size_t caller_size;

    if (signature == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source signature and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    caller_size = view->struct_size;
    if (caller_size != 0u && caller_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "source-signature view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    *view = signature->view;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_source_signature_argument_at(
    const ql_source_signature *signature, size_t index,
    ql_source_type_v1 *output, ql_error *error) {
    if (signature == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source signature and argument output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (index >= signature->view.argument_count) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "source signature has no argument %zu", index);
        return QL_STATUS_NOT_FOUND;
    }
    *output = signature->arguments[index];
    ql_error_clear(error);
    return QL_STATUS_OK;
}

const ql_artifact *QL_CALL ql_source_signature_artifact(
    const ql_source_signature *signature) {
    return signature == NULL ? NULL : signature->artifact;
}

ql_status QL_CALL ql_source_signature_precondition_view(
    const ql_source_signature *signature, ql_signature_view_v1 *view,
    ql_signature_argument_v1 *storage, size_t storage_capacity,
    ql_error *error) {
    size_t index;

    if (signature == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source signature and precondition view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (signature->view.argument_count != 0u &&
        (storage == NULL || storage_capacity < signature->view.argument_count)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "precondition signature storage needs %zu arguments",
                     signature->view.argument_count);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < signature->view.argument_count; ++index) {
        const ql_source_type_v1 *type = &signature->arguments[index];
        ql_signature_argument_init(&storage[index]);
        storage[index].kind = (ql_signature_argument_kind)type->kind;
        storage[index].bit_width = type->kind == QL_SOURCE_TYPE_POINTER
                                       ? signature->view.pointer_width
                                       : type->bit_width;
        storage[index].address_space = type->address_space;
    }
    ql_signature_view_init(view);
    view->pointer_width = signature->view.pointer_width;
    view->arguments = signature->view.argument_count != 0u ? storage : NULL;
    view->argument_count = signature->view.argument_count;
    return ql_signature_view_validate(view, error);
}

ql_status QL_CALL ql_source_type_compatible(const ql_source_type_v1 *left,
                                            const ql_source_type_v1 *right,
                                            ql_error *error) {
    if (left == NULL || right == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "two source types are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (left->kind != right->kind || left->bit_width != right->bit_width ||
        left->address_space != right->address_space ||
        left->pointer_depth != right->pointer_depth) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "source types differ: kind %u/%u, width %u/%u, address space %u/%u, pointer depth %u/%u",
                     (unsigned)left->kind, (unsigned)right->kind,
                     left->bit_width, right->bit_width, left->address_space,
                     right->address_space, left->pointer_depth,
                     right->pointer_depth);
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* --- Derivation from the analyzed C frontend ------------------------------ */

typedef struct signature_integer_entry {
    const char *spelling;
    uint32_t bit_width;
    uint32_t is_signed;
} signature_integer_entry;

/* Frozen ASM2C_GNU_V1 table on x86-64 Linux SysV LP64. Plain `char` is signed
   on this target. Nothing outside this table receives a guessed meaning. */
static const signature_integer_entry signature_integer_table[] = {
    { "char", 8u, 1u },
    { "signedchar", 8u, 1u },
    { "unsignedchar", 8u, 0u },
    { "short", 16u, 1u },
    { "shortint", 16u, 1u },
    { "signedshort", 16u, 1u },
    { "signedshortint", 16u, 1u },
    { "unsignedshort", 16u, 0u },
    { "unsignedshortint", 16u, 0u },
    { "int", 32u, 1u },
    { "signed", 32u, 1u },
    { "signedint", 32u, 1u },
    { "unsigned", 32u, 0u },
    { "unsignedint", 32u, 0u },
    { "long", 64u, 1u },
    { "longint", 64u, 1u },
    { "signedlong", 64u, 1u },
    { "signedlongint", 64u, 1u },
    { "unsignedlong", 64u, 0u },
    { "unsignedlongint", 64u, 0u },
    { "longlong", 64u, 1u },
    { "longlongint", 64u, 1u },
    { "signedlonglong", 64u, 1u },
    { "signedlonglongint", 64u, 1u },
    { "unsignedlonglong", 64u, 0u },
    { "unsignedlonglongint", 64u, 0u }
};

static int normalize_spelling(const char *spelling, char *output,
                              size_t capacity) {
    size_t source_index;
    size_t target_index = 0u;

    if (spelling == NULL) {
        return 0;
    }
    for (source_index = 0u; spelling[source_index] != '\0'; ++source_index) {
        const char ch = spelling[source_index];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
            ch == '\f' || ch == '\v') {
            continue;
        }
        if (target_index + 1u >= capacity) {
            return 0;
        }
        output[target_index++] = ch;
    }
    output[target_index] = '\0';
    return 1;
}

/* --- Typedef resolution --------------------------------------------------- */

/* The corpus this profile serves is anonymized decompiler output, where every
   scalar arrives through a `typedef int TYP_0;` chain. Without resolution the
   signature refuses names the lowering happily accepts, and a function whose
   IR exists gets no signature to describe it.

   The resolution is deliberately its own walk of the syntax tree rather than a
   call into the lowering. `ql_source_signature_bind_ir` is only a real
   cross-check while the two derive the same fact independently; sharing the
   code would make it a restatement. */

#define QL_SIGNATURE_SPELLING_CAPACITY 64u
#define QL_SIGNATURE_TYPEDEF_DEPTH 64u

typedef struct signature_typedef {
    char name[QL_SIGNATURE_SPELLING_CAPACITY];
    char underlying[QL_SIGNATURE_SPELLING_CAPACITY];
    /* The declarator adds a pointer, an array, or a function. */
    uint32_t is_indirect;
    /* The underlying type is a struct, union, or enum specifier. */
    uint32_t is_aggregate;
} signature_typedef;

typedef struct signature_typedefs {
    ql_allocator allocator;
    signature_typedef *items;
    size_t count;
    size_t capacity;
} signature_typedefs;

static void typedefs_dispose(signature_typedefs *table) {
    table->allocator.deallocate(table->allocator.user_data, table->items);
    table->items = NULL;
    table->count = 0u;
    table->capacity = 0u;
}

static int typedefs_add(signature_typedefs *table,
                        const signature_typedef *entry) {
    if (table->count == table->capacity) {
        const size_t capacity = table->capacity == 0u ? 16u
                                                      : table->capacity * 2u;
        void *allocation = table->allocator.reallocate(
            table->allocator.user_data, table->items,
            capacity * sizeof(*table->items));
        if (allocation == NULL) {
            return 0;
        }
        table->items = (signature_typedef *)allocation;
        table->capacity = capacity;
    }
    table->items[table->count++] = *entry;
    return 1;
}

static int copy_node_spelling(const char *source, size_t source_size,
                              const ql_source_range *range, char *output,
                              size_t capacity) {
    size_t length;

    if (range->end_byte < range->start_byte ||
        (size_t)range->end_byte > source_size) {
        return 0;
    }
    length = (size_t)(range->end_byte - range->start_byte);
    if (length + 1u > capacity) {
        return 0;
    }
    memcpy(output, source + range->start_byte, length);
    output[length] = '\0';
    return 1;
}

static int node_kind_is(const ql_c_syntax_node_view *view, const char *kind) {
    return view->kind != NULL && strcmp(view->kind, kind) == 0;
}

static int node_field_is(const ql_c_syntax_node_view *view,
                         const char *field) {
    return view->field_name != NULL && strcmp(view->field_name, field) == 0;
}

static ql_status cursor_view(const ql_c_syntax_cursor *cursor,
                             ql_c_syntax_node_view *view, ql_error *error) {
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    return ql_c_syntax_cursor_current(cursor, view, error);
}

/* Descends a declarator to the identifier it binds, reporting whether the path
   introduced indirection. The cursor is left where it started. */
static int declarator_name(ql_c_syntax_cursor *cursor, const char *source,
                           size_t source_size, char *name, size_t capacity,
                           uint32_t *is_indirect, ql_error *error) {
    size_t depth = 0u;
    size_t guard = 0u;
    int found = 0;

    *is_indirect = 0u;
    while (guard++ < QL_SIGNATURE_TYPEDEF_DEPTH) {
        ql_c_syntax_node_view view;
        int descended = 0;

        if (cursor_view(cursor, &view, error) != QL_STATUS_OK) {
            break;
        }
        if (node_kind_is(&view, "type_identifier") ||
            node_kind_is(&view, "identifier")) {
            found = copy_node_spelling(source, source_size, &view.range, name,
                                       capacity);
            break;
        }
        if (node_kind_is(&view, "pointer_declarator") ||
            node_kind_is(&view, "array_declarator") ||
            node_kind_is(&view, "function_declarator")) {
            *is_indirect = 1u;
        } else if (!node_kind_is(&view, "parenthesized_declarator")) {
            break;
        }
        if (ql_c_syntax_cursor_goto_first_child(cursor) == 0u) {
            break;
        }
        ++depth;
        do {
            if (cursor_view(cursor, &view, error) == QL_STATUS_OK &&
                node_field_is(&view, "declarator")) {
                descended = 1;
                break;
            }
        } while (ql_c_syntax_cursor_goto_next_sibling(cursor) != 0u);
        if (!descended) {
            break;
        }
    }
    while (depth-- != 0u) {
        (void)ql_c_syntax_cursor_goto_parent(cursor);
    }
    return found;
}

/* Reads one `type_definition` node. The cursor is left where it started. */
static ql_status collect_one_typedef(ql_c_syntax_cursor *cursor,
                                     const char *source, size_t source_size,
                                     signature_typedefs *table,
                                     ql_error *error) {
    char underlying[QL_SIGNATURE_SPELLING_CAPACITY];
    uint32_t is_aggregate = 0u;
    int has_underlying = 0;

    if (ql_c_syntax_cursor_goto_first_child(cursor) == 0u) {
        return QL_STATUS_OK;
    }
    do {
        ql_c_syntax_node_view view;
        if (cursor_view(cursor, &view, error) != QL_STATUS_OK ||
            !node_field_is(&view, "type")) {
            continue;
        }
        is_aggregate = (node_kind_is(&view, "struct_specifier") ||
                        node_kind_is(&view, "union_specifier") ||
                        node_kind_is(&view, "enum_specifier"))
                           ? 1u
                           : 0u;
        has_underlying = copy_node_spelling(source, source_size, &view.range,
                                            underlying, sizeof(underlying));
        break;
    } while (ql_c_syntax_cursor_goto_next_sibling(cursor) != 0u);
    (void)ql_c_syntax_cursor_goto_parent(cursor);
    if (!has_underlying) {
        return QL_STATUS_OK;
    }

    if (ql_c_syntax_cursor_goto_first_child(cursor) == 0u) {
        return QL_STATUS_OK;
    }
    do {
        ql_c_syntax_node_view view;
        signature_typedef entry;
        if (cursor_view(cursor, &view, error) != QL_STATUS_OK ||
            !node_field_is(&view, "declarator")) {
            continue;
        }
        memset(&entry, 0, sizeof(entry));
        if (!declarator_name(cursor, source, source_size, entry.name,
                             sizeof(entry.name), &entry.is_indirect, error)) {
            continue;
        }
        entry.is_aggregate = is_aggregate;
        memcpy(entry.underlying, underlying, sizeof(underlying));
        if (!typedefs_add(table, &entry)) {
            (void)ql_c_syntax_cursor_goto_parent(cursor);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
    } while (ql_c_syntax_cursor_goto_next_sibling(cursor) != 0u);
    (void)ql_c_syntax_cursor_goto_parent(cursor);
    return QL_STATUS_OK;
}

static ql_status collect_typedefs_at(ql_c_syntax_cursor *cursor,
                                     const char *source, size_t source_size,
                                     signature_typedefs *table,
                                     ql_error *error) {
    ql_c_syntax_node_view view;
    ql_status status = cursor_view(cursor, &view, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    if (node_kind_is(&view, "type_definition")) {
        status = collect_one_typedef(cursor, source, source_size, table,
                                     error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    if (ql_c_syntax_cursor_goto_first_child(cursor) == 0u) {
        return QL_STATUS_OK;
    }
    do {
        status = collect_typedefs_at(cursor, source, source_size, table,
                                     error);
        if (status != QL_STATUS_OK) {
            break;
        }
    } while (ql_c_syntax_cursor_goto_next_sibling(cursor) != 0u);
    (void)ql_c_syntax_cursor_goto_parent(cursor);
    return status;
}

static ql_status collect_typedefs(const ql_allocator *allocator,
                                  const char *source, size_t source_size,
                                  signature_typedefs *table,
                                  ql_error *error) {
    ql_c_parser *parser = NULL;
    ql_c_syntax_tree *tree = NULL;
    ql_c_syntax_cursor *cursor = NULL;
    ql_status status;

    memset(table, 0, sizeof(*table));
    table->allocator = *allocator;
    if (source == NULL || source_size == 0u) {
        return QL_STATUS_OK;
    }
    status = ql_c_parser_create(allocator, &parser, error);
    if (status == QL_STATUS_OK) {
        status = ql_c_parser_parse(parser, source, source_size, &tree, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_c_syntax_cursor_create(tree, &cursor, error);
    }
    if (status == QL_STATUS_OK) {
        status = collect_typedefs_at(cursor, source, source_size, table,
                                     error);
    }
    ql_c_syntax_cursor_destroy(cursor);
    ql_c_syntax_tree_destroy(tree);
    ql_c_parser_destroy(parser);
    if (status != QL_STATUS_OK) {
        typedefs_dispose(table);
    }
    return status;
}

static const signature_typedef *typedefs_find(
    const signature_typedefs *table, const char *normalized) {
    char candidate[QL_SIGNATURE_SPELLING_CAPACITY];
    size_t index;

    for (index = 0u; index < table->count; ++index) {
        if (!normalize_spelling(table->items[index].name, candidate,
                                sizeof(candidate))) {
            continue;
        }
        if (strcmp(candidate, normalized) == 0) {
            return &table->items[index];
        }
    }
    return NULL;
}

static ql_status type_from_inventory(const ql_c_type_inventory_v1 *inventory,
                                     uint32_t pointer_width,
                                     const signature_typedefs *typedefs,
                                     const char *role,
                                     ql_source_type_v1 *output,
                                     ql_error *error) {
    char normalized[QL_SIGNATURE_SPELLING_CAPACITY];
    size_t chain;
    size_t index;

    ql_source_type_init(output, QL_SOURCE_TYPE_VOID);
    output->qualifiers =
        inventory->qualifiers & (uint32_t)QL_SOURCE_TYPE_QUALIFIER_ALL;
    if ((inventory->shape & QL_C_TYPE_SHAPE_FUNCTION) != 0u ||
        (inventory->shape & QL_C_TYPE_SHAPE_ARRAY) != 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s has an array or function type outside source-signature schema v1",
                     role);
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (inventory->pointer_depth != 0u ||
        (inventory->shape & QL_C_TYPE_SHAPE_POINTER) != 0u) {
        output->kind = QL_SOURCE_TYPE_POINTER;
        output->bit_width = pointer_width;
        output->pointer_depth =
            inventory->pointer_depth != 0u ? inventory->pointer_depth : 1u;
        return QL_STATUS_OK;
    }
    if (!normalize_spelling(inventory->base_spelling, normalized,
                            sizeof(normalized))) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "%s type spelling is outside source-signature schema v1",
                     role);
        return QL_STATUS_TYPE_MISMATCH;
    }
    for (chain = 0u; chain < QL_SIGNATURE_TYPEDEF_DEPTH; ++chain) {
        const signature_typedef *entry;

        if (strcmp(normalized, "void") == 0) {
            output->kind = QL_SOURCE_TYPE_VOID;
            return QL_STATUS_OK;
        }
        if (strcmp(normalized, "_Bool") == 0 ||
            strcmp(normalized, "bool") == 0) {
            output->kind = QL_SOURCE_TYPE_BOOL;
            output->bit_width = 1u;
            return QL_STATUS_OK;
        }
        for (index = 0u; index < sizeof(signature_integer_table) /
                                     sizeof(signature_integer_table[0]);
             ++index) {
            if (strcmp(normalized,
                       signature_integer_table[index].spelling) != 0) {
                continue;
            }
            output->kind = signature_integer_table[index].is_signed != 0u
                               ? QL_SOURCE_TYPE_SIGNED_INTEGER
                               : QL_SOURCE_TYPE_UNSIGNED_INTEGER;
            output->bit_width = signature_integer_table[index].bit_width;
            return QL_STATUS_OK;
        }
        /* Only a name this unit actually declared is resolved. Giving a
           meaning to a name nobody declared is a guess about a type, and a
           wrong guess about a type is a wrong answer about the function. */
        entry = typedefs == NULL ? NULL : typedefs_find(typedefs, normalized);
        if (entry == NULL) {
            break;
        }
        if (entry->is_indirect != 0u) {
            output->kind = QL_SOURCE_TYPE_POINTER;
            output->bit_width = pointer_width;
            output->pointer_depth = 1u;
            return QL_STATUS_OK;
        }
        if (entry->is_aggregate != 0u) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "%s names an aggregate type, which source-signature schema v1 does not carry by value",
                         role);
            return QL_STATUS_TYPE_MISMATCH;
        }
        if (!normalize_spelling(entry->underlying, normalized,
                                sizeof(normalized))) {
            break;
        }
    }
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "%s type '%s' is not in the frozen ASM2C_GNU_V1 table", role,
                 normalized);
    return QL_STATUS_TYPE_MISMATCH;
}

ql_status QL_CALL ql_source_signature_from_c_function(
    const ql_allocator *allocator, const ql_c_frontend_unit *unit,
    const ql_c_function_view *function, ql_c_dialect_profile c_dialect,
    ql_target_abi target_abi, ql_artifact **output, ql_error *error) {
    return ql_source_signature_from_c_function_v2(
        allocator, unit, function, NULL, 0u, c_dialect, target_abi, output,
        error);
}

ql_status QL_CALL ql_source_signature_from_c_function_v2(
    const ql_allocator *allocator, const ql_c_frontend_unit *unit,
    const ql_c_function_view *function, const char *source,
    size_t source_size, ql_c_dialect_profile c_dialect,
    ql_target_abi target_abi, ql_artifact **output, ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_source_signature_definition_v1 definition;
    ql_source_type_v1 arguments[QL_SOURCE_SIGNATURE_MAX_ARGUMENTS];
    signature_typedefs typedefs;
    size_t index;
    ql_status status;

    if (output == NULL || unit == NULL || function == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "unit, function view, and signature output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (function->name == NULL || function->name[0] == '\0') {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "selected C function has no name");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (function->has_variadic_parameters != 0u ||
        function->has_old_style_parameters != 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "variadic and old-style C signatures are outside source-signature schema v1");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (function->parameter_count > QL_SOURCE_SIGNATURE_MAX_ARGUMENTS) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "C function has more than %u parameters",
                     QL_SOURCE_SIGNATURE_MAX_ARGUMENTS);
        return QL_STATUS_TYPE_MISMATCH;
    }

    ql_source_signature_definition_init(&definition);
    definition.c_dialect = c_dialect;
    definition.target_abi = target_abi;
    definition.function_name = function->name;
    definition.function_name_size = strlen(function->name);
    status = collect_typedefs(selected, source, source_size, &typedefs,
                              error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = type_from_inventory(&function->return_type,
                                 definition.pointer_width, &typedefs,
                                 "return type", &definition.return_type,
                                 error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    for (index = 0u; index < function->parameter_count; ++index) {
        ql_c_parameter_view parameter;
        memset(&parameter, 0, sizeof(parameter));
        parameter.struct_size = sizeof(parameter);
        status = ql_c_frontend_parameter_at(unit, function->index, index,
                                            &parameter, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        status = type_from_inventory(&parameter.type,
                                     definition.pointer_width, &typedefs,
                                     "parameter", &arguments[index], error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        if (arguments[index].kind == QL_SOURCE_TYPE_VOID) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "parameter %zu has void type", index);
            status = QL_STATUS_TYPE_MISMATCH;
            goto cleanup;
        }
    }
    definition.arguments = function->parameter_count != 0u ? arguments : NULL;
    definition.argument_count = function->parameter_count;
    status = ql_source_signature_artifact_create(selected, &definition,
                                                 output, error);

cleanup:
    typedefs_dispose(&typedefs);
    return status;
}

static ql_status ir_type_matches(const ql_ir *ir, ql_ir_type_id type_id,
                                 const ql_source_type_v1 *expected,
                                 const char *role, ql_error *error) {
    ql_ir_view_v1 view;
    ql_ir_type_view_v1 type;
    size_t index;
    ql_status status;

    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_ir_get_view(ir, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < view.type_count; ++index) {
        memset(&type, 0, sizeof(type));
        type.struct_size = sizeof(type);
        status = ql_ir_type_at(ir, index, &type, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (type.id == type_id) {
            break;
        }
    }
    if (index == view.type_count) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "IR %s references an unknown type", role);
        return QL_STATUS_TYPE_MISMATCH;
    }
    switch (expected->kind) {
    case QL_SOURCE_TYPE_VOID:
        if (type.kind == QL_IR_TYPE_VOID) {
            return QL_STATUS_OK;
        }
        break;
    case QL_SOURCE_TYPE_BOOL:
        if (type.kind == QL_IR_TYPE_BOOL) {
            return QL_STATUS_OK;
        }
        break;
    case QL_SOURCE_TYPE_SIGNED_INTEGER:
    case QL_SOURCE_TYPE_UNSIGNED_INTEGER:
        if (type.kind == QL_IR_TYPE_BIT_VECTOR &&
            type.bit_width == expected->bit_width) {
            return QL_STATUS_OK;
        }
        break;
    case QL_SOURCE_TYPE_POINTER:
        if (type.kind == QL_IR_TYPE_POINTER &&
            type.address_space == expected->address_space) {
            return QL_STATUS_OK;
        }
        break;
    default:
        break;
    }
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "IR %s type does not match the source signature", role);
    return QL_STATUS_TYPE_MISMATCH;
}

static int name_has_suffix(const ql_ir_value_view_v1 *value,
                           const char *suffix) {
    const size_t length = strlen(suffix);
    return value->name != NULL && value->name_size >= length &&
           memcmp(value->name + value->name_size - length, suffix, length) ==
               0;
}

/* Tail parameter `ordinal` counts from the first one past the C arguments:
   zero is the memory value, then base and size alternate. */
static ql_status object_parameter_matches(const ql_ir *ir,
                                          const ql_ir_value_view_v1 *value,
                                          uint32_t pointer_width,
                                          size_t ordinal, ql_error *error) {
    ql_ir_view_v1 view;
    ql_ir_type_view_v1 type;
    size_t index;
    ql_status status;

    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_ir_get_view(ir, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < view.type_count; ++index) {
        memset(&type, 0, sizeof(type));
        type.struct_size = sizeof(type);
        status = ql_ir_type_at(ir, index, &type, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (type.id == value->type) {
            break;
        }
    }
    if (index == view.type_count) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "IR object parameter references an unknown type");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (ordinal == 0u) {
        if (type.kind == QL_IR_TYPE_MEMORY &&
            name_has_suffix(value, "__memory")) {
            return QL_STATUS_OK;
        }
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "the parameter after the C arguments is not the memory value the flat memory model requires");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (type.kind == QL_IR_TYPE_BIT_VECTOR &&
        type.bit_width == pointer_width &&
        name_has_suffix(value, ((ordinal - 1u) % 2u) == 0u ? ".__base"
                                                           : ".__size")) {
        return QL_STATUS_OK;
    }
    ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                 "IR object parameter %zu is not the %s the flat memory model requires",
                 (ordinal - 1u) / 2u,
                 ((ordinal - 1u) % 2u) == 0u ? "base" : "size");
    return QL_STATUS_TYPE_MISMATCH;
}

ql_status QL_CALL ql_source_signature_bind_ir(
    const ql_source_signature *signature, const ql_ir *ir, ql_error *error) {
    ql_ir_view_v1 view;
    size_t index;
    size_t parameter_index = 0u;
    size_t pointer_count = 0u;
    size_t expected;
    ql_status status;

    if (signature == NULL || ir == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source signature and IR are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_ir_get_view(ir, &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (view.function_name_size != signature->view.function_name_size ||
        memcmp(view.function_name, signature->view.function_name,
               view.function_name_size) != 0) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "IR function name does not match the source signature");
        return QL_STATUS_TYPE_MISMATCH;
    }
    status = ir_type_matches(ir, view.return_type,
                             &signature->view.return_type, "return", error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < signature->view.argument_count; ++index) {
        if (signature->arguments[index].kind == QL_SOURCE_TYPE_POINTER) {
            ++pointer_count;
        }
    }
    /* A function with pointer arguments carries the flat memory model's own
       parameters behind the C ones: one memory value, then a base and a size
       per pointer argument in source order. The tail is checked rather than
       skipped, because a lowering that got it wrong would silently hand the
       miter a different object table than the signature implies. */
    expected = signature->view.argument_count +
               (pointer_count == 0u ? 0u : 1u + 2u * pointer_count);
    for (index = 0u; index < view.value_count; ++index) {
        ql_ir_value_view_v1 value;
        memset(&value, 0, sizeof(value));
        value.struct_size = sizeof(value);
        status = ql_ir_value_at(ir, index, &value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
            continue;
        }
        if (parameter_index >= expected) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "IR declares more parameters than the source signature");
            return QL_STATUS_TYPE_MISMATCH;
        }
        if (parameter_index < signature->view.argument_count) {
            status = ir_type_matches(ir, value.type,
                                     &signature->arguments[parameter_index],
                                     "parameter", error);
        } else {
            status = object_parameter_matches(
                ir, &value, signature->view.pointer_width,
                parameter_index - signature->view.argument_count, error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        ++parameter_index;
    }
    if (parameter_index != expected) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "IR declares %zu parameters but the source signature and its object table imply %zu",
                     parameter_index, expected);
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}
