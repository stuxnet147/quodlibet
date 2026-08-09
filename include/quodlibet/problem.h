#ifndef QUODLIBET_PROBLEM_H
#define QUODLIBET_PROBLEM_H

#include "quodlibet/artifact.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

#define QL_PROBLEM_SCHEMA_VERSION 1u

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

QL_API void QL_CALL ql_problem_definition_init(
    ql_problem_definition_v1 *definition);
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

QL_EXTERN_C_END

#endif
