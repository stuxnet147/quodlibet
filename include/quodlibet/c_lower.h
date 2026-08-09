#ifndef QUODLIBET_C_LOWER_H
#define QUODLIBET_C_LOWER_H

#include "quodlibet/c_frontend.h"
#include "quodlibet/ir.h"

QL_EXTERN_C_BEGIN

#define QL_C_LOWER_SCHEMA_VERSION 1u

typedef struct ql_c_lower_result ql_c_lower_result;

/* A syntactically accepted Tree-sitter function is not necessarily supported
   by the semantic lowering. UNKNOWN is a logical boundary: no proof method may
   treat it as an IR for the input function. */
typedef enum ql_c_lower_support {
    QL_C_LOWER_SUPPORTED = 0,
    QL_C_LOWER_UNKNOWN
} ql_c_lower_support;

typedef enum ql_c_lower_diagnostic_code {
    QL_C_LOWER_DIAGNOSTIC_FRONTEND_UNSUPPORTED = 1,
    QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE,
    QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER,
    QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL,
    QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_LOOP,
    QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_VOLATILE_OR_ATOMIC,
    QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CONTROL_FLOW,
    QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION,
    QL_C_LOWER_DIAGNOSTIC_INVALID_DECLARATION,
    QL_C_LOWER_DIAGNOSTIC_UNDECLARED_IDENTIFIER,
    QL_C_LOWER_DIAGNOSTIC_UNINITIALIZED_READ,
    QL_C_LOWER_DIAGNOSTIC_DUPLICATE_DECLARATION,
    QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR,
    QL_C_LOWER_DIAGNOSTIC_MISSING_RETURN,
    QL_C_LOWER_DIAGNOSTIC_INTEGER_LITERAL_OUT_OF_RANGE
} ql_c_lower_diagnostic_code;

typedef struct ql_c_lower_result_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_c_lower_support support;
    size_t diagnostic_count;
    /* Borrowed from the result. Null exactly when support is UNKNOWN. */
    const ql_artifact *ir_artifact;
    uint64_t reserved[4];
} ql_c_lower_result_view_v1;

typedef struct ql_c_lower_diagnostic_view_v1 {
    size_t struct_size;
    ql_c_lower_diagnostic_code code;
    ql_source_range range;
    const char *construct_kind;
    const char *message;
    uint64_t reserved[2];
} ql_c_lower_diagnostic_view_v1;

/* The unit and selected function must describe source exactly. The initial
   vertical slice accepts loop-free integer/_Bool C under ASM2C_GNU_V1. A
   semantic limitation returns QL_STATUS_OK with an UNKNOWN result and a
   diagnostic; status failures are reserved for API, allocation, and malformed
   syntax failures. */
QL_API ql_status QL_CALL ql_c_lower_selected_function(
    const ql_allocator *allocator, const char *source, size_t source_size,
    const ql_c_frontend_unit *unit, const ql_c_function_view *function,
    ql_c_lower_result **output, ql_error *error);

/* The same lowering against a syntax tree the caller already has. Lowering
   otherwise reparses the source it was just handed, which doubles the parse
   for one function and multiplies it for a unit with several.

   `tree` must be the parse of exactly this source; the range checks the
   lowering already performs against the selected function reject a tree of
   anything else rather than lowering the wrong text. The tree is borrowed
   for the call and is not destroyed. A null tree makes this identical to
   ql_c_lower_selected_function. */
QL_API ql_status QL_CALL ql_c_lower_selected_function_with_tree(
    const ql_allocator *allocator, const char *source, size_t source_size,
    const ql_c_frontend_unit *unit, const ql_c_function_view *function,
    ql_c_syntax_tree *tree, ql_c_lower_result **output, ql_error *error);
QL_API void QL_CALL ql_c_lower_result_destroy(ql_c_lower_result *result);
QL_API ql_status QL_CALL ql_c_lower_result_get_view(
    const ql_c_lower_result *result, ql_c_lower_result_view_v1 *view,
    ql_error *error);
QL_API ql_status QL_CALL ql_c_lower_result_diagnostic_at(
    const ql_c_lower_result *result, size_t index,
    ql_c_lower_diagnostic_view_v1 *view, ql_error *error);

QL_EXTERN_C_END

#endif
