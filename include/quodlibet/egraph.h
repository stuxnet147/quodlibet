#ifndef QUODLIBET_EGRAPH_H
#define QUODLIBET_EGRAPH_H

#include "quodlibet/allocator.h"
#include "quodlibet/hash.h"
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

/* The class a term is created in. Every term starts alone in its own class,
   and that identifier is what the merge records snapshot before any union is
   applied. An independent replay checker needs this seed mapping because the
   current_class in a term view is a post-hoc root, not a derivation state. */
QL_API ql_status QL_CALL ql_egraph_term_initial_class(
    const ql_egraph *graph, ql_egraph_term_id term,
    ql_egraph_class_id *out_class, ql_error *error);

QL_API uint64_t QL_CALL ql_egraph_term_count(const ql_egraph *graph);
QL_API uint64_t QL_CALL ql_egraph_class_count(const ql_egraph *graph);
QL_API uint64_t QL_CALL ql_egraph_merge_count(const ql_egraph *graph);

/* --- Rewrite rule catalogue -------------------------------------------- */

/* A merge record carries only the rule name. The premises that make that rule
   sound (bit width range, signedness reading, the arithmetic model it needs)
   live here, keyed by that name and versioned as a whole. A checker that
   replays the merge log resolves each REWRITE reason against this catalogue
   and discharges the side conditions itself. Bump the version whenever a rule
   is added, removed, or has its premises changed; the digest below then also
   changes, which is what belongs in evidence and cache keys. */
#define QL_EGRAPH_RULE_CATALOGUE_VERSION UINT32_C(1)
#define QL_EGRAPH_RULE_MAX_SUBJECT_OPS 4u

/* The digest of catalogue version 1, pinned so that editing a premise without
   bumping the version is a test failure rather than a silent change of what
   past evidence means. */
#define QL_EGRAPH_RULE_CATALOGUE_DIGEST_HEX \
    "f2666b08e576261a24705f2fdc77b5b4fc2ae67c3cfa3333a615ceddcf596aec"

/* How the recorded right-hand term must relate to the subject term. */
typedef uint32_t ql_egraph_rule_shape;
#define QL_EGRAPH_RULE_SHAPE_INVALID UINT32_C(0)
/* op(a, b) = op(b, a) */
#define QL_EGRAPH_RULE_SHAPE_COMMUTATIVE UINT32_C(1)
/* op(a, a) = a */
#define QL_EGRAPH_RULE_SHAPE_IDEMPOTENT UINT32_C(2)
/* op(op(a)) = a */
#define QL_EGRAPH_RULE_SHAPE_INVOLUTION UINT32_C(3)
/* op(a, e) = a where e is the witness constant */
#define QL_EGRAPH_RULE_SHAPE_IDENTITY_ELEMENT UINT32_C(4)
/* op(a, z) = z where z is the witness constant */
#define QL_EGRAPH_RULE_SHAPE_ABSORBING_ELEMENT UINT32_C(5)
/* op(a, a) = c where c is the result constant */
#define QL_EGRAPH_RULE_SHAPE_SELF_ANNIHILATION UINT32_C(6)
/* op(k) = c for a unary op over a witness constant k */
#define QL_EGRAPH_RULE_SHAPE_CONSTANT_FOLD UINT32_C(7)
/* ite(k, t, e) = t or e for a constant condition k */
#define QL_EGRAPH_RULE_SHAPE_SELECT_BRANCH UINT32_C(8)
/* ite(c, t, t) = t */
#define QL_EGRAPH_RULE_SHAPE_SELECT_SAME UINT32_C(9)
/* equal(a, a) = true */
#define QL_EGRAPH_RULE_SHAPE_REFLEXIVE UINT32_C(10)

/* Constant a side condition demands, named by value class rather than by bit
   pattern so one descriptor covers every width the rule accepts. */
typedef uint32_t ql_egraph_rule_constant;
#define QL_EGRAPH_RULE_CONSTANT_NONE UINT32_C(0)
#define QL_EGRAPH_RULE_CONSTANT_BOOL_FALSE UINT32_C(1)
#define QL_EGRAPH_RULE_CONSTANT_BOOL_TRUE UINT32_C(2)
#define QL_EGRAPH_RULE_CONSTANT_BV_ZERO UINT32_C(3)
#define QL_EGRAPH_RULE_CONSTANT_BV_ONE UINT32_C(4)
#define QL_EGRAPH_RULE_CONSTANT_BV_ONES UINT32_C(5)

