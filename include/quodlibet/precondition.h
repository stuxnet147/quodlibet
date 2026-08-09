#ifndef QUODLIBET_PRECONDITION_H
#define QUODLIBET_PRECONDITION_H

#include "quodlibet/allocator.h"
#include "quodlibet/hash.h"
#include "quodlibet/status.h"

QL_EXTERN_C_BEGIN

#define QL_SIGNATURE_SCHEMA_VERSION 1u
#ifndef QL_PRECONDITION_SCHEMA_VERSION
#  define QL_PRECONDITION_SCHEMA_VERSION 1u
#endif
#define QL_PRECONDITION_MAX_INTEGER_BITS 4096u
#define QL_PRECONDITION_NO_NODE UINT32_MAX

typedef struct ql_precondition ql_precondition;

/* Parameter indices, not source-language names, identify arguments in schema
   v1. This keeps a contract stable when either function renames a parameter. */
typedef enum ql_signature_argument_kind {
    QL_SIGNATURE_ARGUMENT_BOOL = 1,
    QL_SIGNATURE_ARGUMENT_SIGNED_INTEGER,
    QL_SIGNATURE_ARGUMENT_UNSIGNED_INTEGER,
    QL_SIGNATURE_ARGUMENT_POINTER
} ql_signature_argument_kind;

typedef struct ql_signature_argument_v1 {
    size_t struct_size;
    ql_signature_argument_kind kind;
    uint32_t bit_width;
    uint32_t address_space;
    uint64_t reserved[2];
} ql_signature_argument_v1;

/* The signature is borrowed for ql_precondition_parse() only. Pointer
   arguments must have bit_width equal to the byte-aligned pointer_width.
   Non-pointer arguments use address space zero. */
typedef struct ql_signature_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    uint32_t pointer_width;
    const ql_signature_argument_v1 *arguments;
    size_t argument_count;
    uint64_t reserved[2];
} ql_signature_view_v1;

typedef enum ql_precondition_value_kind {
    QL_PRECONDITION_VALUE_INVALID = 0,
    QL_PRECONDITION_VALUE_BOOL,
    QL_PRECONDITION_VALUE_SIGNED_INTEGER,
    QL_PRECONDITION_VALUE_UNSIGNED_INTEGER,
    QL_PRECONDITION_VALUE_POINTER,
    QL_PRECONDITION_VALUE_RANGE
} ql_precondition_value_kind;

typedef enum ql_precondition_node_kind {
    QL_PRECONDITION_NODE_BOOL = 1,
    QL_PRECONDITION_NODE_ARGUMENT,
    QL_PRECONDITION_NODE_INTEGER,
    QL_PRECONDITION_NODE_NOT,
    QL_PRECONDITION_NODE_AND,
    QL_PRECONDITION_NODE_OR,
    QL_PRECONDITION_NODE_IMPLIES,
    QL_PRECONDITION_NODE_EQUAL,
    QL_PRECONDITION_NODE_NOT_EQUAL,
    QL_PRECONDITION_NODE_SIGNED_LESS,
    QL_PRECONDITION_NODE_SIGNED_LESS_EQUAL,
    QL_PRECONDITION_NODE_SIGNED_GREATER,
    QL_PRECONDITION_NODE_SIGNED_GREATER_EQUAL,
    QL_PRECONDITION_NODE_UNSIGNED_LESS,
    QL_PRECONDITION_NODE_UNSIGNED_LESS_EQUAL,
    QL_PRECONDITION_NODE_UNSIGNED_GREATER,
    QL_PRECONDITION_NODE_UNSIGNED_GREATER_EQUAL,
    QL_PRECONDITION_NODE_SIGNED_ADD,
    QL_PRECONDITION_NODE_SIGNED_SUBTRACT,
    QL_PRECONDITION_NODE_SIGNED_MULTIPLY,
    QL_PRECONDITION_NODE_UNSIGNED_ADD,
    QL_PRECONDITION_NODE_UNSIGNED_SUBTRACT,
    QL_PRECONDITION_NODE_UNSIGNED_MULTIPLY,
    QL_PRECONDITION_NODE_RANGE,
    QL_PRECONDITION_NODE_VALID_RANGE,
    QL_PRECONDITION_NODE_ALIGNED,
    QL_PRECONDITION_NODE_DISJOINT
} ql_precondition_node_kind;

