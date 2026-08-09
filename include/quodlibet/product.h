#ifndef QUODLIBET_PRODUCT_H
#define QUODLIBET_PRODUCT_H

#include "quodlibet/ir.h"
#include "quodlibet/problem.h"
#include "quodlibet/signature.h"
#include "quodlibet/solver.h"

QL_EXTERN_C_BEGIN

#define QL_PRODUCT_SCHEMA_VERSION 1u

typedef struct ql_product_query ql_product_query;

/* One shared symbolic input feeding both sides through the problem's argument
   correspondence. `symbol` is the SMT-LIB constant a model assigns. */
typedef struct ql_product_input_v1 {
    size_t struct_size;
    uint32_t index;
    uint32_t left_parameter;
    uint32_t right_parameter;
    ql_source_type_kind kind;
    uint32_t bit_width;
    const char *symbol;
    uint64_t reserved[2];
} ql_product_input_v1;

typedef struct ql_product_query_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_relation relation;
    ql_ub_policy ub_policy;
    /* Exactly the contract observations this encoding constrains. Building
       fails rather than silently dropping an axis, so this always equals the
       contract's requested observations. */
    uint64_t covered_observations;
    ql_solver_logic logic;
    size_t input_count;
    uint32_t maximum_bv_width;
    ql_ir_type_kind return_type_kind;
    uint32_t return_bit_width;
    ql_digest problem_digest;
    ql_digest prefix_digest;
    ql_digest violation_digest;
    ql_digest domain_digest;
    uint64_t reserved[4];
} ql_product_query_view_v1;

/* Builds the relational miter for one problem and its two lowered functions.
   The problem must use schema v2: without the recorded argument
   correspondence there is no stated relation between the two input lists to
   encode. Both signatures are bound to their IR before anything is emitted.

   Unsupported territory is refused, never narrowed: memory, calls, volatile,
   atomic, or I/O effects, non-scalar IR types, opcodes outside the loop-free
   scalar fragment, and pointer predicates in the precondition all return
   QL_STATUS_TYPE_MISMATCH naming the first unsupported axis. */
QL_API ql_status QL_CALL ql_product_query_build(
    const ql_allocator *allocator, const ql_problem *problem,
    const ql_ir *left_ir, const ql_ir *right_ir, ql_product_query **output,
    ql_error *error);
QL_API void QL_CALL ql_product_query_destroy(ql_product_query *query);
QL_API ql_status QL_CALL ql_product_query_get_view(
    const ql_product_query *query, ql_product_query_view_v1 *view,
    ql_error *error);
QL_API ql_status QL_CALL ql_product_query_input_at(
    const ql_product_query *query, size_t index,
    ql_product_input_v1 *output, ql_error *error);

/* Declarations and definitions shared by both terminal queries, including the
   `quodlibet_violation` and `quodlibet_domain` Boolean definitions. */
QL_API const ql_artifact *QL_CALL ql_product_query_prefix_artifact(
    const ql_product_query *query);
/* Asserts that the selected relation is violated. SAT is a candidate
   counterexample and must be decoded and replayed before it means anything. */
QL_API const ql_artifact *QL_CALL ql_product_query_violation_artifact(
    const ql_product_query *query);
/* Asserts that the comparison domain is inhabited. UNSAT here means the
   precondition and UB policy leave nothing to compare, so an UNSAT miter
   would be vacuous rather than a proof. */
QL_API const ql_artifact *QL_CALL ql_product_query_domain_artifact(
    const ql_product_query *query);

QL_EXTERN_C_END

#endif
