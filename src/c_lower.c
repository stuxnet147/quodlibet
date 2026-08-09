#include "quodlibet/c_lower.h"

#include "c_types.h"

#include "quodlibet/ir_interp.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct ql_c_lower_diagnostic_record {
    ql_c_lower_diagnostic_code code;
    ql_source_range range;
    char *construct_kind;
    char *message;
} ql_c_lower_diagnostic_record;

struct ql_c_lower_result {
    ql_allocator allocator;
    ql_c_lower_support support;
    ql_c_lower_diagnostic_record *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;
    ql_artifact *ir_artifact;
};

typedef struct lower_node {
    ql_c_syntax_node_view view;
    size_t parent;
    uint32_t depth;
} lower_node;

/* The C-level meaning lives in c_types.c; this adds only the IR type the
   lowering has already materialised for it. */
typedef struct lower_type {
    ql_c_scalar_kind kind;
    uint32_t width;
    uint32_t rank;
    uint32_t is_signed;
    /* Meaningful only when `kind` is QL_C_SCALAR_POINTER. This slice carries
       pointers to scalars, so one level of indirection is enough. */
    ql_c_scalar_type pointee;
    ql_ir_type_id ir_type;
} lower_type;

typedef struct lower_value {
    ql_ir_value_id value;
    ql_ir_value_id defined;
    lower_type type;
    uint32_t may_ub;
} lower_value;

/* One `typedef` name and the type it stands for. `underlying` is the
   spelling of the definition's type node, which may itself be a typedef name,
   so resolution iterates. */
typedef struct lower_typedef {
    char *name;
    char *underlying;
    uint32_t is_indirect;
    uint32_t is_aggregate;
} lower_typedef;

typedef struct lower_variable {
    char *name;
    size_t name_size;
    lower_type type;
    ql_ir_value_id value;
    uint32_t initialized;
    uint32_t is_const;
    size_t scope_depth;
} lower_variable;

/* One storage region the function may touch. The object table is derived
   from the pointer parameters in source order, so both sides of a problem
   agree on it without exchanging anything. */
typedef struct lower_object {
    ql_ir_value_id base;
    ql_ir_value_id size;
} lower_object;

typedef struct lower_state {
    ql_ir_value_id *values;
    uint8_t *initialized;
    size_t count;
    ql_ir_value_id memory;
} lower_state;

typedef struct lower_context {
    const ql_allocator *allocator;
    const char *source;
    size_t source_size;
    const ql_c_frontend_unit *unit;
    ql_c_function_view function;
    ql_c_lower_result *result;
    lower_node *nodes;
    size_t node_count;
    ql_c_parser *parser;
    ql_c_syntax_tree *tree;
    ql_ir_builder *builder;
    lower_variable *variables;
    size_t variable_count;
    size_t variable_capacity;
    lower_typedef *typedefs;
    size_t typedef_count;
    size_t typedef_capacity;
    size_t scope_depth;
    lower_type return_type;
    ql_ir_block_id current_block;
    uint32_t current_terminated;
    ql_ir_type_id bool_type;
    ql_ir_type_id void_type;
    ql_ir_type_id memory_type;
    ql_ir_type_id bv_types[129];
    ql_ir_type_id pointer_types[129];
    ql_ir_type_id bool_pointer_type;
    /* The memory state threaded through the function, and the objects the
       access guards are written against. */
    ql_ir_value_id memory_value;
    lower_object *objects;
    size_t object_count;
    uint32_t uses_memory;
    ql_ir_value_id true_value;
    ql_ir_value_id false_value;
    uint64_t next_block_label;
    uint32_t unknown;
} lower_context;

static const ql_allocator *select_allocator(const ql_allocator *allocator) {
    return allocator != NULL ? allocator : ql_default_allocator();
}

