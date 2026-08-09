#ifndef QUODLIBET_EGRAPH_H
#define QUODLIBET_EGRAPH_H

#include "quodlibet/allocator.h"
#include "quodlibet/status.h"

QL_EXTERN_C_BEGIN

/* The e-graph is deliberately a pure term engine. It has no memory, control,
   effect, poison, or C undefined-behaviour semantics. Bit-vector arithmetic
   below is total arithmetic modulo 2^width. A C frontend must discharge or
   encode its own language-level side conditions before constructing terms. */

#define QL_EGRAPH_MAX_ARITY 3u
#define QL_EGRAPH_REASON_CAPACITY 64u

typedef struct ql_egraph ql_egraph;
typedef uint32_t ql_egraph_term_id;
typedef uint32_t ql_egraph_class_id;

#define QL_EGRAPH_INVALID_TERM UINT32_C(0)
#define QL_EGRAPH_INVALID_CLASS UINT32_C(0)

typedef uint32_t ql_egraph_sort_kind;
#define QL_EGRAPH_SORT_INVALID UINT32_C(0)
#define QL_EGRAPH_SORT_BOOL UINT32_C(1)
#define QL_EGRAPH_SORT_BITVECTOR UINT32_C(2)

typedef struct ql_egraph_type {
    ql_egraph_sort_kind kind;
    uint32_t bit_width;
} ql_egraph_type;

/* VARIABLE and the two CONSTANT operators are produced only by the dedicated
   constructors. DIV, REM, SHIFT, CHECKED, UB, MEMORY, and EFFECT values are
   named so callers receive a deterministic rejection instead of silently
   assigning them an unintended pure bit-vector meaning. */
typedef uint32_t ql_egraph_operator;
#define QL_EGRAPH_OP_INVALID UINT32_C(0)
#define QL_EGRAPH_OP_VARIABLE UINT32_C(1)
#define QL_EGRAPH_OP_BOOL_CONSTANT UINT32_C(2)
#define QL_EGRAPH_OP_BV_CONSTANT UINT32_C(3)
#define QL_EGRAPH_OP_BOOL_NOT UINT32_C(10)
#define QL_EGRAPH_OP_BOOL_AND UINT32_C(11)
#define QL_EGRAPH_OP_BOOL_OR UINT32_C(12)
#define QL_EGRAPH_OP_BOOL_XOR UINT32_C(13)
#define QL_EGRAPH_OP_BV_NOT UINT32_C(20)
#define QL_EGRAPH_OP_BV_AND UINT32_C(21)
#define QL_EGRAPH_OP_BV_OR UINT32_C(22)
#define QL_EGRAPH_OP_BV_XOR UINT32_C(23)
#define QL_EGRAPH_OP_BV_ADD UINT32_C(24)
#define QL_EGRAPH_OP_BV_SUB UINT32_C(25)
#define QL_EGRAPH_OP_BV_MUL UINT32_C(26)
#define QL_EGRAPH_OP_EQUAL UINT32_C(30)
#define QL_EGRAPH_OP_ITE UINT32_C(31)

#define QL_EGRAPH_OP_BV_UDIV UINT32_C(100)
#define QL_EGRAPH_OP_BV_SDIV UINT32_C(101)
#define QL_EGRAPH_OP_BV_UREM UINT32_C(102)
#define QL_EGRAPH_OP_BV_SREM UINT32_C(103)
#define QL_EGRAPH_OP_BV_SHL UINT32_C(104)
#define QL_EGRAPH_OP_BV_LSHR UINT32_C(105)
#define QL_EGRAPH_OP_BV_ASHR UINT32_C(106)
#define QL_EGRAPH_OP_CHECKED_ARITHMETIC UINT32_C(107)
#define QL_EGRAPH_OP_UNDEFINED_BEHAVIOUR UINT32_C(108)
#define QL_EGRAPH_OP_MEMORY UINT32_C(109)
#define QL_EGRAPH_OP_EFFECT_SEQUENCE UINT32_C(110)

