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

/* One storage region both sides receive. The object table is derived from the
   pointer arguments in source order on each side and then matched through the
   problem's argument correspondence, so the two functions share one table
   without exchanging anything. `base_symbol` and `size_symbol` are the shared
   SMT-LIB constants; `left_base_parameter` and `right_base_parameter` are the
   IR parameter ordinals of that object's base on each side, with its size at
   the next ordinal. */
typedef struct ql_product_object_v1 {
    size_t struct_size;
    uint32_t index;
    uint32_t address_width;
    uint32_t left_base_parameter;
    uint32_t right_base_parameter;
    const char *base_symbol;
    const char *size_symbol;
    uint64_t reserved[2];
} ql_product_object_v1;

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

   The flat memory model is encoded as one shared byte array plus the shared
   object base and size constants; the model's standing constraints and every
   access-definedness predicate are carried over from the IR's own ASSUME and
   UB_GUARD instructions rather than restated here.

   Unsupported territory is refused, never narrowed: calls, volatile, atomic,
   or I/O effects, aggregate or floating IR types, opcodes outside the encoded
   fragment, accesses whose width is not a whole number of bytes, memory
   observation modes finer than the final reachable state, and pointer
   predicates in the precondition all return QL_STATUS_TYPE_MISMATCH naming the
   first unsupported axis. */
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
/* Zero when neither side touches memory. Both sides always agree, because the
   table follows from the two signatures the problem already binds. */
QL_API size_t QL_CALL ql_product_query_object_count(
    const ql_product_query *query);
QL_API ql_status QL_CALL ql_product_query_object_at(
    const ql_product_query *query, size_t index,
    ql_product_object_v1 *output, ql_error *error);
/* The shared initial-memory constant, or null when no object exists. Both
   sides start from this same memory state; its IR parameter ordinal is the
   argument count on either side. */
QL_API const char *QL_CALL ql_product_query_memory_symbol(
    const ql_product_query *query);

/* Declarations and definitions shared by both terminal queries, including the
   `quodlibet_violation` and `quodlibet_domain` Boolean definitions. */
QL_API const ql_artifact *QL_CALL ql_product_query_prefix_artifact(
    const ql_product_query *query);
/* Asserts that the selected relation is violated. SAT is a candidate
   counterexample and must be decoded and replayed before it means anything. */
QL_API const ql_artifact *QL_CALL ql_product_query_violation_artifact(
    const ql_product_query *query);
/* The same violation claim with every object bounded to a size a concrete
   replay can materialize. Null when the query has no objects.

   SAT here is still a genuine violation, so a model it yields may be replayed
   and reported as a counterexample. UNSAT here proves nothing at all: it says
   only that no small-object violation exists, and it must never be promoted.
   Use it solely to find a replayable model after the unbounded query returned
   one that is too large to run. */
QL_API const ql_artifact *QL_CALL ql_product_query_bounded_violation_artifact(
    const ql_product_query *query);
/* Asserts that the comparison domain is inhabited. UNSAT here means the
   precondition and UB policy leave nothing to compare, so an UNSAT miter
   would be vacuous rather than a proof. */
QL_API const ql_artifact *QL_CALL ql_product_query_domain_artifact(
    const ql_product_query *query);

QL_EXTERN_C_END

#endif
