#include "quodlibet/c_lower.h"

#include "c_types.h"

#include "quodlibet/ir_interp.h"

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Compiles to nothing unless QL_STAGE_TIMING is defined. The one hook in this
   file brackets the parse this path does when it was not handed a tree, so a
   stage breakdown can tell that parse apart from the lowering; W8 added it and
   owns it. */
#include "stage_timer.h"

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
    /* How many stars stand between this type and `pointee`: zero when this
       is not a pointer at all, one for `T *`, two for `T **`. */
    uint32_t indirection;
    /* The type at the bottom of the stars. */
    ql_c_scalar_type pointee;
    /* The record this type is, or bottoms out at. SIZE_MAX when neither. */
    size_t record;
    /* Element count when this is an array, zero when it is not. An array is
       storage rather than a value: it has no IR type of its own, it is never
       loaded or stored whole, and every use of its name decays to a pointer
       to its first element. */
    uint64_t array_length;
    ql_ir_type_id ir_type;
} lower_type;

/* An IR type the lowering has already built. The key is the IR shape, not
   the C type: bit-vectors are signless, so `char` and `unsigned char` are one
   IR type and must share one identifier. A load requires its pointer's
   element type to be the very same identifier as its result type, so a second
   identifier for the same shape would break it. */
typedef struct lower_type_binding {
    ql_ir_type_kind kind;
    uint32_t bit_width;
    ql_ir_type_id element_type;
    ql_ir_type_id id;
} lower_type_binding;

/* One member of a struct or union, with the byte offset the target ABI gives
   it. */
typedef struct lower_member {
    char *name;
    struct lower_type type;
    uint64_t offset;
} lower_member;

/* A struct or union declared in this unit. Layout is computed on demand,
   because a member can name a record declared later in the file and because
   a record that contains itself has no layout at all. */
typedef struct lower_record {
    char *tag;
    size_t body_node;
    uint32_t is_union;
    uint32_t layout_state;
    lower_member *members;
    size_t member_count;
    size_t member_capacity;
    uint64_t size;
    uint32_t alignment;
} lower_record;

#define LOWER_LAYOUT_PENDING 0u
#define LOWER_LAYOUT_RUNNING 1u
#define LOWER_LAYOUT_DONE 2u

/* An enumerator, which is an ordinary integer constant that happens to have a
   name. */
typedef struct lower_enumerator {
    char *name;
    uint64_t value;
} lower_enumerator;

typedef struct lower_value {
    ql_ir_value_id value;
    ql_ir_value_id defined;
    lower_type type;
    uint32_t may_ub;
    /* Set when this pointer came from a parameter, directly or by
       arithmetic. The object table is derived from the parameters, so a
       pointer that arrived any other way has no object an access guard could
       name. */
    uint32_t has_object;
} lower_value;

/* One `typedef` name and the type it stands for. `underlying` is the
   spelling of the definition's type node, which may itself be a typedef name,
   so resolution iterates. */
typedef struct lower_typedef {
    char *name;
    char *underlying;
    /* Stars between the typedef and what it names, so `typedef int *T` can
       resolve to a pointer instead of being refused for being one. */
    uint32_t pointer_depth;
    uint32_t is_array_or_function;
    uint32_t is_aggregate;
} lower_typedef;