/* Operand selectors. ANY means the checker may find the witness in either
   operand position; OTHER means the operand that is not the witness. */
#define QL_EGRAPH_RULE_OPERAND_NONE UINT32_C(0xFFFFFFFF)
#define QL_EGRAPH_RULE_OPERAND_ANY UINT32_C(0xFFFFFFFE)
#define QL_EGRAPH_RULE_OPERAND_OTHER UINT32_C(0xFFFFFFFD)

typedef uint32_t ql_egraph_rule_conditions;
#define QL_EGRAPH_RULE_COND_NONE UINT32_C(0)
/* Both merged terms must carry the identical sort and bit width. */
#define QL_EGRAPH_RULE_COND_SAME_TYPE (UINT32_C(1) << 0)
/* The subject operands must carry the subject's own sort. */
#define QL_EGRAPH_RULE_COND_OPERAND_SORT (UINT32_C(1) << 1)
/* The two operands named by equal_operands must already share a class. */
#define QL_EGRAPH_RULE_COND_OPERANDS_SAME_CLASS (UINT32_C(1) << 2)
/* Some operand class must contain a term equal to witness_constant. */
#define QL_EGRAPH_RULE_COND_WITNESS_CONSTANT (UINT32_C(1) << 3)
/* An operand class must contain a further application of subject_op. */
#define QL_EGRAPH_RULE_COND_NESTED_APPLICATION (UINT32_C(1) << 4)
/* Sound only under total arithmetic modulo 2^width. A source language in
   which the operation may overflow into undefined behaviour must discharge
   that obligation before the term reaches the e-graph. */
#define QL_EGRAPH_RULE_COND_TOTAL_ARITHMETIC (UINT32_C(1) << 5)
/* Sound under both the signed and the unsigned reading of its operands. */
#define QL_EGRAPH_RULE_COND_SIGN_AGNOSTIC (UINT32_C(1) << 6)
/* Sound at every bit width the sort admits, subject to the width bounds. */
#define QL_EGRAPH_RULE_COND_WIDTH_AGNOSTIC (UINT32_C(1) << 7)

/* rule_name and soundness point at static storage owned by the library and
   stay valid for the process lifetime. maximum_bit_width is 0 when the rule
   places no upper bound. */
typedef struct ql_egraph_rule_descriptor_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t catalogue_version;
    const char *rule_name;
    const char *soundness;
    ql_egraph_rule_shape shape;
    /* Operators this rule name may have fired on. A few names cover more than
       one operator because the engine shares one reason string for them. */
    uint32_t subject_op_count;
    ql_egraph_operator subject_ops[QL_EGRAPH_RULE_MAX_SUBJECT_OPS];
    uint32_t subject_arity;
    ql_egraph_rule_conditions conditions;
    ql_egraph_rule_constant witness_constant;
    ql_egraph_rule_constant result_constant;
    uint32_t witness_operand;
    uint32_t result_operand;
    uint32_t equal_operands[2];
    uint32_t minimum_bit_width;
    uint32_t maximum_bit_width;
    uint64_t reserved[4];
} ql_egraph_rule_descriptor_v1;

QL_API uint32_t QL_CALL ql_egraph_rule_catalogue_size(void);
QL_API ql_status QL_CALL ql_egraph_rule_catalogue_at(
    uint32_t index, ql_egraph_rule_descriptor_v1 *descriptor,
    ql_error *error);
/* NOT_FOUND for a name the catalogue does not define. A merge record whose
   reason does not resolve is not justified by any known rule. */
QL_API ql_status QL_CALL ql_egraph_rule_lookup(
    const char *rule_name, ql_egraph_rule_descriptor_v1 *descriptor,
    ql_error *error);
/* BLAKE3 over the canonical serialization of the whole catalogue. This is the
   rewrite-set identity that evidence and cache keys carry. */
QL_API ql_status QL_CALL ql_egraph_rule_catalogue_digest(
    ql_digest *digest, ql_error *error);

QL_API const char *QL_CALL ql_egraph_rule_shape_string(
    ql_egraph_rule_shape shape);
QL_API const char *QL_CALL ql_egraph_rule_constant_string(
    ql_egraph_rule_constant constant);

QL_EXTERN_C_END

#endif
