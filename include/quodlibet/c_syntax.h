#ifndef QUODLIBET_C_SYNTAX_H
#define QUODLIBET_C_SYNTAX_H

#include "quodlibet/allocator.h"
#include "quodlibet/status.h"

QL_EXTERN_C_BEGIN

typedef struct ql_c_parser ql_c_parser;
typedef struct ql_c_syntax_tree ql_c_syntax_tree;
typedef struct ql_c_syntax_cursor ql_c_syntax_cursor;

typedef struct ql_source_point {
    uint32_t row;
    uint32_t column;
} ql_source_point;

typedef struct ql_source_range {
    uint32_t start_byte;
    uint32_t end_byte;
    ql_source_point start_point;
    ql_source_point end_point;
} ql_source_range;

typedef enum ql_c_syntax_node_flag {
    QL_C_SYNTAX_NODE_NAMED = UINT32_C(1) << 0,
    QL_C_SYNTAX_NODE_MISSING = UINT32_C(1) << 1,
    QL_C_SYNTAX_NODE_EXTRA = UINT32_C(1) << 2,
    QL_C_SYNTAX_NODE_ERROR = UINT32_C(1) << 3,
    QL_C_SYNTAX_NODE_HAS_ERROR = UINT32_C(1) << 4
} ql_c_syntax_node_flag;

typedef struct ql_c_syntax_node_view {
    size_t struct_size;
    const char *kind;
    const char *field_name;
    ql_source_range range;
    uint32_t child_count;
    uint32_t named_child_count;
    uint32_t flags;
    uint32_t reserved[4];
} ql_c_syntax_node_view;

/* Parser, tree, and cursor objects are not safe for concurrent access. Create
   one parser per worker. Input is parsed as UTF-8 and is borrowed only for the
   duration of ql_c_parser_parse. */
QL_API ql_status QL_CALL ql_c_parser_create(
    const ql_allocator *allocator, ql_c_parser **output, ql_error *error);
QL_API void QL_CALL ql_c_parser_destroy(ql_c_parser *parser);
QL_API ql_status QL_CALL ql_c_parser_parse(
    ql_c_parser *parser, const char *source, size_t source_size,
    ql_c_syntax_tree **output, ql_error *error);

/* A recovered tree is returned even when it contains syntax errors. Such a
   parse still returns QL_STATUS_OK; inspect this predicate or node flags. */
QL_API uint32_t QL_CALL ql_c_syntax_tree_has_errors(
    const ql_c_syntax_tree *tree);
QL_API ql_status QL_CALL ql_c_syntax_tree_root(
    const ql_c_syntax_tree *tree, ql_c_syntax_node_view *view,
    ql_error *error);
QL_API void QL_CALL ql_c_syntax_tree_destroy(ql_c_syntax_tree *tree);

/* A cursor retains its tree. Kind and field-name strings in a node view are
   borrowed constants and must not be freed. Source points are zero-based. */
QL_API ql_status QL_CALL ql_c_syntax_cursor_create(
    ql_c_syntax_tree *tree, ql_c_syntax_cursor **output, ql_error *error);
QL_API void QL_CALL ql_c_syntax_cursor_destroy(ql_c_syntax_cursor *cursor);
QL_API void QL_CALL ql_c_syntax_cursor_reset(ql_c_syntax_cursor *cursor);
QL_API ql_status QL_CALL ql_c_syntax_cursor_current(
    const ql_c_syntax_cursor *cursor, ql_c_syntax_node_view *view,
    ql_error *error);
QL_API uint32_t QL_CALL ql_c_syntax_cursor_goto_first_child(
    ql_c_syntax_cursor *cursor);
QL_API uint32_t QL_CALL ql_c_syntax_cursor_goto_next_sibling(
    ql_c_syntax_cursor *cursor);
QL_API uint32_t QL_CALL ql_c_syntax_cursor_goto_parent(
    ql_c_syntax_cursor *cursor);

QL_EXTERN_C_END

#endif