typedef struct ql_egraph_config_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t reserved32;
    uint64_t max_terms;
    uint64_t max_classes;
    uint64_t max_merges;
    uint32_t max_bit_width;
    uint32_t max_symbol_bytes;
    uint64_t reserved[6];
} ql_egraph_config_v1;

typedef struct ql_egraph_saturation_limits_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t max_iterations;
    uint64_t max_rewrite_applications;
    uint64_t reserved[6];
} ql_egraph_saturation_limits_v1;

typedef uint32_t ql_egraph_stop_reason;
#define QL_EGRAPH_STOP_SATURATED UINT32_C(0)
#define QL_EGRAPH_STOP_ITERATION_LIMIT UINT32_C(1)
#define QL_EGRAPH_STOP_REWRITE_LIMIT UINT32_C(2)
#define QL_EGRAPH_STOP_TERM_LIMIT UINT32_C(3)
#define QL_EGRAPH_STOP_CLASS_LIMIT UINT32_C(4)
#define QL_EGRAPH_STOP_MERGE_LIMIT UINT32_C(5)

typedef struct ql_egraph_saturation_result_v1 {
    uint32_t complete;
    ql_egraph_stop_reason stop_reason;
    uint32_t iterations;
    uint32_t reserved32;
    uint64_t rewrite_applications;
    uint64_t term_count;
    uint64_t class_count;
    uint64_t merge_count;
    uint64_t reserved[4];
} ql_egraph_saturation_result_v1;

typedef uint32_t ql_egraph_verdict;
#define QL_EGRAPH_VERDICT_UNKNOWN UINT32_C(0)
#define QL_EGRAPH_VERDICT_PROVED_EQUAL UINT32_C(1)

typedef struct ql_egraph_proof_result_v1 {
    ql_egraph_verdict verdict;
    /* Conservative dependency marker. If set, the graph contains at least
       one trusted axiom merge and the equality must be reported as a proof
       relative to those assumptions. Inspect the evidence to replay them. */
    uint32_t depends_on_axioms;
    ql_egraph_saturation_result_v1 saturation;
    uint64_t reserved[4];
} ql_egraph_proof_result_v1;

typedef uint32_t ql_egraph_merge_kind;
#define QL_EGRAPH_MERGE_AXIOM UINT32_C(1)
#define QL_EGRAPH_MERGE_REWRITE UINT32_C(2)
#define QL_EGRAPH_MERGE_CONGRUENCE UINT32_C(3)

/* Class identifiers in a merge record are the roots immediately before that
   merge. Operand classes are also snapshots at that point in the derivation.
   Records are append-only and ordered by sequence. */
typedef struct ql_egraph_merge_record_v1 {
    uint64_t sequence;
    ql_egraph_merge_kind kind;
    uint32_t lhs_operand_count;
    uint32_t rhs_operand_count;
    ql_egraph_term_id lhs_term;
    ql_egraph_term_id rhs_term;
    ql_egraph_class_id lhs_class;
    ql_egraph_class_id rhs_class;
    ql_egraph_class_id lhs_operands[QL_EGRAPH_MAX_ARITY];
    ql_egraph_class_id rhs_operands[QL_EGRAPH_MAX_ARITY];
    char reason[QL_EGRAPH_REASON_CAPACITY];
} ql_egraph_merge_record_v1;

typedef struct ql_egraph_evidence_view_v1 {
    const ql_egraph_merge_record_v1 *records;
    size_t count;
} ql_egraph_evidence_view_v1;

/* The view borrows graph-owned storage and is invalidated by the next graph
   mutation. operands contain term identifiers, while current_class is the
   union-find root at query time. */
typedef struct ql_egraph_term_view_v1 {
    ql_egraph_term_id term;
    ql_egraph_class_id current_class;
    ql_egraph_type type;
    ql_egraph_operator op;
    uint32_t operand_count;
    ql_egraph_term_id operands[QL_EGRAPH_MAX_ARITY];
    const char *symbol;
    const uint8_t *constant_le;
    size_t constant_size;
} ql_egraph_term_view_v1;