static ql_status grow_array(const ql_allocator *allocator, void **items,
                            size_t *capacity, size_t item_size,
                            size_t required, ql_error *error) {
    size_t next;
    void *resized;

    if (required <= *capacity) {
        return QL_STATUS_OK;
    }
    next = *capacity != 0u ? *capacity : 8u;
    while (next < required) {
        if (next > SIZE_MAX / 2u) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        next *= 2u;
    }
    if (item_size != 0u && next > SIZE_MAX / item_size) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    resized = allocator->reallocate(allocator->user_data, *items,
                                    next * item_size);
    if (resized == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    *items = resized;
    *capacity = next;
    return QL_STATUS_OK;
}

static char *copy_text(const ql_allocator *allocator, const char *text,
                       size_t size) {
    char *copy;

    if (size == SIZE_MAX) {
        return NULL;
    }
    copy = allocator->allocate(allocator->user_data, size + 1u);
    if (copy == NULL) {
        return NULL;
    }
    if (size != 0u) {
        memcpy(copy, text, size);
    }
    copy[size] = '\0';
    return copy;
}

static char *copy_c_string(const ql_allocator *allocator, const char *text) {
    return copy_text(allocator, text != NULL ? text : "",
                     text != NULL ? strlen(text) : 0u);
}

static ql_status add_diagnostic(ql_c_lower_result *result,
                                ql_c_lower_diagnostic_code code,
                                ql_source_range range,
                                const char *construct_kind,
                                const char *message, ql_error *error) {
    ql_c_lower_diagnostic_record *record;
    ql_status status = grow_array(
        &result->allocator, (void **)&result->diagnostics,
        &result->diagnostic_capacity, sizeof(*result->diagnostics),
        result->diagnostic_count + 1u, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    record = &result->diagnostics[result->diagnostic_count];
    memset(record, 0, sizeof(*record));
    record->code = code;
    record->range = range;
    record->construct_kind = copy_c_string(&result->allocator,
                                           construct_kind);
    record->message = copy_c_string(&result->allocator, message);
    if (record->construct_kind == NULL || record->message == NULL) {
        result->allocator.deallocate(result->allocator.user_data,
                                     record->construct_kind);
        result->allocator.deallocate(result->allocator.user_data,
                                     record->message);
        memset(record, 0, sizeof(*record));
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    ++result->diagnostic_count;
    result->support = QL_C_LOWER_UNKNOWN;
    return QL_STATUS_OK;
}

static ql_status lower_unknown(lower_context *context,
                               ql_c_lower_diagnostic_code code,
                               size_t node, const char *message,
                               ql_error *error) {
    ql_source_range range;
    const char *kind;

    memset(&range, 0, sizeof(range));
    kind = "function_definition";
    if (node < context->node_count) {
        range = context->nodes[node].view.range;
        kind = context->nodes[node].view.kind;
    } else {
        range = context->function.range;
    }
    context->unknown = 1u;
    return add_diagnostic(context->result, code, range, kind, message, error);
}

void QL_CALL ql_c_lower_result_destroy(ql_c_lower_result *result) {
    ql_allocator allocator;
    size_t index;

    if (result == NULL) {
        return;
    }
    allocator = result->allocator;
    for (index = 0u; index < result->diagnostic_count; ++index) {
        allocator.deallocate(allocator.user_data,
                             result->diagnostics[index].construct_kind);
        allocator.deallocate(allocator.user_data,
                             result->diagnostics[index].message);
    }
    allocator.deallocate(allocator.user_data, result->diagnostics);
    ql_artifact_release(result->ir_artifact);
    allocator.deallocate(allocator.user_data, result);
}

ql_status QL_CALL ql_c_lower_result_get_view(
    const ql_c_lower_result *result, ql_c_lower_result_view_v1 *view,
    ql_error *error) {
    if (result == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C lowering result and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view->struct_size != 0u && view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "C lowering result view is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_C_LOWER_SCHEMA_VERSION;
    view->support = result->support;
    view->diagnostic_count = result->diagnostic_count;
    view->ir_artifact = result->ir_artifact;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_c_lower_result_diagnostic_at(
    const ql_c_lower_result *result, size_t index,
    ql_c_lower_diagnostic_view_v1 *view, ql_error *error) {
    const ql_c_lower_diagnostic_record *record;

    if (result == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C lowering result and diagnostic view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view->struct_size != 0u && view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "C lowering diagnostic view is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (index >= result->diagnostic_count) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "C lowering diagnostic does not exist");
        return QL_STATUS_NOT_FOUND;
    }
    record = &result->diagnostics[index];
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->code = record->code;
    view->range = record->range;
    view->construct_kind = record->construct_kind;
    view->message = record->message;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_status collect_nodes(lower_context *context, ql_error *error) {
    ql_c_syntax_cursor *cursor = NULL;
    lower_node *items = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    size_t parent = SIZE_MAX;
    uint32_t depth = 0u;
    ql_status status;

    status = ql_c_syntax_cursor_create(context->tree, &cursor, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (;;) {
        size_t current;

        status = grow_array(context->allocator, (void **)&items, &capacity,
                            sizeof(*items), count + 1u, error);
        if (status != QL_STATUS_OK) {
            goto fail;
        }
        current = count++;
        memset(&items[current], 0, sizeof(items[current]));
        items[current].view.struct_size = sizeof(items[current].view);
        status = ql_c_syntax_cursor_current(cursor, &items[current].view,
                                            error);
        if (status != QL_STATUS_OK) {
            goto fail;
        }
        items[current].parent = parent;
        items[current].depth = depth;
        if (ql_c_syntax_cursor_goto_first_child(cursor) != 0u) {
            parent = current;
            ++depth;
            continue;
        }
        while (ql_c_syntax_cursor_goto_next_sibling(cursor) == 0u) {
            size_t ascended;

            if (ql_c_syntax_cursor_goto_parent(cursor) == 0u) {
                ql_c_syntax_cursor_destroy(cursor);
                context->nodes = items;
                context->node_count = count;
                return QL_STATUS_OK;
            }
            ascended = parent;
            parent = items[ascended].parent;
            --depth;
        }
    }

fail:
    ql_c_syntax_cursor_destroy(cursor);
    context->allocator->deallocate(context->allocator->user_data, items);
    return status;
}

static size_t subtree_end(const lower_context *context, size_t root) {
    size_t end = root + 1u;
    while (end < context->node_count &&
           context->nodes[end].depth > context->nodes[root].depth) {
        ++end;
    }
    return end;
}

static size_t direct_field_child(const lower_context *context, size_t parent,
                                 const char *field) {
    size_t end = subtree_end(context, parent);
    size_t index;

    for (index = parent + 1u; index < end; ++index) {
        if (context->nodes[index].parent == parent &&
            context->nodes[index].view.field_name != NULL &&
            strcmp(context->nodes[index].view.field_name, field) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

static size_t first_named_child(const lower_context *context, size_t parent) {
    size_t end = subtree_end(context, parent);
    size_t index;

    for (index = parent + 1u; index < end; ++index) {
        if (context->nodes[index].parent == parent &&
            (context->nodes[index].view.flags & QL_C_SYNTAX_NODE_NAMED) !=
                0u) {
            return index;
        }
    }
    return SIZE_MAX;
}

static int range_equal(ql_source_range left, ql_source_range right) {
    return left.start_byte == right.start_byte &&
           left.end_byte == right.end_byte;
}

static char *copy_node_text(const lower_context *context, size_t node) {
    ql_source_range range = context->nodes[node].view.range;
    if (range.start_byte > range.end_byte ||
        range.end_byte > context->source_size) {
        return NULL;
    }
    return copy_text(context->allocator, context->source + range.start_byte,
                     (size_t)range.end_byte - (size_t)range.start_byte);
}

static ql_c_scalar_type scalar_of(lower_type type) {
    ql_c_scalar_type scalar;
    memset(&scalar, 0, sizeof(scalar));
    scalar.kind = type.kind;
    scalar.width = type.width;
    scalar.rank = type.rank;
    scalar.is_signed = type.is_signed;
    return scalar;
}

static lower_type type_from_scalar(ql_c_scalar_type scalar) {
    lower_type type;
    memset(&type, 0, sizeof(type));
    type.kind = scalar.kind;
    type.width = scalar.width;
    type.rank = scalar.rank;
    type.is_signed = scalar.is_signed;
    type.ir_type = QL_IR_INVALID_TYPE_ID;
    return type;
}

static lower_type make_bool_type(void) {
    return type_from_scalar(ql_c_scalar_make_bool());
}

static lower_type make_integer_type(uint32_t width, uint32_t rank,
                                    uint32_t is_signed) {
    return type_from_scalar(ql_c_scalar_make_integer(width, rank, is_signed));
}

static int type_same(lower_type left, lower_type right) {
    if (!ql_c_scalar_same(scalar_of(left), scalar_of(right))) {
        return 0;
    }
    if (left.kind != QL_C_SCALAR_POINTER) {
        return 1;
    }
    return ql_c_scalar_same(left.pointee, right.pointee);
}

static lower_type make_pointer_type(ql_c_scalar_type pointee) {
    lower_type type;
    memset(&type, 0, sizeof(type));
    type.kind = QL_C_SCALAR_POINTER;
    type.width = QL_C_POINTER_WIDTH;
    type.rank = 6u;
    type.is_signed = 0u;
    type.pointee = pointee;
    type.ir_type = QL_IR_INVALID_TYPE_ID;
    return type;
}

/* Storage width of the pointee, which is what a load or store moves. */
static uint32_t pointee_byte_width(lower_type pointer) {
    uint32_t bits = pointer.pointee.kind == QL_C_SCALAR_BOOL
                        ? 8u
                        : pointer.pointee.width;
    return (bits + 7u) / 8u;
}

/* Natural alignment on the target ABI, stated the same way the interpreter
   states it: a scalar of N bytes is N-aligned when N is a power of two up to
   sixteen, and nothing else is required. */
static uint32_t natural_alignment(uint32_t byte_width) {
    if (byte_width == 0u || byte_width > 16u ||
        (byte_width & (byte_width - 1u)) != 0u) {
        return 1u;
    }
    return byte_width;
}

/* `allow_void` is set only where C admits an incomplete type: a function's
   return type. Everywhere else void is a type error, not a narrowing. */
/* A declarator that is not just an identifier introduces indirection, which
   the scalar slice cannot represent. Recording that here lets resolution
   report `unsupported_pointer` instead of `unsupported_type`, so the coverage
   tables name the real obstacle. */
static size_t typedef_declarator_name(const lower_context *context,
                                      size_t declarator,
                                      uint32_t *is_indirect) {
    size_t guard = 0u;

    *is_indirect = 0u;
    while (declarator != SIZE_MAX && guard++ < 64u) {
        const char *kind = context->nodes[declarator].view.kind;
        if (strcmp(kind, "type_identifier") == 0 ||
            strcmp(kind, "identifier") == 0) {
            return declarator;
        }
        if (strcmp(kind, "pointer_declarator") == 0 ||
            strcmp(kind, "array_declarator") == 0 ||
            strcmp(kind, "function_declarator") == 0) {
            *is_indirect = 1u;
            declarator = direct_field_child(context, declarator, "declarator");
            continue;
        }
        if (strcmp(kind, "parenthesized_declarator") == 0) {
            declarator = direct_field_child(context, declarator, "declarator");
            continue;
        }
        return SIZE_MAX;
    }
    return SIZE_MAX;
}

static ql_status collect_typedefs(lower_context *context, ql_error *error) {
    size_t index;

    for (index = 0u; index < context->node_count; ++index) {
        size_t type_node;
        size_t end;
        size_t child;
        const char *type_kind;
        uint32_t is_aggregate;

        if (strcmp(context->nodes[index].view.kind, "type_definition") != 0) {
            continue;
        }
        type_node = direct_field_child(context, index, "type");
        if (type_node == SIZE_MAX) {
            continue;
        }
        type_kind = context->nodes[type_node].view.kind;
        is_aggregate = strcmp(type_kind, "struct_specifier") == 0 ||
                       strcmp(type_kind, "union_specifier") == 0 ||
                       strcmp(type_kind, "enum_specifier") == 0;
        end = subtree_end(context, index);
        for (child = index + 1u; child < end; ++child) {
            size_t name_node;
            uint32_t is_indirect;
            lower_typedef *entry;
            ql_status status;

            if (context->nodes[child].parent != index ||
                context->nodes[child].view.field_name == NULL ||
                strcmp(context->nodes[child].view.field_name,
                       "declarator") != 0) {
                continue;
            }
            name_node = typedef_declarator_name(context, child, &is_indirect);
            if (name_node == SIZE_MAX) {
                continue;
            }
            status = grow_array(context->allocator,
                                (void **)&context->typedefs,
                                &context->typedef_capacity,
                                sizeof(*context->typedefs),
                                context->typedef_count + 1u, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            entry = &context->typedefs[context->typedef_count];
            memset(entry, 0, sizeof(*entry));
            entry->name = copy_node_text(context, name_node);
            entry->underlying = copy_node_text(context, type_node);
            entry->is_indirect = is_indirect;
            entry->is_aggregate = is_aggregate;
            if (entry->name == NULL || entry->underlying == NULL) {
                context->allocator->deallocate(context->allocator->user_data,
                                               entry->name);
                context->allocator->deallocate(context->allocator->user_data,
                                               entry->underlying);
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            ++context->typedef_count;
        }
    }
    return QL_STATUS_OK;
}

static const lower_typedef *find_typedef(const lower_context *context,
                                         const char *name) {
    size_t index;
    for (index = context->typedef_count; index-- > 0u;) {
        if (strcmp(context->typedefs[index].name, name) == 0) {
            return &context->typedefs[index];
        }
    }
    return NULL;
}

static void release_typedefs(lower_context *context) {
    size_t index;
    for (index = 0u; index < context->typedef_count; ++index) {
        context->allocator->deallocate(context->allocator->user_data,
                                       context->typedefs[index].name);
        context->allocator->deallocate(context->allocator->user_data,
                                       context->typedefs[index].underlying);
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   context->typedefs);
    context->typedefs = NULL;
    context->typedef_count = 0u;
    context->typedef_capacity = 0u;
}

static ql_status parse_type_spelling(lower_context *context,
                                     const char *spelling, size_t node,
                                     uint32_t allow_void, lower_type *output,
                                     ql_error *error) {
    ql_c_scalar_type scalar;
    size_t hops = 0u;

    /* A typedef name means whatever this unit declared it to mean. Only names
       the unit actually declares are resolved: assuming a meaning for an
       undeclared name would be a guess, and a wrong guess about a type is a
       wrong answer about the function. */
    while (!ql_c_scalar_from_spelling(spelling, &scalar)) {
        const lower_typedef *entry = find_typedef(context, spelling);
        if (entry == NULL) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
                "type spelling names no ASM2C_GNU_V1 scalar type and no "
                "typedef this unit declares", error);
        }
        if (entry->is_indirect != 0u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
                "typedef names a pointer, array, or function type", error);
        }
        if (entry->is_aggregate != 0u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
                "typedef names a struct, union, or enum type", error);
        }
        if (++hops > 64u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
                "typedef chain does not terminate", error);
        }
        spelling = entry->underlying;
    }
    if (scalar.kind == QL_C_SCALAR_VOID && allow_void == 0u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
            "void is not an object type here", error);
    }
    *output = type_from_scalar(scalar);
    return QL_STATUS_OK;
}

static ql_status type_from_inventory(lower_context *context,
                                     const ql_c_type_inventory_v1 *inventory,
                                     size_t node, uint32_t allow_void,
                                     lower_type *output, ql_error *error) {
    if ((inventory->qualifiers &
         (QL_C_TYPE_QUALIFIER_VOLATILE | QL_C_TYPE_QUALIFIER_ATOMIC)) != 0u ||
        inventory->base_kind == QL_C_TYPE_BASE_ATOMIC) {
        return lower_unknown(
            context,
            QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_VOLATILE_OR_ATOMIC, node,
            "volatile and atomic objects require an observable-event lowering",
            error);
    }
    if ((inventory->qualifiers & QL_C_TYPE_QUALIFIER_RESTRICT) != 0u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
            "restrict-qualified declarations require pointer semantics",
            error);
    }
    if ((inventory->shape &
         (QL_C_TYPE_SHAPE_ARRAY | QL_C_TYPE_SHAPE_FUNCTION)) != 0u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
            "array and function-valued declarations are outside this "
            "lowering slice", error);
    }
    if (inventory->pointer_depth > 1u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
            "this slice carries one level of indirection", error);
    }
    if (inventory->pointer_depth == 1u) {
        lower_type pointee;
        ql_status status = parse_type_spelling(context,
                                               inventory->base_spelling, node,
                                               1u, &pointee, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        *output = make_pointer_type(scalar_of(pointee));
        return QL_STATUS_OK;
    }
    /* The inventory's base-kind classification is a fast syntactic hint.
       Multi-keyword integer specifiers vary in Tree-sitter shape, so the
       versioned spelling parser below is the semantic authority. */
    return parse_type_spelling(context, inventory->base_spelling, node,
                               allow_void, output, error);
}

static ql_status ensure_ir_type(lower_context *context, lower_type *type,
                                ql_error *error) {
    ql_ir_type_definition_v1 definition;
    ql_ir_type_id id;
    ql_status status;

    if (type->ir_type != QL_IR_INVALID_TYPE_ID) {
        return QL_STATUS_OK;
    }
    if (type->kind == QL_C_SCALAR_POINTER) {
        lower_type pointee = type_from_scalar(type->pointee);
        ql_ir_type_id *slot;
        ql_status pointee_status;
        if (type->pointee.kind == QL_C_SCALAR_BOOL) {
            slot = &context->bool_pointer_type;
        } else {
            slot = &context->pointer_types[type->pointee.width];
        }
        if (*slot != QL_IR_INVALID_TYPE_ID) {
            type->ir_type = *slot;
            return QL_STATUS_OK;
        }
        pointee_status = ensure_ir_type(context, &pointee, error);
        if (pointee_status != QL_STATUS_OK) {
            return pointee_status;
        }
        ql_ir_type_definition_init(&definition, QL_IR_TYPE_POINTER);
        definition.bit_width = QL_C_POINTER_WIDTH;
        definition.element_type = pointee.ir_type;
        status = ql_ir_builder_add_type(context->builder, &definition, &id,
                                        error);
        if (status == QL_STATUS_OK) {
            *slot = id;
            type->ir_type = id;
        }
        return status;
    }
    if (type->kind == QL_C_SCALAR_VOID) {
        if (context->void_type != QL_IR_INVALID_TYPE_ID) {
            type->ir_type = context->void_type;
            return QL_STATUS_OK;
        }
        ql_ir_type_definition_init(&definition, QL_IR_TYPE_VOID);
        status = ql_ir_builder_add_type(context->builder, &definition, &id,
                                        error);
        if (status == QL_STATUS_OK) {
            context->void_type = id;
            type->ir_type = id;
        }
        return status;
    }
    if (type->kind == QL_C_SCALAR_BOOL) {
        if (context->bool_type != QL_IR_INVALID_TYPE_ID) {
            type->ir_type = context->bool_type;
            return QL_STATUS_OK;
        }
        ql_ir_type_definition_init(&definition, QL_IR_TYPE_BOOL);
        definition.bit_width = 1u;
        status = ql_ir_builder_add_type(context->builder, &definition, &id,
                                        error);
        if (status == QL_STATUS_OK) {
            context->bool_type = id;
            type->ir_type = id;
        }
        return status;
    }
    if (type->width == 0u || type->width > 128u) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "invalid C integer width during IR lowering");
        return QL_STATUS_INTERNAL_ERROR;
    }
    if (context->bv_types[type->width] != QL_IR_INVALID_TYPE_ID) {
        type->ir_type = context->bv_types[type->width];
        return QL_STATUS_OK;
    }
    ql_ir_type_definition_init(&definition, QL_IR_TYPE_BIT_VECTOR);
    definition.bit_width = type->width;
    status = ql_ir_builder_add_type(context->builder, &definition, &id,
                                    error);
    if (status == QL_STATUS_OK) {
        context->bv_types[type->width] = id;
        type->ir_type = id;
    }
    return status;
}

static ql_status emit_instruction(lower_context *context,
                                  ql_ir_opcode opcode,
                                  lower_type *result_type,
                                  const ql_ir_value_id *operands,
                                  size_t operand_count,
                                  const ql_ir_block_id *block_operands,
                                  size_t block_operand_count,
                                  uint64_t effects,
                                  ql_ir_value_id *output,
                                  ql_error *error) {
    ql_ir_instruction_definition_v1 definition;
    ql_ir_type_id result_id;
    ql_ir_instruction_id instruction;
    ql_status status;

    ql_ir_instruction_definition_init(&definition, opcode);
    definition.effects = effects;
    definition.operands = operands;
    definition.operand_count = operand_count;
    definition.block_operands = block_operands;
    definition.block_operand_count = block_operand_count;
    if (result_type != NULL) {
        status = ensure_ir_type(context, result_type, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        result_id = result_type->ir_type;
        definition.result_types = &result_id;
        definition.result_count = 1u;
    }
    return ql_ir_builder_append_instruction(
        context->builder, context->current_block, &definition, &instruction,
        result_type != NULL ? output : NULL, error);
}

/* Emits an instruction whose result type is already an IR type, which is
   what memory-valued instructions need. */
static ql_status emit_typed_instruction(lower_context *context,
                                        ql_ir_opcode opcode,
                                        ql_ir_type_id result_type,
                                        const ql_ir_value_id *operands,
                                        size_t operand_count,
                                        uint64_t effects,
                                        ql_ir_value_id *output,
                                        ql_error *error) {
    ql_ir_instruction_definition_v1 definition;
    ql_ir_instruction_id instruction;

    ql_ir_instruction_definition_init(&definition, opcode);
    definition.effects = effects;
    definition.operands = operands;
    definition.operand_count = operand_count;
    if (result_type != QL_IR_INVALID_TYPE_ID) {
        definition.result_types = &result_type;
        definition.result_count = 1u;
    }
    return ql_ir_builder_append_instruction(
        context->builder, context->current_block, &definition, &instruction,
        result_type != QL_IR_INVALID_TYPE_ID ? output : NULL, error);
}

static ql_status ensure_memory_type(lower_context *context, ql_error *error) {
    ql_ir_type_definition_v1 definition;
    ql_ir_type_id id;
    ql_status status;

    if (context->memory_type != QL_IR_INVALID_TYPE_ID) {
        return QL_STATUS_OK;
    }
    ql_ir_type_definition_init(&definition, QL_IR_TYPE_MEMORY);
    status = ql_ir_builder_add_type(context->builder, &definition, &id,
                                    error);
    if (status == QL_STATUS_OK) {
        context->memory_type = id;
    }
    return status;
}

static ql_status add_constant_bytes(lower_context *context, lower_type *type,
                                    const uint8_t *bytes, size_t size,
                                    ql_ir_value_id *output,
                                    ql_error *error) {
    ql_status status = ensure_ir_type(context, type, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return ql_ir_builder_add_constant(context->builder, type->ir_type, bytes,
                                      size, output, error);
}

static ql_status add_uint_constant(lower_context *context, lower_type type,
                                   uint64_t value, ql_ir_value_id *output,
                                   ql_error *error) {
    uint8_t bytes[16];
    size_t size;
    size_t index;

    memset(bytes, 0, sizeof(bytes));
    size = type.kind == QL_C_SCALAR_BOOL ? 1u : (type.width + 7u) / 8u;
    if (size > sizeof(bytes)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "integer constant is wider than the lowering buffer");
        return QL_STATUS_INTERNAL_ERROR;
    }
    for (index = 0u; index < size && index < 8u; ++index) {
        bytes[index] = (uint8_t)(value & UINT64_C(0xff));
        value >>= 8u;
    }
    if (type.kind == QL_C_SCALAR_BOOL) {
        bytes[0] = bytes[0] != 0u ? 1u : 0u;
    }
    return add_constant_bytes(context, &type, bytes, size, output, error);
}

static ql_status ensure_bool_constants(lower_context *context,
                                       ql_error *error) {
    lower_type type = make_bool_type();
    ql_status status;

    if (context->true_value != QL_IR_INVALID_VALUE_ID) {
        return QL_STATUS_OK;
    }
    status = add_uint_constant(context, type, 0u, &context->false_value,
                               error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return add_uint_constant(context, type, 1u, &context->true_value, error);
}

static ql_status emit_bool_not(lower_context *context, ql_ir_value_id value,
                               ql_ir_value_id *output, ql_error *error) {
    lower_type type = make_bool_type();
    return emit_instruction(context, QL_IR_OPCODE_BOOL_NOT, &type, &value, 1u,
                            NULL, 0u, QL_IR_EFFECT_NONE, output, error);
}

static ql_status emit_bool_and(lower_context *context,
                               ql_ir_value_id left, ql_ir_value_id right,
                               ql_ir_value_id *output, ql_error *error) {
    lower_type type = make_bool_type();
    ql_ir_value_id operands[3];
    ql_status status = ensure_bool_constants(context, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    operands[0] = left;
    operands[1] = right;
    operands[2] = context->false_value;
    return emit_instruction(context, QL_IR_OPCODE_SELECT, &type, operands, 3u,
                            NULL, 0u, QL_IR_EFFECT_NONE, output, error);
}

static ql_status emit_bool_or(lower_context *context,
                              ql_ir_value_id left, ql_ir_value_id right,
                              ql_ir_value_id *output, ql_error *error) {
    lower_type type = make_bool_type();
    ql_ir_value_id operands[3];
    ql_status status = ensure_bool_constants(context, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    operands[0] = left;
    operands[1] = context->true_value;
    operands[2] = right;
    return emit_instruction(context, QL_IR_OPCODE_SELECT, &type, operands, 3u,
                            NULL, 0u, QL_IR_EFFECT_NONE, output, error);
}

static ql_status emit_compare(lower_context *context, ql_ir_opcode opcode,
                              ql_ir_value_id left, ql_ir_value_id right,
                              ql_ir_value_id *output, ql_error *error) {
    lower_type type = make_bool_type();
    ql_ir_value_id operands[2];
    operands[0] = left;
    operands[1] = right;
    return emit_instruction(context, opcode, &type, operands, 2u, NULL, 0u,
                            QL_IR_EFFECT_NONE, output, error);
}

static ql_status emit_ub_guard(lower_context *context,
                               const lower_value *value, ql_error *error) {
    if (value->may_ub == 0u) {
        return QL_STATUS_OK;
    }
    return emit_instruction(context, QL_IR_OPCODE_UB_GUARD, NULL,
                            &value->defined, 1u, NULL, 0u,
                            QL_IR_EFFECT_UNDEFINED_BEHAVIOR, NULL, error);
}

static lower_type address_type(void);
static ql_status convert_value(lower_context *context, lower_value input,
                               lower_type target, lower_value *output,
                               ql_error *error);
static ql_status combine_defined(lower_context *context,
                                 const lower_value *left,
                                 const lower_value *right,
                                 ql_ir_value_id *output, ql_error *error);
static ql_status emit_address_of_pointer(lower_context *context,
                                         lower_value pointer,
                                         lower_value *output,
                                         ql_error *error);

/* The predicate the profile defines for an access: the whole access lies
   inside one live object and the address is naturally aligned. It is built
   out of ordinary bit-vector operations and handed to UB_GUARD, so the
   interpreter and the SMT encoding both simply run it. There is one copy of
   this rule in the IR rather than one per backend. */
static ql_status emit_access_defined(lower_context *context,
                                     lower_value pointer,
                                     uint32_t byte_width,
                                     ql_ir_value_id *output,
                                     ql_error *error) {
    lower_type u64 = address_type();
    lower_type boolean = make_bool_type();
    lower_value address;
    ql_ir_value_id width_constant;
    ql_ir_value_id alignment_mask;
    ql_ir_value_id zero;
    uint32_t alignment = natural_alignment(byte_width);
    size_t index;
    ql_status status;

    status = ensure_bool_constants(context, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_address_of_pointer(context, pointer, &address, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = add_uint_constant(context, u64, byte_width, &width_constant,
                               error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    *output = context->false_value;
    for (index = 0u; index < context->object_count; ++index) {
        const lower_object *object = &context->objects[index];
        ql_ir_value_id operands[2];
        ql_ir_value_id offset;
        ql_ir_value_id room;
        ql_ir_value_id at_or_after;
        ql_ir_value_id within;
        ql_ir_value_id fits;
        ql_ir_value_id inside;

        status = emit_compare(context, QL_IR_OPCODE_ULE, object->base,
                              address.value, &at_or_after, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        operands[0] = address.value;
        operands[1] = object->base;
        status = emit_instruction(context, QL_IR_OPCODE_SUB, &u64, operands,
                                  2u, NULL, 0u, QL_IR_EFFECT_NONE, &offset,
                                  error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_compare(context, QL_IR_OPCODE_ULE, offset, object->size,
                              &within, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        operands[0] = object->size;
        operands[1] = offset;
        status = emit_instruction(context, QL_IR_OPCODE_SUB, &u64, operands,
                                  2u, NULL, 0u, QL_IR_EFFECT_NONE, &room,
                                  error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_compare(context, QL_IR_OPCODE_ULE, width_constant, room,
                              &fits, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_bool_and(context, at_or_after, within, &inside, error);
        if (status == QL_STATUS_OK) {
            status = emit_bool_and(context, inside, fits, &inside, error);
        }
        if (status == QL_STATUS_OK) {
            status = emit_bool_or(context, *output, inside, output, error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    if (alignment > 1u) {
        ql_ir_value_id operands[2];
        ql_ir_value_id masked;
        ql_ir_value_id aligned;
        status = add_uint_constant(context, u64, alignment - 1u,
                                   &alignment_mask, error);
        if (status == QL_STATUS_OK) {
            status = add_uint_constant(context, u64, 0u, &zero, error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        operands[0] = address.value;
        operands[1] = alignment_mask;
        status = emit_instruction(context, QL_IR_OPCODE_BV_AND, &u64,
                                  operands, 2u, NULL, 0u, QL_IR_EFFECT_NONE,
                                  &masked, error);
        if (status == QL_STATUS_OK) {
            status = emit_compare(context, QL_IR_OPCODE_EQ, masked, zero,
                                  &aligned, error);
        }
        if (status == QL_STATUS_OK) {
            status = emit_bool_and(context, *output, aligned, output, error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    (void)boolean;
    return QL_STATUS_OK;
}

/* Guards the access at the point it happens rather than deferring to the
   next observation, which is the tightest place the obligation can sit. */
static ql_status emit_access_guard(lower_context *context,
                                   lower_value pointer, uint32_t byte_width,
                                   ql_ir_value_id inherited_defined,
                                   uint32_t inherited_may_ub,
                                   ql_error *error) {
    ql_ir_value_id defined;
    lower_value guard;
    ql_status status = emit_access_defined(context, pointer, byte_width,
                                           &defined, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    if (inherited_may_ub != 0u) {
        status = emit_bool_and(context, inherited_defined, defined, &defined,
                               error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    memset(&guard, 0, sizeof(guard));
    guard.defined = defined;
    guard.may_ub = 1u;
    return emit_ub_guard(context, &guard, error);
}

static ql_status emit_load(lower_context *context, lower_value pointer,
                           lower_value *output, ql_error *error) {
    lower_type pointee = type_from_scalar(pointer.type.pointee);
    ql_ir_value_id operands[2];
    ql_status status;

    if (pointee.kind == QL_C_SCALAR_VOID) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, SIZE_MAX,
            "a pointer to void has no value to load", error);
    }
    status = emit_access_guard(context, pointer, pointee_byte_width(
                                                     pointer.type),
                               pointer.defined, pointer.may_ub, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ensure_ir_type(context, &pointee, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    operands[0] = context->memory_value;
    operands[1] = pointer.value;
    memset(output, 0, sizeof(*output));
    output->type = pointee;
    output->defined = context->true_value;
    output->may_ub = 0u;
    return emit_typed_instruction(context, QL_IR_OPCODE_LOAD,
                                  pointee.ir_type, operands, 2u,
                                  QL_IR_EFFECT_MEMORY, &output->value, error);
}

static ql_status emit_store(lower_context *context, lower_value pointer,
                            lower_value value, ql_error *error) {
    lower_type pointee = type_from_scalar(pointer.type.pointee);
    lower_value converted;
    ql_ir_value_id operands[3];
    ql_ir_value_id combined;
    uint32_t may_ub = pointer.may_ub | value.may_ub;
    ql_status status;

    if (pointee.kind == QL_C_SCALAR_VOID) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, SIZE_MAX,
            "a pointer to void has no value to store", error);
    }
    status = convert_value(context, value, pointee, &converted, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = combine_defined(context, &pointer, &value, &combined, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_access_guard(context, pointer,
                               pointee_byte_width(pointer.type), combined,
                               may_ub, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    operands[0] = context->memory_value;
    operands[1] = pointer.value;
    operands[2] = converted.value;
    return emit_typed_instruction(context, QL_IR_OPCODE_STORE,
                                  context->memory_type, operands, 3u,
                                  QL_IR_EFFECT_MEMORY,
                                  &context->memory_value, error);
}

/* `p + n` moves by n elements, so the offset is scaled by the pointee's
   storage width before it reaches PTR_ADD. */
static ql_status emit_pointer_offset(lower_context *context,
                                     lower_value pointer, lower_value offset,
                                     int subtract, lower_value *output,
                                     ql_error *error) {
    lower_type u64 = address_type();
    lower_value widened;
    ql_ir_value_id scale;
    ql_ir_value_id scaled;
    ql_ir_value_id operands[2];
    ql_status status;

    if (pointer.type.pointee.kind == QL_C_SCALAR_VOID) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, SIZE_MAX,
            "arithmetic on a pointer to void has no element size", error);
    }
    status = convert_value(context, offset, u64, &widened, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (subtract) {
        status = emit_instruction(context, QL_IR_OPCODE_BV_NEG, &u64,
                                  &widened.value, 1u, NULL, 0u,
                                  QL_IR_EFFECT_NONE, &widened.value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    status = add_uint_constant(context, u64,
                               pointee_byte_width(pointer.type), &scale,
                               error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    operands[0] = widened.value;
    operands[1] = scale;
    status = emit_instruction(context, QL_IR_OPCODE_MUL, &u64, operands, 2u,
                              NULL, 0u, QL_IR_EFFECT_NONE, &scaled, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    *output = pointer;
    operands[0] = pointer.value;
    operands[1] = scaled;
    status = combine_defined(context, &pointer, &offset, &output->defined,
                             error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    output->may_ub = pointer.may_ub | offset.may_ub;
    return emit_instruction(context, QL_IR_OPCODE_PTR_ADD, &pointer.type,
                            operands, 2u, NULL, 0u, QL_IR_EFFECT_NONE,
                            &output->value, error);
}

static ql_status add_block(lower_context *context, const char *prefix,
                           ql_ir_block_id *output, ql_error *error) {
    char label[64];
    int length = snprintf(label, sizeof(label), "%s.%llu", prefix,
                          (unsigned long long)context->next_block_label++);
    if (length < 0 || (size_t)length >= sizeof(label)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "IR block label cannot be formatted");
        return QL_STATUS_INTERNAL_ERROR;
    }
    return ql_ir_builder_add_block(context->builder, label, (size_t)length,
                                   output, error);
}

static ql_status set_branch(lower_context *context, ql_ir_block_id block,
                            ql_ir_block_id target, ql_error *error) {
    ql_ir_terminator_definition_v1 terminator;
    ql_ir_terminator_definition_init(&terminator, QL_IR_TERMINATOR_BRANCH);
    terminator.target = target;
    return ql_ir_builder_set_terminator(context->builder, block, &terminator,
                                        error);
}

static ql_status set_cond_branch(lower_context *context,
                                 ql_ir_block_id block,
                                 ql_ir_value_id condition,
                                 ql_ir_block_id true_target,
                                 ql_ir_block_id false_target,
                                 ql_error *error) {
    ql_ir_terminator_definition_v1 terminator;
    ql_ir_terminator_definition_init(&terminator,
                                     QL_IR_TERMINATOR_COND_BRANCH);
    terminator.condition = condition;
    terminator.target = true_target;
    terminator.false_target = false_target;
    return ql_ir_builder_set_terminator(context->builder, block, &terminator,
                                        error);
}

static ql_status add_variable(lower_context *context, const char *name,
                              size_t name_size, lower_type type,
                              ql_ir_value_id value, uint32_t initialized,
                              uint32_t is_const, size_t node,
                              ql_error *error) {
    lower_variable *variable;
    size_t index;
    ql_status status;

    for (index = context->variable_count; index != 0u; --index) {
        const lower_variable *existing = &context->variables[index - 1u];
        if (existing->scope_depth < context->scope_depth) {
            break;
        }
        if (existing->scope_depth == context->scope_depth &&
            existing->name_size == name_size &&
            memcmp(existing->name, name, name_size) == 0) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_DUPLICATE_DECLARATION, node,
                "identifier is declared more than once in the same scope",
                error);
        }
    }
    status = grow_array(context->allocator, (void **)&context->variables,
                        &context->variable_capacity,
                        sizeof(*context->variables),
                        context->variable_count + 1u, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    variable = &context->variables[context->variable_count];
    memset(variable, 0, sizeof(*variable));
    variable->name = copy_text(context->allocator, name, name_size);
    if (variable->name == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    variable->name_size = name_size;
    variable->type = type;
    variable->value = value;
    variable->initialized = initialized;
    variable->is_const = is_const;
    variable->scope_depth = context->scope_depth;
    ++context->variable_count;
    return QL_STATUS_OK;
}

static lower_variable *find_variable(lower_context *context,
                                     const char *name, size_t name_size) {
    size_t index;
    for (index = context->variable_count; index != 0u; --index) {
        lower_variable *variable = &context->variables[index - 1u];
        if (variable->name_size == name_size &&
            memcmp(variable->name, name, name_size) == 0) {
            return variable;
        }
    }
    return NULL;
}

static void pop_variables(lower_context *context, size_t marker) {
    while (context->variable_count > marker) {
        --context->variable_count;
        context->allocator->deallocate(
            context->allocator->user_data,
            context->variables[context->variable_count].name);
        memset(&context->variables[context->variable_count], 0,
               sizeof(context->variables[context->variable_count]));
    }
}

static ql_status save_state(lower_context *context, size_t count,
                            lower_state *state, ql_error *error) {
    size_t index;
    memset(state, 0, sizeof(*state));
    state->count = count;
    state->memory = context->memory_value;
    if (count == 0u) {
        return QL_STATUS_OK;
    }
    state->values = context->allocator->allocate(
        context->allocator->user_data, count * sizeof(*state->values));
    state->initialized = context->allocator->allocate(
        context->allocator->user_data,
        count * sizeof(*state->initialized));
    if (state->values == NULL || state->initialized == NULL) {
        context->allocator->deallocate(context->allocator->user_data,
                                       state->values);
        context->allocator->deallocate(context->allocator->user_data,
                                       state->initialized);
        memset(state, 0, sizeof(*state));
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    for (index = 0u; index < count; ++index) {
        state->values[index] = context->variables[index].value;
        state->initialized[index] =
            (uint8_t)context->variables[index].initialized;
    }
    return QL_STATUS_OK;
}

static void restore_state(lower_context *context, const lower_state *state) {
    size_t index;
    context->memory_value = state->memory;
    for (index = 0u; index < state->count; ++index) {
        context->variables[index].value = state->values[index];
        context->variables[index].initialized = state->initialized[index];
    }
}

static void destroy_state(lower_context *context, lower_state *state) {
    context->allocator->deallocate(context->allocator->user_data,
                                   state->values);
    context->allocator->deallocate(context->allocator->user_data,
                                   state->initialized);
    memset(state, 0, sizeof(*state));
}

static int type_can_represent(lower_type type, uint64_t value) {
    return ql_c_scalar_can_represent(scalar_of(type), value);
}

static int literal_digit(char ch, uint32_t base, uint32_t *digit) {
    uint32_t value;
    if (ch >= '0' && ch <= '9') {
        value = (uint32_t)(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
        value = 10u + (uint32_t)(ch - 'a');
    } else if (ch >= 'A' && ch <= 'F') {
        value = 10u + (uint32_t)(ch - 'A');
    } else {
        return 0;
    }
    if (value >= base) {
        return 0;
    }
    *digit = value;
    return 1;
}

static ql_status lower_integer_literal(lower_context *context, size_t node,
                                       lower_value *output,
                                       ql_error *error) {
    char *text = copy_node_text(context, node);
    char suffix[4];
    size_t length;
    size_t suffix_start;
    size_t digit_start = 0u;
    size_t suffix_size = 0u;
    size_t index;
    uint32_t base = 10u;
    uint64_t value = 0u;
    uint32_t is_decimal = 1u;
    lower_type candidates[6];
    size_t candidate_count = 0u;
    lower_type selected;
    ql_status status;

    if (text == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    length = strlen(text);
    suffix_start = length;
    while (suffix_start != 0u) {
        char ch = text[suffix_start - 1u];
        if (ch != 'u' && ch != 'U' && ch != 'l' && ch != 'L') {
            break;
        }
        --suffix_start;
    }
    if (length - suffix_start >= sizeof(suffix)) {
        context->allocator->deallocate(context->allocator->user_data, text);
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_INTEGER_LITERAL_OUT_OF_RANGE,
            node, "integer literal suffix is not supported", error);
    }
    for (index = suffix_start; index < length; ++index) {
        char ch = text[index];
        suffix[suffix_size++] =
            (char)((ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch);
    }
    suffix[suffix_size] = '\0';
    if (!(strcmp(suffix, "") == 0 || strcmp(suffix, "u") == 0 ||
          strcmp(suffix, "l") == 0 || strcmp(suffix, "ul") == 0 ||
          strcmp(suffix, "lu") == 0 || strcmp(suffix, "ll") == 0 ||
          strcmp(suffix, "ull") == 0 || strcmp(suffix, "llu") == 0)) {
        context->allocator->deallocate(context->allocator->user_data, text);
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_INTEGER_LITERAL_OUT_OF_RANGE,
            node, "integer literal suffix is not supported", error);
    }
    if (suffix_start >= 2u && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        base = 16u;
        digit_start = 2u;
        is_decimal = 0u;
    } else if (suffix_start >= 2u && text[0] == '0' &&
               (text[1] == 'b' || text[1] == 'B')) {
        base = 2u;
        digit_start = 2u;
        is_decimal = 0u;
    } else if (suffix_start > 1u && text[0] == '0') {
        base = 8u;
        digit_start = 1u;
        is_decimal = 0u;
    }
    if (digit_start == suffix_start && !(suffix_start == 1u && text[0] == '0')) {
        context->allocator->deallocate(context->allocator->user_data, text);
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_INTEGER_LITERAL_OUT_OF_RANGE,
            node, "integer literal has no digits", error);
    }
    if (suffix_start == 1u && text[0] == '0') {
        value = 0u;
    } else {
        for (index = digit_start; index < suffix_start; ++index) {
            uint32_t digit;
            if (literal_digit(text[index], base, &digit) == 0 ||
                value > (UINT64_MAX - digit) / base) {
                context->allocator->deallocate(
                    context->allocator->user_data, text);
                return lower_unknown(
                    context,
                    QL_C_LOWER_DIAGNOSTIC_INTEGER_LITERAL_OUT_OF_RANGE,
                    node,
                    "integer literal is invalid or exceeds 64 bits",
                    error);
            }
            value = value * base + digit;
        }
    }

#define ADD_CANDIDATE(w, r, s) \
    candidates[candidate_count++] = make_integer_type((w), (r), (s))
    if (strcmp(suffix, "u") == 0) {
        ADD_CANDIDATE(32u, 3u, 0u);
        ADD_CANDIDATE(64u, 4u, 0u);
        ADD_CANDIDATE(64u, 5u, 0u);
    } else if (strcmp(suffix, "l") == 0) {
        ADD_CANDIDATE(64u, 4u, 1u);
        if (is_decimal == 0u) {
            ADD_CANDIDATE(64u, 4u, 0u);
        }
        ADD_CANDIDATE(64u, 5u, 1u);
        if (is_decimal == 0u) {
            ADD_CANDIDATE(64u, 5u, 0u);
        }
    } else if (strcmp(suffix, "ul") == 0 ||
               strcmp(suffix, "lu") == 0) {
        ADD_CANDIDATE(64u, 4u, 0u);
        ADD_CANDIDATE(64u, 5u, 0u);
    } else if (strcmp(suffix, "ll") == 0) {
        ADD_CANDIDATE(64u, 5u, 1u);
        if (is_decimal == 0u) {
            ADD_CANDIDATE(64u, 5u, 0u);
        }
    } else if (strcmp(suffix, "ull") == 0 ||
               strcmp(suffix, "llu") == 0) {
        ADD_CANDIDATE(64u, 5u, 0u);
    } else if (is_decimal != 0u) {
        ADD_CANDIDATE(32u, 3u, 1u);
        ADD_CANDIDATE(64u, 4u, 1u);
        ADD_CANDIDATE(64u, 5u, 1u);
    } else {
        ADD_CANDIDATE(32u, 3u, 1u);
        ADD_CANDIDATE(32u, 3u, 0u);
        ADD_CANDIDATE(64u, 4u, 1u);
        ADD_CANDIDATE(64u, 4u, 0u);
        ADD_CANDIDATE(64u, 5u, 1u);
        ADD_CANDIDATE(64u, 5u, 0u);
    }
#undef ADD_CANDIDATE

    memset(&selected, 0, sizeof(selected));
    for (index = 0u; index < candidate_count; ++index) {
        if (type_can_represent(candidates[index], value) != 0) {
            selected = candidates[index];
            break;
        }
    }
    context->allocator->deallocate(context->allocator->user_data, text);
    if (index == candidate_count) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_INTEGER_LITERAL_OUT_OF_RANGE,
            node, "integer literal has no ASM2C_GNU_V1 integer type", error);
    }
    status = add_uint_constant(context, selected, value, &output->value,
                               error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ensure_bool_constants(context, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    output->defined = context->true_value;
    output->type = selected;
    output->may_ub = 0u;
    return QL_STATUS_OK;
}

static ql_status convert_value(lower_context *context, lower_value input,
                               lower_type target, lower_value *output,
                               ql_error *error);

static lower_type address_type(void) {
    return make_integer_type(QL_C_POINTER_WIDTH, 4u, 0u);
}

/* An address as a bit-vector, which is where pointer arithmetic and every
   comparison happen. Under this profile that reinterpretation is exact: a
   pointer is its address. */
static ql_status emit_address_of_pointer(lower_context *context,
                                         lower_value pointer,
                                         lower_value *output,
                                         ql_error *error) {
    lower_type u64 = address_type();
    ql_status status;

    *output = pointer;
    output->type = u64;
    status = emit_instruction(context, QL_IR_OPCODE_PTR_TO_BV, &u64,
                              &pointer.value, 1u, NULL, 0u,
                              QL_IR_EFFECT_NONE, &output->value, error);
    return status;
}

static ql_status emit_pointer_of_address(lower_context *context,
                                         lower_value address,
                                         lower_type target,
                                         lower_value *output,
                                         ql_error *error) {
    ql_status status = ensure_ir_type(context, &target, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    *output = address;
    output->type = target;
    return emit_instruction(context, QL_IR_OPCODE_BV_TO_PTR, &target,
                            &address.value, 1u, NULL, 0u, QL_IR_EFFECT_NONE,
                            &output->value, error);
}

/* Conversions with a pointer on either side all route through the address,
   which keeps null literals, casts, and truth tests on one path. */
static ql_status convert_across_pointer(lower_context *context,
                                        lower_value input, lower_type target,
                                        lower_value *output,
                                        ql_error *error) {
    lower_type u64 = address_type();
    lower_value address;
    ql_status status;

    if (input.type.kind == QL_C_SCALAR_POINTER) {
        status = emit_address_of_pointer(context, input, &address, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (target.kind == QL_C_SCALAR_POINTER) {
            return emit_pointer_of_address(context, address, target, output,
                                           error);
        }
        return convert_value(context, address, target, output, error);
    }
    if (input.type.kind == QL_C_SCALAR_VOID) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, SIZE_MAX,
            "void does not convert to a pointer", error);
    }
    status = convert_value(context, input, u64, &address, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return emit_pointer_of_address(context, address, target, output, error);
}

static ql_status convert_value(lower_context *context, lower_value input,
                               lower_type target, lower_value *output,
                               ql_error *error) {
    ql_ir_opcode opcode;
    ql_ir_value_id zero;
    ql_status status;

    *output = input;
    if (type_same(input.type, target) != 0) {
        output->type = target;
        return QL_STATUS_OK;
    }
    if (input.type.kind == QL_C_SCALAR_POINTER ||
        target.kind == QL_C_SCALAR_POINTER) {
        return convert_across_pointer(context, input, target, output, error);
    }
    if (target.kind == QL_C_SCALAR_BOOL) {
        if (input.type.kind == QL_C_SCALAR_BOOL) {
            output->type = target;
            return QL_STATUS_OK;
        }
        status = add_uint_constant(context, input.type, 0u, &zero, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_compare(context, QL_IR_OPCODE_NE, input.value, zero,
                              &output->value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        output->type = target;
        return QL_STATUS_OK;
    }
    if (input.type.kind == QL_C_SCALAR_BOOL) {
        ql_ir_value_id operands[3];
        status = add_uint_constant(context, target, 1u, &operands[1], error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = add_uint_constant(context, target, 0u, &operands[2], error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        operands[0] = input.value;
        status = emit_instruction(context, QL_IR_OPCODE_SELECT, &target,
                                  operands, 3u, NULL, 0u,
                                  QL_IR_EFFECT_NONE, &output->value, error);
        if (status == QL_STATUS_OK) {
            output->type = target;
        }
        return status;
    } else if (input.type.width < target.width) {
        opcode = input.type.is_signed != 0u ? QL_IR_OPCODE_SEXT
                                            : QL_IR_OPCODE_ZEXT;
    } else if (input.type.width > target.width) {
        opcode = QL_IR_OPCODE_TRUNC;
    } else {
        opcode = QL_IR_OPCODE_IDENTITY;
    }
    status = emit_instruction(context, opcode, &target, &input.value, 1u,
                              NULL, 0u, QL_IR_EFFECT_NONE, &output->value,
                              error);
    if (status == QL_STATUS_OK) {
        output->type = target;
    }
    return status;
}

static ql_status integer_promote(lower_context *context, lower_value input,
                                 lower_value *output, ql_error *error) {
    const lower_type promoted =
        type_from_scalar(ql_c_scalar_promote(scalar_of(input.type)));
    if (!type_same(input.type, promoted)) {
        return convert_value(context, input, promoted, output, error);
    }
    *output = input;
    return QL_STATUS_OK;
}

static lower_type usual_common_type(lower_type left, lower_type right) {
    return type_from_scalar(ql_c_scalar_usual(scalar_of(left),
                                              scalar_of(right)));
}

static ql_status usual_arithmetic_conversions(
    lower_context *context, lower_value left, lower_value right,
    lower_value *converted_left, lower_value *converted_right,
    lower_type *common, ql_error *error) {
    lower_value promoted_left;
    lower_value promoted_right;
    ql_status status = integer_promote(context, left, &promoted_left, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    status = integer_promote(context, right, &promoted_right, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    *common = usual_common_type(promoted_left.type, promoted_right.type);
    status = convert_value(context, promoted_left, *common, converted_left,
                           error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return convert_value(context, promoted_right, *common, converted_right,
                         error);
}

static ql_status combine_defined(lower_context *context,
                                 const lower_value *left,
                                 const lower_value *right,
                                 ql_ir_value_id *output, ql_error *error) {
    if (left->may_ub == 0u && right->may_ub == 0u) {
        ql_status status = ensure_bool_constants(context, error);
        if (status == QL_STATUS_OK) {
            *output = context->true_value;
        }
        return status;
    }
    if (left->may_ub == 0u) {
        *output = right->defined;
        return QL_STATUS_OK;
    }
    if (right->may_ub == 0u) {
        *output = left->defined;
        return QL_STATUS_OK;
    }
    return emit_bool_and(context, left->defined, right->defined, output,
                         error);
}

static ql_status signed_addsub_defined(lower_context *context,
                                       lower_type type,
                                       ql_ir_value_id left,
                                       ql_ir_value_id right,
                                       ql_ir_value_id result,
                                       uint32_t is_subtraction,
                                       ql_ir_value_id *output,
                                       ql_error *error) {
    ql_ir_value_id zero;
    ql_ir_value_id left_negative;
    ql_ir_value_id right_negative;
    ql_ir_value_id result_negative;
    ql_ir_value_id sign_condition;
    ql_ir_value_id result_matches;
    ql_status status;

    status = add_uint_constant(context, type, 0u, &zero, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_compare(context, QL_IR_OPCODE_SLT, left, zero,
                          &left_negative, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_compare(context, QL_IR_OPCODE_SLT, right, zero,
                          &right_negative, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_compare(context, QL_IR_OPCODE_SLT, result, zero,
                          &result_negative, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_compare(context,
                          is_subtraction != 0u ? QL_IR_OPCODE_EQ
                                               : QL_IR_OPCODE_NE,
                          left_negative, right_negative, &sign_condition,
                          error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_compare(context, QL_IR_OPCODE_EQ, left_negative,
                          result_negative, &result_matches, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return emit_bool_or(context, sign_condition, result_matches, output,
                        error);
}

static ql_status emit_arithmetic_result(
    lower_context *context, ql_ir_opcode opcode, lower_type type,
    ql_ir_value_id left, ql_ir_value_id right, ql_ir_value_id inherited_defined,
    uint32_t inherited_may_ub, lower_value *output, ql_error *error) {
    ql_ir_value_id operands[2];
    ql_ir_value_id operation_defined;
    ql_status status;

    operands[0] = left;
    operands[1] = right;
    status = emit_instruction(context, opcode, &type, operands, 2u, NULL, 0u,
                              QL_IR_EFFECT_NONE, &output->value, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    output->type = type;
    output->defined = inherited_defined;
    output->may_ub = inherited_may_ub;
    if (type.is_signed == 0u ||
        (opcode != QL_IR_OPCODE_ADD && opcode != QL_IR_OPCODE_SUB)) {
        return QL_STATUS_OK;
    }
    status = signed_addsub_defined(context, type, left, right, output->value,
                                   opcode == QL_IR_OPCODE_SUB,
                                   &operation_defined, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (output->may_ub != 0u) {
        status = emit_bool_and(context, output->defined, operation_defined,
                               &output->defined, error);
    } else {
        output->defined = operation_defined;
        status = QL_STATUS_OK;
    }
    output->may_ub = 1u;
    return status;
}

static ql_status emit_multiply_result(
    lower_context *context, lower_type type, ql_ir_value_id left,
    ql_ir_value_id right, ql_ir_value_id inherited_defined,
    uint32_t inherited_may_ub, lower_value *output, ql_error *error) {
    ql_ir_value_id operands[2];
    ql_status status;

    operands[0] = left;
    operands[1] = right;
    output->type = type;
    output->defined = inherited_defined;
    output->may_ub = inherited_may_ub;
    if (type.is_signed == 0u) {
        return emit_instruction(context, QL_IR_OPCODE_MUL, &type, operands,
                                2u, NULL, 0u, QL_IR_EFFECT_NONE,
                                &output->value, error);
    }
    {
        lower_type wide = make_integer_type(type.width * 2u, type.rank + 1u,
                                            1u);
        ql_ir_value_id wide_left;
        ql_ir_value_id wide_right;
        ql_ir_value_id wide_product;
        ql_ir_value_id reextended;
        ql_ir_value_id operation_defined;

        status = emit_instruction(context, QL_IR_OPCODE_SEXT, &wide, &left,
                                  1u, NULL, 0u, QL_IR_EFFECT_NONE,
                                  &wide_left, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_instruction(context, QL_IR_OPCODE_SEXT, &wide, &right,
                                  1u, NULL, 0u, QL_IR_EFFECT_NONE,
                                  &wide_right, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        operands[0] = wide_left;
        operands[1] = wide_right;
        status = emit_instruction(context, QL_IR_OPCODE_MUL, &wide, operands,
                                  2u, NULL, 0u, QL_IR_EFFECT_NONE,
                                  &wide_product, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_instruction(context, QL_IR_OPCODE_TRUNC, &type,
                                  &wide_product, 1u, NULL, 0u,
                                  QL_IR_EFFECT_NONE, &output->value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_instruction(context, QL_IR_OPCODE_SEXT, &wide,
                                  &output->value, 1u, NULL, 0u,
                                  QL_IR_EFFECT_NONE, &reextended, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_compare(context, QL_IR_OPCODE_EQ, wide_product,
                              reextended, &operation_defined, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (output->may_ub != 0u) {
            status = emit_bool_and(context, output->defined,
                                   operation_defined, &output->defined,
                                   error);
        } else {
            output->defined = operation_defined;
            status = QL_STATUS_OK;
        }
        output->may_ub = 1u;
        return status;
    }
}

static ql_status signed_min_constant(lower_context *context, lower_type type,
                                     ql_ir_value_id *output,
                                     ql_error *error) {
    uint64_t bits = UINT64_C(1) << (type.width - 1u);
    return add_uint_constant(context, type, bits, output, error);
}

static ql_status all_ones_constant(lower_context *context, lower_type type,
                                   ql_ir_value_id *output,
                                   ql_error *error) {
    uint64_t bits = type.width == 64u
                        ? UINT64_MAX
                        : (UINT64_C(1) << type.width) - UINT64_C(1);
    return add_uint_constant(context, type, bits, output, error);
}

static ql_status emit_divrem_result(
    lower_context *context, uint32_t is_remainder, lower_type type,
    ql_ir_value_id left, ql_ir_value_id right,
    ql_ir_value_id inherited_defined, uint32_t inherited_may_ub,
    lower_value *output, ql_error *error) {
    ql_ir_value_id operands[2];
    ql_ir_value_id zero;
    ql_ir_value_id nonzero;
    ql_ir_value_id operation_defined;
    ql_ir_opcode opcode;
    ql_status status;

    operands[0] = left;
    operands[1] = right;
    opcode = type.is_signed != 0u
                 ? (is_remainder != 0u ? QL_IR_OPCODE_SREM
                                        : QL_IR_OPCODE_SDIV)
                 : (is_remainder != 0u ? QL_IR_OPCODE_UREM
                                        : QL_IR_OPCODE_UDIV);
    status = emit_instruction(context, opcode, &type, operands, 2u, NULL, 0u,
                              QL_IR_EFFECT_NONE, &output->value, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = add_uint_constant(context, type, 0u, &zero, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_compare(context, QL_IR_OPCODE_NE, right, zero, &nonzero,
                          error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    operation_defined = nonzero;
    if (type.is_signed != 0u) {
        ql_ir_value_id minimum;
        ql_ir_value_id minus_one;
        ql_ir_value_id is_minimum;
        ql_ir_value_id is_minus_one;
        ql_ir_value_id overflow;
        ql_ir_value_id no_overflow;

        status = signed_min_constant(context, type, &minimum, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = all_ones_constant(context, type, &minus_one, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_compare(context, QL_IR_OPCODE_EQ, left, minimum,
                              &is_minimum, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_compare(context, QL_IR_OPCODE_EQ, right, minus_one,
                              &is_minus_one, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_bool_and(context, is_minimum, is_minus_one, &overflow,
                               error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_bool_not(context, overflow, &no_overflow, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_bool_and(context, operation_defined, no_overflow,
                               &operation_defined, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    output->type = type;
    output->defined = operation_defined;
    output->may_ub = 1u;
    if (inherited_may_ub != 0u) {
        status = emit_bool_and(context, inherited_defined,
                               operation_defined, &output->defined, error);
    }
    return status;
}

static ql_status emit_shift_result(
    lower_context *context, uint32_t is_left_shift, lower_value left,
    lower_value right, lower_value *output, ql_error *error) {
    lower_value promoted_left;
    lower_value promoted_right;
    lower_value right_u64;
    lower_value right_for_operation;
    lower_type u64 = make_integer_type(64u, 5u, 0u);
    lower_type operation_right_type;
    ql_ir_value_id width_constant;
    ql_ir_value_id amount_valid;
    ql_ir_value_id inherited_defined;
    uint32_t inherited_may_ub;
    ql_ir_value_id operands[2];
    ql_ir_value_id operation_defined;
    ql_status status;

    status = integer_promote(context, left, &promoted_left, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = integer_promote(context, right, &promoted_right, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = combine_defined(context, &promoted_left, &promoted_right,
                             &inherited_defined, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    inherited_may_ub = promoted_left.may_ub | promoted_right.may_ub;
    status = convert_value(context, promoted_right, u64, &right_u64, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = add_uint_constant(context, u64, promoted_left.type.width,
                               &width_constant, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_compare(context, QL_IR_OPCODE_ULT, right_u64.value,
                          width_constant, &amount_valid, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    operation_right_type = make_integer_type(promoted_left.type.width,
                                             promoted_left.type.rank, 0u);
    status = convert_value(context, promoted_right, operation_right_type,
                           &right_for_operation, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    operands[0] = promoted_left.value;
    operands[1] = right_for_operation.value;
    operation_defined = amount_valid;
    output->type = promoted_left.type;

    if (is_left_shift != 0u && promoted_left.type.is_signed != 0u) {
        lower_type wide = make_integer_type(promoted_left.type.width * 2u,
                                            promoted_left.type.rank + 1u,
                                            1u);
        lower_type wide_unsigned = wide;
        lower_value wide_amount;
        ql_ir_value_id wide_left;
        ql_ir_value_id wide_result;
        ql_ir_value_id reextended;
        ql_ir_value_id fits;
        ql_ir_value_id zero;
        ql_ir_value_id nonnegative;

        wide_unsigned.is_signed = 0u;
        status = emit_instruction(context, QL_IR_OPCODE_SEXT, &wide,
                                  &promoted_left.value, 1u, NULL, 0u,
                                  QL_IR_EFFECT_NONE, &wide_left, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = convert_value(context, right_u64, wide_unsigned,
                               &wide_amount, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        operands[0] = wide_left;
        operands[1] = wide_amount.value;
        status = emit_instruction(context, QL_IR_OPCODE_SHL, &wide, operands,
                                  2u, NULL, 0u, QL_IR_EFFECT_NONE,
                                  &wide_result, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_instruction(context, QL_IR_OPCODE_TRUNC,
                                  &promoted_left.type, &wide_result, 1u,
                                  NULL, 0u, QL_IR_EFFECT_NONE,
                                  &output->value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_instruction(context, QL_IR_OPCODE_SEXT, &wide,
                                  &output->value, 1u, NULL, 0u,
                                  QL_IR_EFFECT_NONE, &reextended, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_compare(context, QL_IR_OPCODE_EQ, wide_result,
                              reextended, &fits, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = add_uint_constant(context, promoted_left.type, 0u, &zero,
                                   error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_compare(context, QL_IR_OPCODE_SLE, zero,
                              promoted_left.value, &nonnegative, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_bool_and(context, operation_defined, nonnegative,
                               &operation_defined, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_bool_and(context, operation_defined, fits,
                               &operation_defined, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    } else {
        ql_ir_opcode opcode =
            is_left_shift != 0u
                ? QL_IR_OPCODE_SHL
                : (promoted_left.type.is_signed != 0u ? QL_IR_OPCODE_ASHR
                                                       : QL_IR_OPCODE_LSHR);
        status = emit_instruction(context, opcode, &promoted_left.type,
                                  operands, 2u, NULL, 0u, QL_IR_EFFECT_NONE,
                                  &output->value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    output->defined = operation_defined;
    if (inherited_may_ub != 0u) {
        status = emit_bool_and(context, inherited_defined,
                               operation_defined, &output->defined, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    output->may_ub = 1u;
    return QL_STATUS_OK;
}

static ql_status lower_expression(lower_context *context, size_t node,
                                  lower_value *output, ql_error *error);

static ql_status lower_identifier(lower_context *context, size_t node,
                                  lower_value *output, ql_error *error) {
    char *name = copy_node_text(context, node);
    lower_variable *variable;
    ql_status status;

    if (name == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    variable = find_variable(context, name, strlen(name));
    context->allocator->deallocate(context->allocator->user_data, name);
    if (variable == NULL) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNDECLARED_IDENTIFIER, node,
            "identifier does not name a parameter or visible local variable",
            error);
    }
    if (variable->initialized == 0u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNINITIALIZED_READ, node,
            "read of an uninitialized local has no modeled value", error);
    }
    status = ensure_bool_constants(context, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    output->value = variable->value;
    output->defined = context->true_value;
    output->type = variable->type;
    output->may_ub = 0u;
    return QL_STATUS_OK;
}

static ql_status lower_unary_expression(lower_context *context, size_t node,
                                        lower_value *output,
                                        ql_error *error) {
    size_t argument_node = direct_field_child(context, node, "argument");
    size_t operator_node = direct_field_child(context, node, "operator");
    lower_value argument;
    lower_value promoted;
    char *operator_text;
    ql_status status;

    if (argument_node == SIZE_MAX || operator_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "unary expression is missing an operator or argument", error);
    }
    status = lower_expression(context, argument_node, &argument, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    operator_text = copy_node_text(context, operator_node);
    if (operator_text == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    if (strcmp(operator_text, "!") == 0) {
        lower_value boolean;
        lower_value integer;
        lower_type result_type = make_integer_type(32u, 3u, 1u);
        status = convert_value(context, argument, make_bool_type(), &boolean,
                               error);
        if (status == QL_STATUS_OK) {
            status = emit_bool_not(context, boolean.value, &integer.value,
                                   error);
        }
        if (status == QL_STATUS_OK) {
            integer.type = make_bool_type();
            integer.defined = argument.defined;
            integer.may_ub = argument.may_ub;
            status = convert_value(context, integer, result_type, output,
                                   error);
        }
    } else {
        status = integer_promote(context, argument, &promoted, error);
        if (status == QL_STATUS_OK && strcmp(operator_text, "+") == 0) {
            *output = promoted;
        } else if (status == QL_STATUS_OK &&
                   strcmp(operator_text, "~") == 0) {
            status = emit_instruction(context, QL_IR_OPCODE_BV_NOT,
                                      &promoted.type, &promoted.value, 1u,
                                      NULL, 0u, QL_IR_EFFECT_NONE,
                                      &output->value, error);
            output->type = promoted.type;
            output->defined = promoted.defined;
            output->may_ub = promoted.may_ub;
        } else if (status == QL_STATUS_OK &&
                   strcmp(operator_text, "-") == 0) {
            ql_ir_value_id operation_defined;
            status = emit_instruction(context, QL_IR_OPCODE_BV_NEG,
                                      &promoted.type, &promoted.value, 1u,
                                      NULL, 0u, QL_IR_EFFECT_NONE,
                                      &output->value, error);
            output->type = promoted.type;
            output->defined = promoted.defined;
            output->may_ub = promoted.may_ub;
            if (status == QL_STATUS_OK && promoted.type.is_signed != 0u) {
                ql_ir_value_id minimum;
                status = signed_min_constant(context, promoted.type,
                                             &minimum, error);
                if (status == QL_STATUS_OK) {
                    status = emit_compare(context, QL_IR_OPCODE_NE,
                                          promoted.value, minimum,
                                          &operation_defined, error);
                }
                if (status == QL_STATUS_OK && output->may_ub != 0u) {
                    status = emit_bool_and(context, output->defined,
                                           operation_defined,
                                           &output->defined, error);
                } else if (status == QL_STATUS_OK) {
                    output->defined = operation_defined;
                }
                output->may_ub = 1u;
            }
        } else if (status == QL_STATUS_OK) {
            status = lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "unary operator is outside the integer lowering slice",
                error);
        }
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   operator_text);
    return status;
}

static ql_status lower_logical_expression(
    lower_context *context, const char *operator_text, lower_value left,
    lower_value right, lower_value *output, ql_error *error) {
    lower_value left_bool;
    lower_value right_bool;
    lower_value bool_result;
    lower_type int_type = make_integer_type(32u, 3u, 1u);
    ql_ir_value_id rhs_needed;
    ql_ir_value_id rhs_not_needed;
    ql_ir_value_id rhs_safe;
    ql_ir_value_id left_defined;
    ql_ir_value_id right_defined;
    ql_ir_value_id operands[3];
    ql_status status;

    status = convert_value(context, left, make_bool_type(), &left_bool,
                           error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = convert_value(context, right, make_bool_type(), &right_bool,
                           error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ensure_bool_constants(context, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    left_defined = left.may_ub != 0u ? left.defined : context->true_value;
    right_defined = right.may_ub != 0u ? right.defined : context->true_value;
    if (strcmp(operator_text, "&&") == 0) {
        rhs_needed = left_bool.value;
        operands[0] = left_bool.value;
        operands[1] = right_bool.value;
        operands[2] = context->false_value;
    } else {
        status = emit_bool_not(context, left_bool.value, &rhs_needed, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        operands[0] = left_bool.value;
        operands[1] = context->true_value;
        operands[2] = right_bool.value;
    }
    status = emit_instruction(context, QL_IR_OPCODE_SELECT,
                              &left_bool.type, operands, 3u, NULL, 0u,
                              QL_IR_EFFECT_NONE, &bool_result.value, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_bool_not(context, rhs_needed, &rhs_not_needed, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_bool_or(context, rhs_not_needed, right_defined, &rhs_safe,
                          error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_bool_and(context, left_defined, rhs_safe,
                           &bool_result.defined, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    bool_result.type = make_bool_type();
    bool_result.may_ub = left.may_ub | right.may_ub;
    return convert_value(context, bool_result, int_type, output, error);
}

/* Comparison and arithmetic where at least one side is a pointer. Under this
   profile the comparison is on addresses, so operands in different objects
   compare rather than being undefined; ARCHITECTURE.md records that the
   verdict is relative to the profile for exactly this reason. */
static ql_status lower_pointer_binary(lower_context *context,
                                      const char *operator_text, size_t node,
                                      lower_value left, lower_value right,
                                      lower_value *output, ql_error *error) {
    lower_type u64 = address_type();
    lower_type result_type = make_integer_type(32u, 3u, 1u);
    lower_value left_address;
    lower_value right_address;
    lower_value comparison;
    ql_ir_opcode opcode;
    ql_status status;

    if (strcmp(operator_text, "+") == 0 || strcmp(operator_text, "-") == 0) {
        const int subtract = strcmp(operator_text, "-") == 0;
        if (left.type.kind == QL_C_SCALAR_POINTER &&
            right.type.kind == QL_C_SCALAR_POINTER) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
                "the difference of two pointers is not in this slice", error);
        }
        if (left.type.kind != QL_C_SCALAR_POINTER) {
            if (subtract) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
                    "an integer minus a pointer is not a C expression",
                    error);
            }
            return emit_pointer_offset(context, right, left, 0, output,
                                       error);
        }
        return emit_pointer_offset(context, left, right, subtract, output,
                                   error);
    }
    if (strcmp(operator_text, "==") == 0) {
        opcode = QL_IR_OPCODE_EQ;
    } else if (strcmp(operator_text, "!=") == 0) {
        opcode = QL_IR_OPCODE_NE;
    } else if (strcmp(operator_text, "<") == 0) {
        opcode = QL_IR_OPCODE_ULT;
    } else if (strcmp(operator_text, "<=") == 0) {
        opcode = QL_IR_OPCODE_ULE;
    } else if (strcmp(operator_text, ">") == 0 ||
               strcmp(operator_text, ">=") == 0) {
        lower_value swap = left;
        left = right;
        right = swap;
        opcode = strcmp(operator_text, ">") == 0 ? QL_IR_OPCODE_ULT
                                                 : QL_IR_OPCODE_ULE;
    } else {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
            "operator does not apply to a pointer", error);
    }
    status = convert_value(context, left, u64, &left_address, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = convert_value(context, right, u64, &right_address, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    memset(&comparison, 0, sizeof(comparison));
    comparison.type = make_bool_type();
    status = combine_defined(context, &left, &right, &comparison.defined,
                             error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    comparison.may_ub = left.may_ub | right.may_ub;
    status = emit_compare(context, opcode, left_address.value,
                          right_address.value, &comparison.value, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return convert_value(context, comparison, result_type, output, error);
}

static ql_status lower_binary_expression(lower_context *context, size_t node,
                                         lower_value *output,
                                         ql_error *error) {
    size_t left_node = direct_field_child(context, node, "left");
    size_t right_node = direct_field_child(context, node, "right");
    size_t operator_node = direct_field_child(context, node, "operator");
    lower_value left;
    lower_value right;
    lower_value converted_left;
    lower_value converted_right;
    lower_type common;
    char *operator_text;
    ql_ir_value_id inherited_defined;
    uint32_t inherited_may_ub;
    ql_status status;

    if (left_node == SIZE_MAX || right_node == SIZE_MAX ||
        operator_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "binary expression is missing an operand or operator", error);
    }
    status = lower_expression(context, left_node, &left, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = lower_expression(context, right_node, &right, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    operator_text = copy_node_text(context, operator_node);
    if (operator_text == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    if (strcmp(operator_text, "&&") == 0 ||
        strcmp(operator_text, "||") == 0) {
        status = lower_logical_expression(context, operator_text, left, right,
                                          output, error);
        context->allocator->deallocate(context->allocator->user_data,
                                       operator_text);
        return status;
    }
    if (strcmp(operator_text, "<<") == 0 ||
        strcmp(operator_text, ">>") == 0) {
        status = emit_shift_result(context, strcmp(operator_text, "<<") == 0,
                                   left, right, output, error);
        context->allocator->deallocate(context->allocator->user_data,
                                       operator_text);
        return status;
    }
    if (left.type.kind == QL_C_SCALAR_POINTER ||
        right.type.kind == QL_C_SCALAR_POINTER) {
        status = lower_pointer_binary(context, operator_text, node, left,
                                      right, output, error);
        context->allocator->deallocate(context->allocator->user_data,
                                       operator_text);
        return status;
    }
    status = usual_arithmetic_conversions(
        context, left, right, &converted_left, &converted_right, &common,
        error);
    if (status != QL_STATUS_OK) {
        context->allocator->deallocate(context->allocator->user_data,
                                       operator_text);
        return status;
    }
    status = combine_defined(context, &converted_left, &converted_right,
                             &inherited_defined, error);
    if (status != QL_STATUS_OK) {
        context->allocator->deallocate(context->allocator->user_data,
                                       operator_text);
        return status;
    }
    inherited_may_ub = converted_left.may_ub | converted_right.may_ub;

    if (strcmp(operator_text, "+") == 0 ||
        strcmp(operator_text, "-") == 0) {
        status = emit_arithmetic_result(
            context,
            strcmp(operator_text, "+") == 0 ? QL_IR_OPCODE_ADD
                                               : QL_IR_OPCODE_SUB,
            common, converted_left.value, converted_right.value,
            inherited_defined, inherited_may_ub, output, error);
    } else if (strcmp(operator_text, "*") == 0) {
        status = emit_multiply_result(
            context, common, converted_left.value, converted_right.value,
            inherited_defined, inherited_may_ub, output, error);
    } else if (strcmp(operator_text, "/") == 0 ||
               strcmp(operator_text, "%") == 0) {
        status = emit_divrem_result(
            context, strcmp(operator_text, "%") == 0, common,
            converted_left.value, converted_right.value, inherited_defined,
            inherited_may_ub, output, error);
    } else if (strcmp(operator_text, "&") == 0 ||
               strcmp(operator_text, "|") == 0 ||
               strcmp(operator_text, "^") == 0) {
        ql_ir_value_id operands[2];
        ql_ir_opcode opcode =
            strcmp(operator_text, "&") == 0
                ? QL_IR_OPCODE_BV_AND
                : (strcmp(operator_text, "|") == 0 ? QL_IR_OPCODE_BV_OR
                                                     : QL_IR_OPCODE_BV_XOR);
        operands[0] = converted_left.value;
        operands[1] = converted_right.value;
        status = emit_instruction(context, opcode, &common, operands, 2u,
                                  NULL, 0u, QL_IR_EFFECT_NONE,
                                  &output->value, error);
        output->type = common;
        output->defined = inherited_defined;
        output->may_ub = inherited_may_ub;
    } else if (strcmp(operator_text, "==") == 0 ||
               strcmp(operator_text, "!=") == 0 ||
               strcmp(operator_text, "<") == 0 ||
               strcmp(operator_text, "<=") == 0 ||
               strcmp(operator_text, ">") == 0 ||
               strcmp(operator_text, ">=") == 0) {
        lower_value boolean;
        lower_type int_type = make_integer_type(32u, 3u, 1u);
        ql_ir_opcode opcode;
        ql_ir_value_id compare_left = converted_left.value;
        ql_ir_value_id compare_right = converted_right.value;

        if (strcmp(operator_text, "==") == 0) {
            opcode = QL_IR_OPCODE_EQ;
        } else if (strcmp(operator_text, "!=") == 0) {
            opcode = QL_IR_OPCODE_NE;
        } else if (strcmp(operator_text, "<") == 0) {
            opcode = common.is_signed != 0u ? QL_IR_OPCODE_SLT
                                             : QL_IR_OPCODE_ULT;
        } else if (strcmp(operator_text, "<=") == 0) {
            opcode = common.is_signed != 0u ? QL_IR_OPCODE_SLE
                                             : QL_IR_OPCODE_ULE;
        } else if (strcmp(operator_text, ">") == 0) {
            opcode = common.is_signed != 0u ? QL_IR_OPCODE_SLT
                                             : QL_IR_OPCODE_ULT;
            compare_left = converted_right.value;
            compare_right = converted_left.value;
        } else {
            opcode = common.is_signed != 0u ? QL_IR_OPCODE_SLE
                                             : QL_IR_OPCODE_ULE;
            compare_left = converted_right.value;
            compare_right = converted_left.value;
        }
        status = emit_compare(context, opcode, compare_left, compare_right,
                              &boolean.value, error);
        if (status == QL_STATUS_OK) {
            boolean.type = make_bool_type();
            boolean.defined = inherited_defined;
            boolean.may_ub = inherited_may_ub;
            status = convert_value(context, boolean, int_type, output, error);
        }
    } else {
        status = lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "binary operator is outside the integer lowering slice", error);
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   operator_text);
    return status;
}

/* A cast names its target with a type descriptor: a base type plus an
   optional abstract declarator. An abstract declarator here always means
   indirection, which the scalar slice cannot represent, so it is reported as
   a pointer obstacle rather than a type one. */
static ql_status lower_named_cast(lower_context *context, size_t type_node,
                                  size_t value_node, lower_value *output,
                                  ql_error *error);

static ql_status lower_cast_expression(lower_context *context, size_t node,
                                       lower_value *output,
                                       ql_error *error) {
    size_t descriptor = direct_field_child(context, node, "type");
    size_t value_node = direct_field_child(context, node, "value");
    size_t type_node;
    size_t end;
    size_t child;

    if (descriptor == SIZE_MAX || value_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "cast expression has no type or no operand", error);
    }
    type_node = direct_field_child(context, descriptor, "type");
    if (type_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, descriptor,
            "cast type descriptor names no type", error);
    }
    end = subtree_end(context, descriptor);
    for (child = descriptor + 1u; child < end; ++child) {
        if (context->nodes[child].parent != descriptor ||
            context->nodes[child].view.field_name == NULL ||
            strcmp(context->nodes[child].view.field_name,
                   "declarator") != 0) {
            continue;
        }
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, child,
            "cast to a pointer, array, or function type requires memory "
            "semantics", error);
    }
    if (strcmp(context->nodes[type_node].view.kind,
               "atomic_type_specifier") == 0) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_VOLATILE_OR_ATOMIC,
            type_node, "cast to an atomic type requires observable-event "
            "semantics", error);
    }
    return lower_named_cast(context, type_node, value_node, output, error);
}

/* Converts `value_node` to the scalar type `type_node` spells. */
static ql_status lower_named_cast(lower_context *context, size_t type_node,
                                  size_t value_node, lower_value *output,
                                  ql_error *error) {
    lower_value value;
    lower_type target;
    char *spelling = copy_node_text(context, type_node);
    ql_status status;

    if (spelling == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    status = parse_type_spelling(context, spelling, type_node, 1u, &target,
                                 error);
    context->allocator->deallocate(context->allocator->user_data, spelling);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    if (target.kind == QL_C_SCALAR_VOID) {
        /* `(void)e` discards the value, so there is nothing to hand back as
           an expression result. Statement-level discarding is a separate
           construct and is not folded in here. */
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, type_node,
            "a cast to void produces no value", error);
    }
    status = lower_expression(context, value_node, &value, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    return convert_value(context, value, target, output, error);
}

/* `(T)(e)` is a cast when T names a type and a call when it names a callable,
   and the grammar alone cannot tell which. Tree-sitter resolves the ambiguity
   toward a call, so the lowering has to undo that whenever the callee is a
   parenthesised identifier this unit declared as a typedef and no visible
   object shadows it. Reading these as calls would put casts in the
   unsupported_call column and hide what the corpus actually contains.

   Returns the node spelling the type, or SIZE_MAX when this really is a
   call. */
static size_t disguised_cast_type(lower_context *context, size_t node,
                                  size_t *operand) {
    size_t callee = direct_field_child(context, node, "function");
    size_t arguments = direct_field_child(context, node, "arguments");
    size_t name_node;
    size_t argument_end;
    size_t index;
    size_t only_argument = SIZE_MAX;
    char *name;
    int is_type_name;

    *operand = SIZE_MAX;
    if (callee == SIZE_MAX || arguments == SIZE_MAX ||
        strcmp(context->nodes[callee].view.kind,
               "parenthesized_expression") != 0) {
        return SIZE_MAX;
    }
    name_node = first_named_child(context, callee);
    if (name_node == SIZE_MAX ||
        strcmp(context->nodes[name_node].view.kind, "identifier") != 0) {
        return SIZE_MAX;
    }
    argument_end = subtree_end(context, arguments);
    for (index = arguments + 1u; index < argument_end; ++index) {
        if (context->nodes[index].parent != arguments ||
            (context->nodes[index].view.flags & QL_C_SYNTAX_NODE_NAMED) ==
                0u ||
            strcmp(context->nodes[index].view.kind, "comment") == 0) {
            continue;
        }
        if (only_argument != SIZE_MAX) {
            return SIZE_MAX;
        }
        only_argument = index;
    }
    if (only_argument == SIZE_MAX) {
        return SIZE_MAX;
    }
    name = copy_node_text(context, name_node);
    if (name == NULL) {
        return SIZE_MAX;
    }
    /* An object of the same name shadows the typedef, and then this is a
       call through that object after all. */
    is_type_name = find_variable(context, name, strlen(name)) == NULL &&
                   find_typedef(context, name) != NULL;
    context->allocator->deallocate(context->allocator->user_data, name);
    if (!is_type_name) {
        return SIZE_MAX;
    }
    *operand = only_argument;
    return name_node;
}

/* The address a dereference or a subscript designates. Both a load and a
   store need it, so it is computed once here rather than twice. */
static ql_status lower_designator_address(lower_context *context, size_t node,
                                          lower_value *output,
                                          ql_error *error) {
    const char *kind = context->nodes[node].view.kind;
    ql_status status;

    if (strcmp(kind, "pointer_expression") == 0) {
        size_t operator_node = direct_field_child(context, node, "operator");
        size_t argument_node = direct_field_child(context, node, "argument");
        char *operator_text;
        int is_dereference;
        if (operator_node == SIZE_MAX || argument_node == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "pointer expression is missing an operator or argument",
                error);
        }
        operator_text = copy_node_text(context, operator_node);
        if (operator_text == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        is_dereference = strcmp(operator_text, "*") == 0;
        context->allocator->deallocate(context->allocator->user_data,
                                       operator_text);
        if (!is_dereference) {
            /* Taking an address introduces an object this slice does not
               create yet, so it stays a pointer obstacle rather than being
               guessed at. */
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
                "taking an address requires an object for the operand",
                error);
        }
        status = lower_expression(context, argument_node, output, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        if (output->type.kind != QL_C_SCALAR_POINTER) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
                "dereference applies to a pointer", error);
        }
        return QL_STATUS_OK;
    }
    if (strcmp(kind, "subscript_expression") == 0) {
        size_t base_node = direct_field_child(context, node, "argument");
        size_t index_node = direct_field_child(context, node, "index");
        lower_value base;
        lower_value index;
        if (base_node == SIZE_MAX || index_node == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "subscript is missing its base or index", error);
        }
        status = lower_expression(context, base_node, &base, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        status = lower_expression(context, index_node, &index, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        /* C admits `i[p]` as readily as `p[i]`. */
        if (base.type.kind != QL_C_SCALAR_POINTER) {
            lower_value swap = base;
            base = index;
            index = swap;
        }
        if (base.type.kind != QL_C_SCALAR_POINTER ||
            index.type.kind == QL_C_SCALAR_POINTER) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
                "a subscript needs one pointer operand and one integer",
                error);
        }
        return emit_pointer_offset(context, base, index, 0, output, error);
    }
    return lower_unknown(
        context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
        "expression does not designate an object", error);
}

static ql_status lower_expression(lower_context *context, size_t node,
                                  lower_value *output, ql_error *error) {
    const char *kind;

    if (node >= context->node_count) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "C expression node is out of range");
        return QL_STATUS_INTERNAL_ERROR;
    }
    memset(output, 0, sizeof(*output));
    kind = context->nodes[node].view.kind;
    if (strcmp(kind, "identifier") == 0) {
        return lower_identifier(context, node, output, error);
    }
    if (strcmp(kind, "number_literal") == 0) {
        return lower_integer_literal(context, node, output, error);
    }
    if (strcmp(kind, "parenthesized_expression") == 0) {
        size_t child = first_named_child(context, node);
        if (child == SIZE_MAX ||
            strcmp(context->nodes[child].view.kind,
                   "compound_statement") == 0) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "parenthesized expression does not contain one integer expression",
                error);
        }
        return lower_expression(context, child, output, error);
    }
    if (strcmp(kind, "unary_expression") == 0) {
        return lower_unary_expression(context, node, output, error);
    }
    if (strcmp(kind, "binary_expression") == 0) {
        return lower_binary_expression(context, node, output, error);
    }
    if (strcmp(kind, "cast_expression") == 0) {
        return lower_cast_expression(context, node, output, error);
    }
    if (strcmp(kind, "call_expression") == 0) {
        size_t operand = SIZE_MAX;
        size_t type_node = disguised_cast_type(context, node, &operand);
        if (type_node != SIZE_MAX) {
            return lower_named_cast(context, type_node, operand, output,
                                    error);
        }
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL, node,
            "function calls require external-call or callee-summary semantics",
            error);
    }
    if (strcmp(kind, "pointer_expression") == 0 ||
        strcmp(kind, "subscript_expression") == 0) {
        lower_value address;
        ql_status status = lower_designator_address(context, node, &address,
                                                    error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        return emit_load(context, address, output, error);
    }
    if (
        strcmp(kind, "field_expression") == 0) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
            "pointer and aggregate expressions require memory semantics",
            error);
    }
    return lower_unknown(
        context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
        "expression is outside the loop-free integer lowering slice", error);
}

static ql_status lower_statement(lower_context *context, size_t node,
                                 ql_error *error);

static ql_status lower_store_assignment(lower_context *context, size_t node,
                                        size_t left_node, size_t right_node,
                                        size_t operator_node,
                                        ql_error *error) {
    lower_value address;
    lower_value value;
    char *operator_text = copy_node_text(context, operator_node);
    ql_status status;

    if (operator_text == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    status = strcmp(operator_text, "=") == 0 ? QL_STATUS_OK
                                             : QL_STATUS_INVALID_ARGUMENT;
    context->allocator->deallocate(context->allocator->user_data,
                                   operator_text);
    if (status != QL_STATUS_OK) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "compound assignment is not in the first semantic lowering slice",
            error);
    }
    /* The value is evaluated before the store so that a partial operation in
       it is already guarded when the address is written. */
    status = lower_expression(context, right_node, &value, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = lower_designator_address(context, left_node, &address, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    return emit_store(context, address, value, error);
}

static ql_status lower_assignment(lower_context *context, size_t node,
                                  ql_error *error) {
    size_t left_node = direct_field_child(context, node, "left");
    size_t right_node = direct_field_child(context, node, "right");
    size_t operator_node = direct_field_child(context, node, "operator");
    char *name;
    char *operator_text;
    lower_variable *variable;
    lower_value value;
    lower_value converted;
    ql_status status;

    if (left_node == SIZE_MAX || right_node == SIZE_MAX ||
        operator_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "assignment is missing a target, a value, or an operator", error);
    }
    if (strcmp(context->nodes[left_node].view.kind, "pointer_expression") ==
            0 ||
        strcmp(context->nodes[left_node].view.kind,
               "subscript_expression") == 0) {
        return lower_store_assignment(context, node, left_node, right_node,
                                      operator_node, error);
    }
    if (strcmp(context->nodes[left_node].view.kind, "identifier") != 0) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "only assignment to a scalar local, parameter, or dereference is "
            "supported", error);
    }
    operator_text = copy_node_text(context, operator_node);
    if (operator_text == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    if (strcmp(operator_text, "=") != 0) {
        context->allocator->deallocate(context->allocator->user_data,
                                       operator_text);
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "compound assignment is not in the first semantic lowering slice",
            error);
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   operator_text);
    name = copy_node_text(context, left_node);
    if (name == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    variable = find_variable(context, name, strlen(name));
    context->allocator->deallocate(context->allocator->user_data, name);
    if (variable == NULL) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNDECLARED_IDENTIFIER, left_node,
            "assignment target is not a visible local or parameter", error);
    }
    if (variable->is_const != 0u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, left_node,
            "assignment modifies a const-qualified local", error);
    }
    status = lower_expression(context, right_node, &value, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = convert_value(context, value, variable->type, &converted, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_ub_guard(context, &converted, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    variable->value = converted.value;
    variable->initialized = 1u;
    return QL_STATUS_OK;
}

static ql_status parse_local_type(lower_context *context, size_t declaration,
                                  size_t type_node, lower_type *output,
                                  uint32_t *is_const, ql_error *error) {
    size_t end = subtree_end(context, declaration);
    size_t index;
    char *spelling;

    *is_const = 0u;
    for (index = declaration + 1u; index < end; ++index) {
        char *text;
        const char *kind;
        if (context->nodes[index].parent != declaration) {
            continue;
        }
        kind = context->nodes[index].view.kind;
        if (strcmp(kind, "storage_class_specifier") == 0) {
            text = copy_node_text(context, index);
            if (text == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            if (strcmp(text, "auto") != 0 && strcmp(text, "register") != 0) {
                context->allocator->deallocate(
                    context->allocator->user_data, text);
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, index,
                    "static, extern, and thread-local objects require memory semantics",
                    error);
            }
            context->allocator->deallocate(context->allocator->user_data,
                                           text);
        } else if (strcmp(kind, "type_qualifier") == 0) {
            text = copy_node_text(context, index);
            if (text == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            if (strcmp(text, "volatile") == 0 ||
                strcmp(text, "_Atomic") == 0) {
                context->allocator->deallocate(
                    context->allocator->user_data, text);
                return lower_unknown(
                    context,
                    QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_VOLATILE_OR_ATOMIC,
                    index,
                    "volatile and atomic locals require observable-event semantics",
                    error);
            }
            if (strcmp(text, "const") == 0) {
                *is_const = 1u;
            } else if (strcmp(text, "volatile") != 0 &&
                       strcmp(text, "_Atomic") != 0) {
                context->allocator->deallocate(
                    context->allocator->user_data, text);
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, index,
                    "type qualifier is outside the scalar integer slice",
                    error);
            }
            context->allocator->deallocate(context->allocator->user_data,
                                           text);
        }
    }
    if (strcmp(context->nodes[type_node].view.kind,
               "atomic_type_specifier") == 0) {
        return lower_unknown(
            context,
            QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_VOLATILE_OR_ATOMIC,
            type_node,
            "atomic locals require observable-event semantics", error);
    }
    spelling = copy_node_text(context, type_node);
    if (spelling == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    if (strcmp(context->nodes[type_node].view.kind, "primitive_type") != 0 &&
        strcmp(context->nodes[type_node].view.kind,
               "sized_type_specifier") != 0 &&
        strcmp(context->nodes[type_node].view.kind,
               "type_identifier") != 0) {
        context->allocator->deallocate(context->allocator->user_data,
                                       spelling);
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, type_node,
            "typedef, enum, aggregate, and floating local types are outside this slice",
            error);
    }
    {
        ql_status status = parse_type_spelling(context, spelling, type_node,
                                               0u, output, error);
        context->allocator->deallocate(context->allocator->user_data,
                                       spelling);
        return status;
    }
}

static ql_status lower_declaration(lower_context *context, size_t node,
                                   ql_error *error) {
    size_t type_node = direct_field_child(context, node, "type");
    size_t end = subtree_end(context, node);
    size_t index;
    lower_type type;
    uint32_t is_const;
    size_t declarator_count = 0u;
    ql_status status;

    if (type_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_INVALID_DECLARATION, node,
            "local declaration has no type", error);
    }
    status = parse_local_type(context, node, type_node, &type, &is_const,
                              error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = ensure_ir_type(context, &type, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = node + 1u; index < end; ++index) {
        size_t declarator;
        size_t value_node = SIZE_MAX;
        size_t identifier;
        char *name;
        lower_variable *variable;

        if (context->nodes[index].parent != node ||
            context->nodes[index].view.field_name == NULL ||
            strcmp(context->nodes[index].view.field_name, "declarator") != 0) {
            continue;
        }
        ++declarator_count;
        declarator = index;
        if (strcmp(context->nodes[declarator].view.kind,
                   "init_declarator") == 0) {
            value_node = direct_field_child(context, declarator, "value");
            declarator = direct_field_child(context, declarator,
                                            "declarator");
        }
        if (declarator == SIZE_MAX ||
            strcmp(context->nodes[declarator].view.kind, "identifier") != 0) {
            const char *kind = declarator != SIZE_MAX
                                   ? context->nodes[declarator].view.kind
                                   : "declarator";
            if (strstr(kind, "pointer") != NULL ||
                strstr(kind, "array") != NULL) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER,
                    declarator != SIZE_MAX ? declarator : index,
                    "pointer and array locals require memory semantics",
                    error);
            }
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_INVALID_DECLARATION, index,
                "local declarator must be one scalar identifier", error);
        }
        identifier = declarator;
        name = copy_node_text(context, identifier);
        if (name == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        status = add_variable(context, name, strlen(name), type,
                              QL_IR_INVALID_VALUE_ID, 0u, is_const,
                              identifier, error);
        context->allocator->deallocate(context->allocator->user_data, name);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        variable = &context->variables[context->variable_count - 1u];
        if (value_node != SIZE_MAX) {
            lower_value value;
            lower_value converted;
            status = lower_expression(context, value_node, &value, error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
            status = convert_value(context, value, type, &converted, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            status = emit_ub_guard(context, &converted, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            variable->value = converted.value;
            variable->initialized = 1u;
        } else if (is_const != 0u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_INVALID_DECLARATION,
                identifier,
                "const-qualified local must have an initializer in this slice",
                error);
        }
    }
    if (declarator_count == 0u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_INVALID_DECLARATION, node,
            "declaration does not introduce a scalar local", error);
    }
    return QL_STATUS_OK;
}

static ql_status lower_compound(lower_context *context, size_t node,
                                uint32_t create_scope, ql_error *error) {
    size_t marker = context->variable_count;
    size_t end = subtree_end(context, node);
    size_t index;
    ql_status status = QL_STATUS_OK;

    if (create_scope != 0u) {
        ++context->scope_depth;
    }
    for (index = node + 1u; index < end; ++index) {
        if (context->nodes[index].parent != node ||
            (context->nodes[index].view.flags & QL_C_SYNTAX_NODE_NAMED) == 0u) {
            continue;
        }
        if (strcmp(context->nodes[index].view.kind, "comment") == 0) {
            continue;
        }
        if (context->current_terminated != 0u) {
            break;
        }
        status = lower_statement(context, index, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            break;
        }
    }
    if (create_scope != 0u) {
        pop_variables(context, marker);
        --context->scope_depth;
    }
    return status;
}

static ql_status emit_typed_instruction_phi(lower_context *context,
                                           ql_ir_type_id type,
                                           const ql_ir_value_id *operands,
                                           const ql_ir_block_id *blocks,
                                           ql_ir_value_id *output,
                                           ql_error *error) {
    ql_ir_instruction_definition_v1 definition;
    ql_ir_instruction_id instruction;

    ql_ir_instruction_definition_init(&definition, QL_IR_OPCODE_PHI);
    definition.operands = operands;
    definition.operand_count = 2u;
    definition.block_operands = blocks;
    definition.block_operand_count = 2u;
    definition.result_types = &type;
    definition.result_count = 1u;
    return ql_ir_builder_append_instruction(
        context->builder, context->current_block, &definition, &instruction,
        output, error);
}

static ql_status merge_branch_states(
    lower_context *context, const lower_state *left, ql_ir_block_id left_block,
    const lower_state *right, ql_ir_block_id right_block, ql_error *error) {
    size_t index;
    for (index = 0u; index < left->count; ++index) {
        lower_variable *variable = &context->variables[index];
        if (left->initialized[index] == 0u ||
            right->initialized[index] == 0u) {
            variable->initialized = 0u;
            variable->value = QL_IR_INVALID_VALUE_ID;
            continue;
        }
        variable->initialized = 1u;
        if (left->values[index] == right->values[index]) {
            variable->value = left->values[index];
        } else {
            ql_ir_value_id operands[2];
            ql_ir_block_id blocks[2];
            ql_status status;
            operands[0] = left->values[index];
            operands[1] = right->values[index];
            blocks[0] = left_block;
            blocks[1] = right_block;
            status = emit_instruction(context, QL_IR_OPCODE_PHI,
                                      &variable->type, operands, 2u, blocks,
                                      2u, QL_IR_EFFECT_NONE,
                                      &variable->value, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
    }
    /* Memory is as much a merged value as any variable: a store on one branch
       and not the other leaves the join with two versions to reconcile. */
    if (context->uses_memory != 0u && left->memory != right->memory) {
        ql_ir_value_id operands[2];
        ql_ir_block_id blocks[2];
        operands[0] = left->memory;
        operands[1] = right->memory;
        blocks[0] = left_block;
        blocks[1] = right_block;
        return emit_typed_instruction_phi(context, context->memory_type,
                                          operands, blocks,
                                          &context->memory_value, error);
    }
    context->memory_value = left->memory;
    return QL_STATUS_OK;
}

static ql_status lower_if_statement(lower_context *context, size_t node,
                                    ql_error *error) {
    size_t condition_node = direct_field_child(context, node, "condition");
    size_t consequence_node = direct_field_child(context, node,
                                                 "consequence");
    size_t alternative_node = direct_field_child(context, node,
                                                 "alternative");
    size_t variable_count = context->variable_count;
    lower_value condition;
    lower_value boolean;
    lower_state entry_state;
    lower_state true_state;
    lower_state false_state;
    ql_ir_block_id condition_block = context->current_block;
    ql_ir_block_id true_block;
    ql_ir_block_id false_block;
    ql_ir_block_id true_end = QL_IR_INVALID_BLOCK_ID;
    ql_ir_block_id false_end = QL_IR_INVALID_BLOCK_ID;
    uint32_t true_live;
    uint32_t false_live;
    ql_status status;

    memset(&entry_state, 0, sizeof(entry_state));
    memset(&true_state, 0, sizeof(true_state));
    memset(&false_state, 0, sizeof(false_state));
    if (condition_node == SIZE_MAX || consequence_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CONTROL_FLOW, node,
            "if statement is missing a condition or consequence", error);
    }
    status = lower_expression(context, condition_node, &condition, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = convert_value(context, condition, make_bool_type(), &boolean,
                           error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_ub_guard(context, &boolean, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = save_state(context, variable_count, &entry_state, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = add_block(context, "if.then", &true_block, error);
    if (status == QL_STATUS_OK) {
        status = add_block(context, "if.else", &false_block, error);
    }
    if (status == QL_STATUS_OK) {
        status = set_cond_branch(context, condition_block, boolean.value,
                                 true_block, false_block, error);
    }
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    restore_state(context, &entry_state);
    context->current_block = true_block;
    context->current_terminated = 0u;
    status = lower_statement(context, consequence_node, error);
    pop_variables(context, variable_count);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        goto cleanup;
    }
    true_live = context->current_terminated == 0u;
    true_end = context->current_block;
    status = save_state(context, variable_count, &true_state, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    restore_state(context, &entry_state);
    context->current_block = false_block;
    context->current_terminated = 0u;
    if (alternative_node != SIZE_MAX) {
        size_t alternative_statement = first_named_child(context,
                                                         alternative_node);
        if (alternative_statement == SIZE_MAX) {
            status = lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CONTROL_FLOW,
                alternative_node, "else clause has no statement", error);
            goto cleanup;
        }
        status = lower_statement(context, alternative_statement, error);
        pop_variables(context, variable_count);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            goto cleanup;
        }
    }
    false_live = context->current_terminated == 0u;
    false_end = context->current_block;
    status = save_state(context, variable_count, &false_state, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    if (true_live == 0u && false_live == 0u) {
        context->current_block = QL_IR_INVALID_BLOCK_ID;
        context->current_terminated = 1u;
        status = QL_STATUS_OK;
    } else {
        ql_ir_block_id join;
        status = add_block(context, "if.join", &join, error);
        if (status != QL_STATUS_OK) {
            goto cleanup;
        }
        if (true_live != 0u) {
            status = set_branch(context, true_end, join, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
        }
        if (false_live != 0u) {
            status = set_branch(context, false_end, join, error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
        }
        context->current_block = join;
        context->current_terminated = 0u;
        if (true_live != 0u && false_live != 0u) {
            status = merge_branch_states(context, &true_state, true_end,
                                         &false_state, false_end, error);
        } else if (true_live != 0u) {
            restore_state(context, &true_state);
        } else {
            restore_state(context, &false_state);
        }
    }

cleanup:
    destroy_state(context, &entry_state);
    destroy_state(context, &true_state);
    destroy_state(context, &false_state);
    return status;
}

/* Falling off the end of a void function returns, so the same terminator
   serves the explicit `return;` and the implicit one. */
static ql_status terminate_void_return(lower_context *context,
                                       ql_error *error) {
    ql_ir_terminator_definition_v1 terminator;
    ql_status status;

    ql_ir_terminator_definition_init(&terminator, QL_IR_TERMINATOR_RETURN);
    terminator.memory = context->uses_memory != 0u ? context->memory_value
                                                   : QL_IR_INVALID_VALUE_ID;
    status = ql_ir_builder_set_terminator(context->builder,
                                          context->current_block,
                                          &terminator, error);
    if (status == QL_STATUS_OK) {
        context->current_terminated = 1u;
    }
    return status;
}

static ql_status lower_return_statement(lower_context *context, size_t node,
                                        ql_error *error) {
    size_t value_node = first_named_child(context, node);
    lower_value value;
    lower_value converted;
    ql_ir_terminator_definition_v1 terminator;
    ql_status status;

    if (context->return_type.kind == QL_C_SCALAR_VOID) {
        if (value_node != SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
                "a void function cannot return a value", error);
        }
        return terminate_void_return(context, error);
    }
    if (value_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
            "integer and _Bool functions must return a value", error);
    }
    status = lower_expression(context, value_node, &value, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = convert_value(context, value, context->return_type, &converted,
                           error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_ub_guard(context, &converted, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    ql_ir_terminator_definition_init(&terminator, QL_IR_TERMINATOR_RETURN);
    terminator.return_value = converted.value;
    /* Memory is observable, so a function that writes has to say what it
       left behind. */
    terminator.memory = context->uses_memory != 0u ? context->memory_value
                                                   : QL_IR_INVALID_VALUE_ID;
    status = ql_ir_builder_set_terminator(context->builder,
                                          context->current_block,
                                          &terminator, error);
    if (status == QL_STATUS_OK) {
        context->current_terminated = 1u;
    }
    return status;
}

static ql_status lower_statement(lower_context *context, size_t node,
                                 ql_error *error) {
    const char *kind = context->nodes[node].view.kind;

    if (strcmp(kind, "compound_statement") == 0) {
        return lower_compound(context, node, 1u, error);
    }
    if (strcmp(kind, "declaration") == 0) {
        return lower_declaration(context, node, error);
    }
    if (strcmp(kind, "if_statement") == 0) {
        return lower_if_statement(context, node, error);
    }
    if (strcmp(kind, "return_statement") == 0) {
        return lower_return_statement(context, node, error);
    }
    if (strcmp(kind, "expression_statement") == 0) {
        size_t expression = first_named_child(context, node);
        lower_value value;
        ql_status status;
        if (expression == SIZE_MAX) {
            return QL_STATUS_OK;
        }
        if (strcmp(context->nodes[expression].view.kind,
                   "assignment_expression") == 0) {
            return lower_assignment(context, expression, error);
        }
        status = lower_expression(context, expression, &value, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        return emit_ub_guard(context, &value, error);
    }
    if (strcmp(kind, "for_statement") == 0 ||
        strcmp(kind, "while_statement") == 0 ||
        strcmp(kind, "do_statement") == 0) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_LOOP, node,
            "loops require invariants, unrolling, or a recurrence proof method",
            error);
    }
    if (strcmp(kind, "switch_statement") == 0 ||
        strcmp(kind, "goto_statement") == 0 ||
        strcmp(kind, "labeled_statement") == 0 ||
        strcmp(kind, "break_statement") == 0 ||
        strcmp(kind, "continue_statement") == 0) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CONTROL_FLOW, node,
            "control-flow construct is outside the acyclic if/return slice",
            error);
    }
    return lower_unknown(
        context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CONTROL_FLOW, node,
        "statement is outside the acyclic integer lowering slice", error);
}

static ql_status validate_selected_function(
    const ql_c_frontend_unit *unit, const ql_c_function_view *selected,
    ql_c_function_view *canonical, ql_error *error) {
    ql_status status;

    if (unit == NULL || selected == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C frontend unit and selected function are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (selected->struct_size != 0u &&
        selected->struct_size < sizeof(*selected)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "selected C function view is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    memset(canonical, 0, sizeof(*canonical));
    canonical->struct_size = sizeof(*canonical);
    status = ql_c_frontend_function_at(unit, selected->index, canonical,
                                       error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (selected->name == NULL ||
        strcmp(selected->name, canonical->name) != 0 ||
        !range_equal(selected->range, canonical->range) ||
        !range_equal(selected->body_range, canonical->body_range)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "selected C function view does not belong to the unit");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

static size_t locate_function_node(const lower_context *context) {
    size_t index;
    for (index = 0u; index < context->node_count; ++index) {
        if (strcmp(context->nodes[index].view.kind,
                   "function_definition") == 0 &&
            range_equal(context->nodes[index].view.range,
                        context->function.range)) {
            return index;
        }
    }
    return SIZE_MAX;
}

static ql_status check_function_specifiers(lower_context *context,
                                           size_t function_node,
                                           ql_error *error) {
    size_t end = subtree_end(context, function_node);
    size_t index;

    for (index = function_node + 1u; index < end; ++index) {
        char *text;
        if (context->nodes[index].parent != function_node ||
            strcmp(context->nodes[index].view.kind, "type_qualifier") != 0) {
            continue;
        }
        text = copy_node_text(context, index);
        if (text == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        if (strcmp(text, "const") != 0 && strcmp(text, "volatile") != 0 &&
            strcmp(text, "_Atomic") != 0) {
            context->allocator->deallocate(context->allocator->user_data,
                                           text);
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CONTROL_FLOW,
                index,
                "function specifier changes termination semantics outside this slice",
                error);
        }
        context->allocator->deallocate(context->allocator->user_data, text);
    }
    return QL_STATUS_OK;
}

/* The object table is derived from the pointer parameters in source order,
   so it needs nothing exchanged between the two sides of a problem: the same
   signature yields the same table. The IR parameter list is therefore
      [ the C parameters, in source order ]
      [ __memory, once, when the function has any pointer parameter ]
      [ <name>.__base and <name>.__size, per pointer parameter, in source
        order ]
   which keeps the first N parameters lined up with the C arguments and puts
   everything the memory model needs behind them. */
static ql_status add_object_parameters(lower_context *context,
                                       ql_error *error) {
    lower_type u64 = address_type();
    size_t index;
    size_t next = 0u;
    ql_status status;

    if (context->uses_memory == 0u) {
        return QL_STATUS_OK;
    }
    status = ensure_memory_type(context, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_ir_builder_add_parameter(context->builder,
                                         context->memory_type, "__memory",
                                         8u, &context->memory_value, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ensure_ir_type(context, &u64, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < context->variable_count; ++index) {
        const lower_variable *variable = &context->variables[index];
        char name[128];
        lower_object *object;
        if (variable->type.kind != QL_C_SCALAR_POINTER) {
            continue;
        }
        object = &context->objects[next++];
        if (snprintf(name, sizeof(name), "%s.__base", variable->name) < 0) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "object parameter name does not fit");
            return QL_STATUS_INTERNAL_ERROR;
        }
        status = ql_ir_builder_add_parameter(context->builder, u64.ir_type,
                                             name, strlen(name),
                                             &object->base, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (snprintf(name, sizeof(name), "%s.__size", variable->name) < 0) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "object parameter name does not fit");
            return QL_STATUS_INTERNAL_ERROR;
        }
        status = ql_ir_builder_add_parameter(context->builder, u64.ir_type,
                                             name, strlen(name),
                                             &object->size, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

static ql_status emit_assume(lower_context *context, ql_ir_value_id predicate,
                             ql_error *error) {
    return emit_instruction(context, QL_IR_OPCODE_ASSUME, NULL, &predicate,
                            1u, NULL, 0u, QL_IR_EFFECT_NONE, NULL, error);
}

/* The model's three standing constraints, stated in the IR so that the SMT
   encoding inherits them instead of restating them: objects are non-empty and
   above the first page, they do not wrap, and distinct objects are disjoint.
   The interpreter checks the same three on its object table, so a run and a
   query cannot disagree about which layouts are admissible. */
static ql_status emit_object_assumptions(lower_context *context,
                                         ql_error *error) {
    lower_type u64 = address_type();
    ql_ir_value_id first_address;
    ql_ir_value_id zero;
    size_t index;
    size_t other;
    ql_status status;

    if (context->object_count == 0u) {
        return QL_STATUS_OK;
    }
    status = add_uint_constant(context, u64,
                               QL_IR_INTERP_FIRST_OBJECT_ADDRESS,
                               &first_address, error);
    if (status == QL_STATUS_OK) {
        status = add_uint_constant(context, u64, 0u, &zero, error);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < context->object_count; ++index) {
        const lower_object *object = &context->objects[index];
        ql_ir_value_id operands[2];
        ql_ir_value_id limit;
        ql_ir_value_id predicate;

        status = emit_compare(context, QL_IR_OPCODE_ULE, first_address,
                              object->base, &predicate, error);
        if (status == QL_STATUS_OK) {
            status = emit_assume(context, predicate, error);
        }
        if (status == QL_STATUS_OK) {
            status = emit_compare(context, QL_IR_OPCODE_NE, object->size,
                                  zero, &predicate, error);
        }
        if (status == QL_STATUS_OK) {
            status = emit_assume(context, predicate, error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        operands[0] = object->base;
        operands[1] = object->size;
        status = emit_instruction(context, QL_IR_OPCODE_ADD, &u64, operands,
                                  2u, NULL, 0u, QL_IR_EFFECT_NONE, &limit,
                                  error);
        if (status == QL_STATUS_OK) {
            status = emit_compare(context, QL_IR_OPCODE_ULE, object->base,
                                  limit, &predicate, error);
        }
        if (status == QL_STATUS_OK) {
            status = emit_assume(context, predicate, error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        for (other = 0u; other < index; ++other) {
            const lower_object *earlier = &context->objects[other];
            ql_ir_value_id earlier_limit;
            ql_ir_value_id before;
            ql_ir_value_id after;
            operands[0] = earlier->base;
            operands[1] = earlier->size;
            status = emit_instruction(context, QL_IR_OPCODE_ADD, &u64,
                                      operands, 2u, NULL, 0u,
                                      QL_IR_EFFECT_NONE, &earlier_limit,
                                      error);
            if (status == QL_STATUS_OK) {
                status = emit_compare(context, QL_IR_OPCODE_ULE,
                                      earlier_limit, object->base, &before,
                                      error);
            }
            if (status == QL_STATUS_OK) {
                status = emit_compare(context, QL_IR_OPCODE_ULE, limit,
                                      earlier->base, &after, error);
            }
            if (status == QL_STATUS_OK) {
                status = emit_bool_or(context, before, after, &predicate,
                                      error);
            }
            if (status == QL_STATUS_OK) {
                status = emit_assume(context, predicate, error);
            }
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
    }
    return QL_STATUS_OK;
}

static ql_status initialize_parameters(lower_context *context,
                                       ql_error *error) {
    size_t index;
    ql_status status;

    for (index = 0u; index < context->function.parameter_count; ++index) {
        ql_c_parameter_view parameter;
        lower_type type;
        ql_ir_value_id value;

        memset(&parameter, 0, sizeof(parameter));
        parameter.struct_size = sizeof(parameter);
        status = ql_c_frontend_parameter_at(
            context->unit, context->function.index, index, &parameter,
            error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = type_from_inventory(context, &parameter.type, SIZE_MAX,
                                     0u, &type, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        status = ensure_ir_type(context, &type, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_ir_builder_add_parameter(
            context->builder, type.ir_type, parameter.name,
            strlen(parameter.name), &value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = add_variable(
            context, parameter.name, strlen(parameter.name), type, value, 1u,
            (parameter.type.qualifiers & QL_C_TYPE_QUALIFIER_CONST) != 0u,
            SIZE_MAX, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
    }
    for (index = 0u; index < context->variable_count; ++index) {
        if (context->variables[index].type.kind == QL_C_SCALAR_POINTER) {
            ++context->object_count;
        }
    }
    if (context->object_count != 0u) {
        context->uses_memory = 1u;
        context->objects = context->allocator->allocate(
            context->allocator->user_data,
            context->object_count * sizeof(*context->objects));
        if (context->objects == NULL) {
            context->object_count = 0u;
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        memset(context->objects, 0,
               context->object_count * sizeof(*context->objects));
    }
    return add_object_parameters(context, error);
}

static void cleanup_context(lower_context *context) {
    pop_variables(context, 0u);
    release_typedefs(context);
    context->allocator->deallocate(context->allocator->user_data,
                                   context->objects);
    context->objects = NULL;
    context->allocator->deallocate(context->allocator->user_data,
                                   context->variables);
    ql_ir_builder_destroy(context->builder);
    context->allocator->deallocate(context->allocator->user_data,
                                   context->nodes);
    ql_c_syntax_tree_destroy(context->tree);
    ql_c_parser_destroy(context->parser);
}

ql_status QL_CALL ql_c_lower_selected_function(
    const ql_allocator *allocator, const char *source, size_t source_size,
    const ql_c_frontend_unit *unit, const ql_c_function_view *function,
    ql_c_lower_result **output, ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_c_lower_result *result = NULL;
    lower_context context;
    ql_c_function_view canonical;
    size_t function_node;
    size_t body_node;
    size_t index;
    ql_status status;

    if (output == NULL || source == NULL || source_size == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "source and C lowering output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (ql_allocator_is_valid(selected) == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (source_size > UINT32_MAX) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C source exceeds the v1 source-range limit");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_selected_function(unit, function, &canonical, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (canonical.range.end_byte > source_size ||
        canonical.body_range.end_byte > source_size) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "selected function range is outside the supplied source");
        return QL_STATUS_INVALID_ARGUMENT;
    }

    result = selected->allocate(selected->user_data, sizeof(*result));
    if (result == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(result, 0, sizeof(*result));
    result->allocator = *selected;
    result->support = QL_C_LOWER_SUPPORTED;

    memset(&context, 0, sizeof(context));
    context.allocator = selected;
    context.source = source;
    context.source_size = source_size;
    context.unit = unit;
    context.function = canonical;
    context.result = result;
    context.bool_type = QL_IR_INVALID_TYPE_ID;
    context.void_type = QL_IR_INVALID_TYPE_ID;
    context.memory_type = QL_IR_INVALID_TYPE_ID;
    context.bool_pointer_type = QL_IR_INVALID_TYPE_ID;
    context.memory_value = QL_IR_INVALID_VALUE_ID;
    context.true_value = QL_IR_INVALID_VALUE_ID;
    context.false_value = QL_IR_INVALID_VALUE_ID;
    context.current_block = QL_IR_INVALID_BLOCK_ID;
    for (index = 0u; index < sizeof(context.bv_types) /
                                     sizeof(context.bv_types[0]);
         ++index) {
        context.bv_types[index] = QL_IR_INVALID_TYPE_ID;
        context.pointer_types[index] = QL_IR_INVALID_TYPE_ID;
    }

    if (canonical.support != QL_C_FUNCTION_SUPPORTED) {
        status = lower_unknown(
            &context, QL_C_LOWER_DIAGNOSTIC_FRONTEND_UNSUPPORTED, SIZE_MAX,
            "selected function failed the restricted-C syntactic gate",
            error);
        if (status == QL_STATUS_OK) {
            *output = result;
            ql_error_clear(error);
            return QL_STATUS_OK;
        }
        ql_c_lower_result_destroy(result);
        return status;
    }

    status = ql_c_parser_create(selected, &context.parser, error);
    if (status == QL_STATUS_OK) {
        status = ql_c_parser_parse(context.parser, source, source_size,
                                   &context.tree, error);
    }
    if (status == QL_STATUS_OK &&
        ql_c_syntax_tree_has_errors(context.tree) != 0u) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "C source has a recovered syntax error");
        status = QL_STATUS_PARSE_ERROR;
    }
    if (status == QL_STATUS_OK) {
        status = collect_nodes(&context, error);
    }
    if (status == QL_STATUS_OK) {
        status = collect_typedefs(&context, error);
    }
    if (status != QL_STATUS_OK) {
        cleanup_context(&context);
        ql_c_lower_result_destroy(result);
        return status;
    }
    function_node = locate_function_node(&context);
    if (function_node == SIZE_MAX) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "supplied source does not contain the selected function range");
        cleanup_context(&context);
        ql_c_lower_result_destroy(result);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    body_node = direct_field_child(&context, function_node, "body");
    if (body_node == SIZE_MAX ||
        !range_equal(context.nodes[body_node].view.range,
                     canonical.body_range)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "supplied source does not match the selected function body");
        cleanup_context(&context);
        ql_c_lower_result_destroy(result);
        return QL_STATUS_INVALID_ARGUMENT;
    }

    status = check_function_specifiers(&context, function_node, error);
    if (status == QL_STATUS_OK && context.unknown == 0u) {
        status = ql_ir_builder_create(selected, &context.builder, error);
    }
    if (status == QL_STATUS_OK && context.unknown == 0u) {
        status = type_from_inventory(&context, &canonical.return_type,
                                     function_node, 1u, &context.return_type,
                                     error);
    }
    if (status == QL_STATUS_OK && context.unknown == 0u) {
        status = ensure_ir_type(&context, &context.return_type, error);
    }
    if (status == QL_STATUS_OK && context.unknown == 0u) {
        status = ql_ir_builder_set_function(
            context.builder, canonical.name, strlen(canonical.name),
            context.return_type.ir_type, error);
    }
    if (status == QL_STATUS_OK && context.unknown == 0u) {
        status = initialize_parameters(&context, error);
    }
    if (status == QL_STATUS_OK && context.unknown == 0u) {
        ql_ir_block_id entry;
        status = add_block(&context, "entry", &entry, error);
        if (status == QL_STATUS_OK) {
            status = ql_ir_builder_set_entry_block(context.builder, entry,
                                                   error);
        }
        if (status == QL_STATUS_OK) {
            context.current_block = entry;
            context.current_terminated = 0u;
            status = emit_object_assumptions(&context, error);
        }
        if (status == QL_STATUS_OK) {
            status = lower_compound(&context, body_node, 0u, error);
        }
    }
    if (status == QL_STATUS_OK && context.unknown == 0u &&
        context.current_terminated == 0u) {
        if (context.return_type.kind == QL_C_SCALAR_VOID) {
            status = terminate_void_return(&context, error);
        } else {
            status = lower_unknown(
                &context, QL_C_LOWER_DIAGNOSTIC_MISSING_RETURN, body_node,
                "a reachable path leaves an integer function without "
                "returning", error);
        }
    }
    if (status == QL_STATUS_OK && context.unknown == 0u) {
        status = ql_ir_builder_finish(context.builder, &result->ir_artifact,
                                      error);
    }
    if (status == QL_STATUS_OK) {
        result->support = context.unknown != 0u ? QL_C_LOWER_UNKNOWN
                                               : QL_C_LOWER_SUPPORTED;
        cleanup_context(&context);
        *output = result;
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    cleanup_context(&context);
    ql_c_lower_result_destroy(result);
    return status;
}
