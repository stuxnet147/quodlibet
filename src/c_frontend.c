#include "quodlibet/c_frontend.h"

#include <limits.h>
#include <string.h>

typedef struct ql_c_syntax_record {
    ql_c_syntax_node_view view;
    size_t parent;
    uint32_t depth;
} ql_c_syntax_record;

typedef struct ql_c_parameter_record {
    char *name;
    ql_source_range range;
    ql_c_type_inventory_v1 type;
} ql_c_parameter_record;

typedef struct ql_c_function_record {
    char *name;
    char *signature_spelling;
    ql_source_range range;
    ql_source_range body_range;
    ql_c_type_inventory_v1 return_type;
    ql_c_parameter_record *parameters;
    size_t parameter_count;
    size_t parameter_capacity;
    size_t diagnostic_count;
    ql_c_function_support support;
    uint32_t has_variadic_parameters;
    uint32_t has_old_style_parameters;
} ql_c_function_record;

typedef struct ql_c_diagnostic_record {
    ql_c_frontend_diagnostic_code code;
    uint32_t function_index;
    ql_source_range range;
    const char *construct_kind;
    const char *message;
} ql_c_diagnostic_record;

struct ql_c_frontend_unit {
    ql_allocator allocator;
    ql_c_function_record *functions;
    size_t function_count;
    size_t function_capacity;
    ql_c_diagnostic_record *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_capacity;
    ql_c_function_support support;
    /* The parse this analysis was built from, kept so that a caller going on
       to lower does not have to parse the same source a second time. */
    ql_c_syntax_tree *tree;
};

static const ql_allocator *select_allocator(const ql_allocator *allocator) {
    return allocator != NULL ? allocator : ql_default_allocator();
}

