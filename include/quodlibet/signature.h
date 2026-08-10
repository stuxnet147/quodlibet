#ifndef QUODLIBET_SIGNATURE_H
#define QUODLIBET_SIGNATURE_H

#include "quodlibet/artifact.h"
#include "quodlibet/c_frontend.h"
#include "quodlibet/ir.h"
#include "quodlibet/precondition.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

#define QL_SOURCE_SIGNATURE_SCHEMA_VERSION 1u
#define QL_ARTIFACT_KIND_SOURCE_SIGNATURE "quodlibet.source-signature"
#define QL_SOURCE_SIGNATURE_MAX_ARGUMENTS 64u

typedef struct ql_source_signature ql_source_signature;

/* IR bit-vector types are signless, so source signedness, pointer identity,
   and the ABI profile cannot be recovered from a quodlibet.ir artifact. This
   artifact is the separately versioned record of that information. Values 1
   through 4 deliberately match ql_signature_argument_kind so non-floating
   arguments can type-check a precondition without a second vocabulary. */
typedef enum ql_source_type_kind {
    QL_SOURCE_TYPE_VOID = 0,
    QL_SOURCE_TYPE_BOOL = 1,
    QL_SOURCE_TYPE_SIGNED_INTEGER = 2,
    QL_SOURCE_TYPE_UNSIGNED_INTEGER = 3,
    QL_SOURCE_TYPE_POINTER = 4,
    QL_SOURCE_TYPE_FLOAT = 5
} ql_source_type_kind;

/* Bit values match ql_c_type_qualifier. */
typedef enum ql_source_type_qualifier {
    QL_SOURCE_TYPE_QUALIFIER_CONST = UINT32_C(1) << 0,
    QL_SOURCE_TYPE_QUALIFIER_VOLATILE = UINT32_C(1) << 1,
    QL_SOURCE_TYPE_QUALIFIER_RESTRICT = UINT32_C(1) << 2,
    QL_SOURCE_TYPE_QUALIFIER_ATOMIC = UINT32_C(1) << 3
} ql_source_type_qualifier;

#define QL_SOURCE_TYPE_QUALIFIER_ALL                                     \
    (QL_SOURCE_TYPE_QUALIFIER_CONST | QL_SOURCE_TYPE_QUALIFIER_VOLATILE | \
     QL_SOURCE_TYPE_QUALIFIER_RESTRICT | QL_SOURCE_TYPE_QUALIFIER_ATOMIC)

/* A void type uses bit_width zero. A bool uses bit_width one. An integer or
   floating type uses its exact source width. A pointer uses the signature's
   pointer_width and may name a non-default address space. pointer_depth is
   retained so that `int *` and `int **` remain distinguishable inputs to a
   later memory model. */
typedef struct ql_source_type_v1 {
    size_t struct_size;
    ql_source_type_kind kind;
    uint32_t bit_width;
    uint32_t address_space;
    uint32_t qualifiers;
    uint32_t pointer_depth;
    uint64_t reserved[2];
} ql_source_type_v1;

typedef struct ql_source_signature_definition_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_c_dialect_profile c_dialect;
    ql_target_abi target_abi;
    uint32_t pointer_width;
    const char *function_name;
    size_t function_name_size;
    ql_source_type_v1 return_type;
    const ql_source_type_v1 *arguments;
    size_t argument_count;
    uint64_t reserved[4];
} ql_source_signature_definition_v1;

/* Pointers in this view remain valid until the owning signature is released. */
typedef struct ql_source_signature_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_c_dialect_profile c_dialect;
    ql_target_abi target_abi;
    uint32_t pointer_width;
    const char *function_name;
    size_t function_name_size;
    ql_source_type_v1 return_type;
    size_t argument_count;
    ql_digest artifact_digest;
    uint64_t reserved[4];
} ql_source_signature_view_v1;

QL_API void QL_CALL ql_source_type_init(ql_source_type_v1 *type,
                                        ql_source_type_kind kind);