typedef struct lower_variable {
    char *name;
    size_t name_size;
    lower_type type;
    /* Set for a pointer whose object the table declares. */
    uint32_t has_object;
    /* Set when this variable lives in storage rather than in an SSA value,
       which is what taking its address requires. Its value is then whatever
       memory holds, and `value` is unused. */
    uint32_t is_stack;
    /* The address of that storage, typed as a pointer to the variable. */
    ql_ir_value_id address;
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

/* A function this unit declares but does not define. Its types are resolved
   the first time it is called, so a declaration mentioning something outside
   this slice only costs the bodies that actually call it. */
typedef struct lower_callee {
    char *name;
    size_t declaration_node;
    size_t declarator_node;
    uint32_t return_pointer_depth;
    uint32_t is_variadic;
    uint32_t resolved;
    lower_type return_type;
    lower_type *parameters;
    size_t parameter_count;
    size_t parameter_capacity;
} lower_callee;

/* A string literal. C makes it an array of char with static storage
   duration, so it is an object here like any other array, with its bytes
   written into the entry block because the declaration states them. */
typedef struct lower_string {
    size_t node;
    unsigned char *bytes;
    size_t size;          /* including the terminating NUL */
    size_t object;
    ql_ir_value_id address;
} lower_string;

/* An object with static storage duration. It is an object in the flat
   memory model exactly as a pointer parameter's region is, which is what puts
   a write to it and a call in the same order: both thread the memory state.

   Its initial contents are left unknown unless the declaration states them.
   In this corpus `int GLB_0;` is a placeholder the extractor synthesised for
   a global whose real definition lives elsewhere, not a tentative definition
   of a zeroed object, so assuming zero would be assuming a value the program
   never promised. */
typedef struct lower_global {
    char *name;
    size_t declaration_node;
    size_t declarator_node;
    size_t initializer_node;
    lower_type type;
    size_t object;
    ql_ir_value_id address;
    uint32_t referenced;
} lower_global;

/* Storage for one address-taken name. `is_parameter` marks the ones whose
   incoming value has to be written into the slot on entry. */
typedef struct lower_stack_slot {
    char *name;
    lower_type type;
    size_t object;
    ql_ir_value_id address;
    uint32_t is_parameter;
} lower_stack_slot;

typedef struct lower_state {
    ql_ir_value_id *values;
    uint8_t *initialized;
    size_t count;
    ql_ir_value_id memory;
    ql_ir_value_id trace;
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
    uint32_t owns_tree;
    ql_ir_builder *builder;
    lower_variable *variables;
    size_t variable_count;
    size_t variable_capacity;
    lower_typedef *typedefs;
    size_t typedef_count;
    size_t typedef_capacity;
    lower_record *records;
    size_t record_count;
    size_t record_capacity;
    lower_enumerator *enumerators;
    size_t enumerator_count;
    size_t enumerator_capacity;
    size_t scope_depth;
    lower_type return_type;
    ql_ir_block_id current_block;
    uint32_t current_terminated;
    ql_ir_type_id memory_type;
    lower_type_binding *type_cache;
    size_t type_cache_count;
    size_t type_cache_capacity;
    /* The memory state threaded through the function, and the objects the
       access guards are written against. */
    ql_ir_value_id memory_value;
    lower_object *objects;
    size_t object_count;
    size_t object_capacity;
    /* Objects the caller supplied, which are the ones the entry block states
       assumptions for. Anything past this the function made for itself. */
    size_t parameter_object_count;
    /* Names this function takes the address of, so their locals get storage
       instead of an SSA value. */
    char **address_taken;
    size_t address_taken_count;
    size_t address_taken_capacity;
    /* The storage those names get. The IR builder requires every parameter to
       precede the first constant and instruction, so the base and size
       parameters are created before the entry block and the address value is
       computed inside it. */
    lower_stack_slot *stack_slots;
    size_t stack_slot_count;
    size_t stack_slot_capacity;
    lower_callee *callees;
    size_t callee_count;
    size_t callee_capacity;
    lower_string *strings;
    size_t string_count;
    size_t string_capacity;
    lower_global *globals;
    /* Set while a global's declaration is being read, which is what tells
       parse_local_type that `static` and `extern` are the storage duration
       this object already has rather than one it cannot model. */
    uint32_t parsing_global;
    size_t global_count;
    size_t global_capacity;
    /* The observable order of external calls. Threaded like memory, and
       present only when the body actually calls something. */
    ql_ir_type_id trace_type;
    ql_ir_value_id trace_value;
    uint32_t makes_calls;
    size_t body_node;
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
    type.record = SIZE_MAX;
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
    if (left.kind == QL_C_SCALAR_RECORD) {
        return left.record == right.record;
    }
    if (left.kind != QL_C_SCALAR_POINTER) {
        return 1;
    }
    if (left.indirection != right.indirection ||
        !ql_c_scalar_same(left.pointee, right.pointee)) {
        return 0;
    }
    return left.record == right.record;
}

static lower_type make_pointer_type(ql_c_scalar_type pointee) {
    lower_type type;
    memset(&type, 0, sizeof(type));
    type.kind = QL_C_SCALAR_POINTER;
    type.width = QL_C_POINTER_WIDTH;
    type.rank = 6u;
    type.is_signed = 0u;
    type.indirection = 1u;
    type.pointee = pointee;
    type.record = SIZE_MAX;
    type.ir_type = QL_IR_INVALID_TYPE_ID;
    return type;
}

static lower_type make_record_pointer_type(size_t record) {
    ql_c_scalar_type pointee;
    lower_type type;
    memset(&pointee, 0, sizeof(pointee));
    pointee.kind = QL_C_SCALAR_RECORD;
    type = make_pointer_type(pointee);
    type.record = record;
    return type;
}

static lower_type make_record_type(size_t record);
static lower_type type_from_scalar(ql_c_scalar_type scalar);

/* One star off. */
static lower_type pointer_target(lower_type pointer) {
    if (pointer.indirection > 1u) {
        lower_type target = pointer;
        target.indirection = pointer.indirection - 1u;
        target.ir_type = QL_IR_INVALID_TYPE_ID;
        return target;
    }
    if (pointer.pointee.kind == QL_C_SCALAR_RECORD) {
        return make_record_type(pointer.record);
    }
    return type_from_scalar(pointer.pointee);
}

/* One star on. */
static lower_type make_pointer_to(lower_type target) {
    if (target.kind == QL_C_SCALAR_POINTER) {
        lower_type type = target;
        type.indirection = target.indirection + 1u;
        type.ir_type = QL_IR_INVALID_TYPE_ID;
        return type;
    }
    if (target.kind == QL_C_SCALAR_RECORD) {
        return make_record_pointer_type(target.record);
    }
    return make_pointer_type(scalar_of(target));
}

static lower_type make_record_type(size_t record) {
    lower_type type;
    memset(&type, 0, sizeof(type));
    type.kind = QL_C_SCALAR_RECORD;
    type.record = record;
    type.ir_type = QL_IR_INVALID_TYPE_ID;
    return type;
}

static uint64_t record_size(const lower_context *context, size_t record);
static uint32_t record_alignment(const lower_context *context, size_t record);

/* Storage width of the pointee, which is what a load or store moves and what
   pointer arithmetic steps by. */
static uint64_t pointee_byte_width(const lower_context *context,
                                   lower_type pointer) {
    uint32_t bits;
    if (pointer.indirection > 1u) {
        return QL_C_POINTER_WIDTH / 8u;
    }
    if (pointer.pointee.kind == QL_C_SCALAR_RECORD) {
        return record_size(context, pointer.record);
    }
    bits = pointer.pointee.kind == QL_C_SCALAR_BOOL ? 8u
                                                    : pointer.pointee.width;
    return (bits + 7u) / 8u;
}

/* Storage width of an object of this type. */
static lower_type array_element(lower_type array) {
    lower_type element = array;
    element.array_length = 0u;
    element.ir_type = QL_IR_INVALID_TYPE_ID;
    return element;
}

static lower_type make_array_of(lower_type element, uint64_t length) {
    lower_type array = element;
    array.array_length = length;
    array.ir_type = QL_IR_INVALID_TYPE_ID;
    return array;
}

static uint64_t type_byte_width(const lower_context *context,
                                lower_type type) {
    if (type.array_length != 0u) {
        return type.array_length * type_byte_width(context,
                                                   array_element(type));
    }
    if (type.kind == QL_C_SCALAR_RECORD) {
        return record_size(context, type.record);
    }
    if (type.kind == QL_C_SCALAR_POINTER) {
        return QL_C_POINTER_WIDTH / 8u;
    }
    return ((type.kind == QL_C_SCALAR_BOOL ? 8u : type.width) + 7u) / 8u;
}

static uint32_t type_alignment(const lower_context *context,
                               lower_type type) {
    if (type.array_length != 0u) {
        /* An array is as aligned as one element. Taking its whole size would
           over-align every member that follows it. */
        return type_alignment(context, array_element(type));
    }
    if (type.kind == QL_C_SCALAR_RECORD) {
        return record_alignment(context, type.record);
    }
    return (uint32_t)type_byte_width(context, type);
}

/* Natural alignment on the target ABI, stated the same way the interpreter
   states it: a scalar of N bytes is N-aligned when N is a power of two up to
   sixteen, and nothing else is required. */
static uint32_t natural_alignment(uint64_t byte_width) {
    if (byte_width == 0u || byte_width > 16u ||
        (byte_width & (byte_width - 1u)) != 0u) {
        return 1u;
    }
    return (uint32_t)byte_width;
}

/* `allow_void` is set only where C admits an incomplete type: a function's
   return type. Everywhere else void is a type error, not a narrowing. */
/* A declarator that is not just an identifier introduces indirection, which
   the scalar slice cannot represent. Recording that here lets resolution
   report `unsupported_pointer` instead of `unsupported_type`, so the coverage
   tables name the real obstacle. */
static size_t typedef_declarator_name(const lower_context *context,
                                      size_t declarator,
                                      uint32_t *pointer_depth,
                                      uint32_t *is_array_or_function) {
    size_t guard = 0u;

    *pointer_depth = 0u;
    *is_array_or_function = 0u;
    while (declarator != SIZE_MAX && guard++ < 64u) {
        const char *kind = context->nodes[declarator].view.kind;
        if (strcmp(kind, "type_identifier") == 0 ||
            strcmp(kind, "identifier") == 0) {
            return declarator;
        }
        if (strcmp(kind, "pointer_declarator") == 0) {
            ++(*pointer_depth);
            declarator = direct_field_child(context, declarator, "declarator");
            continue;
        }
        if (strcmp(kind, "array_declarator") == 0 ||
            strcmp(kind, "function_declarator") == 0) {
            *is_array_or_function = 1u;
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

/* A local only needs storage if something asks for its address. Finding that
   out up front keeps every other local in an SSA value, where it is cheaper
   and easier to reason about. */
static const lower_typedef *find_typedef(const lower_context *context,
                                         const char *name);
static ql_status parse_type_spelling(lower_context *context,
                                     const char *spelling, size_t node,
                                     uint32_t allow_void,
                                     lower_type *output, ql_error *error);
static size_t member_declarator_name(const lower_context *context,
                                     size_t declarator,
                                     uint32_t *pointer_depth,
                                     uint64_t *array_length, int *rejected);

static ql_status remember_storage_name(lower_context *context,
                                       const char *text, ql_error *error) {
    size_t existing;
    char *name;
    ql_status status;

    for (existing = 0u; existing < context->address_taken_count; ++existing) {
        if (strcmp(context->address_taken[existing], text) == 0) {
            return QL_STATUS_OK;
        }
    }
    status = grow_array(context->allocator, (void **)&context->address_taken,
                        &context->address_taken_capacity,
                        sizeof(*context->address_taken),
                        context->address_taken_count + 1u, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    name = copy_text(context->allocator, text, strlen(text));
    if (name == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    context->address_taken[context->address_taken_count++] = name;
    return QL_STATUS_OK;
}

/* An array or a record local is storage whether or not its address is ever
   written down. `a[i]` and `v.f` are addresses into it just as `&a` is, and
   neither has a value that could live in an SSA register instead: an array
   decays to a pointer wherever it is named, and a record is only ever reached
   through its members. So both get an object up front, on the same footing as
   an address-taken scalar. */
static ql_status collect_storage_locals(lower_context *context,
                                        size_t body_node, ql_error *error) {
    size_t end = subtree_end(context, body_node);
    size_t index;

    for (index = body_node + 1u; index < end; ++index) {
        size_t type_node;
        size_t declaration_end;
        size_t child;
        int base_is_record;

        if (strcmp(context->nodes[index].view.kind, "declaration") != 0) {
            continue;
        }
        type_node = direct_field_child(context, index, "type");
        if (type_node == SIZE_MAX) {
            continue;
        }
        base_is_record =
            strcmp(context->nodes[type_node].view.kind,
                   "struct_specifier") == 0 ||
            strcmp(context->nodes[type_node].view.kind,
                   "union_specifier") == 0;
        if (!base_is_record &&
            strcmp(context->nodes[type_node].view.kind,
                   "type_identifier") == 0) {
            char *spelling = copy_node_text(context, type_node);
            lower_type resolved;
            ql_status status;
            if (spelling == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            status = parse_type_spelling(context, spelling, type_node, 1u,
                                         &resolved, error);
            context->allocator->deallocate(context->allocator->user_data,
                                           spelling);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
            base_is_record = resolved.kind == QL_C_SCALAR_RECORD;
        }
        declaration_end = subtree_end(context, index);
        for (child = index + 1u; child < declaration_end; ++child) {
            size_t declarator = child;
            uint32_t pointer_depth;
            uint64_t array_length;
            int rejected;
            size_t named;
            char *text;
            ql_status status;

            if (context->nodes[child].parent != index ||
                context->nodes[child].view.field_name == NULL ||
                strcmp(context->nodes[child].view.field_name,
                       "declarator") != 0) {
                continue;
            }
            if (strcmp(context->nodes[declarator].view.kind,
                       "init_declarator") == 0) {
                declarator = direct_field_child(context, declarator,
                                                "declarator");
            }
            if (declarator == SIZE_MAX) {
                continue;
            }
            named = member_declarator_name(context, declarator,
                                           &pointer_depth, &array_length,
                                           &rejected);
            if (named == SIZE_MAX || rejected != 0) {
                continue;
            }
            if (array_length == 0u &&
                (pointer_depth != 0u || !base_is_record)) {
                continue;
            }
            text = copy_node_text(context, named);
            if (text == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            status = remember_storage_name(context, text, error);
            context->allocator->deallocate(context->allocator->user_data,
                                           text);
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
    }
    return QL_STATUS_OK;
}

/* The longest literal this slice writes out. Each byte becomes a store in
   the entry block, so an unbounded literal would be an unbounded prologue. */
#define LOWER_MAX_STRING_BYTES 256u

/* Reads the bytes a literal denotes. Only the escapes C spells with a single
   letter and the octal and hex forms are decoded; anything else is refused
   rather than passed through as its own text, because a literal whose bytes
   are wrong is a wrong object, not a missing one. */
static int decode_string_literal(const char *text, size_t size,
                                 unsigned char *out, size_t *out_size) {
    size_t index = 0u;
    size_t written = 0u;

    while (index < size) {
        char c = text[index];
        /* Adjacent literals concatenate, and a prefix marks encoding this
           slice does not carry. */
        if (c == 'L' || c == 'u' || c == 'U') {
            return 0;
        }
        if (c != '"') {
            ++index;
            continue;
        }
        ++index;
        while (index < size && text[index] != '"') {
            unsigned char value;
            if (written >= LOWER_MAX_STRING_BYTES) {
                return 0;
            }
            if (text[index] != '\\') {
                out[written++] = (unsigned char)text[index++];
                continue;
            }
            if (++index >= size) {
                return 0;
            }
            switch (text[index]) {
            case 'n': value = 10u; break;
            case 't': value = 9u; break;
            case 'r': value = 13u; break;
            case '0': value = 0u; break;
            case 'a': value = 7u; break;
            case 'b': value = 8u; break;
            case 'f': value = 12u; break;
            case 'v': value = 11u; break;
            case '\\': value = 92u; break;
            case '\'': value = 39u; break;
            case '"': value = 34u; break;
            case '?': value = 63u; break;
            default: return 0;
            }
            out[written++] = value;
            ++index;
        }
        if (index >= size) {
            return 0;
        }
        ++index;
    }
    if (written >= LOWER_MAX_STRING_BYTES) {
        return 0;
    }
    out[written] = 0u;
    *out_size = written + 1u;
    return 1;
}

static ql_status collect_string_literals(lower_context *context,
                                         size_t body_node, ql_error *error) {
    size_t end = subtree_end(context, body_node);
    size_t index;

    for (index = body_node + 1u; index < end; ++index) {
        unsigned char decoded[LOWER_MAX_STRING_BYTES + 1u];
        size_t decoded_size = 0u;
        lower_string *entry;
        char *text;
        int ok;
        ql_status status;

        if (strcmp(context->nodes[index].view.kind, "string_literal") != 0) {
            continue;
        }
        text = copy_node_text(context, index);
        if (text == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        ok = decode_string_literal(text, strlen(text), decoded,
                                   &decoded_size);
        context->allocator->deallocate(context->allocator->user_data, text);
        if (!ok) {
            /* Left uncollected. The expression path reports it where it is
               used, which is where the diagnostic belongs. */
            continue;
        }
        status = grow_array(context->allocator, (void **)&context->strings,
                            &context->string_capacity,
                            sizeof(*context->strings),
                            context->string_count + 1u, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        entry = &context->strings[context->string_count];
        memset(entry, 0, sizeof(*entry));
        entry->bytes = context->allocator->allocate(
            context->allocator->user_data, decoded_size);
        if (entry->bytes == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        memcpy(entry->bytes, decoded, decoded_size);
        entry->node = index;
        entry->size = decoded_size;
        entry->object = SIZE_MAX;
        entry->address = QL_IR_INVALID_VALUE_ID;
        ++context->string_count;
        context->uses_memory = 1u;
    }
    return QL_STATUS_OK;
}

static lower_string *find_string(lower_context *context, size_t node) {
    size_t index;
    for (index = 0u; index < context->string_count; ++index) {
        if (context->strings[index].node == node) {
            return &context->strings[index];
        }
    }
    return NULL;
}

static void release_strings(lower_context *context) {
    size_t index;
    for (index = 0u; index < context->string_count; ++index) {
        context->allocator->deallocate(context->allocator->user_data,
                                       context->strings[index].bytes);
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   context->strings);
    context->strings = NULL;
    context->string_count = 0u;
    context->string_capacity = 0u;
}

static ql_status collect_address_taken(lower_context *context,
                                       size_t body_node, ql_error *error) {
    size_t end = subtree_end(context, body_node);
    size_t index;

    for (index = body_node + 1u; index < end; ++index) {
        size_t operator_node;
        size_t argument_node;
        char *operator_text;
        char *name;
        int is_address_of;
        size_t existing;
        ql_status status;

        if (strcmp(context->nodes[index].view.kind,
                   "pointer_expression") != 0) {
            continue;
        }
        operator_node = direct_field_child(context, index, "operator");
        argument_node = direct_field_child(context, index, "argument");
        if (operator_node == SIZE_MAX || argument_node == SIZE_MAX ||
            strcmp(context->nodes[argument_node].view.kind,
                   "identifier") != 0) {
            continue;
        }
        operator_text = copy_node_text(context, operator_node);
        if (operator_text == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        is_address_of = strcmp(operator_text, "&") == 0;
        context->allocator->deallocate(context->allocator->user_data,
                                       operator_text);
        if (!is_address_of) {
            continue;
        }
        name = copy_node_text(context, argument_node);
        if (name == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        for (existing = 0u; existing < context->address_taken_count;
             ++existing) {
            if (strcmp(context->address_taken[existing], name) == 0) {
                break;
            }
        }
        if (existing < context->address_taken_count) {
            context->allocator->deallocate(context->allocator->user_data,
                                           name);
            continue;
        }
        status = grow_array(context->allocator,
                            (void **)&context->address_taken,
                            &context->address_taken_capacity,
                            sizeof(*context->address_taken),
                            context->address_taken_count + 1u, error);
        if (status != QL_STATUS_OK) {
            context->allocator->deallocate(context->allocator->user_data,
                                           name);
            return status;
        }
        context->address_taken[context->address_taken_count++] = name;
    }
    return QL_STATUS_OK;
}


/* A call may write memory and is itself observable, so both the memory and
   the trace parameter have to exist before the body runs. */
static lower_callee *find_callee(lower_context *context,
                                 const char *name);

static size_t member_declarator_name(const lower_context *context,
                                     size_t declarator,
                                     uint32_t *pointer_depth,
                                     uint64_t *array_length, int *rejected);

/* A file-scope declaration that names an object rather than a function. */
static ql_status collect_globals(lower_context *context, ql_error *error) {
    size_t index;

    for (index = 0u; index < context->node_count; ++index) {
        size_t end;
        size_t child;

        if (strcmp(context->nodes[index].view.kind, "declaration") != 0 ||
            context->nodes[index].depth != 1u) {
            continue;
        }
        end = subtree_end(context, index);
        for (child = index + 1u; child < end; ++child) {
            size_t declarator = child;
            size_t initializer = SIZE_MAX;
            size_t named;
            uint32_t pointer_depth;
            uint64_t array_length;
            int rejected;
            lower_global *entry;
            ql_status status;

            if (context->nodes[child].parent != index ||
                context->nodes[child].view.field_name == NULL ||
                strcmp(context->nodes[child].view.field_name,
                       "declarator") != 0) {
                continue;
            }
            if (strcmp(context->nodes[declarator].view.kind,
                       "init_declarator") == 0) {
                initializer = direct_field_child(context, declarator,
                                                 "value");
                declarator = direct_field_child(context, declarator,
                                                "declarator");
            }
            if (declarator == SIZE_MAX ||
                strcmp(context->nodes[declarator].view.kind,
                       "function_declarator") == 0) {
                continue;
            }
            named = member_declarator_name(context, declarator,
                                           &pointer_depth, &array_length,
                                           &rejected);
            if (named == SIZE_MAX || rejected != 0) {
                continue;
            }
            {
                /* `extern int x;` followed by `int x = 3;` names one object,
                   not two. Two entries would become two objects the model
                   assumes are disjoint, which would make a write through one
                   invisible to a read through the other. The declaration that
                   states a value wins, since that is the definition. */
                char *text = copy_node_text(context, named);
                size_t existing;
                int duplicate = 0;
                if (text == NULL) {
                    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                    return QL_STATUS_OUT_OF_MEMORY;
                }
                for (existing = 0u; existing < context->global_count;
                     ++existing) {
                    if (strcmp(context->globals[existing].name, text) != 0) {
                        continue;
                    }
                    duplicate = 1;
                    if (initializer != SIZE_MAX) {
                        context->globals[existing].declaration_node = index;
                        context->globals[existing].declarator_node =
                            declarator;
                        context->globals[existing].initializer_node =
                            initializer;
                    }
                    break;
                }
                context->allocator->deallocate(context->allocator->user_data,
                                               text);
                if (duplicate) {
                    continue;
                }
            }
            status = grow_array(context->allocator,
                                (void **)&context->globals,
                                &context->global_capacity,
                                sizeof(*context->globals),
                                context->global_count + 1u, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            entry = &context->globals[context->global_count];
            memset(entry, 0, sizeof(*entry));
            entry->name = copy_node_text(context, named);
            if (entry->name == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            entry->declaration_node = index;
            entry->declarator_node = declarator;
            entry->initializer_node = initializer;
            entry->object = SIZE_MAX;
            entry->address = QL_IR_INVALID_VALUE_ID;
            ++context->global_count;
        }
    }
    return QL_STATUS_OK;
}

/* Only the globals the body names get an object. Creating one for every
   declaration in the unit would add parameters and a quadratic pile of
   disjointness assumptions for storage the function never touches. */
static void collect_global_uses(lower_context *context, size_t body_node) {
    size_t end = subtree_end(context, body_node);
    size_t index;

    for (index = body_node + 1u; index < end; ++index) {
        char *name;
        size_t global;

        if (strcmp(context->nodes[index].view.kind, "identifier") != 0) {
            continue;
        }
        name = copy_node_text(context, index);
        if (name == NULL) {
            continue;
        }
        for (global = 0u; global < context->global_count; ++global) {
            if (strcmp(context->globals[global].name, name) != 0) {
                continue;
            }
            if (context->globals[global].referenced == 0u) {
                context->globals[global].referenced = 1u;
                context->uses_memory = 1u;
            }
            break;
        }
        context->allocator->deallocate(context->allocator->user_data, name);
    }
}

static void release_globals(lower_context *context) {
    size_t index;
    for (index = 0u; index < context->global_count; ++index) {
        context->allocator->deallocate(context->allocator->user_data,
                                       context->globals[index].name);
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   context->globals);
    context->globals = NULL;
    context->global_count = 0u;
    context->global_capacity = 0u;
}

static void collect_calls(lower_context *context, size_t body_node) {
    size_t end = subtree_end(context, body_node);
    size_t index;

    for (index = body_node + 1u; index < end; ++index) {
        size_t function_node;
        char *name;
        int is_call;

        if (strcmp(context->nodes[index].view.kind, "call_expression") != 0) {
            continue;
        }
        function_node = direct_field_child(context, index, "function");
        if (function_node == SIZE_MAX ||
            strcmp(context->nodes[function_node].view.kind,
                   "identifier") != 0) {
            /* A parenthesised callee is the cast Tree-sitter resolved toward
               a call, and casts neither touch memory nor are observable. */
            continue;
        }
        name = copy_node_text(context, function_node);
        if (name == NULL) {
            continue;
        }
        is_call = find_callee(context, name) != NULL;
        context->allocator->deallocate(context->allocator->user_data, name);
        if (!is_call) {
            continue;
        }
        context->makes_calls = 1u;
        context->uses_memory = 1u;
        return;
    }
}

static void release_address_taken(lower_context *context) {
    size_t index;
    for (index = 0u; index < context->address_taken_count; ++index) {
        context->allocator->deallocate(context->allocator->user_data,
                                       context->address_taken[index]);
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   context->address_taken);
    context->address_taken = NULL;
    context->address_taken_count = 0u;
    context->address_taken_capacity = 0u;
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
            uint32_t pointer_depth;
            uint32_t is_array_or_function;
            lower_typedef *entry;
            ql_status status;

            if (context->nodes[child].parent != index ||
                context->nodes[child].view.field_name == NULL ||
                strcmp(context->nodes[child].view.field_name,
                       "declarator") != 0) {
                continue;
            }
            name_node = typedef_declarator_name(context, child,
                                                &pointer_depth,
                                                &is_array_or_function);
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
            entry->pointer_depth = pointer_depth;
            entry->is_array_or_function = is_array_or_function;
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

static uint64_t record_size(const lower_context *context, size_t record) {
    return record < context->record_count ? context->records[record].size
                                          : 0u;
}

static uint32_t record_alignment(const lower_context *context,
                                 size_t record) {
    return record < context->record_count
               ? context->records[record].alignment
               : 1u;
}

/* Records are looked up by tag. A tagless struct can only be named where it
   is written, which this slice does not need. */
static size_t find_record(const lower_context *context, const char *tag,
                          uint32_t is_union) {
    size_t index;
    if (tag == NULL) {
        return SIZE_MAX;
    }
    for (index = 0u; index < context->record_count; ++index) {
        const lower_record *record = &context->records[index];
        if (record->tag != NULL && record->is_union == is_union &&
            strcmp(record->tag, tag) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

/* Enumerators are integer constants with names. Only values this profile can
   state are taken; anything else stays unresolved rather than guessed. */
static ql_status collect_enumerators(lower_context *context,
                                     ql_error *error) {
    size_t index;

    for (index = 0u; index < context->node_count; ++index) {
        size_t end;
        size_t child;
        uint64_t next_value = 0u;

        if (strcmp(context->nodes[index].view.kind, "enumerator_list") != 0) {
            continue;
        }
        end = subtree_end(context, index);
        for (child = index + 1u; child < end; ++child) {
            size_t name_node;
            size_t value_node;
            lower_enumerator *entry;
            ql_status status;

            if (context->nodes[child].parent != index ||
                strcmp(context->nodes[child].view.kind, "enumerator") != 0) {
                continue;
            }
            name_node = direct_field_child(context, child, "name");
            value_node = direct_field_child(context, child, "value");
            if (name_node == SIZE_MAX) {
                continue;
            }
            if (value_node != SIZE_MAX) {
                char *text = copy_node_text(context, value_node);
                unsigned long long parsed = 0ull;
                char *stop = NULL;
                if (text == NULL) {
                    ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                    return QL_STATUS_OUT_OF_MEMORY;
                }
                parsed = strtoull(text, &stop, 0);
                if (stop == text || (stop != NULL && *stop != '\0')) {
                    /* A computed enumerator is not a literal; leaving it out
                       makes the identifier unresolved rather than wrong. */
                    context->allocator->deallocate(
                        context->allocator->user_data, text);
                    continue;
                }
                context->allocator->deallocate(context->allocator->user_data,
                                               text);
                next_value = (uint64_t)parsed;
            }
            status = grow_array(context->allocator,
                                (void **)&context->enumerators,
                                &context->enumerator_capacity,
                                sizeof(*context->enumerators),
                                context->enumerator_count + 1u, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            entry = &context->enumerators[context->enumerator_count];
            memset(entry, 0, sizeof(*entry));
            entry->name = copy_node_text(context, name_node);
            if (entry->name == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            entry->value = next_value;
            ++context->enumerator_count;
            ++next_value;
        }
    }
    return QL_STATUS_OK;
}

static const lower_enumerator *find_enumerator(const lower_context *context,
                                               const char *name) {
    size_t index;
    for (index = 0u; index < context->enumerator_count; ++index) {
        if (strcmp(context->enumerators[index].name, name) == 0) {
            return &context->enumerators[index];
        }
    }
    return NULL;
}

/* A prototype is a declaration whose declarator is a function declarator.
   Only the nodes are recorded here; the types are resolved at the first call,
   so a prototype naming something outside this slice costs only the bodies
   that call it. */
static ql_status collect_callees(lower_context *context, ql_error *error) {
    size_t index;

    for (index = 0u; index < context->node_count; ++index) {
        size_t end;
        size_t child;

        if (strcmp(context->nodes[index].view.kind, "declaration") != 0) {
            continue;
        }
        end = subtree_end(context, index);
        for (child = index + 1u; child < end; ++child) {
            size_t declarator = child;
            size_t name_node;
            lower_callee *entry;
            uint32_t return_pointer_depth = 0u;
            size_t hops = 0u;
            ql_status status;

            if (context->nodes[child].parent != index ||
                context->nodes[child].view.field_name == NULL ||
                strcmp(context->nodes[child].view.field_name,
                       "declarator") != 0) {
                continue;
            }
            /* `T *f(...)` nests the function declarator inside the pointer
               declarators, and those stars belong to the return type. Walking
               them off here is what lets a callee that returns a pointer be
               declared at all: stopping at the outermost node would leave it
               looking undeclared at every call site. */
            while (declarator != SIZE_MAX && hops++ < 8u &&
                   strcmp(context->nodes[declarator].view.kind,
                          "pointer_declarator") == 0) {
                ++return_pointer_depth;
                declarator = direct_field_child(context, declarator,
                                                "declarator");
            }
            if (declarator == SIZE_MAX ||
                strcmp(context->nodes[declarator].view.kind,
                       "function_declarator") != 0) {
                continue;
            }
            name_node = direct_field_child(context, declarator, "declarator");
            if (name_node == SIZE_MAX ||
                strcmp(context->nodes[name_node].view.kind,
                       "identifier") != 0) {
                continue;
            }
            status = grow_array(context->allocator,
                                (void **)&context->callees,
                                &context->callee_capacity,
                                sizeof(*context->callees),
                                context->callee_count + 1u, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            entry = &context->callees[context->callee_count];
            memset(entry, 0, sizeof(*entry));
            entry->name = copy_node_text(context, name_node);
            if (entry->name == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            entry->declaration_node = index;
            entry->declarator_node = declarator;
            entry->return_pointer_depth = return_pointer_depth;
            ++context->callee_count;
        }
    }
    return QL_STATUS_OK;
}

static lower_callee *find_callee(lower_context *context, const char *name) {
    size_t index;
    for (index = 0u; index < context->callee_count; ++index) {
        if (strcmp(context->callees[index].name, name) == 0) {
            return &context->callees[index];
        }
    }
    return NULL;
}

static void release_callees(lower_context *context) {
    size_t index;
    for (index = 0u; index < context->callee_count; ++index) {
        context->allocator->deallocate(context->allocator->user_data,
                                       context->callees[index].parameters);
        context->allocator->deallocate(context->allocator->user_data,
                                       context->callees[index].name);
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   context->callees);
    context->callees = NULL;
    context->callee_count = 0u;
    context->callee_capacity = 0u;
}

static ql_status collect_records(lower_context *context, ql_error *error) {
    size_t index;

    for (index = 0u; index < context->node_count; ++index) {
        const char *kind = context->nodes[index].view.kind;
        uint32_t is_union;
        size_t body;
        size_t name_node;
        char *tag;
        lower_record *record;
        ql_status status;

        if (strcmp(kind, "struct_specifier") == 0) {
            is_union = 0u;
        } else if (strcmp(kind, "union_specifier") == 0) {
            is_union = 1u;
        } else {
            continue;
        }
        body = direct_field_child(context, index, "body");
        if (body == SIZE_MAX) {
            /* A mention without a body refers to a definition elsewhere. */
            continue;
        }
        name_node = direct_field_child(context, index, "name");
        tag = name_node == SIZE_MAX ? NULL
                                    : copy_node_text(context, name_node);
        if (name_node != SIZE_MAX && tag == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        if (find_record(context, tag, is_union) != SIZE_MAX) {
            context->allocator->deallocate(context->allocator->user_data,
                                           tag);
            continue;
        }
        status = grow_array(context->allocator, (void **)&context->records,
                            &context->record_capacity,
                            sizeof(*context->records),
                            context->record_count + 1u, error);
        if (status != QL_STATUS_OK) {
            context->allocator->deallocate(context->allocator->user_data,
                                           tag);
            return status;
        }
        record = &context->records[context->record_count++];
        memset(record, 0, sizeof(*record));
        record->tag = tag;
        record->body_node = body;
        record->is_union = is_union;
        record->layout_state = LOWER_LAYOUT_PENDING;
    }
    return QL_STATUS_OK;
}

static void release_records(lower_context *context) {
    size_t index;
    size_t member;
    for (index = 0u; index < context->record_count; ++index) {
        lower_record *record = &context->records[index];
        for (member = 0u; member < record->member_count; ++member) {
            context->allocator->deallocate(context->allocator->user_data,
                                           record->members[member].name);
        }
        context->allocator->deallocate(context->allocator->user_data,
                                       record->members);
        context->allocator->deallocate(context->allocator->user_data,
                                       record->tag);
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   context->records);
    context->records = NULL;
    context->record_count = 0u;
    context->record_capacity = 0u;
}

static void release_enumerators(lower_context *context) {
    size_t index;
    for (index = 0u; index < context->enumerator_count; ++index) {
        context->allocator->deallocate(context->allocator->user_data,
                                       context->enumerators[index].name);
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   context->enumerators);
    context->enumerators = NULL;
    context->enumerator_count = 0u;
    context->enumerator_capacity = 0u;
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

static ql_status ensure_record_layout(lower_context *context, size_t record,
                                      size_t node, ql_error *error);
static ql_status parse_type_spelling(lower_context *context,
                                     const char *spelling, size_t node,
                                     uint32_t allow_void,
                                     lower_type *output,
                                     ql_error *error);

/* Resolves a type node together with the indirection its declarator adds.
   This is the one place that knows how a struct, a union, an enum, a typedef
   name, and a plain scalar each turn into a lowering type. */
static ql_status resolve_type_node(lower_context *context, size_t type_node,
                                   uint32_t pointer_depth, lower_type *output,
                                   ql_error *error) {
    const char *kind = context->nodes[type_node].view.kind;
    ql_status status;

    if (pointer_depth > 2u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, type_node,
            "this slice carries at most two levels of indirection", error);
    }
    if (strcmp(kind, "struct_specifier") == 0 ||
        strcmp(kind, "union_specifier") == 0) {
        const uint32_t is_union = strcmp(kind, "union_specifier") == 0;
        size_t name_node = direct_field_child(context, type_node, "name");
        char *tag = name_node == SIZE_MAX
                        ? NULL
                        : copy_node_text(context, name_node);
        size_t record;
        if (name_node != SIZE_MAX && tag == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        record = find_record(context, tag, is_union);
        context->allocator->deallocate(context->allocator->user_data, tag);
        if (record == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, type_node,
                "struct or union has no definition in this unit", error);
        }
        if (pointer_depth == 0u) {
            /* Only a member held by value needs its layout now. A pointer to
               the enclosing record is how linked structures are written, and
               demanding a layout here would call every one of them
               self-containing. */
            status = ensure_record_layout(context, record, type_node, error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
            *output = make_record_type(record);
            return QL_STATUS_OK;
        }
        *output = make_record_pointer_type(record);
        return QL_STATUS_OK;
    }
    {
        char *spelling = copy_node_text(context, type_node);
        lower_type base;
        if (spelling == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        status = parse_type_spelling(context, spelling, type_node,
                                     pointer_depth != 0u, &base, error);
        context->allocator->deallocate(context->allocator->user_data,
                                       spelling);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        if (base.kind == QL_C_SCALAR_RECORD) {
            *output = pointer_depth == 0u ? base : make_pointer_to(base);
            return QL_STATUS_OK;
        }
        *output = pointer_depth == 0u ? base : make_pointer_to(base);
        return QL_STATUS_OK;
    }
}

/* Walks a member declarator down to its name, counting the stars on the way.
   An array or function declarator inside a record is not laid out here. */
/* `T a[]` said an array but not how long. */
#define LOWER_ARRAY_BOUND_FROM_INITIALIZER UINT64_MAX
#define LOWER_MAX_INITIALIZER_ELEMENTS UINT64_C(256)

/* Counts positional elements without interpreting them. Designators need a
   member/index map rather than a count and are left for their own work unit. */
static int positional_initializer_count(const lower_context *context,
                                        size_t node, uint64_t *count) {
    size_t end;
    size_t child;

    *count = 0u;
    if (node == SIZE_MAX ||
        strcmp(context->nodes[node].view.kind, "initializer_list") != 0) {
        return 0;
    }
    end = subtree_end(context, node);
    for (child = node + 1u; child < end; ++child) {
        if (context->nodes[child].parent != node ||
            (context->nodes[child].view.flags & QL_C_SYNTAX_NODE_NAMED) ==
                0u ||
            strcmp(context->nodes[child].view.kind, "comment") == 0) {
            continue;
        }
        if (strcmp(context->nodes[child].view.kind, "initializer_pair") ==
            0) {
            return 0;
        }
        ++(*count);
        if (*count > LOWER_MAX_INITIALIZER_ELEMENTS) {
            return 0;
        }
    }
    return 1;
}

/* An array bound has to be a constant this pass can read, because the
   object's size is what keeps an out-of-bounds access out of bounds. A bound
   that is a run-time expression, or one this pass cannot fold, is refused
   rather than guessed at. `T a[]` with no bound is a pointer parameter in C,
   and is reported as a zero length for the caller to interpret. */
static int constant_array_bound(const lower_context *context, size_t node,
                                uint64_t *length) {
    const char *kind;
    char *text;
    int ok = 0;

    if (node == SIZE_MAX) {
        return 0;
    }
    kind = context->nodes[node].view.kind;
    text = copy_node_text(context, node);
    if (text == NULL) {
        return 0;
    }
    if (strcmp(kind, "number_literal") == 0) {
        char *stop = NULL;
        unsigned long long value = strtoull(text, &stop, 0);
        /* Only a plain unsuffixed or integer-suffixed count, and never zero:
           a zero-length array has no bytes for an access to be inside. */
        if (stop != text && value != 0ull && value <= (1ull << 32)) {
            while (*stop == 'u' || *stop == 'U' || *stop == 'l' ||
                   *stop == 'L') {
                ++stop;
            }
            if (*stop == '\0') {
                *length = (uint64_t)value;
                ok = 1;
            }
        }
    } else if (strcmp(kind, "identifier") == 0) {
        const lower_enumerator *enumerator = find_enumerator(context, text);
        if (enumerator != NULL && enumerator->value != 0u) {
            *length = (uint64_t)enumerator->value;
            ok = 1;
        }
    }
    context->allocator->deallocate(context->allocator->user_data, text);
    return ok;
}

static size_t member_declarator_name(const lower_context *context,
                                     size_t declarator,
                                     uint32_t *pointer_depth,
                                     uint64_t *array_length, int *rejected) {
    size_t guard = 0u;

    *pointer_depth = 0u;
    *array_length = 0u;
    *rejected = 0;
    while (declarator != SIZE_MAX && guard++ < 64u) {
        const char *kind = context->nodes[declarator].view.kind;
        if (strcmp(kind, "field_identifier") == 0 ||
            strcmp(kind, "identifier") == 0 ||
            strcmp(kind, "type_identifier") == 0) {
            return declarator;
        }
        if (strcmp(kind, "pointer_declarator") == 0) {
            ++(*pointer_depth);
            declarator = direct_field_child(context, declarator,
                                            "declarator");
            continue;
        }
        if (strcmp(kind, "parenthesized_declarator") == 0) {
            declarator = direct_field_child(context, declarator,
                                            "declarator");
            continue;
        }
        if (strcmp(kind, "array_declarator") == 0) {
            size_t size_node = direct_field_child(context, declarator,
                                                  "size");
            if (*array_length != 0u) {
                /* A second bound is a multidimensional array, which is one
                   object with a shape this slice does not carry. */
                *rejected = 1;
                return SIZE_MAX;
            }
            if (size_node != SIZE_MAX &&
                !constant_array_bound(context, size_node, array_length)) {
                *rejected = 1;
                return SIZE_MAX;
            }
            if (size_node == SIZE_MAX) {
                /* `T a[]`: an array whose bound is stated somewhere else,
                   by an initialiser or by the adjustment C makes to a
                   parameter. The caller decides which. */
                *array_length = LOWER_ARRAY_BOUND_FROM_INITIALIZER;
            }
            declarator = direct_field_child(context, declarator,
                                            "declarator");
            continue;
        }
        *rejected = 1;
        return SIZE_MAX;
    }
    *rejected = 1;
    return SIZE_MAX;
}

/* x86-64 SysV layout: each member sits at the next offset its alignment
   allows, the record's alignment is the widest member's, and the total is
   rounded up so that an array of the record stays aligned. A union puts every
   member at zero. */
static ql_status ensure_record_layout(lower_context *context, size_t record,
                                      size_t node, ql_error *error) {
    lower_record *entry;
    size_t body;
    size_t end;
    size_t child;
    uint64_t offset = 0u;
    uint32_t alignment = 1u;
    ql_status status;

    if (record >= context->record_count) {
        return lower_unknown(context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE,
                             node, "record is not declared in this unit",
                             error);
    }
    entry = &context->records[record];
    if (entry->layout_state == LOWER_LAYOUT_DONE) {
        return QL_STATUS_OK;
    }
    if (entry->layout_state == LOWER_LAYOUT_RUNNING) {
        /* A record that contains itself by value has no layout, and Tree
           sitter accepts the declaration, so this has to be refused rather
           than recursed into. */
        return lower_unknown(context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE,
                             node, "record contains itself by value", error);
    }
    entry->layout_state = LOWER_LAYOUT_RUNNING;
    body = entry->body_node;
    end = subtree_end(context, body);
    for (child = body + 1u; child < end; ++child) {
        size_t type_node;
        size_t declarator_end;
        size_t declarator;
        if (context->nodes[child].parent != body ||
            strcmp(context->nodes[child].view.kind,
                   "field_declaration") != 0) {
            continue;
        }
        type_node = direct_field_child(context, child, "type");
        if (type_node == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_INVALID_DECLARATION, child,
                "record member has no type", error);
        }
        declarator_end = subtree_end(context, child);
        for (declarator = child + 1u; declarator < declarator_end;
             ++declarator) {
            uint32_t pointer_depth;
            uint64_t member_array_length;
            int rejected;
            size_t name_node;
            lower_member *member;
            lower_type member_type;
            uint64_t member_size;
            uint32_t member_alignment;
            const lower_record *reread;

            if (context->nodes[declarator].parent != child ||
                context->nodes[declarator].view.field_name == NULL ||
                strcmp(context->nodes[declarator].view.field_name,
                       "declarator") != 0) {
                continue;
            }
            name_node = member_declarator_name(context, declarator,
                                               &pointer_depth,
                                               &member_array_length,
                                               &rejected);
            if (rejected != 0 || name_node == SIZE_MAX) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE,
                    declarator,
                    "function and bit-field members, and members whose array "
                    "bound this pass cannot fold, are outside this slice",
                    error);
            }
            status = resolve_type_node(context, type_node, pointer_depth,
                                       &member_type, error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
            if (member_array_length == LOWER_ARRAY_BOUND_FROM_INITIALIZER) {
                /* A flexible array member has no size of its own, so the
                   record has none this slice can state. */
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE,
                    declarator,
                    "a flexible array member leaves the record without a "
                    "size this slice can state", error);
            }
            if (member_array_length != 0u) {
                /* An array member takes its element's alignment and its
                   element count in bytes, which is what puts the members
                   after it at the offsets a compiled struct uses. */
                member_type = make_array_of(member_type,
                                            member_array_length);
            }
            if (member_type.kind == QL_C_SCALAR_VOID) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, declarator,
                    "a record member cannot have void type", error);
            }
            member_size = type_byte_width(context, member_type);
            member_alignment = type_alignment(context, member_type);
            if (member_alignment == 0u) {
                member_alignment = 1u;
            }
            /* `entry` may have been invalidated by a nested layout growing
               the record table. */
            status = grow_array(context->allocator,
                                (void **)&context->records[record].members,
                                &context->records[record].member_capacity,
                                sizeof(lower_member),
                                context->records[record].member_count + 1u,
                                error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            entry = &context->records[record];
            member = &entry->members[entry->member_count];
            memset(member, 0, sizeof(*member));
            member->name = copy_node_text(context, name_node);
            if (member->name == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            member->type = member_type;
            if (entry->is_union != 0u) {
                member->offset = 0u;
                if (member_size > entry->size) {
                    entry->size = member_size;
                }
            } else {
                offset = (offset + member_alignment - 1u) /
                         member_alignment * member_alignment;
                member->offset = offset;
                offset += member_size;
            }
            if (member_alignment > alignment) {
                alignment = member_alignment;
            }
            ++entry->member_count;
            reread = entry;
            (void)reread;
        }
    }
    entry = &context->records[record];
    entry->alignment = alignment;
    if (entry->is_union == 0u) {
        entry->size = (offset + alignment - 1u) / alignment * alignment;
    } else {
        entry->size = (entry->size + alignment - 1u) / alignment * alignment;
    }
    if (entry->size == 0u) {
        return lower_unknown(context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE,
                             node, "record has no members", error);
    }
    entry->layout_state = LOWER_LAYOUT_DONE;
    return QL_STATUS_OK;
}

static ql_status parse_type_spelling(lower_context *context,
                                     const char *spelling, size_t node,
                                     uint32_t allow_void, lower_type *output,
                                     ql_error *error) {
    ql_c_scalar_type scalar;
    size_t hops = 0u;
    ql_status status;

    /* A typedef name means whatever this unit declared it to mean. Only names
       the unit actually declares are resolved: assuming a meaning for an
       undeclared name would be a guess, and a wrong guess about a type is a
       wrong answer about the function. */
    while (!ql_c_scalar_from_spelling(spelling, &scalar)) {
        const lower_typedef *entry;
        if (strncmp(spelling, "struct", 6u) == 0 ||
            strncmp(spelling, "union", 5u) == 0) {
            const uint32_t is_union = spelling[0] == 'u';
            const char *tag = spelling + (is_union ? 5u : 6u);
            size_t record;
            while (*tag == ' ' || *tag == '\t' || *tag == '\n') {
                ++tag;
            }
            record = find_record(context, tag, is_union);
            if (record == SIZE_MAX) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
                    "struct or union has no definition in this unit", error);
            }
            status = ensure_record_layout(context, record, node, error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
            *output = make_record_type(record);
            return QL_STATUS_OK;
        }
        if (strncmp(spelling, "enum", 4u) == 0) {
            /* An enumeration's values are ints under this ABI, and its
               enumerators are collected separately as named constants. */
            *output = make_integer_type(32u, 3u, 1u);
            return QL_STATUS_OK;
        }
        entry = find_typedef(context, spelling);
        if (entry == NULL) {
            /* The unit's own declarations win; this is the fallback for the
               preamble typedefs record extraction leaves behind. */
            if (ql_c_scalar_from_corpus_typedef(spelling, &scalar)) {
                *output = type_from_scalar(scalar);
                return QL_STATUS_OK;
            }
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
                "type spelling names no ASM2C_GNU_V1 scalar type, no typedef "
                "this unit declares, and no corpus preamble typedef", error);
        }
        if (entry->is_array_or_function != 0u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
                "typedef names an array or function type", error);
        }
        if (entry->pointer_depth != 0u) {
            /* `typedef int *T` is a pointer type, not a reason to give up.
               The indirection it adds rides on top of whatever the rest of
               the chain resolves to. */
            lower_type base;
            ql_status resolved = parse_type_spelling(context,
                                                     entry->underlying, node,
                                                     1u, &base, error);
            if (resolved != QL_STATUS_OK || context->unknown != 0u) {
                return resolved;
            }
            if (entry->pointer_depth > 2u) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
                    "this slice carries at most two levels of indirection", error);
            }
            *output = make_pointer_to(base);
            return QL_STATUS_OK;
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
    if ((inventory->shape & QL_C_TYPE_SHAPE_FUNCTION) != 0u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
            "function-valued declarations are outside this lowering slice",
            error);
    }
    if ((inventory->shape & QL_C_TYPE_SHAPE_ARRAY) != 0u) {
        /* A parameter declared `T a[]` or `T a[N]` is a `T *`. C adjusts it
           that way itself, and the bound, where there is one, says nothing
           the callee can rely on. Treating it as anything else would invent
           an object the caller never promised. */
        ql_c_type_inventory_v1 adjusted = *inventory;
        if (allow_void != 0u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
                "a function cannot return an array", error);
        }
        adjusted.shape =
            (uint32_t)(adjusted.shape & ~(uint32_t)QL_C_TYPE_SHAPE_ARRAY);
        adjusted.pointer_depth += 1u;
        return type_from_inventory(context, &adjusted, node, allow_void,
                                   output, error);
    }
    if (inventory->pointer_depth > 2u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
            "this slice carries at most two levels of indirection", error);
    }
    if (inventory->pointer_depth == 1u) {
        lower_type pointee;
        ql_status status = parse_type_spelling(context,
                                               inventory->base_spelling, node,
                                               1u, &pointee, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        *output = make_pointer_to(pointee);
        return QL_STATUS_OK;
    }
    /* The inventory's base-kind classification is a fast syntactic hint.
       Multi-keyword integer specifiers vary in Tree-sitter shape, so the
       versioned spelling parser below is the semantic authority. */
    {
        ql_status status = parse_type_spelling(context,
                                               inventory->base_spelling, node,
                                               allow_void, output, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        if (output->kind == QL_C_SCALAR_RECORD) {
            /* Members of a record are reachable; the whole record as a value
               is not, because this slice has no aggregate IR value and no
               object to give it. */
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
                "a struct or union passed or returned by value is outside "
                "this slice", error);
        }
        return QL_STATUS_OK;
    }
}

/* Builds, and remembers, the IR type for a C type. Two occurrences of one C
   type must share one IR type identifier, because a load requires its
   pointer's element type to be that same identifier as its result type. One
   cache keyed by the C type keeps that true for every shape, including the
   nested pointers an ad-hoc slot per width could not express. */
static ql_status ensure_ir_type(lower_context *context, lower_type *type,
                                ql_error *error) {
    ql_ir_type_definition_v1 definition;
    ql_ir_type_id id;
    ql_status status;
    size_t index;

    if (type->ir_type != QL_IR_INVALID_TYPE_ID) {
        return QL_STATUS_OK;
    }
    if (type->kind == QL_C_SCALAR_POINTER) {
        lower_type target = pointer_target(*type);
        if (target.kind == QL_C_SCALAR_RECORD) {
            /* A record has no IR type of its own under this profile: a
               pointer to one is an address into bytes, and every member
               access reinterprets it at the member's type. */
            target = make_integer_type(8u, 1u, 0u);
        }
        status = ensure_ir_type(context, &target, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        ql_ir_type_definition_init(&definition, QL_IR_TYPE_POINTER);
        definition.bit_width = QL_C_POINTER_WIDTH;
        definition.element_type = target.ir_type;
    } else if (type->kind == QL_C_SCALAR_VOID) {
        ql_ir_type_definition_init(&definition, QL_IR_TYPE_VOID);
    } else if (type->kind == QL_C_SCALAR_BOOL) {
        ql_ir_type_definition_init(&definition, QL_IR_TYPE_BOOL);
        definition.bit_width = 1u;
    } else if (type->kind == QL_C_SCALAR_INTEGER && type->width != 0u &&
               type->width <= 128u) {
        ql_ir_type_definition_init(&definition, QL_IR_TYPE_BIT_VECTOR);
        definition.bit_width = type->width;
    } else {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "C type has no IR representation during lowering");
        return QL_STATUS_INTERNAL_ERROR;
    }
    for (index = 0u; index < context->type_cache_count; ++index) {
        const lower_type_binding *binding = &context->type_cache[index];
        if (binding->kind == definition.kind &&
            binding->bit_width == definition.bit_width &&
            binding->element_type == definition.element_type) {
            type->ir_type = binding->id;
            return QL_STATUS_OK;
        }
    }
    status = ql_ir_builder_add_type(context->builder, &definition, &id,
                                    error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = grow_array(context->allocator, (void **)&context->type_cache,
                        &context->type_cache_capacity,
                        sizeof(*context->type_cache),
                        context->type_cache_count + 1u, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    context->type_cache[context->type_cache_count].kind = definition.kind;
    context->type_cache[context->type_cache_count].bit_width =
        definition.bit_width;
    context->type_cache[context->type_cache_count].element_type =
        definition.element_type;
    context->type_cache[context->type_cache_count].id = id;
    ++context->type_cache_count;
    type->ir_type = id;
    return QL_STATUS_OK;
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

static ql_status ensure_trace_type(lower_context *context,
                                   ql_error *error) {
    ql_ir_type_definition_v1 definition;
    ql_ir_type_id id;
    ql_status status;

    if (context->trace_type != QL_IR_INVALID_TYPE_ID) {
        return QL_STATUS_OK;
    }
    ql_ir_type_definition_init(&definition, QL_IR_TYPE_EVENT_TRACE);
    status = ql_ir_builder_add_type(context->builder, &definition, &id,
                                    error);
    if (status == QL_STATUS_OK) {
        context->trace_type = id;
    }
    return status;
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
                                     uint64_t byte_width,
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
                                   lower_value pointer, uint64_t byte_width,
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

static lower_value stack_address(const lower_context *context,
                                 const lower_variable *variable);

static ql_status emit_load(lower_context *context, lower_value pointer,
                           lower_value *output, ql_error *error) {
    lower_type pointee = pointer_target(pointer.type);
    ql_ir_value_id operands[2];
    ql_status status;

    if (pointer.has_object == 0u) {
        /* Its object is not in the table, so no guard could ever justify the
           access. Refusing here leaves an UNKNOWN rather than IR that is
           undefined on every input, which would be worse: it would look
           lowered and prove nothing. */
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, SIZE_MAX,
            "dereferencing a pointer read out of memory needs an object this "
            "slice does not declare", error);
    }
    if (pointee.kind == QL_C_SCALAR_VOID) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, SIZE_MAX,
            "a pointer to void has no value to load", error);
    }
    status = emit_access_guard(context, pointer, pointee_byte_width(context,
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
    lower_type pointee = pointer_target(pointer.type);
    lower_value converted;
    ql_ir_value_id operands[3];
    ql_ir_value_id combined;
    uint32_t may_ub = pointer.may_ub | value.may_ub;
    ql_status status;

    if (pointer.has_object == 0u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, SIZE_MAX,
            "storing through a pointer read out of memory needs an object "
            "this slice does not declare", error);
    }
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
                               pointee_byte_width(context, pointer.type),
                               combined,
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

    if (pointer.type.indirection == 1u &&
        pointer.type.pointee.kind == QL_C_SCALAR_VOID) {
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
                               pointee_byte_width(context, pointer.type), &scale,
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
    output->has_object = pointer.has_object;
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
    {
        size_t slot;
        for (slot = 0u; slot < context->stack_slot_count; ++slot) {
            if (strcmp(context->stack_slots[slot].name, variable->name) != 0) {
                continue;
            }
            for (index = context->variable_count; index != 0u; --index) {
                const lower_variable *visible =
                    &context->variables[index - 1u];
                if (visible->name_size == name_size &&
                    memcmp(visible->name, name, name_size) == 0) {
                    context->allocator->deallocate(
                        context->allocator->user_data, variable->name);
                    memset(variable, 0, sizeof(*variable));
                    return lower_unknown(
                        context,
                        QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
                        "shadowing an address-taken name needs a distinct "
                        "stack object", error);
                }
            }
            if (type_same(context->stack_slots[slot].type, type) == 0 ||
                context->stack_slots[slot].type.array_length !=
                    type.array_length) {
                context->allocator->deallocate(
                    context->allocator->user_data, variable->name);
                memset(variable, 0, sizeof(*variable));
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER,
                    node,
                    "an address-taken name resolves to declarations with "
                    "different storage types", error);
            }
            /* The slot was created before the entry block; the variable only
               binds to it now. */
            variable->is_stack = 1u;
            variable->address = context->stack_slots[slot].address;
            variable->has_object = 1u;
            break;
        }
    }
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
    state->trace = context->trace_value;
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
    context->trace_value = state->trace;
    for (index = 0u; index < state->count; ++index) {
        context->variables[index].initialized = state->initialized[index];
        if (context->variables[index].is_stack != 0u) {
            continue;
        }
        context->variables[index].value = state->values[index];
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
    uint32_t is_negative = 0u;
    lower_type candidates[6];
    size_t candidate_count = 0u;
    lower_type selected;
    ql_status status;

    if (text == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    length = strlen(text);
    /* Tree-sitter C folds a leading sign into a number_literal. C still
       chooses the literal's type from the unsigned magnitude first and then
       applies unary plus or minus. Keep those two steps separate here: using
       the sign as a digit rejected every corpus spelling such as `-1`, while
       choosing a type from the signed mathematical value would get cases
       such as `-2147483648` and `-1u` wrong. */
    if (length != 0u && (text[0] == '-' || text[0] == '+')) {
        is_negative = text[0] == '-';
        digit_start = 1u;
    }
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
    if (suffix_start >= digit_start + 2u && text[digit_start] == '0' &&
        (text[digit_start + 1u] == 'x' ||
         text[digit_start + 1u] == 'X')) {
        base = 16u;
        digit_start += 2u;
        is_decimal = 0u;
    } else if (suffix_start >= digit_start + 2u &&
               text[digit_start] == '0' &&
               (text[digit_start + 1u] == 'b' ||
                text[digit_start + 1u] == 'B')) {
        base = 2u;
        digit_start += 2u;
        is_decimal = 0u;
    } else if (suffix_start > digit_start + 1u &&
               text[digit_start] == '0') {
        base = 8u;
        ++digit_start;
        is_decimal = 0u;
    }
    if (digit_start == suffix_start) {
        context->allocator->deallocate(context->allocator->user_data, text);
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_INTEGER_LITERAL_OUT_OF_RANGE,
            node, "integer literal has no digits", error);
    }
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
    if (is_negative != 0u) {
        ql_ir_value_id magnitude = output->value;
        status = emit_instruction(context, QL_IR_OPCODE_BV_NEG, &selected,
                                  &magnitude, 1u, NULL, 0u,
                                  QL_IR_EFFECT_NONE, &output->value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
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
    output->has_object = address.has_object;
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
    if (input.type.kind == QL_C_SCALAR_VOID ||
        target.kind == QL_C_SCALAR_VOID) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, SIZE_MAX,
            "a void expression cannot convert to an object value", error);
    }
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
    if (variable == NULL) {
        const lower_enumerator *enumerator = find_enumerator(context, name);
        context->allocator->deallocate(context->allocator->user_data, name);
        if (enumerator != NULL) {
            lower_type type = make_integer_type(32u, 3u, 1u);
            ql_status constant_status = ensure_bool_constants(context, error);
            memset(output, 0, sizeof(*output));
            output->type = type;
            output->may_ub = 0u;
            if (constant_status != QL_STATUS_OK) {
                return constant_status;
            }
            output->defined = context->true_value;
            return add_uint_constant(context, type, enumerator->value,
                                     &output->value, error);
        }
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNDECLARED_IDENTIFIER, node,
            "identifier does not name a parameter, local, or enumerator",
            error);
    }
    context->allocator->deallocate(context->allocator->user_data, name);
    if (variable->initialized == 0u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNINITIALIZED_READ, node,
            "read of an uninitialized local has no modeled value", error);
    }
    status = ensure_bool_constants(context, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (variable->type.array_length != 0u) {
        /* Naming an array yields a pointer to its first element. It is never
           loaded: there is no value of array type to load. */
        memset(output, 0, sizeof(*output));
        *output = stack_address(context, variable);
        return QL_STATUS_OK;
    }
    if (variable->type.kind == QL_C_SCALAR_RECORD) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
            "a struct or union value is outside this slice; only its members "
            "are", error);
    }
    if (variable->is_stack != 0u) {
        return emit_load(context, stack_address(context, variable), output,
                         error);
    }
    output->value = variable->value;
    output->defined = context->true_value;
    output->type = variable->type;
    output->may_ub = 0u;
    output->has_object = variable->has_object;
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
            /* C measures the gap in elements, so the byte difference is
               divided by the element size. That size is a non-zero constant,
               which is why the guard below can be satisfied outright while
               still standing where the verifier requires one. */
            lower_type difference_type = make_integer_type(64u, 4u, 1u);
            lower_value left_bytes;
            lower_value right_bytes;
            ql_ir_value_id operands[2];
            ql_ir_value_id gap;
            ql_ir_value_id scale;
            if (subtract == 0) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
                    "two pointers cannot be added", error);
            }
            if (!type_same(left.type, right.type)) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
                    "pointer difference needs both sides to have the same "
                    "type", error);
            }
            status = emit_address_of_pointer(context, left, &left_bytes,
                                             error);
            if (status == QL_STATUS_OK) {
                status = emit_address_of_pointer(context, right,
                                                 &right_bytes, error);
            }
            if (status != QL_STATUS_OK) {
                return status;
            }
            operands[0] = left_bytes.value;
            operands[1] = right_bytes.value;
            status = emit_instruction(context, QL_IR_OPCODE_SUB,
                                      &difference_type, operands, 2u, NULL,
                                      0u, QL_IR_EFFECT_NONE, &gap, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            status = add_uint_constant(context, difference_type,
                                       pointee_byte_width(context,
                                                          left.type),
                                       &scale, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            memset(output, 0, sizeof(*output));
            output->type = difference_type;
            status = combine_defined(context, &left, &right,
                                     &output->defined, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            if (output->defined == QL_IR_INVALID_VALUE_ID) {
                status = ensure_bool_constants(context, error);
                if (status != QL_STATUS_OK) {
                    return status;
                }
                output->defined = context->true_value;
            }
            output->may_ub = 1u;
            operands[0] = gap;
            operands[1] = scale;
            return emit_instruction(context, QL_IR_OPCODE_SDIV,
                                    &difference_type, operands, 2u, NULL, 0u,
                                    QL_IR_EFFECT_NONE, &output->value, error);
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

/* Applies one C binary operator to two operands that are already lowered.
   Compound assignment and the increment operators are defined in terms of the
   same operators, so they call this rather than restating the conversion and
   definedness rules a second time. `operator_text` is borrowed. */
static ql_status apply_binary_operator(lower_context *context,
                                       const char *operator_text, size_t node,
                                       lower_value left, lower_value right,
                                       lower_value *output, ql_error *error) {
    lower_value converted_left;
    lower_value converted_right;
    lower_type common;
    ql_ir_value_id inherited_defined;
    uint32_t inherited_may_ub;
    ql_status status;

    if (strcmp(operator_text, "&&") == 0 ||
        strcmp(operator_text, "||") == 0) {
        return lower_logical_expression(context, operator_text, left, right,
                                        output, error);
    }
    if (strcmp(operator_text, "<<") == 0 ||
        strcmp(operator_text, ">>") == 0) {
        return emit_shift_result(context, strcmp(operator_text, "<<") == 0,
                                 left, right, output, error);
    }
    if (left.type.kind == QL_C_SCALAR_POINTER ||
        right.type.kind == QL_C_SCALAR_POINTER) {
        return lower_pointer_binary(context, operator_text, node, left, right,
                                    output, error);
    }
    status = usual_arithmetic_conversions(
        context, left, right, &converted_left, &converted_right, &common,
        error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = combine_defined(context, &converted_left, &converted_right,
                             &inherited_defined, error);
    if (status != QL_STATUS_OK) {
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
    return status;
}

static ql_status lower_binary_expression(lower_context *context, size_t node,
                                         lower_value *output,
                                         ql_error *error) {
    size_t left_node = direct_field_child(context, node, "left");
    size_t right_node = direct_field_child(context, node, "right");
    size_t operator_node = direct_field_child(context, node, "operator");
    lower_value left;
    lower_value right;
    char *operator_text;
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
    status = apply_binary_operator(context, operator_text, node, left, right,
                                   output, error);
    context->allocator->deallocate(context->allocator->user_data,
                                   operator_text);
    return status;
}

/* A cast names its target with a type descriptor: a base type plus an
   optional abstract declarator. An abstract declarator here always means
   indirection, which the scalar slice cannot represent, so it is reported as
   a pointer obstacle rather than a type one. */
static ql_status lower_named_cast(lower_context *context, size_t type_node,
                                  size_t value_node, uint32_t pointer_depth,
                                  lower_value *output, ql_error *error);

/* Counts the stars an abstract declarator spells. Fails when it spells
   something else, which is an array or a function type. */
static int abstract_pointer_depth(const lower_context *context,
                                  size_t declarator, uint32_t *depth) {
    size_t guard = 0u;

    *depth = 0u;
    while (declarator != SIZE_MAX && guard++ < 64u) {
        const char *kind = context->nodes[declarator].view.kind;
        if (strcmp(kind, "abstract_pointer_declarator") == 0) {
            ++(*depth);
        } else if (strcmp(kind, "abstract_parenthesized_declarator") != 0) {
            return 0;
        }
        declarator = direct_field_child(context, declarator, "declarator");
    }
    return declarator == SIZE_MAX;
}

static const lower_member *find_member(const lower_record *record,
                                       const char *name);

/* Determines the declared type of an unevaluated designator. This is kept
   separate from lower_expression because sizeof must not emit a load, call,
   store, UB guard, or any other effect from its operand. */
static ql_status query_designator_type(lower_context *context, size_t node,
                                       lower_type *output,
                                       ql_error *error) {
    const char *kind = context->nodes[node].view.kind;

    if (strcmp(kind, "parenthesized_expression") == 0) {
        size_t inner = first_named_child(context, node);
        if (inner == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "an unevaluated parenthesized expression is empty", error);
        }
        return query_designator_type(context, inner, output, error);
    }
    if (strcmp(kind, "identifier") == 0) {
        char *name = copy_node_text(context, node);
        lower_variable *variable;
        if (name == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        variable = find_variable(context, name, strlen(name));
        if (variable != NULL) {
            *output = variable->type;
            context->allocator->deallocate(context->allocator->user_data,
                                           name);
            return QL_STATUS_OK;
        }
        if (find_enumerator(context, name) != NULL) {
            *output = make_integer_type(32u, 3u, 1u);
            context->allocator->deallocate(context->allocator->user_data,
                                           name);
            return QL_STATUS_OK;
        }
        context->allocator->deallocate(context->allocator->user_data, name);
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNDECLARED_IDENTIFIER, node,
            "sizeof names no visible object or enumerator", error);
    }
    if (strcmp(kind, "char_literal") == 0) {
        *output = make_integer_type(32u, 3u, 1u);
        return QL_STATUS_OK;
    }
    if (strcmp(kind, "string_literal") == 0) {
        lower_string *literal = find_string(context, node);
        if (literal == NULL) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "the string literal encoding is outside this sizeof query",
                error);
        }
        *output = make_array_of(make_integer_type(8u, 1u, 1u),
                                (uint64_t)literal->size);
        return QL_STATUS_OK;
    }
    if (strcmp(kind, "pointer_expression") == 0) {
        size_t operator_node = direct_field_child(context, node, "operator");
        size_t argument_node = direct_field_child(context, node, "argument");
        char *operator_text;
        lower_type argument;
        ql_status status;
        if (operator_node == SIZE_MAX || argument_node == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "an unevaluated pointer expression is incomplete", error);
        }
        operator_text = copy_node_text(context, operator_node);
        if (operator_text == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        status = query_designator_type(context, argument_node, &argument,
                                       error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            context->allocator->deallocate(context->allocator->user_data,
                                           operator_text);
            return status;
        }
        if (strcmp(operator_text, "*") == 0) {
            context->allocator->deallocate(context->allocator->user_data,
                                           operator_text);
            if (argument.kind != QL_C_SCALAR_POINTER) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
                    "sizeof dereference requires a pointer operand", error);
            }
            *output = pointer_target(argument);
            return QL_STATUS_OK;
        }
        if (strcmp(operator_text, "&") == 0) {
            context->allocator->deallocate(context->allocator->user_data,
                                           operator_text);
            *output = make_pointer_to(argument);
            return QL_STATUS_OK;
        }
        context->allocator->deallocate(context->allocator->user_data,
                                       operator_text);
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "the unevaluated pointer operator is outside this type query",
            error);
    }
    if (strcmp(kind, "subscript_expression") == 0) {
        size_t base_node = direct_field_child(context, node, "argument");
        lower_type base;
        ql_status status;
        if (base_node == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "an unevaluated subscript has no base", error);
        }
        status = query_designator_type(context, base_node, &base, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        if (base.array_length != 0u) {
            *output = array_element(base);
            return QL_STATUS_OK;
        }
        if (base.kind == QL_C_SCALAR_POINTER) {
            *output = pointer_target(base);
            return QL_STATUS_OK;
        }
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
            "sizeof subscript requires an array or pointer base", error);
    }
    if (strcmp(kind, "field_expression") == 0) {
        size_t argument_node = direct_field_child(context, node, "argument");
        size_t field_node = direct_field_child(context, node, "field");
        size_t operator_node = direct_field_child(context, node, "operator");
        lower_type base;
        const lower_member *member;
        char *operator_text;
        char *field_name;
        size_t record;
        int through_pointer;
        ql_status status;

        if (argument_node == SIZE_MAX || field_node == SIZE_MAX ||
            operator_node == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "an unevaluated member expression is incomplete", error);
        }
        status = query_designator_type(context, argument_node, &base, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        operator_text = copy_node_text(context, operator_node);
        if (operator_text == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        through_pointer = strcmp(operator_text, "->") == 0;
        context->allocator->deallocate(context->allocator->user_data,
                                       operator_text);
        if (through_pointer) {
            if (base.kind != QL_C_SCALAR_POINTER ||
                base.indirection != 1u ||
                base.pointee.kind != QL_C_SCALAR_RECORD) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
                    "sizeof -> requires a pointer to a record", error);
            }
            record = base.record;
        } else {
            if (base.kind != QL_C_SCALAR_RECORD) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
                    "sizeof . requires a record object", error);
            }
            record = base.record;
        }
        status = ensure_record_layout(context, record, node, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        field_name = copy_node_text(context, field_node);
        if (field_name == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        member = find_member(&context->records[record], field_name);
        context->allocator->deallocate(context->allocator->user_data,
                                       field_name);
        if (member == NULL) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNDECLARED_IDENTIFIER,
                field_node, "the record has no such member", error);
        }
        *output = member->type;
        return QL_STATUS_OK;
    }
    return lower_unknown(
        context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
        "sizeof(expression) cannot determine this operand type without "
        "evaluating it", error);
}

/* Resolves the type descriptor carried by `sizeof(type)`. The expression
   form deliberately does not come through here: asking for its type must not
   lower and therefore evaluate it. That needs a separate static type query. */
static ql_status lower_sizeof_type(lower_context *context, size_t node,
                                   lower_value *output, ql_error *error) {
    size_t descriptor = direct_field_child(context, node, "type");
    size_t value_node = direct_field_child(context, node, "value");
    size_t diagnostic_node = descriptor != SIZE_MAX ? descriptor : node;
    size_t type_node;
    size_t end;
    size_t child;
    uint32_t pointer_depth = 0u;
    lower_type measured;
    lower_type size_type = make_integer_type(64u, 4u, 0u);
    uint64_t size;
    ql_status status;

    if (descriptor == SIZE_MAX) {
        /* Tree-sitter cannot know that an identifier is a typedef. It parses
           `sizeof(T)` as an expression, just as it parses `(T)(x)` as a call.
           Recover only the unambiguous case this unit's symbol tables settle:
           one parenthesised identifier that names a typedef and is not
           shadowed by a visible object. */
        size_t identifier = SIZE_MAX;
        char *name = NULL;
        ql_c_scalar_type corpus_type;
        int names_type = 0;
        if (value_node != SIZE_MAX &&
            strcmp(context->nodes[value_node].view.kind,
                   "parenthesized_expression") == 0) {
            identifier = first_named_child(context, value_node);
        }
        if (identifier != SIZE_MAX &&
            strcmp(context->nodes[identifier].view.kind, "identifier") == 0) {
            name = copy_node_text(context, identifier);
            diagnostic_node = identifier;
        }
        if (name != NULL &&
            find_variable(context, name, strlen(name)) == NULL) {
            names_type = find_typedef(context, name) != NULL ||
                         ql_c_scalar_from_corpus_typedef(name,
                                                        &corpus_type);
        }
        if (names_type != 0) {
            status = parse_type_spelling(context, name, identifier, 0u,
                                         &measured, error);
            context->allocator->deallocate(context->allocator->user_data,
                                           name);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
        } else if (value_node != SIZE_MAX) {
            context->allocator->deallocate(context->allocator->user_data,
                                           name);
            status = query_designator_type(context, value_node, &measured,
                                           error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
        } else {
            context->allocator->deallocate(context->allocator->user_data,
                                           name);
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "sizeof has no type or expression operand", error);
        }
    } else {
        type_node = direct_field_child(context, descriptor, "type");
        if (type_node == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, descriptor,
                "sizeof type descriptor names no type", error);
        }
        end = subtree_end(context, descriptor);
        for (child = descriptor + 1u; child < end; ++child) {
            if (context->nodes[child].parent != descriptor ||
                context->nodes[child].view.field_name == NULL ||
                strcmp(context->nodes[child].view.field_name,
                       "declarator") != 0) {
                continue;
            }
            if (!abstract_pointer_depth(context, child, &pointer_depth)) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, child,
                    "sizeof of an array or function type needs a complete "
                    "type-descriptor query", error);
            }
            if (pointer_depth > 2u) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, child,
                    "this slice carries at most two levels of indirection",
                    error);
            }
            break;
        }
        if (strcmp(context->nodes[type_node].view.kind,
                   "atomic_type_specifier") == 0) {
            return lower_unknown(
                context,
                QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_VOLATILE_OR_ATOMIC,
                type_node,
                "sizeof of an atomic type is outside the current type model",
                error);
        }
        status = resolve_type_node(context, type_node, pointer_depth,
                                   &measured, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
    }
    size = type_byte_width(context, measured);
    if (size == 0u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, diagnostic_node,
            "sizeof requires a complete object type", error);
    }
    status = ensure_bool_constants(context, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(output, 0, sizeof(*output));
    output->type = size_type;
    output->defined = context->true_value;
    output->may_ub = 0u;
    return add_uint_constant(context, size_type, size, &output->value, error);
}

static ql_status lower_cast_expression(lower_context *context, size_t node,
                                       lower_value *output,
                                       ql_error *error) {
    size_t descriptor = direct_field_child(context, node, "type");
    size_t value_node = direct_field_child(context, node, "value");
    size_t type_node;
    size_t end;
    size_t child;
    uint32_t pointer_depth = 0u;

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
        if (!abstract_pointer_depth(context, child, &pointer_depth)) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, child,
                "a cast to an array or function type has no value in this "
                "slice", error);
        }
        if (pointer_depth > 2u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, child,
                "this slice carries at most two levels of indirection",
                error);
        }
        break;
    }
    if (strcmp(context->nodes[type_node].view.kind,
               "atomic_type_specifier") == 0) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_VOLATILE_OR_ATOMIC,
            type_node, "cast to an atomic type requires observable-event "
            "semantics", error);
    }
    return lower_named_cast(context, type_node, value_node, pointer_depth,
                            output, error);
}

/* Converts `value_node` to the scalar type `type_node` spells. */
static ql_status lower_named_cast(lower_context *context, size_t type_node,
                                  size_t value_node, uint32_t pointer_depth,
                                  lower_value *output, ql_error *error) {
    lower_value value;
    lower_type target;
    ql_status status;

    if (pointer_depth == 0u) {
        /* A scalar target, where `void` still has to reach the report below
           rather than being refused as a type that cannot stand alone. */
        char *spelling = copy_node_text(context, type_node);
        if (spelling == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        status = parse_type_spelling(context, spelling, type_node, 1u,
                                     &target, error);
        context->allocator->deallocate(context->allocator->user_data,
                                       spelling);
    } else {
        /* A pointer target. resolve_type_node already knows how to name a
           struct or a union and how to put stars on a base type, which is
           exactly what this is. The cast changes nothing about the value
           under this profile: a pointer is its address, so the conversion is
           a reinterpretation and the object that address came from travels
           with it. Whether that object is one this slice declared is what
           decides a later dereference, and nothing about the cast changes
           the answer. */
        status = resolve_type_node(context, type_node, pointer_depth, &target,
                                   error);
    }
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    if (target.kind == QL_C_SCALAR_VOID) {
        /* The operand is still evaluated for effects and undefined
           behaviour. Only its value disappears. Keeping its definedness on
           the result lets an expression statement or comma-left observation
           emit the same guard it would without the cast. */
        status = lower_expression(context, value_node, &value, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        *output = value;
        output->type = target;
        output->value = QL_IR_INVALID_VALUE_ID;
        output->has_object = 0u;
        return QL_STATUS_OK;
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
static const lower_member *find_member(const lower_record *record,
                                       const char *name) {
    size_t index;
    for (index = 0u; index < record->member_count; ++index) {
        if (strcmp(record->members[index].name, name) == 0) {
            return &record->members[index];
        }
    }
    return NULL;
}

/* The address of `base + offset`, typed so that a load there yields the
   member. A pointer member is loaded as a plain address and reinterpreted by
   the caller, which keeps one indirection level in the type and still lets
   `p->next->f` work. */
static lower_type member_load_type(lower_type member) {
    if (member.array_length != 0u) {
        /* An array member is addressed at its element type, which is what
           makes h.slots[i] the ordinary pointer arithmetic it already is. */
        return array_element(member);
    }
    if (member.kind == QL_C_SCALAR_POINTER) {
        return make_integer_type(QL_C_POINTER_WIDTH, 4u, 0u);
    }
    return member;
}

static lower_type address_type(void);

static ql_status lower_designator_address(lower_context *context, size_t node,
                                          lower_value *output,
                                          lower_type *declared,
                                          ql_error *error);

static ql_status lower_member_address(lower_context *context, size_t node,
                                      lower_value *output,
                                      lower_type *declared,
                                      ql_error *error) {
    size_t argument_node = direct_field_child(context, node, "argument");
    size_t field_node = direct_field_child(context, node, "field");
    size_t operator_node = direct_field_child(context, node, "operator");
    lower_value base;
    lower_type base_declared;
    lower_value address;
    lower_type u64 = address_type();
    const lower_member *member;
    char *operator_text;
    char *field_name;
    ql_ir_value_id offset_constant;
    ql_ir_value_id operands[2];
    size_t record;
    int through_pointer;
    ql_status status;

    if (argument_node == SIZE_MAX || field_node == SIZE_MAX ||
        operator_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "member access is missing its object, operator, or field",
            error);
    }
    operator_text = copy_node_text(context, operator_node);
    if (operator_text == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    through_pointer = strcmp(operator_text, "->") == 0;
    context->allocator->deallocate(context->allocator->user_data,
                                   operator_text);

    if (through_pointer) {
        status = lower_expression(context, argument_node, &base, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        if (base.type.kind != QL_C_SCALAR_POINTER ||
            base.type.indirection != 1u ||
            base.type.pointee.kind != QL_C_SCALAR_RECORD) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
                "the left of -> must be a pointer to a struct or union",
                error);
        }
        record = base.type.record;
    } else {
        /* `x.f` needs x's address, so x has to be something this slice can
           designate: a dereference, a subscript, or another member. */
        status = lower_designator_address(context, argument_node, &base,
                                          &base_declared, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        if (base_declared.kind != QL_C_SCALAR_RECORD) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
                "the left of . must be a struct or union object", error);
        }
        record = base_declared.record;
    }
    status = ensure_record_layout(context, record, node, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    field_name = copy_node_text(context, field_node);
    if (field_name == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    member = find_member(&context->records[record], field_name);
    context->allocator->deallocate(context->allocator->user_data, field_name);
    if (member == NULL) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNDECLARED_IDENTIFIER, field_node,
            "the record has no such member", error);
    }
    *declared = member->type;

    status = emit_address_of_pointer(context, base, &address, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = add_uint_constant(context, u64, member->offset,
                               &offset_constant, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    operands[0] = address.value;
    operands[1] = offset_constant;
    status = emit_instruction(context, QL_IR_OPCODE_ADD, &u64, operands, 2u,
                              NULL, 0u, QL_IR_EFFECT_NONE, &address.value,
                              error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    {
        lower_type loaded = member_load_type(member->type);
        lower_type pointer = make_pointer_to(loaded);
        address.defined = base.defined;
        address.may_ub = base.may_ub;
        address.has_object = base.has_object;
        return emit_pointer_of_address(context, address, pointer, output,
                                       error);
    }
}

static ql_status lower_designator_address(lower_context *context, size_t node,
                                          lower_value *output,
                                          lower_type *declared,
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
        *declared = pointer_target(output->type);
        return QL_STATUS_OK;
    }
    if (strcmp(kind, "field_expression") == 0) {
        return lower_member_address(context, node, output, declared, error);
    }
    if (strcmp(kind, "parenthesized_expression") == 0) {
        size_t inner = first_named_child(context, node);
        if (inner == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "parenthesised designator is empty", error);
        }
        return lower_designator_address(context, inner, output, declared,
                                        error);
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
        status = emit_pointer_offset(context, base, index, 0, output,
                                     error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        *declared = pointer_target(base.type);
        return QL_STATUS_OK;
    }
    if (strcmp(kind, "identifier") == 0) {
        char *name = copy_node_text(context, node);
        lower_variable *variable;
        if (name == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        variable = find_variable(context, name, strlen(name));
        context->allocator->deallocate(context->allocator->user_data, name);
        if (variable == NULL) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNDECLARED_IDENTIFIER, node,
                "identifier does not name a visible local or parameter",
                error);
        }
        if (variable->is_stack == 0u) {
            /* The pre-pass decides which locals get storage, so reaching
               here means this name was never seen with an address taken. */
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
                "this local lives in a value, not in storage", error);
        }
        if (variable->initialized == 0u) {
            /* A plain assignment to the local reaches its stack address
               through resolve_assignment_target instead of this path. Any
               other address use can escape to a read before the lowering has
               put a value in the slot, which this slice does not model. */
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNINITIALIZED_READ, node,
                "the address of an uninitialized local may escape before a "
                "value is stored", error);
        }
        *output = stack_address(context, variable);
        *declared = variable->type;
        return QL_STATUS_OK;
    }
    return lower_unknown(
        context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
        "expression does not designate an object", error);
}

/* Reads the object an address names, at its declared type. Split out of the
   designator path because a compound assignment reaches the same read from an
   address it has already computed and must not compute twice. */
static ql_status load_at_address(lower_context *context, size_t node,
                                 lower_value address, lower_type declared,
                                 lower_value *output, ql_error *error) {
    lower_value loaded;
    ql_status status;

    if (declared.array_length != 0u) {
        /* Naming an array yields a pointer to its first element. There is no
           value of array type to load, here or anywhere. */
        *output = address;
        return QL_STATUS_OK;
    }
    if (declared.kind == QL_C_SCALAR_RECORD) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
            "a struct or union value is outside this slice; only its members "
            "are", error);
    }
    status = emit_load(context, address, &loaded, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    if (declared.kind != QL_C_SCALAR_POINTER ||
        loaded.type.kind == QL_C_SCALAR_POINTER) {
        /* A member of a record is addressed at its byte offset, so loading a
           pointer member yields an address that still has to be read as a
           pointer. Loading through a pointer-to-pointer does not: the IR type
           of that load is already a pointer, and wrapping it again would be
           converting a pointer as though it were an address, which the IR
           rejects. Either way the value carries no object, which is what
           makes a later dereference of it refuse rather than pretend. */
        *output = loaded;
        return QL_STATUS_OK;
    }
    return emit_pointer_of_address(context, loaded, declared, output, error);
}

/* Reads whatever a designator names. A record-valued designator has no value
   this slice can produce, and a pointer member arrives as a plain address
   that has to be reinterpreted at its declared type. */
static ql_status lower_designator_load(lower_context *context, size_t node,
                                       lower_value *output,
                                       ql_error *error) {
    lower_value address;
    lower_type declared;
    ql_status status = lower_designator_address(context, node, &address,
                                                &declared, error);

    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    return load_at_address(context, node, address, declared, output, error);
}

/* Resolves a prototype's return and parameter types. Done at the first call
   rather than at collection so that a prototype naming something this slice
   cannot carry costs only the bodies that call it. */
static ql_status resolve_callee(lower_context *context, lower_callee *callee,
                                size_t node, ql_error *error) {
    size_t parameters_node;
    size_t type_node;
    size_t end;
    size_t child;
    ql_status status;

    if (callee->resolved != 0u) {
        return QL_STATUS_OK;
    }
    type_node = direct_field_child(context, callee->declaration_node, "type");
    if (type_node == SIZE_MAX) {
        return lower_unknown(context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL,
                             node, "the callee has no declared return type",
                             error);
    }
    /* Stars between the return type and the function name belong to the
       return type, not to the function; `collect_callees` counted them off
       the declarator chain on the way in. */
    status = resolve_type_node(context, type_node,
                               callee->return_pointer_depth,
                               &callee->return_type, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    if (callee->return_type.kind == QL_C_SCALAR_RECORD) {
        return lower_unknown(context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL,
                             node,
                             "a callee returning a record by value is outside "
                             "this slice", error);
    }
    parameters_node = direct_field_child(context, callee->declarator_node,
                                         "parameters");
    if (parameters_node == SIZE_MAX) {
        return lower_unknown(context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL,
                             node, "the callee has no parameter list", error);
    }
    end = subtree_end(context, parameters_node);
    for (child = parameters_node + 1u; child < end; ++child) {
        lower_type parameter;
        size_t parameter_type;
        size_t declarator_end;
        size_t inner;
        uint32_t depth = 0u;

        if (context->nodes[child].parent != parameters_node) {
            continue;
        }
        if (strcmp(context->nodes[child].view.kind,
                   "variadic_parameter") == 0) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL, node,
                "a variadic callee has no fixed signature to check against",
                error);
        }
        if (strcmp(context->nodes[child].view.kind,
                   "parameter_declaration") != 0) {
            continue;
        }
        parameter_type = direct_field_child(context, child, "type");
        if (parameter_type == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL, node,
                "a callee parameter has no declared type", error);
        }
        /* A prototype's parameters are often unnamed, so the stars are
           counted off the abstract declarator instead of a name. */
        declarator_end = subtree_end(context, child);
        for (inner = child + 1u; inner < declarator_end; ++inner) {
            const char *kind = context->nodes[inner].view.kind;
            if (context->nodes[inner].parent != child) {
                continue;
            }
            if (strcmp(kind, "abstract_pointer_declarator") == 0 ||
                strcmp(kind, "pointer_declarator") == 0) {
                size_t cursor = inner;
                while (cursor != SIZE_MAX) {
                    const char *inner_kind =
                        context->nodes[cursor].view.kind;
                    if (strcmp(inner_kind, "abstract_pointer_declarator") !=
                            0 &&
                        strcmp(inner_kind, "pointer_declarator") != 0) {
                        break;
                    }
                    ++depth;
                    cursor = direct_field_child(context, cursor,
                                                "declarator");
                }
            }
        }
        if (depth == 0u) {
            /* `f(void)` declares no parameters at all, so this has to be
               recognised before the type is resolved: at parameter position
               `void` is not an object type, and asking for one reports a type
               error on a prototype that is perfectly ordinary C. */
            char *spelling = copy_node_text(context, parameter_type);
            int is_void;
            if (spelling == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            is_void = strcmp(spelling, "void") == 0;
            context->allocator->deallocate(context->allocator->user_data,
                                           spelling);
            if (is_void) {
                continue;
            }
        }
        status = resolve_type_node(context, parameter_type, depth, &parameter,
                                   error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        if (parameter.kind == QL_C_SCALAR_VOID) {
            /* Defensive: the spelling check above already took this path. */
            continue;
        }
        if (parameter.kind == QL_C_SCALAR_RECORD) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL, node,
                "a callee taking a record by value is outside this slice",
                error);
        }
        status = grow_array(context->allocator,
                            (void **)&callee->parameters,
                            &callee->parameter_capacity,
                            sizeof(lower_type),
                            callee->parameter_count + 1u, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        callee->parameters[callee->parameter_count++] = parameter;
    }
    callee->resolved = 1u;
    return QL_STATUS_OK;
}

