#ifndef QUODLIBET_C_FRONTEND_H
#define QUODLIBET_C_FRONTEND_H

#include "quodlibet/allocator.h"
#include "quodlibet/c_syntax.h"
#include "quodlibet/status.h"

QL_EXTERN_C_BEGIN

#define QL_C_FRONTEND_SCHEMA_VERSION 1u
#define QL_C_FRONTEND_UNIT_DIAGNOSTIC UINT32_MAX

typedef struct ql_c_frontend_unit ql_c_frontend_unit;

/* This is a syntactic eligibility result. QL_C_FUNCTION_SUPPORTED means that
   the definition passed the common restricted-C gate; it does not mean that
   identifiers have been resolved or that a proof IR has been produced. */
typedef enum ql_c_function_support {
    QL_C_FUNCTION_SUPPORTED = 0,
    QL_C_FUNCTION_UNSUPPORTED
} ql_c_function_support;

typedef enum ql_c_frontend_diagnostic_code {
    QL_C_FRONTEND_DIAGNOSTIC_PREPROCESSING_REQUIRED = 1,
    QL_C_FRONTEND_DIAGNOSTIC_VARIADIC_FUNCTION,
    QL_C_FRONTEND_DIAGNOSTIC_OLD_STYLE_FUNCTION,
    QL_C_FRONTEND_DIAGNOSTIC_UNSUPPORTED_CONSTRUCT,
    QL_C_FRONTEND_DIAGNOSTIC_UNSUPPORTED_SIGNATURE,
    QL_C_FRONTEND_DIAGNOSTIC_UNNAMED_PARAMETER,
    QL_C_FRONTEND_DIAGNOSTIC_DUPLICATE_DEFINITION
} ql_c_frontend_diagnostic_code;

typedef enum ql_c_type_base_kind {
    QL_C_TYPE_BASE_UNKNOWN = 0,
    QL_C_TYPE_BASE_VOID,
    QL_C_TYPE_BASE_BOOL,
    QL_C_TYPE_BASE_INTEGER,
    QL_C_TYPE_BASE_FLOAT,
    QL_C_TYPE_BASE_STRUCT,
    QL_C_TYPE_BASE_UNION,
    QL_C_TYPE_BASE_ENUM,
    QL_C_TYPE_BASE_TYPEDEF_NAME,
    QL_C_TYPE_BASE_ATOMIC,
    QL_C_TYPE_BASE_OTHER
} ql_c_type_base_kind;

typedef enum ql_c_type_shape {
    QL_C_TYPE_SHAPE_POINTER = UINT32_C(1) << 0,
    QL_C_TYPE_SHAPE_ARRAY = UINT32_C(1) << 1,
    QL_C_TYPE_SHAPE_FUNCTION = UINT32_C(1) << 2
} ql_c_type_shape;

typedef enum ql_c_type_qualifier {
    QL_C_TYPE_QUALIFIER_CONST = UINT32_C(1) << 0,
    QL_C_TYPE_QUALIFIER_VOLATILE = UINT32_C(1) << 1,
    QL_C_TYPE_QUALIFIER_RESTRICT = UINT32_C(1) << 2,
    QL_C_TYPE_QUALIFIER_ATOMIC = UINT32_C(1) << 3
} ql_c_type_qualifier;

/* Strings in all views are owned by the unit and remain valid until the unit
   is destroyed. The declarator spelling intentionally retains the identifier
   so that a later type checker can reconstruct pointer, array, and function
   binding without reparsing a normalized approximation. */
typedef struct ql_c_type_inventory_v1 {
    size_t struct_size;
    ql_c_type_base_kind base_kind;
    uint32_t shape;
    uint32_t qualifiers;
    uint32_t pointer_depth;
    uint32_t array_rank;
    const char *base_spelling;
    const char *declarator_spelling;
    ql_source_range base_range;
    ql_source_range declarator_range;
    uint64_t reserved[2];
} ql_c_type_inventory_v1;

typedef struct ql_c_frontend_unit_view {
    size_t struct_size;
    uint32_t schema_version;
    ql_c_function_support support;
    size_t function_count;
    size_t diagnostic_count;
    uint64_t reserved[2];
} ql_c_frontend_unit_view;