typedef enum ql_precondition_access {
    QL_PRECONDITION_ACCESS_READ = UINT32_C(1) << 0,
    QL_PRECONDITION_ACCESS_WRITE = UINT32_C(1) << 1
} ql_precondition_access;

/* Children point into the owning precondition and remain valid until it is
   destroyed. Integer values are canonical decimal strings. Arithmetic is a
   fixed-width bit-vector operation; signedness controls operand typing and
   comparison interpretation, while add/subtract/multiply wrap modulo 2^N. */
typedef struct ql_precondition_node_view_v1 {
    size_t struct_size;
    uint32_t index;
    ql_precondition_node_kind kind;
    ql_precondition_value_kind value_kind;
    uint32_t bit_width;
    uint32_t address_space;
    const uint32_t *children;
    size_t child_count;
    uint32_t argument_index;
    uint32_t boolean_value;
    const char *integer_value;
    size_t integer_value_size;
    uint32_t access;
    uint64_t alignment;
    uint32_t nullable;
    uint32_t alias_group;
    uint64_t reserved[2];
} ql_precondition_node_view_v1;

/* canonical_bytes is canonical schema-v1 JSON. signature_digest binds the
   argument types used for type checking. digest binds both that signature and
   canonical_bytes, so the same text cannot acquire a different cached meaning
   under another signature. */
typedef struct ql_precondition_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    uint32_t root_node;
    size_t node_count;
    const char *canonical_bytes;
    size_t canonical_size;
    ql_digest signature_digest;
    ql_digest digest;
    uint64_t reserved[2];
} ql_precondition_view_v1;

QL_API void QL_CALL ql_signature_argument_init(
    ql_signature_argument_v1 *argument);
QL_API void QL_CALL ql_signature_view_init(ql_signature_view_v1 *signature);
QL_API ql_status QL_CALL ql_signature_view_validate(
    const ql_signature_view_v1 *signature, ql_error *error);

/* Schema v1 JSON has exactly schema_version and expression at its root.

   Values:
     true | false
     {"op":"arg","index":N}
     {"op":"int","signed":B,"width":N,"value":"DECIMAL"}

   Boolean operators:
     not(value), and(args), or(args), implies(left,right)

   Binary typed operators:
     eq, ne
     slt, sle, sgt, sge, ult, ule, ugt, uge
     sadd, ssub, smul, uadd, usub, umul

   A range object has pointer, offset, and bytes. Its pointer is pointer-typed,
   offset is a signed integer of pointer width, and bytes is an unsigned integer
   of pointer width. A constant byte count must be greater than zero.

   Pointer predicates:
     valid_range(range, read, write, alignment, nullable, alias_group)
     aligned(pointer, offset, alignment)
     disjoint(left_range, right_range)

   alignment is a nonzero power of two. valid_range requires read or write.
   nullable makes the range predicate conditional when the pointer is null.
   Alias group zero is ungrouped; distinct nonzero groups promise disjoint
   storage, while equal groups may alias. A statically empty root precondition
   is rejected instead of permitting a vacuous proof. */
QL_API ql_status QL_CALL ql_precondition_parse(
    const ql_allocator *allocator, const char *json, size_t json_size,
    const ql_signature_view_v1 *signature, ql_precondition **output,
    ql_error *error);
QL_API void QL_CALL ql_precondition_destroy(ql_precondition *precondition);
QL_API ql_status QL_CALL ql_precondition_get_view(
    const ql_precondition *precondition, ql_precondition_view_v1 *view,
    ql_error *error);
QL_API ql_status QL_CALL ql_precondition_node_at(
    const ql_precondition *precondition, size_t index,
    ql_precondition_node_view_v1 *view, ql_error *error);

QL_EXTERN_C_END

#endif
