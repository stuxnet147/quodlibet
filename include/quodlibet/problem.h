#ifndef QUODLIBET_PROBLEM_H
#define QUODLIBET_PROBLEM_H

#include "quodlibet/artifact.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

#define QL_PROBLEM_SCHEMA_VERSION 1u
#define QL_PROBLEM_SCHEMA_VERSION_2 2u

typedef struct ql_problem ql_problem;

/* Input used to create a canonical, immutable quodlibet.problem artifact. */
typedef struct ql_problem_definition_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_semantic_contract_v1 contract;
    const char *left_source;
    size_t left_source_size;
    const char *left_function_name;
    size_t left_function_name_size;
    const char *right_source;
    size_t right_source_size;
    const char *right_function_name;
    size_t right_function_name_size;
    uint64_t reserved[4];
} ql_problem_definition_v1;

/* Pointers in this view remain valid until the owning ql_problem is released. */
typedef struct ql_problem_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_semantic_contract_v1 contract;
    const char *left_source;
    size_t left_source_size;
    const char *left_function_name;
    size_t left_function_name_size;
    const char *right_source;
    size_t right_source_size;
    const char *right_function_name;
    size_t right_function_name_size;
    ql_digest artifact_digest;
    uint64_t reserved[4];
} ql_problem_view_v1;

/* Schema v2 additions. Version 1 remains readable and unchanged; a v2 problem
   additionally binds both resolved source signatures, the argument
   correspondence between them, and the typed-precondition digest. */

/* Indices name arguments of the two source signatures. The correspondence must
   be a total bijection: an unmapped argument would be an unconstrained input
   that no relation could be stated over. */
typedef struct ql_problem_argument_binding_v1 {
    size_t struct_size;
    uint32_t left_index;
    uint32_t right_index;
    uint64_t reserved[2];
} ql_problem_argument_binding_v1;

/* The precondition in `contract` is type-checked against the LEFT signature;
   `argument_bindings` transports each argument index to the right side. Both
   signature artifacts are borrowed for the duration of the create call. */
typedef struct ql_problem_definition_v2 {
    size_t struct_size;
    uint32_t schema_version;
    ql_semantic_contract_v1 contract;
    const char *left_source;
    size_t left_source_size;
    const char *left_function_name;
    size_t left_function_name_size;
    const char *right_source;
    size_t right_source_size;
    const char *right_function_name;
    size_t right_function_name_size;
    const ql_artifact *left_signature;
    const ql_artifact *right_signature;
    const ql_problem_argument_binding_v1 *argument_bindings;
    size_t argument_binding_count;
    uint64_t reserved[4];
} ql_problem_definition_v2;

/* Pointers in this view remain valid until the owning ql_problem is released. */
typedef struct ql_problem_view_v2 {
    size_t struct_size;
    uint32_t schema_version;
    ql_semantic_contract_v1 contract;
    const char *left_source;
    size_t left_source_size;
    const char *left_function_name;
    size_t left_function_name_size;
    const char *right_source;
    size_t right_source_size;
    const char *right_function_name;
    size_t right_function_name_size;
    ql_digest left_signature_digest;
    ql_digest right_signature_digest;
    ql_digest precondition_digest;
    size_t argument_binding_count;
    ql_digest artifact_digest;
    uint64_t reserved[4];
} ql_problem_view_v2;

QL_API void QL_CALL ql_problem_definition_init(
    ql_problem_definition_v1 *definition);
QL_API void QL_CALL ql_problem_definition_v2_init(
    ql_problem_definition_v2 *definition);
QL_API ql_status QL_CALL ql_problem_artifact_create_v2(
    const ql_allocator *allocator,
    const ql_problem_definition_v2 *definition, ql_artifact **output,
    ql_error *error);
QL_API ql_status QL_CALL ql_problem_artifact_create(
    const ql_allocator *allocator,
    const ql_problem_definition_v1 *definition, ql_artifact **output,
    ql_error *error);

/* Opening validates the artifact kind, both schema versions, and all fields. */
QL_API ql_status QL_CALL ql_problem_open(const ql_allocator *allocator,
                                         const ql_artifact *artifact,
                                         ql_problem **output,
                                         ql_error *error);
QL_API void QL_CALL ql_problem_retain(ql_problem *problem);
QL_API void QL_CALL ql_problem_release(ql_problem *problem);
QL_API ql_status QL_CALL ql_problem_get_view(const ql_problem *problem,
                                             ql_problem_view_v1 *view,
                                             ql_error *error);
/* Returns QL_STATUS_SCHEMA_MISMATCH for a schema v1 problem. */
QL_API ql_status QL_CALL ql_problem_get_view_v2(const ql_problem *problem,
                                                ql_problem_view_v2 *view,
                                                ql_error *error);
QL_API ql_status QL_CALL ql_problem_argument_binding_at(
    const ql_problem *problem, size_t index,
    ql_problem_argument_binding_v1 *output, ql_error *error);
/* Borrowed source-signature artifacts rebuilt from the problem payload and
   verified against the recorded digests. Null for a schema v1 problem. */
QL_API const ql_artifact *QL_CALL ql_problem_left_signature_artifact(
    const ql_problem *problem);
QL_API const ql_artifact *QL_CALL ql_problem_right_signature_artifact(
    const ql_problem *problem);

/* The single gate every proof method must pass before it may emit any
   PROVED_* verdict.

   QL_STATUS_OK means the problem states what is being proved: both resolved
   source-signature digests, the argument correspondence, and the
   typed-precondition digest are bound into one artifact identity.

   A schema v1 problem never passes. Its precondition is not bound to either
   source signature and it records no argument correspondence, so it is
   storage and configuration rather than proof evidence. A v1 problem carrying
   a non-null precondition is reported separately from an empty one because
   that case is the one a caller is most likely to mistake for evidence. */
QL_API ql_status QL_CALL ql_problem_require_proof_binding(
    const ql_problem *problem, ql_error *error);

QL_EXTERN_C_END

#endif