static ql_status grow_array(const ql_allocator *allocator, void **items,
                            size_t *capacity, size_t item_size,
                            size_t required, ql_error *error) {
    size_t new_capacity;
    void *resized;

    if (required <= *capacity) {
        return QL_STATUS_OK;
    }
    new_capacity = *capacity != 0u ? *capacity : 8u;
    while (new_capacity < required) {
        if (new_capacity > SIZE_MAX / 2u) {
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        new_capacity *= 2u;
    }
    if (new_capacity > SIZE_MAX / item_size) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    resized = allocator->reallocate(allocator->user_data, *items,
                                    new_capacity * item_size);
    if (resized == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    *items = resized;
    *capacity = new_capacity;
    return QL_STATUS_OK;
}

static ql_status collect_syntax_nodes(const ql_allocator *allocator,
                                      ql_c_syntax_tree *tree,
                                      ql_c_syntax_record **records,
                                      size_t *record_count,
                                      ql_error *error) {
    ql_c_syntax_cursor *cursor = NULL;
    ql_c_syntax_record *items = NULL;
    size_t count = 0u;
    size_t capacity = 0u;
    size_t parent = SIZE_MAX;
    uint32_t depth = 0u;
    ql_status status;

    *records = NULL;
    *record_count = 0u;
    status = ql_c_syntax_cursor_create(tree, &cursor, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (;;) {
        size_t current;

        status = grow_array(allocator, (void **)&items, &capacity,
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
                *records = items;
                *record_count = count;
                return QL_STATUS_OK;
            }
            ascended = parent;
            parent = items[ascended].parent;
            --depth;
        }
    }

fail:
    ql_c_syntax_cursor_destroy(cursor);
    allocator->deallocate(allocator->user_data, items);
    return status;
}

static size_t subtree_end(const ql_c_syntax_record *nodes, size_t node_count,
                          size_t root) {
    size_t end = root + 1u;
    while (end < node_count && nodes[end].depth > nodes[root].depth) {
        ++end;
    }
    return end;
}

static size_t direct_field_child(const ql_c_syntax_record *nodes,
                                 size_t node_count, size_t parent,
                                 const char *field_name) {
    size_t end = subtree_end(nodes, node_count, parent);
    size_t index;

    for (index = parent + 1u; index < end; ++index) {
        if (nodes[index].parent == parent &&
            nodes[index].view.field_name != NULL &&
            strcmp(nodes[index].view.field_name, field_name) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

static size_t first_direct_declarator_child(
    const ql_c_syntax_record *nodes, size_t node_count, size_t parent) {
    size_t end = subtree_end(nodes, node_count, parent);
    size_t index;

    for (index = parent + 1u; index < end; ++index) {
        const char *kind;
        if (nodes[index].parent != parent) {
            continue;
        }
        kind = nodes[index].view.kind;
        if (strcmp(kind, "identifier") == 0 ||
            strcmp(kind, "field_identifier") == 0 ||
            strstr(kind, "declarator") != NULL) {
            return index;
        }
    }
    return SIZE_MAX;
}

static size_t declarator_identifier(const ql_c_syntax_record *nodes,
                                    size_t node_count, size_t declarator) {
    size_t current = declarator;

    while (current != SIZE_MAX) {
        size_t next;
        if (strcmp(nodes[current].view.kind, "identifier") == 0 ||
            strcmp(nodes[current].view.kind, "field_identifier") == 0) {
            return current;
        }
        next = direct_field_child(nodes, node_count, current, "declarator");
        if (next == SIZE_MAX) {
            next = first_direct_declarator_child(nodes, node_count, current);
        }
        if (next == current) {
            return SIZE_MAX;
        }
        current = next;
    }
    return SIZE_MAX;
}

static char *copy_source_range(const ql_allocator *allocator,
                               const char *source, size_t source_size,
                               ql_source_range range, int trim) {
    size_t start = range.start_byte;
    size_t end = range.end_byte;
    char *text;

    if (start > end || end > source_size) {
        return NULL;
    }
    if (trim != 0) {
        while (start < end &&
               (source[start] == ' ' || source[start] == '\t' ||
                source[start] == '\r' || source[start] == '\n' ||
                source[start] == '\f' || source[start] == '\v')) {
            ++start;
        }
        while (end > start &&
               (source[end - 1u] == ' ' || source[end - 1u] == '\t' ||
                source[end - 1u] == '\r' || source[end - 1u] == '\n' ||
                source[end - 1u] == '\f' || source[end - 1u] == '\v')) {
            --end;
        }
    }
    if (end - start == SIZE_MAX) {
        return NULL;
    }
    text = allocator->allocate(allocator->user_data, end - start + 1u);
    if (text == NULL) {
        return NULL;
    }
    if (end != start) {
        memcpy(text, source + start, end - start);
    }
    text[end - start] = '\0';
    return text;
}

static char *copy_source_excluding_range(
    const ql_allocator *allocator, const char *source, size_t source_size,
    ql_source_range whole, ql_source_range excluded) {
    size_t prefix_size;
    size_t suffix_size;
    size_t result_size;
    char *text;

    if (whole.start_byte > excluded.start_byte ||
        excluded.start_byte > excluded.end_byte ||
        excluded.end_byte > whole.end_byte ||
        whole.end_byte > source_size) {
        return NULL;
    }
    prefix_size = excluded.start_byte - whole.start_byte;
    suffix_size = whole.end_byte - excluded.end_byte;
    if (prefix_size > SIZE_MAX - suffix_size) {
        return NULL;
    }
    result_size = prefix_size + suffix_size;
    if (result_size == SIZE_MAX) {
        return NULL;
    }
    text = allocator->allocate(allocator->user_data, result_size + 1u);
    if (text == NULL) {
        return NULL;
    }
    if (prefix_size != 0u) {
        memcpy(text, source + whole.start_byte, prefix_size);
    }
    if (suffix_size != 0u) {
        memcpy(text + prefix_size, source + excluded.end_byte, suffix_size);
    }
    text[result_size] = '\0';
    return text;
}

static uint32_t qualifier_from_spelling(const char *spelling) {
    uint32_t result = 0u;

    if (spelling == NULL) {
        return 0u;
    }
    if (strcmp(spelling, "const") == 0) {
        result |= QL_C_TYPE_QUALIFIER_CONST;
    } else if (strcmp(spelling, "volatile") == 0) {
        result |= QL_C_TYPE_QUALIFIER_VOLATILE;
    } else if (strcmp(spelling, "restrict") == 0 ||
               strcmp(spelling, "__restrict") == 0 ||
               strcmp(spelling, "__restrict__") == 0) {
        result |= QL_C_TYPE_QUALIFIER_RESTRICT;
    } else if (strcmp(spelling, "_Atomic") == 0) {
        result |= QL_C_TYPE_QUALIFIER_ATOMIC;
    }
    return result;
}

static ql_c_type_base_kind classify_base_type(const char *kind,
                                              const char *spelling) {
    if (strcmp(kind, "struct_specifier") == 0) {
        return QL_C_TYPE_BASE_STRUCT;
    }
    if (strcmp(kind, "union_specifier") == 0) {
        return QL_C_TYPE_BASE_UNION;
    }
    if (strcmp(kind, "enum_specifier") == 0) {
        return QL_C_TYPE_BASE_ENUM;
    }
    if (strcmp(kind, "type_identifier") == 0) {
        return QL_C_TYPE_BASE_TYPEDEF_NAME;
    }
    if (strcmp(kind, "atomic_type_specifier") == 0) {
        return QL_C_TYPE_BASE_ATOMIC;
    }
    if (strcmp(kind, "primitive_type") == 0 ||
        strcmp(kind, "sized_type_specifier") == 0) {
        if (strcmp(spelling, "void") == 0) {
            return QL_C_TYPE_BASE_VOID;
        }
        if (strcmp(spelling, "_Bool") == 0 ||
            strcmp(spelling, "bool") == 0) {
            return QL_C_TYPE_BASE_BOOL;
        }
        if (strstr(spelling, "float") != NULL ||
            strstr(spelling, "double") != NULL ||
            strstr(spelling, "_Float") != NULL) {
            return QL_C_TYPE_BASE_FLOAT;
        }
        return QL_C_TYPE_BASE_INTEGER;
    }
    return QL_C_TYPE_BASE_OTHER;
}

static void destroy_type(const ql_allocator *allocator,
                         ql_c_type_inventory_v1 *type) {
    allocator->deallocate(allocator->user_data, (void *)type->base_spelling);
    allocator->deallocate(allocator->user_data,
                          (void *)type->declarator_spelling);
    memset(type, 0, sizeof(*type));
}

static ql_status build_type_inventory(
    const ql_allocator *allocator, const char *source, size_t source_size,
    const ql_c_syntax_record *nodes, size_t node_count, size_t owner,
    size_t type_node, size_t declarator, ql_c_type_inventory_v1 *type,
    ql_error *error) {
    size_t current;
    size_t end;

    memset(type, 0, sizeof(*type));
    type->struct_size = sizeof(*type);
    if (type_node == SIZE_MAX) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "C declaration has no type node");
        return QL_STATUS_INTERNAL_ERROR;
    }
    type->base_range = nodes[type_node].view.range;
    type->base_spelling = copy_source_range(
        allocator, source, source_size, type->base_range, 1);
    if (type->base_spelling == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    type->base_kind = classify_base_type(nodes[type_node].view.kind,
                                         type->base_spelling);

    if (declarator != SIZE_MAX) {
        type->declarator_range = nodes[declarator].view.range;
        type->declarator_spelling = copy_source_range(
            allocator, source, source_size, type->declarator_range, 1);
    } else {
        type->declarator_spelling = allocator->allocate(
            allocator->user_data, 1u);
        if (type->declarator_spelling != NULL) {
            ((char *)type->declarator_spelling)[0] = '\0';
        }
    }
    if (type->declarator_spelling == NULL) {
        destroy_type(allocator, type);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }

    current = declarator;
    while (current != SIZE_MAX) {
        const char *kind = nodes[current].view.kind;
        size_t next;
        if (strcmp(kind, "pointer_declarator") == 0 ||
            strcmp(kind, "abstract_pointer_declarator") == 0) {
            type->shape |= QL_C_TYPE_SHAPE_POINTER;
            ++type->pointer_depth;
        } else if (strcmp(kind, "array_declarator") == 0 ||
                   strcmp(kind, "abstract_array_declarator") == 0) {
            type->shape |= QL_C_TYPE_SHAPE_ARRAY;
            ++type->array_rank;
        } else if (strcmp(kind, "function_declarator") == 0 ||
                   strcmp(kind, "abstract_function_declarator") == 0) {
            type->shape |= QL_C_TYPE_SHAPE_FUNCTION;
        }
        if (strcmp(kind, "identifier") == 0 ||
            strcmp(kind, "field_identifier") == 0) {
            break;
        }
        next = direct_field_child(nodes, node_count, current, "declarator");
        if (next == SIZE_MAX) {
            next = first_direct_declarator_child(nodes, node_count, current);
        }
        current = next;
    }

    end = subtree_end(nodes, node_count, owner);
    for (current = owner + 1u; current < end; ++current) {
        char *spelling;
        int belongs_to_declarator = 0;
        size_t ancestor;

        if (strcmp(nodes[current].view.kind, "type_qualifier") != 0) {
            continue;
        }
        if (nodes[current].parent == owner) {
            belongs_to_declarator = 1;
        } else if (declarator != SIZE_MAX) {
            ancestor = nodes[current].parent;
            while (ancestor != SIZE_MAX && ancestor != owner) {
                if (ancestor == declarator) {
                    belongs_to_declarator = 1;
                    break;
                }
                ancestor = nodes[ancestor].parent;
            }
        }
        if (belongs_to_declarator == 0) {
            continue;
        }
        spelling = copy_source_range(allocator, source, source_size,
                                     nodes[current].view.range, 1);
        if (spelling == NULL) {
            destroy_type(allocator, type);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        type->qualifiers |= qualifier_from_spelling(spelling);
        allocator->deallocate(allocator->user_data, spelling);
    }
    return QL_STATUS_OK;
}

static ql_status build_return_type_inventory(
    const ql_allocator *allocator, const char *source, size_t source_size,
    const ql_c_syntax_record *nodes, size_t node_count, size_t owner,
    size_t type_node, size_t declarator, size_t parameter_list,
    ql_c_type_inventory_v1 *type, ql_error *error) {
    size_t current = declarator;
    size_t function_shape_count = 0u;
    char *spelling;
    ql_status status = build_type_inventory(
        allocator, source, source_size, nodes, node_count, owner, type_node,
        declarator, type, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    while (current != SIZE_MAX) {
        size_t next;
        const char *kind = nodes[current].view.kind;
        if (strcmp(kind, "function_declarator") == 0 ||
            strcmp(kind, "abstract_function_declarator") == 0) {
            ++function_shape_count;
        }
        if (strcmp(kind, "identifier") == 0 ||
            strcmp(kind, "field_identifier") == 0) {
            break;
        }
        next = direct_field_child(nodes, node_count, current, "declarator");
        if (next == SIZE_MAX) {
            next = first_direct_declarator_child(nodes, node_count, current);
        }
        current = next;
    }

    /* One function layer describes the definition itself rather than its
       return type. Additional layers describe a function-returning pointer. */
    if (function_shape_count <= 1u) {
        type->shape &= ~((uint32_t)QL_C_TYPE_SHAPE_FUNCTION);
    }
    if (declarator == SIZE_MAX || parameter_list == SIZE_MAX) {
        return QL_STATUS_OK;
    }
    spelling = copy_source_excluding_range(
        allocator, source, source_size, nodes[declarator].view.range,
        nodes[parameter_list].view.range);
    if (spelling == NULL) {
        destroy_type(allocator, type);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    allocator->deallocate(allocator->user_data,
                          (void *)type->declarator_spelling);
    type->declarator_spelling = spelling;
    type->declarator_range = nodes[declarator].view.range;
    return QL_STATUS_OK;
}

static void destroy_function(const ql_allocator *allocator,
                             ql_c_function_record *function) {
    size_t index;

    allocator->deallocate(allocator->user_data, function->name);
    allocator->deallocate(allocator->user_data,
                          function->signature_spelling);
    destroy_type(allocator, &function->return_type);
    for (index = 0u; index < function->parameter_count; ++index) {
        allocator->deallocate(allocator->user_data,
                              function->parameters[index].name);
        destroy_type(allocator, &function->parameters[index].type);
    }
    allocator->deallocate(allocator->user_data, function->parameters);
    memset(function, 0, sizeof(*function));
}

void QL_CALL ql_c_frontend_unit_destroy(ql_c_frontend_unit *unit) {
    ql_allocator allocator;
    size_t index;

    if (unit == NULL) {
        return;
    }
    allocator = unit->allocator;
    for (index = 0u; index < unit->function_count; ++index) {
        destroy_function(&allocator, &unit->functions[index]);
    }
    allocator.deallocate(allocator.user_data, unit->functions);
    allocator.deallocate(allocator.user_data, unit->diagnostics);
    ql_c_syntax_tree_destroy(unit->tree);
    allocator.deallocate(allocator.user_data, unit);
}

static ql_status add_diagnostic(ql_c_frontend_unit *unit,
                                ql_c_frontend_diagnostic_code code,
                                uint32_t function_index,
                                ql_source_range range,
                                const char *construct_kind,
                                const char *message, ql_error *error) {
    ql_status status = grow_array(
        &unit->allocator, (void **)&unit->diagnostics,
        &unit->diagnostic_capacity, sizeof(*unit->diagnostics),
        unit->diagnostic_count + 1u, error);
    ql_c_diagnostic_record *diagnostic;

    if (status != QL_STATUS_OK) {
        return status;
    }
    diagnostic = &unit->diagnostics[unit->diagnostic_count++];
    diagnostic->code = code;
    diagnostic->function_index = function_index;
    diagnostic->range = range;
    diagnostic->construct_kind = construct_kind;
    diagnostic->message = message;
    unit->support = QL_C_FUNCTION_UNSUPPORTED;
    if (function_index != QL_C_FRONTEND_UNIT_DIAGNOSTIC &&
        function_index < unit->function_count) {
        unit->functions[function_index].support =
            QL_C_FUNCTION_UNSUPPORTED;
        ++unit->functions[function_index].diagnostic_count;
    }
    return QL_STATUS_OK;
}

static int starts_with(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int unsupported_construct(const char *kind) {
    static const char *const exact[] = {
        "attribute_declaration",
        "attribute_specifier",
        "extension_expression",
        "generic_expression",
        "gnu_asm_expression",
        "seh_except_clause",
        "seh_finally_clause",
        "seh_leave_statement",
        "seh_try_statement",
        "typeof_specifier"
    };
    size_t index;

    if (starts_with(kind, "ms_")) {
        return 1;
    }
    for (index = 0u; index < sizeof(exact) / sizeof(exact[0]); ++index) {
        if (strcmp(kind, exact[index]) == 0) {
            return 1;
        }
    }
    return 0;
}

static ql_status add_parameter(
    ql_c_frontend_unit *unit, ql_c_function_record *function,
    const char *source, size_t source_size, const ql_c_syntax_record *nodes,
    size_t node_count, size_t parameter_node, size_t function_index,
    ql_error *error) {
    size_t type_node = direct_field_child(nodes, node_count, parameter_node,
                                          "type");
    size_t declarator = direct_field_child(nodes, node_count, parameter_node,
                                           "declarator");
    size_t identifier = declarator != SIZE_MAX
                            ? declarator_identifier(nodes, node_count,
                                                    declarator)
                            : SIZE_MAX;
    ql_c_parameter_record *parameter;
    ql_status status;

    status = grow_array(&unit->allocator, (void **)&function->parameters,
                        &function->parameter_capacity,
                        sizeof(*function->parameters),
                        function->parameter_count + 1u, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    parameter = &function->parameters[function->parameter_count];
    memset(parameter, 0, sizeof(*parameter));
    parameter->range = nodes[parameter_node].view.range;
    if (identifier != SIZE_MAX) {
        parameter->name = copy_source_range(
            &unit->allocator, source, source_size,
            nodes[identifier].view.range, 0);
    } else {
        parameter->name = unit->allocator.allocate(
            unit->allocator.user_data, 1u);
        if (parameter->name != NULL) {
            parameter->name[0] = '\0';
        }
    }
    if (parameter->name == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    status = build_type_inventory(
        &unit->allocator, source, source_size, nodes, node_count,
        parameter_node, type_node, declarator, &parameter->type, error);
    if (status != QL_STATUS_OK) {
        unit->allocator.deallocate(unit->allocator.user_data,
                                   parameter->name);
        memset(parameter, 0, sizeof(*parameter));
        return status;
    }
    ++function->parameter_count;
    if (identifier == SIZE_MAX) {
        return add_diagnostic(
            unit, QL_C_FRONTEND_DIAGNOSTIC_UNNAMED_PARAMETER,
            (uint32_t)function_index, parameter->range,
            nodes[parameter_node].view.kind,
            "function definition parameter has no identifier", error);
    }
    return QL_STATUS_OK;
}

static ql_status analyze_parameters(
    ql_c_frontend_unit *unit, ql_c_function_record *function,
    const char *source, size_t source_size, const ql_c_syntax_record *nodes,
    size_t node_count, size_t parameter_list, size_t function_index,
    ql_error *error) {
    size_t end = subtree_end(nodes, node_count, parameter_list);
    size_t parameter_declaration_count = 0u;
    size_t sole_parameter = SIZE_MAX;
    size_t index;
    ql_status status;

    for (index = parameter_list + 1u; index < end; ++index) {
        if (nodes[index].parent == parameter_list &&
            strcmp(nodes[index].view.kind, "parameter_declaration") == 0) {
            ++parameter_declaration_count;
            sole_parameter = index;
        }
    }
    if (parameter_declaration_count == 1u) {
        size_t type_node = direct_field_child(nodes, node_count,
                                              sole_parameter, "type");
        size_t declarator = direct_field_child(nodes, node_count,
                                               sole_parameter, "declarator");
        if (type_node != SIZE_MAX && declarator == SIZE_MAX) {
            char *spelling = copy_source_range(
                &unit->allocator, source, source_size,
                nodes[type_node].view.range, 1);
            if (spelling == NULL) {
                ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
                return QL_STATUS_OUT_OF_MEMORY;
            }
            if (strcmp(spelling, "void") == 0) {
                unit->allocator.deallocate(unit->allocator.user_data,
                                           spelling);
                return QL_STATUS_OK;
            }
            unit->allocator.deallocate(unit->allocator.user_data, spelling);
        }
    }

    for (index = parameter_list + 1u; index < end; ++index) {
        const char *kind;
        if (nodes[index].parent != parameter_list) {
            continue;
        }
        kind = nodes[index].view.kind;
        if (strcmp(kind, "parameter_declaration") == 0) {
            status = add_parameter(unit, function, source, source_size,
                                   nodes, node_count, index, function_index,
                                   error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        } else if (strcmp(kind, "variadic_parameter") == 0) {
            function->has_variadic_parameters = 1u;
            status = add_diagnostic(
                unit, QL_C_FRONTEND_DIAGNOSTIC_VARIADIC_FUNCTION,
                (uint32_t)function_index, nodes[index].view.range, kind,
                "variadic functions are outside the restricted-C contract",
                error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        } else if (strcmp(kind, "identifier") == 0) {
            function->has_old_style_parameters = 1u;
            status = add_diagnostic(
                unit, QL_C_FRONTEND_DIAGNOSTIC_OLD_STYLE_FUNCTION,
                (uint32_t)function_index, nodes[index].view.range, kind,
                "K&R function definitions are outside the restricted-C contract",
                error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
    }
    return QL_STATUS_OK;
}

static ql_status analyze_function(
    ql_c_frontend_unit *unit, const char *source, size_t source_size,
    const ql_c_syntax_record *nodes, size_t node_count, size_t function_node,
    ql_error *error) {
    ql_c_function_record *function;
    size_t function_index = unit->function_count;
    size_t end = subtree_end(nodes, node_count, function_node);
    size_t declarator = direct_field_child(nodes, node_count, function_node,
                                           "declarator");
    size_t type_node = direct_field_child(nodes, node_count, function_node,
                                          "type");
    size_t body = direct_field_child(nodes, node_count, function_node, "body");
    size_t function_declarator = SIZE_MAX;
    size_t name_declarator = SIZE_MAX;
    size_t identifier = SIZE_MAX;
    size_t parameter_list = SIZE_MAX;
    size_t index;
    ql_status status;

    if (function_index > UINT32_MAX) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "too many C function definitions");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    status = grow_array(&unit->allocator, (void **)&unit->functions,
                        &unit->function_capacity, sizeof(*unit->functions),
                        function_index + 1u, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    function = &unit->functions[function_index];
    memset(function, 0, sizeof(*function));
    function->range = nodes[function_node].view.range;
    function->support = QL_C_FUNCTION_SUPPORTED;
    ++unit->function_count;

    if (declarator != SIZE_MAX) {
        size_t declarator_end = subtree_end(nodes, node_count, declarator);
        for (index = declarator; index < declarator_end; ++index) {
            if (strcmp(nodes[index].view.kind, "function_declarator") == 0) {
                size_t candidate_declarator = direct_field_child(
                    nodes, node_count, index, "declarator");
                if (candidate_declarator != SIZE_MAX &&
                    declarator_identifier(nodes, node_count,
                                          candidate_declarator) != SIZE_MAX) {
                    /* The deepest function declarator is the definition's
                       input signature. Outer layers belong to its return
                       declarator, as in a function returning a function
                       pointer. */
                    function_declarator = index;
                }
            }
        }
    }
    if (function_declarator != SIZE_MAX) {
        name_declarator = direct_field_child(
            nodes, node_count, function_declarator, "declarator");
        parameter_list = direct_field_child(
            nodes, node_count, function_declarator, "parameters");
        if (name_declarator != SIZE_MAX) {
            identifier = declarator_identifier(nodes, node_count,
                                               name_declarator);
        }
    }

    if (identifier != SIZE_MAX) {
        function->name = copy_source_range(
            &unit->allocator, source, source_size,
            nodes[identifier].view.range, 0);
    } else {
        function->name = unit->allocator.allocate(unit->allocator.user_data,
                                                  1u);
        if (function->name != NULL) {
            function->name[0] = '\0';
        }
    }
    if (function->name == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }

    if (body != SIZE_MAX) {
        ql_source_range signature_range = function->range;
        function->body_range = nodes[body].view.range;
        signature_range.end_byte = function->body_range.start_byte;
        signature_range.end_point = function->body_range.start_point;
        function->signature_spelling = copy_source_range(
            &unit->allocator, source, source_size, signature_range, 1);
    } else {
        function->signature_spelling = NULL;
    }
    if (function->signature_spelling == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }

    status = build_return_type_inventory(
        &unit->allocator, source, source_size, nodes, node_count,
        function_node, type_node, declarator, parameter_list,
        &function->return_type, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (function_declarator == SIZE_MAX || identifier == SIZE_MAX ||
        parameter_list == SIZE_MAX) {
        status = add_diagnostic(
            unit, QL_C_FRONTEND_DIAGNOSTIC_UNSUPPORTED_SIGNATURE,
            (uint32_t)function_index, nodes[function_node].view.range,
            nodes[function_node].view.kind,
            "function signature cannot be inventoried by the restricted-C frontend",
            error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    } else {
        status = analyze_parameters(
            unit, function, source, source_size, nodes, node_count,
            parameter_list, function_index, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }

    for (index = function_node + 1u; index < end; ++index) {
        const char *kind = nodes[index].view.kind;
        if (strcmp(kind, "variadic_parameter") == 0 ||
            starts_with(kind, "preproc_")) {
            continue;
        }
        if ((strcmp(kind, "function_definition") == 0 &&
             index != function_node) || unsupported_construct(kind)) {
            status = add_diagnostic(
                unit, QL_C_FRONTEND_DIAGNOSTIC_UNSUPPORTED_CONSTRUCT,
                (uint32_t)function_index, nodes[index].view.range, kind,
                "construct is outside the common restricted-C lowering contract",
                error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
    }
    return QL_STATUS_OK;
}

static ql_status classify_preprocessing(
    ql_c_frontend_unit *unit, const ql_c_syntax_record *nodes,
    size_t node_count, ql_error *error) {
    size_t index;
    ql_source_range first_range;
    const char *first_kind = NULL;
    ql_status status;

    memset(&first_range, 0, sizeof(first_range));
    for (index = 0u; index < node_count; ++index) {
        if (starts_with(nodes[index].view.kind, "preproc_")) {
            first_range = nodes[index].view.range;
            first_kind = nodes[index].view.kind;
            break;
        }
    }
    if (first_kind == NULL) {
        return QL_STATUS_OK;
    }
    if (unit->function_count == 0u) {
        return add_diagnostic(
            unit, QL_C_FRONTEND_DIAGNOSTIC_PREPROCESSING_REQUIRED,
            QL_C_FRONTEND_UNIT_DIAGNOSTIC, first_range, first_kind,
            "preprocessor directives require a separate preprocessing stage",
            error);
    }
    for (index = 0u; index < unit->function_count; ++index) {
        status = add_diagnostic(
            unit, QL_C_FRONTEND_DIAGNOSTIC_PREPROCESSING_REQUIRED,
            (uint32_t)index, first_range, first_kind,
            "preprocessor directives require a separate preprocessing stage",
            error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

static ql_status classify_duplicate_definitions(ql_c_frontend_unit *unit,
                                                ql_error *error) {
    size_t left;
    size_t right;
    ql_status status;

    for (left = 0u; left < unit->function_count; ++left) {
        for (right = left + 1u; right < unit->function_count; ++right) {
            if (strcmp(unit->functions[left].name,
                       unit->functions[right].name) != 0) {
                continue;
            }
            status = add_diagnostic(
                unit, QL_C_FRONTEND_DIAGNOSTIC_DUPLICATE_DEFINITION,
                (uint32_t)left, unit->functions[left].range,
                "function_definition",
                "function name has more than one definition", error);
            if (status != QL_STATUS_OK) {
                return status;
            }
            status = add_diagnostic(
                unit, QL_C_FRONTEND_DIAGNOSTIC_DUPLICATE_DEFINITION,
                (uint32_t)right, unit->functions[right].range,
                "function_definition",
                "function name has more than one definition", error);
            if (status != QL_STATUS_OK) {
                return status;
            }
        }
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_c_frontend_analyze(
    const ql_allocator *allocator, const char *source, size_t source_size,
    ql_c_frontend_unit **output, ql_error *error) {
    return ql_c_frontend_analyze_with_parser(allocator, NULL, source,
                                             source_size, output, error);
}

ql_status QL_CALL ql_c_frontend_analyze_with_parser(
    const ql_allocator *allocator, ql_c_parser *borrowed_parser,
    const char *source, size_t source_size, ql_c_frontend_unit **output,
    ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    ql_c_parser *owned_parser = NULL;
    ql_c_parser *parser = borrowed_parser;
    ql_c_syntax_tree *tree = NULL;
    ql_c_syntax_record *nodes = NULL;
    size_t node_count = 0u;
    ql_c_frontend_unit *unit = NULL;
    ql_status status;
    size_t index;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C frontend output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (!ql_allocator_is_valid(selected) ||
        (source == NULL && source_size != 0u)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "valid allocator and C source are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (source != NULL && memchr(source, '\0', source_size) != NULL) {
        ql_error_set(error, QL_STATUS_PARSE_ERROR,
                     "C source contains an embedded NUL byte");
        return QL_STATUS_PARSE_ERROR;
    }
    if (parser == NULL) {
        status = ql_c_parser_create(selected, &owned_parser, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        parser = owned_parser;
    }
    status = ql_c_parser_parse(parser, source, source_size, &tree, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = collect_syntax_nodes(selected, tree, &nodes, &node_count, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    if (ql_c_syntax_tree_has_errors(tree) != 0u) {
        for (index = 0u; index < node_count; ++index) {
            if ((nodes[index].view.flags &
                 (QL_C_SYNTAX_NODE_ERROR | QL_C_SYNTAX_NODE_MISSING)) != 0u) {
                ql_error_set(error, QL_STATUS_PARSE_ERROR,
                             "C syntax error near byte %u",
                             nodes[index].view.range.start_byte);
                status = QL_STATUS_PARSE_ERROR;
                goto cleanup;
            }
        }
        ql_error_set(error, QL_STATUS_PARSE_ERROR, "C syntax error");
        status = QL_STATUS_PARSE_ERROR;
        goto cleanup;
    }

    unit = selected->allocate(selected->user_data, sizeof(*unit));
    if (unit == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        status = QL_STATUS_OUT_OF_MEMORY;
        goto cleanup;
    }
    memset(unit, 0, sizeof(*unit));
    unit->allocator = *selected;
    unit->support = QL_C_FUNCTION_SUPPORTED;
    /* The unit owns the tree from here, including on the failure paths
       below, which is why the local handle is cleared. */
    unit->tree = tree;
    tree = NULL;

    for (index = 0u; index < node_count; ++index) {
        if (strcmp(nodes[index].view.kind, "function_definition") == 0 &&
            (nodes[index].parent == SIZE_MAX ||
             strcmp(nodes[nodes[index].parent].view.kind,
                    "translation_unit") == 0)) {
            status = analyze_function(unit, source != NULL ? source : "",
                                      source_size, nodes, node_count, index,
                                      error);
            if (status != QL_STATUS_OK) {
                goto cleanup;
            }
        }
    }
    status = classify_preprocessing(unit, nodes, node_count, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }
    status = classify_duplicate_definitions(unit, error);
    if (status != QL_STATUS_OK) {
        goto cleanup;
    }

    *output = unit;
    unit = NULL;
    ql_error_clear(error);
    status = QL_STATUS_OK;

cleanup:
    ql_c_frontend_unit_destroy(unit);
    selected->deallocate(selected->user_data, nodes);
    ql_c_syntax_tree_destroy(tree);
    /* A borrowed parser outlives the call; only one made here is freed. */
    ql_c_parser_destroy(owned_parser);
    return status;
}

ql_status QL_CALL ql_c_frontend_unit_borrow_tree(
    const ql_c_frontend_unit *unit, ql_c_syntax_tree **output,
    ql_error *error) {
    if (unit == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C frontend unit and tree output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = unit->tree;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_status validate_view(const void *view, size_t struct_size,
                               size_t required_size, const char *name,
                               ql_error *error) {
    if (view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "%s is required",
                     name);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (struct_size != 0u && struct_size < required_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH, "%s is too small", name);
        return QL_STATUS_ABI_MISMATCH;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_c_frontend_unit_get_view(
    const ql_c_frontend_unit *unit, ql_c_frontend_unit_view *view,
    ql_error *error) {
    ql_status status;

    if (unit == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C frontend unit is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_view(view, view != NULL ? view->struct_size : 0u,
                           sizeof(*view), "C frontend unit view", error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_C_FRONTEND_SCHEMA_VERSION;
    view->support = unit->support;
    view->function_count = unit->function_count;
    view->diagnostic_count = unit->diagnostic_count;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static ql_status fill_function_view(const ql_c_frontend_unit *unit,
                                    size_t index, ql_c_function_view *view,
                                    ql_error *error) {
    const ql_c_function_record *function;
    ql_status status = validate_view(
        view, view != NULL ? view->struct_size : 0u, sizeof(*view),
        "C function view", error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    if (index >= unit->function_count) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "C function index %zu does not exist", index);
        return QL_STATUS_NOT_FOUND;
    }
    function = &unit->functions[index];
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->index = (uint32_t)index;
    view->support = function->support;
    view->name = function->name;
    view->signature_spelling = function->signature_spelling;
    view->range = function->range;
    view->body_range = function->body_range;
    view->return_type = function->return_type;
    view->parameter_count = function->parameter_count;
    view->diagnostic_count = function->diagnostic_count;
    view->has_variadic_parameters = function->has_variadic_parameters;
    view->has_old_style_parameters = function->has_old_style_parameters;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_c_frontend_function_at(
    const ql_c_frontend_unit *unit, size_t index, ql_c_function_view *view,
    ql_error *error) {
    if (unit == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C frontend unit is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return fill_function_view(unit, index, view, error);
}

ql_status QL_CALL ql_c_frontend_select_function(
    const ql_c_frontend_unit *unit, const char *name, size_t name_size,
    ql_c_function_view *view, ql_error *error) {
    size_t index;
    size_t match = SIZE_MAX;

    if (unit == NULL || name == NULL || name_size == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C frontend unit and function name are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u; index < unit->function_count; ++index) {
        if (strlen(unit->functions[index].name) == name_size &&
            memcmp(unit->functions[index].name, name, name_size) == 0) {
            if (match != SIZE_MAX) {
                ql_error_set(error, QL_STATUS_ALREADY_EXISTS,
                             "C function name is ambiguous");
                return QL_STATUS_ALREADY_EXISTS;
            }
            match = index;
        }
    }
    if (match == SIZE_MAX) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "C function definition was not found");
        return QL_STATUS_NOT_FOUND;
    }
    return fill_function_view(unit, match, view, error);
}

ql_status QL_CALL ql_c_frontend_parameter_at(
    const ql_c_frontend_unit *unit, size_t function_index,
    size_t parameter_index, ql_c_parameter_view *view, ql_error *error) {
    const ql_c_parameter_record *parameter;
    ql_status status;

    if (unit == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C frontend unit is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_view(view, view != NULL ? view->struct_size : 0u,
                           sizeof(*view), "C parameter view", error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (function_index >= unit->function_count ||
        parameter_index >= unit->functions[function_index].parameter_count) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "C function parameter does not exist");
        return QL_STATUS_NOT_FOUND;
    }
    parameter = &unit->functions[function_index].parameters[parameter_index];
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->function_index = (uint32_t)function_index;
    view->parameter_index = (uint32_t)parameter_index;
    view->name = parameter->name;
    view->range = parameter->range;
    view->type = parameter->type;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static void fill_diagnostic_view(const ql_c_diagnostic_record *diagnostic,
                                 ql_c_frontend_diagnostic_view *view) {
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->code = diagnostic->code;
    view->function_index = diagnostic->function_index;
    view->range = diagnostic->range;
    view->construct_kind = diagnostic->construct_kind;
    view->message = diagnostic->message;
}

ql_status QL_CALL ql_c_frontend_diagnostic_at(
    const ql_c_frontend_unit *unit, size_t index,
    ql_c_frontend_diagnostic_view *view, ql_error *error) {
    ql_status status;

    if (unit == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C frontend unit is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_view(view, view != NULL ? view->struct_size : 0u,
                           sizeof(*view), "C frontend diagnostic view",
                           error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (index >= unit->diagnostic_count) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "C frontend diagnostic does not exist");
        return QL_STATUS_NOT_FOUND;
    }
    fill_diagnostic_view(&unit->diagnostics[index], view);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_c_frontend_function_diagnostic_at(
    const ql_c_frontend_unit *unit, size_t function_index, size_t index,
    ql_c_frontend_diagnostic_view *view, ql_error *error) {
    size_t diagnostic_index;
    size_t seen = 0u;
    ql_status status;

    if (unit == NULL || function_index >= unit->function_count) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "valid C function index is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_view(view, view != NULL ? view->struct_size : 0u,
                           sizeof(*view), "C frontend diagnostic view",
                           error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (diagnostic_index = 0u;
         diagnostic_index < unit->diagnostic_count; ++diagnostic_index) {
        if (unit->diagnostics[diagnostic_index].function_index !=
            (uint32_t)function_index) {
            continue;
        }
        if (seen++ == index) {
            fill_diagnostic_view(&unit->diagnostics[diagnostic_index], view);
            ql_error_clear(error);
            return QL_STATUS_OK;
        }
    }
    ql_error_set(error, QL_STATUS_NOT_FOUND,
                 "C function diagnostic does not exist");
    return QL_STATUS_NOT_FOUND;
}