QL_API void QL_CALL ql_source_signature_definition_init(
    ql_source_signature_definition_v1 *definition);
QL_API ql_status QL_CALL ql_source_signature_artifact_create(
    const ql_allocator *allocator,
    const ql_source_signature_definition_v1 *definition,
    ql_artifact **output, ql_error *error);

/* Opening validates the artifact kind, the schema version, every type field,
   and the ABI profile. */
QL_API ql_status QL_CALL ql_source_signature_open(
    const ql_allocator *allocator, const ql_artifact *artifact,
    ql_source_signature **output, ql_error *error);
QL_API void QL_CALL ql_source_signature_retain(
    ql_source_signature *signature);
QL_API void QL_CALL ql_source_signature_release(
    ql_source_signature *signature);
QL_API ql_status QL_CALL ql_source_signature_get_view(
    const ql_source_signature *signature,
    ql_source_signature_view_v1 *view, ql_error *error);
QL_API ql_status QL_CALL ql_source_signature_argument_at(
    const ql_source_signature *signature, size_t index,
    ql_source_type_v1 *output, ql_error *error);
/* Borrowed from the signature; valid until its final release. */
QL_API const ql_artifact *QL_CALL ql_source_signature_artifact(
    const ql_source_signature *signature);

/* Fills a precondition signature view backed by caller storage, so a typed
   precondition is checked against exactly this artifact's argument types.
   Schema v1 preconditions have no floating expression vocabulary, so a
   floating argument produces QL_STATUS_TYPE_MISMATCH. */
QL_API ql_status QL_CALL ql_source_signature_precondition_view(
    const ql_source_signature *signature, ql_signature_view_v1 *view,
    ql_signature_argument_v1 *storage, size_t storage_capacity,
    ql_error *error);

/* Derives the signature of one analyzed C function. The base-spelling mapping
   is the frozen ASM2C_GNU_V1 table; it is deliberately independent of the
   lowering so that ql_source_signature_bind_ir() is a real cross-check rather
   than a restatement. A construct outside the frozen table is an explicit
   QL_STATUS_TYPE_MISMATCH, never a guessed width or signedness.

   This form resolves no typedef, so a parameter spelled with a name this unit
   declared is refused. Prefer the v2 form below, which is given the source and
   resolves those names. */
QL_API ql_status QL_CALL ql_source_signature_from_c_function(
    const ql_allocator *allocator, const ql_c_frontend_unit *unit,
    const ql_c_function_view *function, ql_c_dialect_profile c_dialect,
    ql_target_abi target_abi, ql_artifact **output, ql_error *error);
/* The recommended form. Given the unit's source it resolves a typedef name to
   the type this unit declared for it, following chains and reporting a
   typedef of a pointer, array, or function as a pointer.

   Only a name this unit actually declares is resolved: giving a meaning to an
   undeclared name would be a guess about a type, and a wrong guess about a
   type is a wrong answer about the function. The resolution walks the syntax
   tree itself rather than calling the lowering, which is what keeps
   ql_source_signature_bind_ir() an independent check. */
QL_API ql_status QL_CALL ql_source_signature_from_c_function_v2(
    const ql_allocator *allocator, const ql_c_frontend_unit *unit,
    const ql_c_function_view *function, const char *source,
    size_t source_size, ql_c_dialect_profile c_dialect,
    ql_target_abi target_abi, ql_artifact **output, ql_error *error);

/* Rejects any disagreement between the signature and a lowered IR function:
   name, parameter count, per-parameter kind and width, and return type. */
QL_API ql_status QL_CALL ql_source_signature_bind_ir(
    const ql_source_signature *signature, const ql_ir *ir, ql_error *error);

/* Two arguments correspond only when kind, width, address space, and pointer
   depth agree. Signedness is part of the kind, so a signed and an unsigned
   argument of equal width never correspond. */
QL_API ql_status QL_CALL ql_source_type_compatible(
    const ql_source_type_v1 *left, const ql_source_type_v1 *right,
    ql_error *error);

QL_EXTERN_C_END

#endif