QL_API void QL_CALL ql_egraph_config_init(ql_egraph_config_v1 *config);
QL_API void QL_CALL ql_egraph_saturation_limits_init(
    ql_egraph_saturation_limits_v1 *limits);

QL_API ql_status QL_CALL ql_egraph_create(
    const ql_egraph_config_v1 *config, const ql_allocator *allocator,
    ql_egraph **out_graph, ql_error *error);
QL_API void QL_CALL ql_egraph_destroy(ql_egraph *graph);

QL_API uint32_t QL_CALL ql_egraph_operator_is_supported(
    ql_egraph_operator op);

QL_API ql_status QL_CALL ql_egraph_make_bool_variable(
    ql_egraph *graph, const char *symbol, ql_egraph_term_id *out_term,
    ql_error *error);
QL_API ql_status QL_CALL ql_egraph_make_bv_variable(
    ql_egraph *graph, const char *symbol, uint32_t bit_width,
    ql_egraph_term_id *out_term, ql_error *error);
QL_API ql_status QL_CALL ql_egraph_make_bool_constant(
    ql_egraph *graph, uint32_t value, ql_egraph_term_id *out_term,
    ql_error *error);
QL_API ql_status QL_CALL ql_egraph_make_bv_constant(
    ql_egraph *graph, uint32_t bit_width, const uint8_t *little_endian,
    size_t byte_count, ql_egraph_term_id *out_term, ql_error *error);
QL_API ql_status QL_CALL ql_egraph_make_bv_u64(
    ql_egraph *graph, uint32_t bit_width, uint64_t value,
    ql_egraph_term_id *out_term, ql_error *error);
QL_API ql_status QL_CALL ql_egraph_make_operation(
    ql_egraph *graph, ql_egraph_operator op,
    const ql_egraph_term_id *operands, size_t operand_count,
    ql_egraph_term_id *out_term, ql_error *error);

/* Adds a trusted, typed equality. The engine does not establish that this
   equality is true. Every PROVED_EQUAL result in a graph containing such a
   merge is conservatively marked assumption-relative. rule_name is copied
   into the evidence log and must be non-empty and shorter than the reason
   capacity. */
QL_API ql_status QL_CALL ql_egraph_assume_equal(
    ql_egraph *graph, ql_egraph_term_id lhs, ql_egraph_term_id rhs,
    const char *rule_name, ql_error *error);

QL_API ql_status QL_CALL ql_egraph_saturate(
    ql_egraph *graph, const ql_egraph_saturation_limits_v1 *limits,
    ql_egraph_saturation_result_v1 *result, ql_error *error);

/* PROVED_EQUAL is returned only after complete saturation and root equality.
   Resource exhaustion or distinct roots always yield UNKNOWN, never a proof
   of inequality. */
QL_API ql_status QL_CALL ql_egraph_prove_equal(
    ql_egraph *graph, ql_egraph_term_id lhs, ql_egraph_term_id rhs,
    const ql_egraph_saturation_limits_v1 *limits,
    ql_egraph_proof_result_v1 *result, ql_error *error);

/* Extracts the least node-count representative. Ties use a stable structural
   ordering, then term id, so a fixed term insertion sequence is deterministic. */
QL_API ql_status QL_CALL ql_egraph_extract(
    const ql_egraph *graph, ql_egraph_term_id root_term,
    ql_egraph_term_id *out_term, uint64_t *out_cost, ql_error *error);

QL_API ql_status QL_CALL ql_egraph_get_term(
    const ql_egraph *graph, ql_egraph_term_id term,
    ql_egraph_term_view_v1 *view, ql_error *error);
QL_API ql_status QL_CALL ql_egraph_evidence(
    const ql_egraph *graph, ql_egraph_evidence_view_v1 *view,
    ql_error *error);

QL_API uint64_t QL_CALL ql_egraph_term_count(const ql_egraph *graph);
QL_API uint64_t QL_CALL ql_egraph_class_count(const ql_egraph *graph);
QL_API uint64_t QL_CALL ql_egraph_merge_count(const ql_egraph *graph);

QL_EXTERN_C_END

#endif
