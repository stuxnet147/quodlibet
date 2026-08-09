#include "quodlibet/c_syntax.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-c.h>

struct ql_c_parser {
    ql_allocator allocator;
    TSParser *parser;
};

struct ql_c_syntax_tree {
    atomic_uint reference_count;
    ql_allocator allocator;
    TSTree *tree;
};

struct ql_c_syntax_cursor {
    ql_allocator allocator;
    ql_c_syntax_tree *tree;
    TSTreeCursor cursor;
};

static const ql_allocator *select_allocator(const ql_allocator *allocator) {
    return allocator != NULL ? allocator : ql_default_allocator();
}

static ql_status fill_node_view(TSNode node, const char *field_name,
                                ql_c_syntax_node_view *view,
                                ql_error *error) {
    TSPoint start;
    TSPoint end;
    uint32_t flags = 0u;

    if (view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C syntax node view is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view->struct_size != 0u &&
        view->struct_size < sizeof(ql_c_syntax_node_view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "C syntax node view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (ts_node_is_null(node)) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "Tree-sitter returned a null syntax node");
        return QL_STATUS_INTERNAL_ERROR;
    }

    start = ts_node_start_point(node);
    end = ts_node_end_point(node);
    if (ts_node_is_named(node)) {
        flags |= QL_C_SYNTAX_NODE_NAMED;
    }
    if (ts_node_is_missing(node)) {
        flags |= QL_C_SYNTAX_NODE_MISSING;
    }
    if (ts_node_is_extra(node)) {
        flags |= QL_C_SYNTAX_NODE_EXTRA;
    }
    if (ts_node_is_error(node)) {
        flags |= QL_C_SYNTAX_NODE_ERROR;
    }
    if (ts_node_has_error(node)) {
        flags |= QL_C_SYNTAX_NODE_HAS_ERROR;
    }

    view->struct_size = sizeof(*view);
    view->kind = ts_node_type(node);
    view->field_name = field_name;
    view->range.start_byte = ts_node_start_byte(node);
    view->range.end_byte = ts_node_end_byte(node);
    view->range.start_point.row = start.row;
    view->range.start_point.column = start.column;
    view->range.end_point.row = end.row;
    view->range.end_point.column = end.column;
    view->child_count = ts_node_child_count(node);
    view->named_child_count = ts_node_named_child_count(node);
    view->flags = flags;
    memset(view->reserved, 0, sizeof(view->reserved));
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static void retain_tree(ql_c_syntax_tree *tree) {
    (void)atomic_fetch_add_explicit(&tree->reference_count, 1u,
                                    memory_order_relaxed);
}

ql_status QL_CALL ql_c_parser_create(const ql_allocator *allocator,
                                     ql_c_parser **output,
                                     ql_error *error) {
    const ql_allocator *selected = select_allocator(allocator);
    const TSLanguage *language;
    ql_c_parser *parser;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C parser output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }

    parser = selected->allocate(selected->user_data, sizeof(*parser));
    if (parser == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(parser, 0, sizeof(*parser));
    parser->allocator = *selected;
    parser->parser = ts_parser_new();
    if (parser->parser == NULL) {
        selected->deallocate(selected->user_data, parser);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }

    language = tree_sitter_c();
    if (language == NULL || !ts_parser_set_language(parser->parser, language)) {
        uint32_t language_version =
            language != NULL ? ts_language_abi_version(language) : 0u;
        ts_parser_delete(parser->parser);
        selected->deallocate(selected->user_data, parser);
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "Tree-sitter C grammar ABI %u is incompatible with "
                     "runtime range %u..%u",
                     language_version,
                     (uint32_t)TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION,
                     (uint32_t)TREE_SITTER_LANGUAGE_VERSION);
        return QL_STATUS_ABI_MISMATCH;
    }

    *output = parser;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_c_parser_destroy(ql_c_parser *parser) {
    ql_allocator allocator;

    if (parser == NULL) {
        return;
    }
    allocator = parser->allocator;
    ts_parser_delete(parser->parser);
    allocator.deallocate(allocator.user_data, parser);
}

ql_status QL_CALL ql_c_parser_parse(ql_c_parser *parser, const char *source,
                                    size_t source_size,
                                    ql_c_syntax_tree **output,
                                    ql_error *error) {
    ql_c_syntax_tree *tree;
    TSTree *parsed;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C syntax tree output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (parser == NULL || (source == NULL && source_size != 0u)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C parser and valid source are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (source_size > UINT32_MAX) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C source exceeds Tree-sitter's 32-bit input limit");
        return QL_STATUS_INVALID_ARGUMENT;
    }

    ts_parser_reset(parser->parser);
    parsed = ts_parser_parse_string(parser->parser, NULL,
                                    source != NULL ? source : "",
                                    (uint32_t)source_size);
    if (parsed == NULL) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "Tree-sitter failed to produce a C syntax tree");
        return QL_STATUS_INTERNAL_ERROR;
    }
    tree = parser->allocator.allocate(parser->allocator.user_data,
                                      sizeof(*tree));
    if (tree == NULL) {
        ts_tree_delete(parsed);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(tree, 0, sizeof(*tree));
    atomic_init(&tree->reference_count, 1u);
    tree->allocator = parser->allocator;
    tree->tree = parsed;
    *output = tree;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

uint32_t QL_CALL ql_c_syntax_tree_has_errors(
    const ql_c_syntax_tree *tree) {
    return tree != NULL && ts_node_has_error(ts_tree_root_node(tree->tree))
               ? 1u
               : 0u;
}

ql_status QL_CALL ql_c_syntax_tree_root(const ql_c_syntax_tree *tree,
                                        ql_c_syntax_node_view *view,
                                        ql_error *error) {
    if (tree == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C syntax tree is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return fill_node_view(ts_tree_root_node(tree->tree), NULL, view, error);
}

void QL_CALL ql_c_syntax_tree_destroy(ql_c_syntax_tree *tree) {
    ql_allocator allocator;

    if (tree == NULL ||
        atomic_fetch_sub_explicit(&tree->reference_count, 1u,
                                  memory_order_acq_rel) != 1u) {
        return;
    }
    allocator = tree->allocator;
    ts_tree_delete(tree->tree);
    allocator.deallocate(allocator.user_data, tree);
}

ql_status QL_CALL ql_c_syntax_cursor_create(ql_c_syntax_tree *tree,
                                            ql_c_syntax_cursor **output,
                                            ql_error *error) {
    ql_c_syntax_cursor *cursor;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C syntax cursor output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (tree == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C syntax tree is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    cursor = tree->allocator.allocate(tree->allocator.user_data,
                                      sizeof(*cursor));
    if (cursor == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    cursor->allocator = tree->allocator;
    cursor->tree = tree;
    cursor->cursor = ts_tree_cursor_new(ts_tree_root_node(tree->tree));
    retain_tree(tree);
    *output = cursor;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_c_syntax_cursor_destroy(ql_c_syntax_cursor *cursor) {
    ql_allocator allocator;

    if (cursor == NULL) {
        return;
    }
    allocator = cursor->allocator;
    ts_tree_cursor_delete(&cursor->cursor);
    ql_c_syntax_tree_destroy(cursor->tree);
    allocator.deallocate(allocator.user_data, cursor);
}

void QL_CALL ql_c_syntax_cursor_reset(ql_c_syntax_cursor *cursor) {
    if (cursor != NULL) {
        ts_tree_cursor_reset(&cursor->cursor,
                             ts_tree_root_node(cursor->tree->tree));
    }
}

ql_status QL_CALL ql_c_syntax_cursor_current(
    const ql_c_syntax_cursor *cursor, ql_c_syntax_node_view *view,
    ql_error *error) {
    if (cursor == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "C syntax cursor is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return fill_node_view(
        ts_tree_cursor_current_node(&cursor->cursor),
        ts_tree_cursor_current_field_name(&cursor->cursor), view, error);
}

uint32_t QL_CALL ql_c_syntax_cursor_goto_first_child(
    ql_c_syntax_cursor *cursor) {
    return cursor != NULL && ts_tree_cursor_goto_first_child(&cursor->cursor)
               ? 1u
               : 0u;
}

uint32_t QL_CALL ql_c_syntax_cursor_goto_next_sibling(
    ql_c_syntax_cursor *cursor) {
    return cursor != NULL && ts_tree_cursor_goto_next_sibling(&cursor->cursor)
               ? 1u
               : 0u;
}

uint32_t QL_CALL ql_c_syntax_cursor_goto_parent(
    ql_c_syntax_cursor *cursor) {
    return cursor != NULL && ts_tree_cursor_goto_parent(&cursor->cursor)
               ? 1u
               : 0u;
}