/* An external call is uninterpreted: it may read and write memory and it is
   itself observable, so it consumes and produces both the memory state and
   the event trace. Whether the trace is compared is the contract's business,
   exactly as it already is for the memory a return carries. */
static ql_status lower_call_expression(lower_context *context, size_t node,
                                       lower_value *output,
                                       ql_error *error) {
    size_t function_node = direct_field_child(context, node, "function");
    size_t arguments_node = direct_field_child(context, node, "arguments");
    lower_callee *callee;
    char *name;
    ql_ir_value_id operands[32];
    ql_ir_type_id result_types[3];
    ql_ir_value_id results[3];
    ql_ir_instruction_definition_v1 definition;
    ql_ir_instruction_id instruction;
    size_t operand_count = 0u;
    size_t result_count = 0u;
    size_t argument_index = 0u;
    size_t end;
    size_t child;
    ql_status status;

    if (function_node == SIZE_MAX || arguments_node == SIZE_MAX ||
        strcmp(context->nodes[function_node].view.kind, "identifier") != 0) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL, node,
            "only a call of a declared function by name is in this slice",
            error);
    }
    name = copy_node_text(context, function_node);
    if (name == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    callee = find_callee(context, name);
    context->allocator->deallocate(context->allocator->user_data, name);
    if (callee == NULL) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL, node,
            "the callee has no declaration in this unit", error);
    }
    status = resolve_callee(context, callee, node, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }

    /* The observable states this call is handed are the ones that stand
       after its arguments have been evaluated, not before. An argument may
       itself contain a call, and reading the states first would hand the
       outer call a history and a memory from before the inner one ran --
       which is to say it would lose the inner call's effects. The two
       operands are filled in below, once the arguments are in. */
    operand_count = 2u;
    end = subtree_end(context, arguments_node);
    for (child = arguments_node + 1u; child < end; ++child) {
        lower_value argument;
        lower_value converted;

        if (context->nodes[child].parent != arguments_node ||
            (context->nodes[child].view.flags & QL_C_SYNTAX_NODE_NAMED) ==
                0u ||
            strcmp(context->nodes[child].view.kind, "comment") == 0) {
            continue;
        }
        if (argument_index >= callee->parameter_count) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL, node,
                "the call passes more arguments than the callee declares",
                error);
        }
        if (operand_count >= sizeof(operands) / sizeof(operands[0])) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL, node,
                "the call has more arguments than this slice carries", error);
        }
        status = lower_expression(context, child, &argument, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        status = convert_value(context, argument,
                               callee->parameters[argument_index], &converted,
                               error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        status = emit_ub_guard(context, &converted, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        operands[operand_count++] = converted.value;
        ++argument_index;
    }
    if (argument_index != callee->parameter_count) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL, node,
            "the call passes fewer arguments than the callee declares",
            error);
    }
    operands[0] = context->trace_value;
    operands[1] = context->memory_value;

    result_types[result_count++] = context->trace_type;
    result_types[result_count++] = context->memory_type;
    if (callee->return_type.kind != QL_C_SCALAR_VOID) {
        status = ensure_ir_type(context, &callee->return_type, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        result_types[result_count++] = callee->return_type.ir_type;
    }
    ql_ir_instruction_definition_init(&definition, QL_IR_OPCODE_CALL);
    definition.operands = operands;
    definition.operand_count = operand_count;
    definition.result_types = result_types;
    definition.result_count = result_count;
    definition.symbol = callee->name;
    definition.symbol_size = strlen(callee->name);
    definition.effects = QL_IR_EFFECT_CALL | QL_IR_EFFECT_MEMORY |
                         QL_IR_EFFECT_IO;
    status = ql_ir_builder_append_instruction(context->builder,
                                              context->current_block,
                                              &definition, &instruction,
                                              results, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    context->trace_value = results[0];
    context->memory_value = results[1];
    memset(output, 0, sizeof(*output));
    status = ensure_bool_constants(context, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    output->defined = context->true_value;
    output->may_ub = 0u;
    if (callee->return_type.kind == QL_C_SCALAR_VOID) {
        output->type = callee->return_type;
        output->value = QL_IR_INVALID_VALUE_ID;
        return QL_STATUS_OK;
    }
    output->type = callee->return_type;
    output->value = results[2];
    return QL_STATUS_OK;
}

/* Defined below, next to the assignment they share a target model with. */
static ql_status lower_assignment_value(lower_context *context, size_t node,
                                        lower_value *output, ql_error *error);
static ql_status lower_update_expression(lower_context *context, size_t node,
                                         lower_value *output,
                                         ql_error *error);

/* `c ? a : b`. The value is selected, but the definedness short-circuits the
   way `&&` already does: only the arm the condition chooses has to be defined.
   Requiring both would invent undefined behaviour C does not have, exactly as
   it would for the right operand of `&&`. */
static ql_status lower_conditional_expression(lower_context *context,
                                              size_t node,
                                              lower_value *output,
                                              ql_error *error) {
    size_t condition_node = direct_field_child(context, node, "condition");
    size_t consequence_node = direct_field_child(context, node, "consequence");
    size_t alternative_node = direct_field_child(context, node, "alternative");
    lower_value condition;
    lower_value condition_bool;
    lower_value consequence;
    lower_value alternative;
    lower_value left;
    lower_value right;
    lower_type common;
    ql_ir_value_id operands[3];
    ql_ir_value_id not_condition;
    ql_ir_value_id then_safe;
    ql_ir_value_id else_safe;
    ql_ir_value_id arms_safe;
    ql_ir_value_id condition_defined;
    ql_ir_value_id consequence_defined;
    ql_ir_value_id alternative_defined;
    ql_status status;

    if (condition_node == SIZE_MAX || consequence_node == SIZE_MAX ||
        alternative_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "conditional expression is missing one of its three operands",
            error);
    }
    status = lower_expression(context, condition_node, &condition, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = lower_expression(context, consequence_node, &consequence, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = lower_expression(context, alternative_node, &alternative, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = convert_value(context, condition, make_bool_type(),
                           &condition_bool, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    if (consequence.type.kind == QL_C_SCALAR_VOID ||
        alternative.type.kind == QL_C_SCALAR_VOID ||
        consequence.type.kind == QL_C_SCALAR_RECORD ||
        alternative.type.kind == QL_C_SCALAR_RECORD ||
        consequence.type.array_length != 0u ||
        alternative.type.array_length != 0u) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "a conditional whose arms are void, a record, or an array is "
            "outside this slice", error);
    }
    if (consequence.type.kind == QL_C_SCALAR_POINTER ||
        alternative.type.kind == QL_C_SCALAR_POINTER) {
        /* Two addresses select to an address. The result carries no object,
           because naming either arm's object for both would claim a
           provenance the expression does not have. */
        lower_type u64 = address_type();
        if (consequence.type.kind != alternative.type.kind) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
                "a conditional mixing a pointer and an integer is outside "
                "this slice", error);
        }
        status = convert_value(context, consequence, u64, &left, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        status = convert_value(context, alternative, u64, &right, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        common = u64;
    } else {
        status = usual_arithmetic_conversions(context, consequence,
                                              alternative, &left, &right,
                                              &common, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
    }
    status = ensure_bool_constants(context, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(output, 0, sizeof(*output));
    operands[0] = condition_bool.value;
    operands[1] = left.value;
    operands[2] = right.value;
    status = emit_instruction(context, QL_IR_OPCODE_SELECT, &common, operands,
                              3u, NULL, 0u, QL_IR_EFFECT_NONE, &output->value,
                              error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    condition_defined =
        condition.may_ub != 0u ? condition.defined : context->true_value;
    consequence_defined =
        consequence.may_ub != 0u ? consequence.defined : context->true_value;
    alternative_defined =
        alternative.may_ub != 0u ? alternative.defined : context->true_value;
    status = emit_bool_not(context, condition_bool.value, &not_condition,
                           error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_bool_or(context, not_condition, consequence_defined,
                          &then_safe, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_bool_or(context, condition_bool.value, alternative_defined,
                          &else_safe, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_bool_and(context, then_safe, else_safe, &arms_safe, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = emit_bool_and(context, condition_defined, arms_safe,
                           &output->defined, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    output->type = common;
    output->may_ub = 1u;
    if (common.kind == QL_C_SCALAR_POINTER) {
        return QL_STATUS_OK;
    }
    return QL_STATUS_OK;
}

/* `a, b`. The left operand is evaluated for its effects and discarded, and
   the expression's value is the right one's. */
static ql_status lower_comma_expression(lower_context *context, size_t node,
                                        lower_value *output,
                                        ql_error *error) {
    size_t left_node = direct_field_child(context, node, "left");
    size_t right_node = direct_field_child(context, node, "right");
    lower_value discarded;
    ql_status status;

    if (left_node == SIZE_MAX || right_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "comma expression is missing an operand", error);
    }
    if (strcmp(context->nodes[left_node].view.kind,
               "assignment_expression") == 0) {
        status = lower_assignment_value(context, left_node, NULL, error);
    } else {
        status = lower_expression(context, left_node, &discarded, error);
        if (status == QL_STATUS_OK && context->unknown == 0u) {
            /* The discarded value is still observed: C sequences it before
               the right operand, so undefined behaviour in it has happened
               whether or not anybody reads the result. */
            status = emit_ub_guard(context, &discarded, error);
        }
    }
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    return lower_expression(context, right_node, output, error);
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
            return lower_named_cast(context, type_node, operand, 0u, output,
                                    error);
        }
        return lower_call_expression(context, node, output, error);
    }
    if (strcmp(kind, "pointer_expression") == 0) {
        size_t operator_node = direct_field_child(context, node, "operator");
        char *operator_text = operator_node == SIZE_MAX
                                  ? NULL
                                  : copy_node_text(context, operator_node);
        int is_address_of;
        if (operator_text == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        is_address_of = strcmp(operator_text, "&") == 0;
        context->allocator->deallocate(context->allocator->user_data,
                                       operator_text);
        if (is_address_of) {
            /* Anything this slice can designate already has an address, so
               taking it is just not loading. */
            size_t argument_node = direct_field_child(context, node,
                                                      "argument");
            lower_type declared;
            if (argument_node == SIZE_MAX) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION,
                    node, "address-of has no operand", error);
            }
            return lower_designator_address(context, argument_node, output,
                                            &declared, error);
        }
        return lower_designator_load(context, node, output, error);
    }
    if (strcmp(kind, "subscript_expression") == 0 ||
        strcmp(kind, "field_expression") == 0) {
        return lower_designator_load(context, node, output, error);
    }
    if (
        strcmp(kind, "field_expression") == 0) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, node,
            "pointer and aggregate expressions require memory semantics",
            error);
    }
    if (strcmp(kind, "string_literal") == 0) {
        lower_string *literal = find_string(context, node);
        ql_status status;
        if (literal == NULL || literal->address == QL_IR_INVALID_VALUE_ID) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "a string literal with an encoding prefix, an escape this "
                "pass cannot decode, or more bytes than this slice writes",
                error);
        }
        memset(output, 0, sizeof(*output));
        output->value = literal->address;
        output->type = make_pointer_to(make_integer_type(8u, 1u, 1u));
        output->has_object = 1u;
        status = ensure_bool_constants(context, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        output->defined = context->true_value;
        return ensure_ir_type(context, &output->type, error);
    }
    if (strcmp(kind, "char_literal") == 0) {
        unsigned char decoded[LOWER_MAX_STRING_BYTES + 1u];
        size_t decoded_size = 0u;
        char *text = copy_node_text(context, node);
        ql_status status;
        int ok;
        if (text == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        /* A character constant has type int in C, and its value is the
           character's. The same decoder reads it, with the quotes swapped. */
        {
            size_t length = strlen(text);
            size_t at;
            for (at = 0u; at < length; ++at) {
                if (text[at] == '\'') {
                    text[at] = '"';
                }
            }
            ok = decode_string_literal(text, length, decoded, &decoded_size);
        }
        context->allocator->deallocate(context->allocator->user_data, text);
        if (!ok || decoded_size != 2u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "a multi-character or wide character constant is outside "
                "this slice", error);
        }
        memset(output, 0, sizeof(*output));
        output->type = make_integer_type(32u, 3u, 1u);
        status = ensure_bool_constants(context, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        output->defined = context->true_value;
        return add_uint_constant(context, output->type,
                                 (uint64_t)decoded[0], &output->value,
                                 error);
    }
    if (strcmp(kind, "conditional_expression") == 0) {
        return lower_conditional_expression(context, node, output, error);
    }
    if (strcmp(kind, "comma_expression") == 0) {
        return lower_comma_expression(context, node, output, error);
    }
    if (strcmp(kind, "assignment_expression") == 0) {
        return lower_assignment_value(context, node, output, error);
    }
    if (strcmp(kind, "update_expression") == 0) {
        return lower_update_expression(context, node, output, error);
    }
    if (strcmp(kind, "sizeof_expression") == 0) {
        return lower_sizeof_type(context, node, output, error);
    }
    return lower_unknown(
        context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
        "expression is outside the loop-free integer lowering slice", error);
}

static ql_status lower_statement(lower_context *context, size_t node,
                                 ql_error *error);

/* Where an assignment, a compound assignment, or an increment writes. A
   register target is a local the pre-pass left in an SSA value; every other
   target is an address, computed once so that `a[i()] += 1` calls `i` once. */
typedef struct lower_target {
    lower_variable *variable; /* NULL when the target is an address */
    /* The local backing an address target, when it is a stack slot. This is
       separate from variable because the write still has to go through
       memory, but it also establishes definite initialization. */
    lower_variable *stack_variable;
    lower_value address;
    lower_type declared;
} lower_target;

static ql_status resolve_assignment_target(lower_context *context,
                                           size_t left_node,
                                           lower_target *target,
                                           ql_error *error) {
    const char *kind = context->nodes[left_node].view.kind;

    memset(target, 0, sizeof(*target));
    if (strcmp(kind, "identifier") == 0) {
        char *name = copy_node_text(context, left_node);
        lower_variable *variable;
        if (name == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        variable = find_variable(context, name, strlen(name));
        context->allocator->deallocate(context->allocator->user_data, name);
        if (variable == NULL) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNDECLARED_IDENTIFIER,
                left_node, "assignment target is not a visible local or "
                "parameter", error);
        }
        if (variable->is_const != 0u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, left_node,
                "assignment modifies a const-qualified local", error);
        }
        target->declared = variable->type;
        if (variable->is_stack == 0u) {
            target->variable = variable;
            return QL_STATUS_OK;
        }
        target->stack_variable = variable;
        target->address = stack_address(context, variable);
        return QL_STATUS_OK;
    }
    if (strcmp(kind, "pointer_expression") == 0 ||
        strcmp(kind, "subscript_expression") == 0 ||
        strcmp(kind, "field_expression") == 0 ||
        strcmp(kind, "parenthesized_expression") == 0) {
        return lower_designator_address(context, left_node, &target->address,
                                        &target->declared, error);
    }
    return lower_unknown(
        context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, left_node,
        "only assignment to a scalar local, parameter, or dereference is "
        "supported", error);
}

/* Writes `value` to a resolved target, converting it to the target's declared
   type first. `stored` receives what the assignment expression's value is. */
static ql_status write_assignment_target(lower_context *context, size_t node,
                                         const lower_target *target,
                                         lower_value value,
                                         lower_value *stored,
                                         ql_error *error) {
    lower_value converted;
    ql_status status;

    if (target->declared.kind == QL_C_SCALAR_RECORD) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
            "assigning a whole struct or union is outside this slice", error);
    }
    if (target->declared.kind == QL_C_SCALAR_VOID ||
        target->declared.array_length != 0u) {
        /* `*p = v` through a `void *`, and an array name, are not objects an
           assignment can write. Converting to the type first would ask the IR
           for a value of a type no value can have. */
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, node,
            "assignment target is not an object type this slice can write",
            error);
    }
    status = convert_value(context, value, target->declared, &converted,
                           error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    if (stored != NULL) {
        *stored = converted;
    }
    if (target->variable != NULL) {
        status = emit_ub_guard(context, &converted, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        target->variable->value = converted.value;
        target->variable->initialized = 1u;
        target->variable->has_object = converted.has_object;
        if (stored != NULL) {
            *stored = converted;
        }
        return QL_STATUS_OK;
    }
    if (target->declared.kind == QL_C_SCALAR_POINTER) {
        /* Storage holds an address, so a pointer value is written as one. */
        lower_value address;
        status = emit_address_of_pointer(context, converted, &address, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = emit_store(context, target->address, address, error);
    } else {
        status = emit_store(context, target->address, converted, error);
    }
    if (status == QL_STATUS_OK && context->unknown == 0u &&
        target->stack_variable != NULL) {
        target->stack_variable->initialized = 1u;
    }
    return status;
}

/* `x = v`, `x op= v`, `++x` and `x++` all read, combine, and write the same
   object, so they share one implementation rather than three that could drift.
   `operator_text` is NULL for a plain assignment and otherwise the binary
   operator the compound form stands for; C defines `x op= v` as `x = x op v`
   with `x` evaluated once, which is what resolving the target first buys.
   `output` receives the expression's value and may be NULL when it is
   discarded; `yields_old_value` selects the postfix reading. */
static ql_status lower_read_modify_write(lower_context *context, size_t node,
                                         size_t left_node,
                                         const char *operator_text,
                                         lower_value operand,
                                         int yields_old_value,
                                         lower_value *output,
                                         ql_error *error) {
    lower_target target;
    lower_value old;
    lower_value combined;
    ql_status status = resolve_assignment_target(context, left_node, &target,
                                                 error);

    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    if (operator_text == NULL) {
        return write_assignment_target(context, node, &target, operand,
                                       output, error);
    }
    if (target.variable != NULL) {
        if (target.variable->initialized == 0u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNINITIALIZED_READ, left_node,
                "a compound assignment reads its target before writing it",
                error);
        }
        memset(&old, 0, sizeof(old));
        old.value = target.variable->value;
        old.type = target.variable->type;
        old.has_object = target.variable->has_object;
        status = ensure_bool_constants(context, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        old.defined = context->true_value;
    } else {
        if (target.stack_variable != NULL &&
            target.stack_variable->initialized == 0u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNINITIALIZED_READ, left_node,
                "a compound assignment reads its target before writing it",
                error);
        }
        status = load_at_address(context, left_node, target.address,
                                 target.declared, &old, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
    }
    status = apply_binary_operator(context, operator_text, node, old, operand,
                                   &combined, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = write_assignment_target(context, node, &target, combined,
                                     yields_old_value ? NULL : output, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    if (yields_old_value != 0 && output != NULL) {
        *output = old;
    }
    return QL_STATUS_OK;
}

/* The compound operators, each paired with the binary operator it stands for.
   `>>=` and `<<=` are here too: the shift's own definedness rules apply
   unchanged, which is exactly what reusing the binary path gives. */
static const char *compound_binary_operator(const char *assignment) {
    static const struct {
        const char *assignment;
        const char *binary;
    } table[] = {
        {"+=", "+"},  {"-=", "-"},  {"*=", "*"},   {"/=", "/"},
        {"%=", "%"},  {"&=", "&"},  {"|=", "|"},   {"^=", "^"},
        {"<<=", "<<"}, {">>=", ">>"}
    };
    size_t index;
    for (index = 0u; index < sizeof(table) / sizeof(table[0]); ++index) {
        if (strcmp(assignment, table[index].assignment) == 0) {
            return table[index].binary;
        }
    }
    return NULL;
}

static ql_status lower_assignment_value(lower_context *context, size_t node,
                                        lower_value *output,
                                        ql_error *error) {
    size_t left_node = direct_field_child(context, node, "left");
    size_t right_node = direct_field_child(context, node, "right");
    size_t operator_node = direct_field_child(context, node, "operator");
    const char *binary = NULL;
    char *operator_text;
    lower_value value;
    ql_status status;

    if (left_node == SIZE_MAX || right_node == SIZE_MAX ||
        operator_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "assignment is missing a target, a value, or an operator", error);
    }
    operator_text = copy_node_text(context, operator_node);
    if (operator_text == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    if (strcmp(operator_text, "=") != 0) {
        binary = compound_binary_operator(operator_text);
        if (binary == NULL) {
            context->allocator->deallocate(context->allocator->user_data,
                                           operator_text);
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "assignment operator is outside this slice", error);
        }
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   operator_text);
    /* The right operand is evaluated before the target is written, so a
       partial operation inside it is already guarded when the store happens. */
    status = lower_expression(context, right_node, &value, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    return lower_read_modify_write(context, node, left_node, binary, value, 0,
                                   output, error);
}

static ql_status lower_assignment(lower_context *context, size_t node,
                                  ql_error *error) {
    return lower_assignment_value(context, node, NULL, error);
}

/* `++x` and `x--`. C defines these as adding or subtracting one, and on a
   pointer that is one element rather than one byte, which the binary path
   already knows. */
static ql_status lower_update_expression(lower_context *context, size_t node,
                                         lower_value *output,
                                         ql_error *error) {
    size_t argument_node = direct_field_child(context, node, "argument");
    size_t operator_node = direct_field_child(context, node, "operator");
    lower_value one;
    char *operator_text;
    const char *binary;
    int is_postfix;
    ql_status status;

    if (argument_node == SIZE_MAX || operator_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "increment is missing its operand or operator", error);
    }
    operator_text = copy_node_text(context, operator_node);
    if (operator_text == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    binary = strcmp(operator_text, "++") == 0
                 ? "+"
                 : (strcmp(operator_text, "--") == 0 ? "-" : NULL);
    context->allocator->deallocate(context->allocator->user_data,
                                   operator_text);
    if (binary == NULL) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
            "update operator is outside this slice", error);
    }
    /* Tree-sitter puts the operator before the argument for a prefix update
       and after it for a postfix one, and only the postfix form has the
       object's old value. */
    is_postfix = context->nodes[operator_node].view.range.start_byte >
                 context->nodes[argument_node].view.range.start_byte;
    memset(&one, 0, sizeof(one));
    one.type = make_integer_type(32u, 3u, 1u);
    status = ensure_bool_constants(context, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    one.defined = context->true_value;
    status = add_uint_constant(context, one.type, 1u, &one.value, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return lower_read_modify_write(context, node, argument_node, binary, one,
                                   is_postfix, output, error);
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
            {
                const int is_storage_duration =
                    context->parsing_global != 0u &&
                    (strcmp(text, "static") == 0 ||
                     strcmp(text, "extern") == 0);
                if (strcmp(text, "auto") != 0 &&
                    strcmp(text, "register") != 0 && !is_storage_duration) {
                    context->allocator->deallocate(
                        context->allocator->user_data, text);
                    return lower_unknown(
                        context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, index,
                        "static, extern, and thread-local objects require "
                        "memory semantics", error);
                }
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
    if (strcmp(context->nodes[type_node].view.kind,
               "struct_specifier") == 0 ||
        strcmp(context->nodes[type_node].view.kind,
               "union_specifier") == 0) {
        /* A record local is storage, and resolve_type_node already knows how
           to find the declared record and lay it out. Whether a declarator
           then adds a star is decided per declarator, as it is for every
           other base type here. */
        context->allocator->deallocate(context->allocator->user_data,
                                       spelling);
        return resolve_type_node(context, type_node, 0u, output, error);
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
        /* Void and record bases are admissible here because a declarator may
           still add a star. Whether they survive is decided per declarator. */
        ql_status status = parse_type_spelling(context, spelling, type_node,
                                               1u, output, error);
        context->allocator->deallocate(context->allocator->user_data,
                                       spelling);
        return status;
    }
}

/* Produces an address at a byte offset inside local aggregate storage. Arrays
   use a pointer to their element, because C array values never exist as IR
   values; records and scalars use a pointer to the declared type. */
static ql_status aggregate_child_address(lower_context *context,
                                         lower_value base, uint64_t offset,
                                         lower_type child,
                                         lower_value *output,
                                         ql_error *error) {
    lower_type u64 = address_type();
    lower_type pointer =
        make_pointer_to(child.array_length != 0u ? array_element(child)
                                                 : child);
    lower_value address;
    ql_ir_value_id amount;
    ql_ir_value_id operands[2];
    ql_status status = emit_address_of_pointer(context, base, &address,
                                               error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    status = add_uint_constant(context, u64, offset, &amount, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    operands[0] = address.value;
    operands[1] = amount;
    status = emit_instruction(context, QL_IR_OPCODE_ADD, &u64, operands, 2u,
                              NULL, 0u, QL_IR_EFFECT_NONE, &address.value,
                              error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return emit_pointer_of_address(context, address, pointer, output, error);
}

static ql_status zero_initialize_object(lower_context *context, size_t node,
                                        lower_value address, lower_type type,
                                        ql_error *error) {
    ql_status status;

    if (type.array_length != 0u) {
        lower_type element = array_element(type);
        uint64_t width = type_byte_width(context, element);
        uint64_t index;
        if (type.array_length > LOWER_MAX_INITIALIZER_ELEMENTS ||
            width == 0u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
                "zero-initializing this local array would exceed the "
                "bounded aggregate slice", error);
        }
        for (index = 0u; index < type.array_length; ++index) {
            lower_value element_address;
            status = aggregate_child_address(context, address, index * width,
                                             element, &element_address,
                                             error);
            if (status == QL_STATUS_OK) {
                status = zero_initialize_object(context, node,
                                                element_address, element,
                                                error);
            }
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
        }
        return QL_STATUS_OK;
    }
    if (type.kind == QL_C_SCALAR_RECORD) {
        lower_record *record;
        size_t count;
        size_t index;
        status = ensure_record_layout(context, type.record, node, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        record = &context->records[type.record];
        count = record->is_union != 0u && record->member_count != 0u
                    ? 1u
                    : record->member_count;
        for (index = 0u; index < count; ++index) {
            lower_value member_address;
            const lower_member *member = &record->members[index];
            status = aggregate_child_address(context, address, member->offset,
                                             member->type, &member_address,
                                             error);
            if (status == QL_STATUS_OK) {
                status = zero_initialize_object(context, node,
                                                member_address, member->type,
                                                error);
            }
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
        }
        return QL_STATUS_OK;
    }
    {
        lower_value zero;
        memset(&zero, 0, sizeof(zero));
        status = ensure_bool_constants(context, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        zero.type = type;
        zero.defined = context->true_value;
        if (type.kind == QL_C_SCALAR_POINTER) {
            lower_value raw;
            memset(&raw, 0, sizeof(raw));
            raw.type = address_type();
            raw.defined = context->true_value;
            status = add_uint_constant(context, raw.type, 0u, &raw.value,
                                       error);
            if (status == QL_STATUS_OK) {
                status = emit_pointer_of_address(context, raw, type, &zero,
                                                 error);
            }
        } else {
            status = add_uint_constant(context, type, 0u, &zero.value, error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        return emit_store(context, address, zero, error);
    }
}

static ql_status initialize_object(lower_context *context, size_t node,
                                   lower_value address, lower_type type,
                                   uint32_t brace_elided,
                                   ql_error *error) {
    const int is_list =
        strcmp(context->nodes[node].view.kind, "initializer_list") == 0;
    ql_status status;

    if (type.array_length == 0u && type.kind != QL_C_SCALAR_RECORD) {
        size_t value_node = node;
        lower_value value;
        if (is_list) {
            size_t end = subtree_end(context, node);
            size_t child;
            value_node = SIZE_MAX;
            for (child = node + 1u; child < end; ++child) {
                if (context->nodes[child].parent != node ||
                    (context->nodes[child].view.flags &
                     QL_C_SYNTAX_NODE_NAMED) == 0u ||
                    strcmp(context->nodes[child].view.kind, "comment") == 0) {
                    continue;
                }
                if (value_node != SIZE_MAX ||
                    strcmp(context->nodes[child].view.kind,
                           "initializer_pair") == 0) {
                    return lower_unknown(
                        context,
                        QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, child,
                        "a scalar initializer needs one positional value",
                        error);
                }
                value_node = child;
            }
            if (value_node == SIZE_MAX) {
                return zero_initialize_object(context, node, address, type,
                                              error);
            }
        }
        status = lower_expression(context, value_node, &value, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        return emit_store(context, address, value, error);
    }

    if (is_list) {
        uint64_t count;
        if (!positional_initializer_count(context, node, &count)) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION, node,
                "designated or oversized aggregate initialization needs a "
                "member or index map", error);
        }
    }
    status = zero_initialize_object(context, node, address, type, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    if (!is_list) {
        /* C brace elision lets `{0}` initialize the first nested subobject.
           Recursing once captures that case without pretending to implement
           the full flattened positional stream. */
        lower_value first_address;
        lower_type first_type;
        uint64_t first_offset = 0u;
        if (brace_elided == 0u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE, node,
                "copying an array or record value needs aggregate value "
                "semantics", error);
        }
        if (type.array_length != 0u) {
            first_type = array_element(type);
        } else {
            lower_record *record = &context->records[type.record];
            if (record->member_count == 0u) {
                return QL_STATUS_OK;
            }
            first_type = record->members[0].type;
            first_offset = record->members[0].offset;
        }
        status = aggregate_child_address(context, address, first_offset,
                                         first_type, &first_address, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return initialize_object(context, node, first_address, first_type,
                                 1u, error);
    }
    {
        size_t end = subtree_end(context, node);
        size_t child;
        size_t ordinal = 0u;
        for (child = node + 1u; child < end; ++child) {
            lower_type child_type;
            uint64_t offset;
            lower_value child_address;
            if (context->nodes[child].parent != node ||
                (context->nodes[child].view.flags &
                 QL_C_SYNTAX_NODE_NAMED) == 0u ||
                strcmp(context->nodes[child].view.kind, "comment") == 0) {
                continue;
            }
            if (strcmp(context->nodes[child].view.kind,
                       "initializer_pair") == 0) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION,
                    child,
                    "designated aggregate initialization needs a member or "
                    "index map", error);
            }
            if (type.array_length != 0u) {
                if ((uint64_t)ordinal >= type.array_length) {
                    return lower_unknown(
                        context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, child,
                        "too many positional array initializers", error);
                }
                child_type = array_element(type);
                offset = (uint64_t)ordinal *
                         type_byte_width(context, child_type);
            } else {
                lower_record *record = &context->records[type.record];
                size_t limit = record->is_union != 0u &&
                                       record->member_count != 0u
                                   ? 1u
                                   : record->member_count;
                if (ordinal >= limit) {
                    return lower_unknown(
                        context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, child,
                        "too many positional record initializers", error);
                }
                child_type = record->members[ordinal].type;
                offset = record->members[ordinal].offset;
            }
            status = aggregate_child_address(context, address, offset,
                                             child_type, &child_address,
                                             error);
            if (status == QL_STATUS_OK) {
                status = initialize_object(context, child, child_address,
                                           child_type, 1u, error);
            }
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
            ++ordinal;
        }
    }
    return QL_STATUS_OK;
}

static ql_status lower_declaration(lower_context *context, size_t node,
                                   ql_error *error) {
    size_t type_node = direct_field_child(context, node, "type");
    size_t end = subtree_end(context, node);
    size_t index;
    lower_type base_type;
    lower_type declarator_type;
    uint32_t is_const;
    size_t declarator_count = 0u;
    ql_status status;

    if (type_node == SIZE_MAX) {
        return lower_unknown(
            context, QL_C_LOWER_DIAGNOSTIC_INVALID_DECLARATION, node,
            "local declaration has no type", error);
    }
    status = parse_local_type(context, node, type_node, &base_type,
                              &is_const, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
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
        {
            uint32_t pointer_depth = 0u;
            uint64_t array_length = 0u;
            int rejected = 0;
            size_t named = declarator == SIZE_MAX
                               ? SIZE_MAX
                               : member_declarator_name(context, declarator,
                                                        &pointer_depth,
                                                        &array_length,
                                                        &rejected);
            if (named == SIZE_MAX || rejected != 0) {
                return lower_unknown(
                    context,
                    rejected != 0
                        ? QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE
                        : QL_C_LOWER_DIAGNOSTIC_INVALID_DECLARATION,
                    declarator != SIZE_MAX ? declarator : index,
                    "a function local, a multidimensional array, or an array "
                    "whose bound this pass cannot fold", error);
            }
            if (pointer_depth > 2u) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, named,
                    "this slice carries at most two levels of indirection", error);
            }
            declarator_type = base_type;
            if (pointer_depth == 1u) {
                declarator_type = make_pointer_to(base_type);
            } else if (pointer_depth == 2u) {
                declarator_type = make_pointer_to(make_pointer_to(base_type));
            } else if (base_type.kind == QL_C_SCALAR_VOID) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR, named,
                    "void is not an object type here", error);
            }
            if (array_length != 0u) {
                if (array_length == LOWER_ARRAY_BOUND_FROM_INITIALIZER &&
                    !positional_initializer_count(context, value_node,
                                                  &array_length)) {
                    return lower_unknown(
                        context,
                        QL_C_LOWER_DIAGNOSTIC_INVALID_DECLARATION, named,
                        "an inferred local array bound needs a small "
                        "positional initializer list", error);
                }
                declarator_type = make_array_of(declarator_type,
                                                array_length);
            }
            if (declarator_type.kind == QL_C_SCALAR_RECORD) {
                status = ensure_record_layout(context, declarator_type.record,
                                              named, error);
                if (status != QL_STATUS_OK || context->unknown != 0u) {
                    return status;
                }
            }
            /* An array and a record are storage, never a value, so neither
               has an IR type of its own. Asking for one here would be asking
               for the representation of something that is never loaded or
               stored whole. */
            if (declarator_type.array_length == 0u &&
                declarator_type.kind != QL_C_SCALAR_RECORD) {
                status = ensure_ir_type(context, &declarator_type, error);
                if (status != QL_STATUS_OK) {
                    return status;
                }
            }
            identifier = named;
        }
        name = copy_node_text(context, identifier);
        if (name == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        status = add_variable(context, name, strlen(name), declarator_type,
                              QL_IR_INVALID_VALUE_ID, 0u, is_const,
                              identifier, error);
        context->allocator->deallocate(context->allocator->user_data, name);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        variable = &context->variables[context->variable_count - 1u];
        if (declarator_type.array_length != 0u ||
            declarator_type.kind == QL_C_SCALAR_RECORD) {
            if (value_node != SIZE_MAX) {
                status = initialize_object(
                    context, value_node, stack_address(context, variable),
                    declarator_type, 0u, error);
                if (status != QL_STATUS_OK || context->unknown != 0u) {
                    return status;
                }
            }
            /* An aggregate without an explicit initializer has indeterminate
               bytes, but members may still be written before they are read.
               The current member model has no per-subobject initialization
               bits, so preserve the previous storage contract for that case.
               Explicit initialization is fully written above. */
            variable->initialized = 1u;
            continue;
        }
        if (value_node != SIZE_MAX) {
            lower_value value;
            lower_value converted;
            status = lower_expression(context, value_node, &value, error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
            status = convert_value(context, value, declarator_type,
                                   &converted, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            status = emit_ub_guard(context, &converted, error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            variable = &context->variables[context->variable_count - 1u];
            if (variable->is_stack != 0u) {
                status = emit_store(context,
                                    stack_address(context, variable),
                                    converted, error);
                if (status != QL_STATUS_OK || context->unknown != 0u) {
                    return status;
                }
                variable = &context->variables[context->variable_count - 1u];
                variable->initialized = 1u;
            } else {
                variable->value = converted.value;
                variable->initialized = 1u;
                /* A pointer local is only as well-founded as what was put in
                   it. */
                variable->has_object = converted.has_object;
            }
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
            "declaration does not introduce a local object", error);
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
        if (variable->is_stack != 0u) {
            /* Memory already carries it, and the memory PHI below merges
               that. A second PHI over a value it does not have would be
               wrong. Definite initialization still has to hold on both live
               paths before a later read or address escape is allowed. */
            variable->initialized =
                left->initialized[index] != 0u &&
                        right->initialized[index] != 0u
                    ? 1u
                    : 0u;
            continue;
        }
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
            /* The branches disagreed, so whatever object one of them could
               name, the merge cannot name both. */
            variable->has_object = 0u;
        }
    }
    /* Memory is as much a merged value as any variable: a store on one branch
       and not the other leaves the join with two versions to reconcile. */
    if (context->makes_calls != 0u && left->trace != right->trace) {
        ql_ir_value_id operands[2];
        ql_ir_block_id blocks[2];
        ql_status status;
        operands[0] = left->trace;
        operands[1] = right->trace;
        blocks[0] = left_block;
        blocks[1] = right_block;
        status = emit_typed_instruction_phi(context, context->trace_type,
                                            operands, blocks,
                                            &context->trace_value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    } else {
        context->trace_value = left->trace;
    }
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
    terminator.event_trace = context->makes_calls != 0u
                                 ? context->trace_value
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
    if (status != QL_STATUS_OK || context->unknown != 0u) {
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
    terminator.event_trace = context->makes_calls != 0u
                                 ? context->trace_value
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
static ql_status add_object(lower_context *context, const char *label,
                            const uint64_t *fixed_size,
                            size_t *index_out, ql_error *error);

/* The declared type of an address-taken name, found either among the
   parameters or in the declaration that introduces it. Sizing the slot
   correctly is what keeps an out-of-bounds access out of bounds; rounding
   every slot up to a machine word would quietly make overruns look legal. */
static ql_status stack_slot_type(lower_context *context, const char *name,
                                 lower_type *output, uint32_t *is_parameter,
                                 ql_error *error) {
    size_t index;
    size_t end;

    *is_parameter = 0u;
    for (index = 0u; index < context->variable_count; ++index) {
        if (strcmp(context->variables[index].name, name) == 0) {
            *output = context->variables[index].type;
            *is_parameter = 1u;
            return QL_STATUS_OK;
        }
    }
    end = subtree_end(context, context->body_node);
    for (index = context->body_node + 1u; index < end; ++index) {
        size_t type_node;
        size_t declaration_end;
        size_t child;
        lower_type base;
        uint32_t is_const;
        ql_status status;

        if (strcmp(context->nodes[index].view.kind, "declaration") != 0) {
            continue;
        }
        type_node = direct_field_child(context, index, "type");
        if (type_node == SIZE_MAX) {
            continue;
        }
        declaration_end = subtree_end(context, index);
        for (child = index + 1u; child < declaration_end; ++child) {
            size_t declarator = child;
            size_t initializer = SIZE_MAX;
            uint32_t pointer_depth;
            uint64_t array_length;
            int rejected;
            size_t named;
            char *candidate;
            int matches;

            if (context->nodes[child].parent != index ||
                context->nodes[child].view.field_name == NULL ||
                strcmp(context->nodes[child].view.field_name,
                       "declarator") != 0) {
                continue;
            }
            if (strcmp(context->nodes[declarator].view.kind,
                       "init_declarator") == 0) {
                initializer = direct_field_child(context, declarator,
                                                 "value");
                declarator = direct_field_child(context, declarator,
                                                "declarator");
            }
            if (declarator == SIZE_MAX) {
                continue;
            }
            named = member_declarator_name(context, declarator,
                                           &pointer_depth, &array_length,
                                           &rejected);
            if (named == SIZE_MAX || rejected != 0) {
                continue;
            }
            candidate = copy_node_text(context, named);
            if (candidate == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            matches = strcmp(candidate, name) == 0;
            context->allocator->deallocate(context->allocator->user_data,
                                           candidate);
            if (!matches) {
                continue;
            }
            status = parse_local_type(context, index, type_node, &base,
                                      &is_const, error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
            if (pointer_depth > 2u) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, named,
                    "this slice carries at most two levels of indirection",
                    error);
            }
            *output = base;
            while (pointer_depth-- > 0u) {
                *output = make_pointer_to(*output);
            }
            if (array_length != 0u) {
                if (array_length == LOWER_ARRAY_BOUND_FROM_INITIALIZER &&
                    !positional_initializer_count(context, initializer,
                                                  &array_length)) {
                    return lower_unknown(
                        context,
                        QL_C_LOWER_DIAGNOSTIC_INVALID_DECLARATION, named,
                        "an inferred local array bound needs a small "
                        "positional initializer list", error);
                }
                *output = make_array_of(*output, array_length);
            }
            return QL_STATUS_OK;
        }
    }
    return lower_unknown(
        context, QL_C_LOWER_DIAGNOSTIC_UNDECLARED_IDENTIFIER,
        context->body_node,
        "an address is taken of a name this function does not declare",
        error);
}

/* Every slot's base and size parameter, created while parameters may still be
   added. Nothing is emitted here. */
static ql_status add_stack_slot_objects(lower_context *context,
                                        ql_error *error) {
    size_t index;

    for (index = 0u; index < context->address_taken_count; ++index) {
        const char *name = context->address_taken[index];
        lower_stack_slot *slot;
        size_t global;
        int is_global = 0;
        lower_type type;
        uint32_t is_parameter;
        char label[160];
        size_t object;
        ql_status status;

        /* A global already has an object of its own, so taking its address
           must yield that object rather than a second, disjoint copy. */
        for (global = 0u; global < context->global_count; ++global) {
            if (context->globals[global].referenced != 0u &&
                strcmp(context->globals[global].name, name) == 0) {
                is_global = 1;
                break;
            }
        }
        if (is_global) {
            continue;
        }
        status = stack_slot_type(context, name, &type, &is_parameter, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        if (type.kind == QL_C_SCALAR_VOID) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE,
                context->body_node,
                "storage for a void local is outside this slice", error);
        }
        if (type.kind == QL_C_SCALAR_RECORD) {
            status = ensure_record_layout(context, type.record,
                                          context->body_node, error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
        }
        if (type.kind == QL_C_SCALAR_POINTER && type.indirection >= 2u) {
            /* Its address is one level deeper than the name itself, so a
               two-level pointer whose address is taken needs three, which is
               past what this slice carries. */
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER,
                context->body_node,
                "taking the address of a two-level pointer needs a third "
                "level of indirection", error);
        }
        if (snprintf(label, sizeof(label), "%s@%zu", name,
                     context->object_count) < 0) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "stack object label does not fit");
            return QL_STATUS_INTERNAL_ERROR;
        }
        status = add_object(context, label, NULL, &object, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = grow_array(context->allocator,
                            (void **)&context->stack_slots,
                            &context->stack_slot_capacity,
                            sizeof(*context->stack_slots),
                            context->stack_slot_count + 1u, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        slot = &context->stack_slots[context->stack_slot_count];
        memset(slot, 0, sizeof(*slot));
        slot->name = copy_text(context->allocator, name, strlen(name));
        if (slot->name == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        slot->type = type;
        slot->object = object;
        slot->address = QL_IR_INVALID_VALUE_ID;
        slot->is_parameter = is_parameter;
        ++context->stack_slot_count;
    }
    return QL_STATUS_OK;
}

/* The byte count a global's string initialiser states, including the
   terminating NUL, or zero when it does not state one this pass can read. */
static size_t global_string_bytes(lower_context *context, size_t node) {
    unsigned char decoded[LOWER_MAX_STRING_BYTES + 1u];
    size_t decoded_size = 0u;
    char *text;
    int ok;

    if (node == SIZE_MAX ||
        strcmp(context->nodes[node].view.kind, "string_literal") != 0) {
        return 0u;
    }
    text = copy_node_text(context, node);
    if (text == NULL) {
        return 0u;
    }
    ok = decode_string_literal(text, strlen(text), decoded, &decoded_size);
    context->allocator->deallocate(context->allocator->user_data, text);
    return ok ? decoded_size : 0u;
}

/* Storage with static storage duration becomes an object under exactly the
   discipline a pointer parameter's region gets: a base and a size parameter,
   the three standing model constraints, and a pinned size. Reads become loads
   and writes become stores, which is what keeps a write to a global and a
   call in the order the source wrote them: both thread the memory state. */
static ql_status add_global_objects(lower_context *context, ql_error *error) {
    size_t index;

    for (index = 0u; index < context->global_count; ++index) {
        lower_global *global = &context->globals[index];
        size_t type_node;
        uint32_t pointer_depth = 0u;
        uint64_t array_length = 0u;
        int rejected = 0;
        uint32_t is_const = 0u;
        lower_type type;
        char label[160];
        size_t object;
        ql_status status;

        if (global->referenced == 0u) {
            continue;
        }
        if (find_variable(context, global->name, strlen(global->name)) !=
            NULL) {
            /* A parameter of the same name shadows the global throughout the
               body, so the global is never reached and must not get an object
               that would then shadow the parameter in the variable table. */
            global->referenced = 0u;
            continue;
        }
        type_node = direct_field_child(context, global->declaration_node,
                                       "type");
        if (type_node == SIZE_MAX) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE,
                global->declaration_node,
                "a global with no declared type has no size to give it",
                error);
        }
        (void)member_declarator_name(context, global->declarator_node,
                                     &pointer_depth, &array_length,
                                     &rejected);
        if (rejected != 0) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE,
                global->declaration_node,
                "this global's declarator is outside this slice", error);
        }
        context->parsing_global = 1u;
        status = parse_local_type(context, global->declaration_node, type_node,
                                  &type, &is_const, error);
        context->parsing_global = 0u;
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
        if (pointer_depth > 2u) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER,
                global->declaration_node,
                "this slice carries at most two levels of indirection", error);
        }
        while (pointer_depth-- > 0u) {
            type = make_pointer_to(type);
        }
        if (array_length == LOWER_ARRAY_BOUND_FROM_INITIALIZER) {
            /* Only a string literal states a bound this pass can read. The
               object's size is what keeps an out-of-bounds access out of
               bounds, so a bound nobody states is refused, not assumed. */
            size_t bytes = global_string_bytes(context,
                                               global->initializer_node);
            if (bytes == 0u) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE,
                    global->declaration_node,
                    "an array with no bound needs a string initialiser this "
                    "pass can read", error);
            }
            array_length = (uint64_t)bytes;
        }
        if (type.kind == QL_C_SCALAR_VOID) {
            return lower_unknown(
                context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE,
                global->declaration_node,
                "storage for a void global is outside this slice", error);
        }
        if (array_length != 0u) {
            type = make_array_of(type, array_length);
        }
        if (type.kind == QL_C_SCALAR_RECORD) {
            status = ensure_record_layout(context, type.record,
                                          global->declaration_node, error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
        }
        if (snprintf(label, sizeof(label), "%s@%zu", global->name,
                     context->object_count) < 0) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "global object label does not fit");
            return QL_STATUS_INTERNAL_ERROR;
        }
        status = add_object(context, label, NULL, &object, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        global->type = type;
        global->object = object;
    }
    return QL_STATUS_OK;
}

static ql_status add_string_objects(lower_context *context,
                                   ql_error *error) {
    size_t index;

    for (index = 0u; index < context->string_count; ++index) {
        char label[64];
        size_t object;
        ql_status status;

        if (snprintf(label, sizeof(label), "__string%zu@%zu", index,
                     context->object_count) < 0) {
            ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                         "string object label does not fit");
            return QL_STATUS_INTERNAL_ERROR;
        }
        status = add_object(context, label, NULL, &object, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        context->strings[index].object = object;
    }
    return QL_STATUS_OK;
}

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
    if (context->makes_calls != 0u) {
        status = ensure_trace_type(context, error);
        if (status == QL_STATUS_OK) {
            status = ql_ir_builder_add_parameter(context->builder,
                                                 context->trace_type,
                                                 "__trace", 7u,
                                                 &context->trace_value,
                                                 error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    status = ensure_ir_type(context, &u64, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < context->variable_count; ++index) {
        const lower_variable *variable = &context->variables[index];
        size_t object;
        if (variable->type.kind != QL_C_SCALAR_POINTER) {
            continue;
        }
        status = add_object(context, variable->name, NULL, &object, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        (void)next;
    }
    context->parameter_object_count = context->object_count;
    status = add_stack_slot_objects(context, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    status = add_global_objects(context, error);
    if (status != QL_STATUS_OK || context->unknown != 0u) {
        return status;
    }
    return add_string_objects(context, error);
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
static ql_status emit_assumptions_for_object(lower_context *context,
                                             size_t index, ql_error *error);

/* Appends one object and its base and size parameters. Callers past the entry
   block pass a known size, which an assumption pins so that nothing is free
   to pick a different one. */
static ql_status add_object(lower_context *context, const char *label,
                            const uint64_t *fixed_size, size_t *index_out,
                            ql_error *error) {
    lower_type u64 = address_type();
    lower_object *object;
    char name[160];
    ql_status status;

    status = ensure_ir_type(context, &u64, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = grow_array(context->allocator, (void **)&context->objects,
                        &context->object_capacity,
                        sizeof(*context->objects),
                        context->object_count + 1u, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    object = &context->objects[context->object_count];
    memset(object, 0, sizeof(*object));
    if (snprintf(name, sizeof(name), "%s.__base", label) < 0) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "object parameter name does not fit");
        return QL_STATUS_INTERNAL_ERROR;
    }
    status = ql_ir_builder_add_parameter(context->builder, u64.ir_type, name,
                                         strlen(name), &object->base, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (snprintf(name, sizeof(name), "%s.__size", label) < 0) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "object parameter name does not fit");
        return QL_STATUS_INTERNAL_ERROR;
    }
    status = ql_ir_builder_add_parameter(context->builder, u64.ir_type, name,
                                         strlen(name), &object->size, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    *index_out = context->object_count++;
    if (fixed_size == NULL) {
        return QL_STATUS_OK;
    }
    status = emit_assumptions_for_object(context, *index_out, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    {
        ql_ir_value_id expected;
        ql_ir_value_id predicate;
        status = add_uint_constant(context, u64, *fixed_size, &expected,
                                   error);
        if (status == QL_STATUS_OK) {
            status = emit_compare(context, QL_IR_OPCODE_EQ, object->size,
                                  expected, &predicate, error);
        }
        if (status == QL_STATUS_OK) {
            status = emit_assume(context, predicate, error);
        }
    }
    return status;
}


/* The address of a variable that lives in storage, as a value. */
static lower_value stack_address(const lower_context *context,
                                 const lower_variable *variable) {
    lower_value address;
    memset(&address, 0, sizeof(address));
    address.value = variable->address;
    /* An array's storage is addressed at its element type, which is what
       makes `a[i]` the ordinary pointer arithmetic it already is and `&a`
       the same address the array's name gives. */
    address.type = variable->type.array_length != 0u
                       ? make_pointer_to(array_element(variable->type))
                       : make_pointer_to(variable->type);
    address.defined = context->true_value;
    address.may_ub = 0u;
    address.has_object = 1u;
    return address;
}

static ql_status emit_assumptions_for_object(lower_context *context,
                                             size_t index,
                                             ql_error *error) {
    lower_type u64 = address_type();
    ql_ir_value_id first_address;
    ql_ir_value_id zero;
    size_t other;
    ql_status status;

    status = add_uint_constant(context, u64,
                               QL_IR_INTERP_FIRST_OBJECT_ADDRESS,
                               &first_address, error);
    if (status == QL_STATUS_OK) {
        status = add_uint_constant(context, u64, 0u, &zero, error);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }
    {
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

/* A parameter arrives as a value. If something takes its address it also
   needs storage, so the entry block gives it a slot and writes the incoming
   value there once. */
static ql_status materialize_stack_slots(lower_context *context,
                                         ql_error *error) {
    size_t index;

    for (index = 0u; index < context->stack_slot_count; ++index) {
        lower_stack_slot *slot = &context->stack_slots[index];
        lower_type pointer = make_pointer_to(slot->type);
        uint64_t size = type_byte_width(context, slot->type);
        lower_value address;
        lower_value pointer_value;
        ql_ir_value_id expected;
        ql_ir_value_id predicate;
        ql_status status;

        status = emit_assumptions_for_object(context, slot->object, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        /* The size is known here, so it is pinned rather than left for a
           solver to choose. */
        status = add_uint_constant(context, address_type(), size, &expected,
                                   error);
        if (status == QL_STATUS_OK) {
            status = emit_compare(context, QL_IR_OPCODE_EQ,
                                  context->objects[slot->object].size,
                                  expected, &predicate, error);
        }
        if (status == QL_STATUS_OK) {
            status = emit_assume(context, predicate, error);
        }
        if (status == QL_STATUS_OK) {
            status = ensure_bool_constants(context, error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        memset(&address, 0, sizeof(address));
        address.value = context->objects[slot->object].base;
        address.type = address_type();
        address.defined = context->true_value;
        address.has_object = 1u;
        status = emit_pointer_of_address(context, address, pointer,
                                         &pointer_value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        slot->address = pointer_value.value;
    }
    /* Parameters entered the variable table before their slots existed, so
       they bind here, once the addresses are computed. */
    for (index = 0u; index < context->variable_count; ++index) {
        lower_variable *variable = &context->variables[index];
        size_t slot;
        for (slot = 0u; slot < context->stack_slot_count; ++slot) {
            if (strcmp(context->stack_slots[slot].name, variable->name) != 0) {
                continue;
            }
            variable->is_stack = 1u;
            variable->address = context->stack_slots[slot].address;
            variable->has_object = 1u;
            break;
        }
    }
    /* A parameter arrives as a value, so its slot has to be given that value
       once before the body runs. */
    for (index = 0u; index < context->variable_count; ++index) {
        lower_variable *variable = &context->variables[index];
        lower_value incoming;
        ql_status status;
        if (variable->is_stack == 0u) {
            continue;
        }
        memset(&incoming, 0, sizeof(incoming));
        incoming.value = variable->value;
        incoming.type = variable->type;
        incoming.defined = context->true_value;
        incoming.has_object = variable->has_object;
        status = emit_store(context, stack_address(context, variable),
                            incoming, error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

/* Writes a run of bytes into an object through its own base pointer. Both a
   string literal and an initialised char array come to the same thing, so
   they share one writer rather than two that could drift. */
/* States that a run of bytes stands at the start of an object, as one IR
   instruction carrying the bytes.

   This used to be a store per byte. That said the same thing, but it said it
   as a write the program never performs, and it cost an address, a constant,
   a bounds guard over every live object, and a store for each byte -- so the
   work grew with the byte count and the object count multiplied together.
   The bytes are constants the program already states, so the IR states them
   as constants. */
static ql_status assume_bytes(lower_context *context, lower_value address,
                              const unsigned char *bytes, size_t size,
                              ql_error *error) {
    ql_ir_instruction_definition_v1 definition;
    ql_ir_instruction_id instruction;
    ql_ir_value_id operands[2];
    ql_ir_value_id holds;
    ql_ir_type_id result_type;
    lower_type boolean = make_bool_type();
    ql_status status;

    if (size == 0u) {
        return QL_STATUS_OK;
    }
    status = ensure_ir_type(context, &boolean, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    result_type = boolean.ir_type;
    operands[0] = context->memory_value;
    operands[1] = address.value;
    ql_ir_instruction_definition_init(&definition,
                                      QL_IR_OPCODE_MEMORY_IMAGE);
    definition.operands = operands;
    definition.operand_count = 2u;
    definition.result_types = &result_type;
    definition.result_count = 1u;
    definition.image = bytes;
    definition.image_size = size;
    status = ql_ir_builder_append_instruction(context->builder,
                                              context->current_block,
                                              &definition, &instruction,
                                              &holds, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    return emit_assume(context, holds, error);
}

static ql_status materialize_globals(lower_context *context,
                                     ql_error *error) {
    size_t index;

    for (index = 0u; index < context->global_count; ++index) {
        lower_global *global = &context->globals[index];
        lower_type pointer;
        uint64_t size;
        lower_value address;
        lower_value pointer_value;
        lower_variable *variable;
        ql_ir_value_id expected;
        ql_ir_value_id predicate;
        ql_status status;

        if (global->referenced == 0u || global->object == SIZE_MAX) {
            continue;
        }
        pointer = make_pointer_to(global->type);
        size = type_byte_width(context, global->type);
        status = emit_assumptions_for_object(context, global->object, error);
        if (status == QL_STATUS_OK) {
            status = add_uint_constant(context, address_type(), size,
                                       &expected, error);
        }
        if (status == QL_STATUS_OK) {
            status = emit_compare(context, QL_IR_OPCODE_EQ,
                                  context->objects[global->object].size,
                                  expected, &predicate, error);
        }
        if (status == QL_STATUS_OK) {
            status = emit_assume(context, predicate, error);
        }
        if (status == QL_STATUS_OK) {
            status = ensure_bool_constants(context, error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        memset(&address, 0, sizeof(address));
        address.value = context->objects[global->object].base;
        address.type = address_type();
        address.defined = context->true_value;
        address.has_object = 1u;
        status = emit_pointer_of_address(context, address, pointer,
                                         &pointer_value, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        global->address = pointer_value.value;

        status = grow_array(context->allocator, (void **)&context->variables,
                            &context->variable_capacity,
                            sizeof(*context->variables),
                            context->variable_count + 1u, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        variable = &context->variables[context->variable_count];
        memset(variable, 0, sizeof(*variable));
        variable->name = copy_text(context->allocator, global->name,
                                   strlen(global->name));
        if (variable->name == NULL) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        variable->name_size = strlen(global->name);
        variable->type = global->type;
        variable->is_stack = 1u;
        variable->address = global->address;
        variable->has_object = 1u;
        variable->initialized = 1u;
        /* File scope, so a local of the same name shadows it: find_variable
           searches from the most recent entry backwards. */
        variable->scope_depth = 0u;
        ++context->variable_count;

        /* A stated initial value is written before the body runs, so the
           memory the body starts from is the one the declaration promised
           rather than whatever the caller happened to supply. A declaration
           that states nothing leaves the contents unknown; in this corpus
           `int GLB_0;` is a placeholder for a definition that lives in
           another translation unit, not a tentative definition of zero. */
        if (global->type.array_length != 0u) {
            unsigned char decoded[LOWER_MAX_STRING_BYTES + 1u];
            size_t decoded_size = 0u;
            char *text = global->initializer_node == SIZE_MAX
                             ? NULL
                             : copy_node_text(context,
                                              global->initializer_node);
            int ok = 0;
            if (text != NULL) {
                ok = decode_string_literal(text, strlen(text), decoded,
                                           &decoded_size);
                context->allocator->deallocate(context->allocator->user_data,
                                               text);
            }
            if (ok) {
                status = assume_bytes(context,
                                      stack_address(context, variable),
                                      decoded, decoded_size, error);
                if (status != QL_STATUS_OK || context->unknown != 0u) {
                    return status;
                }
            } else if (global->initializer_node != SIZE_MAX) {
                return lower_unknown(
                    context, QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION,
                    global->initializer_node,
                    "an array initialiser other than a string literal this "
                    "pass can read is outside this slice", error);
            }
            continue;
        }
        if (global->initializer_node != SIZE_MAX) {
            lower_value initial;
            lower_value converted;
            status = lower_expression(context, global->initializer_node,
                                      &initial, error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
            status = convert_value(context, initial, global->type, &converted,
                                   error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
            status = emit_store(context,
                                stack_address(context, variable), converted,
                                error);
            if (status != QL_STATUS_OK || context->unknown != 0u) {
                return status;
            }
        }
    }
    return QL_STATUS_OK;
}

/* A literal's bytes are stated by the program, so they are written into the
   object as one image instruction, exactly as an initialised char array's
   are. Nothing writes them: the program states them, so the IR states them
   and the entry block assumes they hold. */
static ql_status materialize_strings(lower_context *context,
                                     ql_error *error) {
    lower_type byte = make_integer_type(8u, 1u, 1u);
    size_t index;

    for (index = 0u; index < context->string_count; ++index) {
        lower_string *literal = &context->strings[index];
        lower_type pointer = make_pointer_to(byte);
        lower_value address;
        lower_value base;
        ql_ir_value_id expected;
        ql_ir_value_id predicate;
        ql_status status;

        if (literal->object == SIZE_MAX) {
            continue;
        }
        status = emit_assumptions_for_object(context, literal->object, error);
        if (status == QL_STATUS_OK) {
            status = add_uint_constant(context, address_type(),
                                       (uint64_t)literal->size, &expected,
                                       error);
        }
        if (status == QL_STATUS_OK) {
            status = emit_compare(context, QL_IR_OPCODE_EQ,
                                  context->objects[literal->object].size,
                                  expected, &predicate, error);
        }
        if (status == QL_STATUS_OK) {
            status = emit_assume(context, predicate, error);
        }
        if (status == QL_STATUS_OK) {
            status = ensure_bool_constants(context, error);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
        memset(&address, 0, sizeof(address));
        address.value = context->objects[literal->object].base;
        address.type = address_type();
        address.defined = context->true_value;
        address.has_object = 1u;
        status = emit_pointer_of_address(context, address, pointer, &base,
                                         error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        literal->address = base.value;
        status = assume_bytes(context, base, literal->bytes, literal->size,
                              error);
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

/* The entry block states the assumptions for the objects the caller
   supplied. Objects the function makes for itself state their own where they
   are made, which is what lets one appear part-way through a body. */
static ql_status emit_object_assumptions(lower_context *context,
                                         ql_error *error) {
    size_t index;
    for (index = 0u; index < context->parameter_object_count; ++index) {
        ql_status status = emit_assumptions_for_object(context, index, error);
        if (status != QL_STATUS_OK) {
            return status;
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
        if (status == QL_STATUS_OK && context->unknown == 0u &&
            type.kind == QL_C_SCALAR_POINTER) {
            context->variables[context->variable_count - 1u].has_object = 1u;
        }
        if (status != QL_STATUS_OK || context->unknown != 0u) {
            return status;
        }
    }
    /* A pointer parameter brings an object with it, and a local whose
       address is taken will need one later. Either way the function touches
       memory, and the memory parameter has to exist before the body runs. */
    for (index = 0u; index < context->variable_count; ++index) {
        if (context->variables[index].type.kind == QL_C_SCALAR_POINTER) {
            context->uses_memory = 1u;
        }
    }
    if (context->address_taken_count != 0u) {
        context->uses_memory = 1u;
    }
    return add_object_parameters(context, error);
}

static void cleanup_context(lower_context *context) {
    size_t index;

    pop_variables(context, 0u);
    release_typedefs(context);
    release_address_taken(context);
    release_callees(context);
    release_globals(context);
    release_strings(context);
    for (index = 0u; index < context->stack_slot_count; ++index) {
        context->allocator->deallocate(context->allocator->user_data,
                                       context->stack_slots[index].name);
    }
    context->allocator->deallocate(context->allocator->user_data,
                                   context->stack_slots);
    context->stack_slots = NULL;
    context->stack_slot_count = 0u;
    context->allocator->deallocate(context->allocator->user_data,
                                   context->type_cache);
    context->type_cache = NULL;
    release_records(context);
    release_enumerators(context);
    context->allocator->deallocate(context->allocator->user_data,
                                   context->objects);
    context->objects = NULL;
    context->allocator->deallocate(context->allocator->user_data,
                                   context->variables);
    ql_ir_builder_destroy(context->builder);
    context->allocator->deallocate(context->allocator->user_data,
                                   context->nodes);
    /* A tree the caller lent us outlives the call, and then no parser was
       created either. */
    if (context->owns_tree != 0u) {
        ql_c_syntax_tree_destroy(context->tree);
    }
    ql_c_parser_destroy(context->parser);
}

ql_status QL_CALL ql_c_lower_selected_function(
    const ql_allocator *allocator, const char *source, size_t source_size,
    const ql_c_frontend_unit *unit, const ql_c_function_view *function,
    ql_c_lower_result **output, ql_error *error) {
    return ql_c_lower_selected_function_with_tree(allocator, source,
                                                  source_size, unit, function,
                                                  NULL, output, error);
}

ql_status QL_CALL ql_c_lower_selected_function_with_tree(
    const ql_allocator *allocator, const char *source, size_t source_size,
    const ql_c_frontend_unit *unit, const ql_c_function_view *function,
    ql_c_syntax_tree *borrowed_tree, ql_c_lower_result **output,
    ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_c_lower_result *result = NULL;
    lower_context context;
    ql_c_function_view canonical;
    size_t function_node;
    size_t body_node;
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
    context.memory_type = QL_IR_INVALID_TYPE_ID;
    context.trace_type = QL_IR_INVALID_TYPE_ID;
    context.trace_value = QL_IR_INVALID_VALUE_ID;
    context.memory_value = QL_IR_INVALID_VALUE_ID;
    context.true_value = QL_IR_INVALID_VALUE_ID;
    context.false_value = QL_IR_INVALID_VALUE_ID;
    context.current_block = QL_IR_INVALID_BLOCK_ID;

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

    if (borrowed_tree != NULL) {
        context.tree = borrowed_tree;
        context.owns_tree = 0u;
        status = QL_STATUS_OK;
    } else {
        QL_STAGE_MARK(stage_parse);
        context.owns_tree = 1u;
        status = ql_c_parser_create(selected, &context.parser, error);
        if (status == QL_STATUS_OK) {
            status = ql_c_parser_parse(context.parser, source, source_size,
                                       &context.tree, error);
        }
        QL_STAGE_ADD(QL_STAGE_PARSE_LOWER, stage_parse);
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
        status = collect_callees(&context, error);
    }
    if (status == QL_STATUS_OK) {
        status = collect_globals(&context, error);
    }
    if (status == QL_STATUS_OK) {
        status = collect_records(&context, error);
    }
    if (status == QL_STATUS_OK) {
        status = collect_enumerators(&context, error);
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

    context.body_node = body_node;
    collect_calls(&context, body_node);
    collect_global_uses(&context, body_node);
    status = collect_string_literals(&context, body_node, error);
    if (status == QL_STATUS_OK) {
        status = collect_storage_locals(&context, body_node, error);
    }
    if (status == QL_STATUS_OK) {
        status = collect_address_taken(&context, body_node, error);
    }
    if (status != QL_STATUS_OK) {
        cleanup_context(&context);
        ql_c_lower_result_destroy(result);
        return status;
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
            status = materialize_stack_slots(&context, error);
        }
        if (status == QL_STATUS_OK && context.unknown == 0u) {
            status = materialize_globals(&context, error);
        }
        if (status == QL_STATUS_OK && context.unknown == 0u) {
            status = materialize_strings(&context, error);
        }
        if (status == QL_STATUS_OK && context.unknown == 0u) {
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