typedef struct ql_c_function_view {
    size_t struct_size;
    uint32_t index;
    ql_c_function_support support;
    const char *name;
    const char *signature_spelling;
    ql_source_range range;
    ql_source_range body_range;
    ql_c_type_inventory_v1 return_type;
    size_t parameter_count;
    size_t diagnostic_count;
    uint32_t has_variadic_parameters;
    uint32_t has_old_style_parameters;
    uint64_t reserved[2];
} ql_c_function_view;

typedef struct ql_c_parameter_view {
    size_t struct_size;
    uint32_t function_index;
    uint32_t parameter_index;
    const char *name;
    ql_source_range range;
    ql_c_type_inventory_v1 type;
    uint64_t reserved[2];
} ql_c_parameter_view;

typedef struct ql_c_frontend_diagnostic_view {
    size_t struct_size;
    ql_c_frontend_diagnostic_code code;
    uint32_t function_index;
    ql_source_range range;
    const char *construct_kind;
    const char *message;
    uint64_t reserved[2];
} ql_c_frontend_diagnostic_view;

/* The input is parsed as one translation unit. Preprocessor directives are
   detected but not expanded; a unit containing one is classified unsupported
   until the caller supplies preprocessed source. */
QL_API ql_status QL_CALL ql_c_frontend_analyze(
    const ql_allocator *allocator, const char *source, size_t source_size,
    ql_c_frontend_unit **output, ql_error *error);

/* The same analysis against a parser the caller owns and keeps. Creating a
   Tree-sitter parser costs more than parsing a short function, so a caller
   working through a corpus reuses one instead of paying for it per unit. A
   null parser makes this identical to ql_c_frontend_analyze.

   The parser is borrowed for the call and is neither reset nor destroyed. It
   must not be used from another thread while this runs. */
QL_API ql_status QL_CALL ql_c_frontend_analyze_with_parser(
    const ql_allocator *allocator, ql_c_parser *parser, const char *source,
    size_t source_size, ql_c_frontend_unit **output, ql_error *error);

QL_API void QL_CALL ql_c_frontend_unit_destroy(ql_c_frontend_unit *unit);

/* The parse this unit was built from, borrowed. A caller that goes on to
   lower would otherwise parse the same source a second time, which is the
   whole cost of the frontend for a short function.

   The tree belongs to the unit and stays valid until the unit is destroyed.
   Destroying it, or using it after the unit is gone, is a mistake; hand it to
   ql_c_lower_selected_function_with_tree and let the unit outlive that call.
   Analysis of a unit that failed to parse never produces a unit at all, so a
   unit that exists always has a tree. */
QL_API ql_status QL_CALL ql_c_frontend_unit_borrow_tree(
    const ql_c_frontend_unit *unit, ql_c_syntax_tree **output,
    ql_error *error);

QL_API ql_status QL_CALL ql_c_frontend_unit_get_view(
    const ql_c_frontend_unit *unit, ql_c_frontend_unit_view *view,
    ql_error *error);
QL_API ql_status QL_CALL ql_c_frontend_function_at(
    const ql_c_frontend_unit *unit, size_t index, ql_c_function_view *view,
    ql_error *error);
QL_API ql_status QL_CALL ql_c_frontend_select_function(
    const ql_c_frontend_unit *unit, const char *name, size_t name_size,
    ql_c_function_view *view, ql_error *error);
QL_API ql_status QL_CALL ql_c_frontend_parameter_at(
    const ql_c_frontend_unit *unit, size_t function_index,
    size_t parameter_index, ql_c_parameter_view *view, ql_error *error);

QL_API ql_status QL_CALL ql_c_frontend_diagnostic_at(
    const ql_c_frontend_unit *unit, size_t index,
    ql_c_frontend_diagnostic_view *view, ql_error *error);
QL_API ql_status QL_CALL ql_c_frontend_function_diagnostic_at(
    const ql_c_frontend_unit *unit, size_t function_index, size_t index,
    ql_c_frontend_diagnostic_view *view, ql_error *error);

QL_EXTERN_C_END

#endif
