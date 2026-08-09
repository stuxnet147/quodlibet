#ifndef QUODLIBET_EGRAPH_CHECK_H
#define QUODLIBET_EGRAPH_CHECK_H

#include "quodlibet/egraph.h"

QL_EXTERN_C_BEGIN

/* An independent replay checker for the e-graph merge log.

   This translation unit shares no code with the e-graph engine. It rebuilds
   its own union-find from the recorded terms and walks the merge records in
   sequence, and for each record it asks whether the rule the record names
   actually justifies that merge in the state the earlier records produced. A
   defect in the engine's rewrite matching does not propagate here, because
   nothing here calls into the engine.

   Two properties of the evidence format the checker relies on, both of which
   it enforces rather than assumes:

   - The representative of a class is the smallest class identifier in it.
     This is a canonical form, so the checker derives it from its own union
     rather than copying the engine's tie-breaking, and a record whose class
     snapshot disagrees is rejected.
   - In a REWRITE record, lhs_term is the subject the rule fired on and
     rhs_term is the term the rule rewrote it to. In a CONGRUENCE record the
     two sides are interchangeable.

   A checked replay is not a proof that the two roots denote the same source
   behaviour. It establishes only that every merge follows from the recorded
   rules plus the axioms, and it reports the axioms separately so a caller
   cannot mistake an assumption-relative result for a checked one. */

#define QL_EGRAPH_CHECK_SCHEMA_VERSION 1u
#define QL_EGRAPH_CHECK_DETAIL_CAPACITY 256u

typedef struct ql_egraph_check_term_v1 {
    size_t struct_size;
    ql_egraph_term_id term;
    /* The class the term was created in, before any union. */
    ql_egraph_class_id initial_class;
    ql_egraph_type type;
    ql_egraph_operator op;
    uint32_t operand_count;
    ql_egraph_term_id operands[QL_EGRAPH_MAX_ARITY];
    const char *symbol;
    const uint8_t *constant_le;
    size_t constant_size;
    uint64_t reserved[4];
} ql_egraph_check_term_v1;

/* terms must be dense and ordered: terms[i].term == i + 1. Operands must name
   earlier terms, which is what makes the term table acyclic. */
typedef struct ql_egraph_check_input_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t schema_version;
    const ql_egraph_check_term_v1 *terms;
    size_t term_count;
    const ql_egraph_merge_record_v1 *records;
    size_t record_count;
    /* The rewrite set the log was produced under. The checker refuses a log
       whose catalogue it does not carry, because it would otherwise discharge
       side conditions from a different rule set than the one that fired. */
    uint32_t rule_catalogue_version;
    uint32_t reserved32;
    ql_digest rule_catalogue_digest;
    uint64_t reserved[4];
} ql_egraph_check_input_v1;

typedef uint32_t ql_egraph_check_verdict;
#define QL_EGRAPH_CHECK_VERDICT_INVALID UINT32_C(0)
/* The named rule and the replayed state justify the merge. */
#define QL_EGRAPH_CHECK_VERDICT_JUSTIFIED UINT32_C(1)
/* A trusted axiom. Not justified by anything the checker can see; every
   downstream result is relative to it. */
#define QL_EGRAPH_CHECK_VERDICT_ASSUMED UINT32_C(2)
/* The merge does not follow. This is a defect in the engine, the evidence, or
   the catalogue, and must not be resolved by trusting the engine. */
#define QL_EGRAPH_CHECK_VERDICT_REJECTED UINT32_C(3)

typedef uint32_t ql_egraph_check_code;
#define QL_EGRAPH_CHECK_CODE_NONE UINT32_C(0)
#define QL_EGRAPH_CHECK_CODE_AXIOM UINT32_C(1)
#define QL_EGRAPH_CHECK_CODE_SEQUENCE_GAP UINT32_C(2)
#define QL_EGRAPH_CHECK_CODE_UNKNOWN_TERM UINT32_C(3)
#define QL_EGRAPH_CHECK_CODE_TYPE_MISMATCH UINT32_C(4)
#define QL_EGRAPH_CHECK_CODE_STALE_CLASS UINT32_C(5)
#define QL_EGRAPH_CHECK_CODE_ALREADY_MERGED UINT32_C(6)
#define QL_EGRAPH_CHECK_CODE_OPERAND_SNAPSHOT UINT32_C(7)
#define QL_EGRAPH_CHECK_CODE_UNKNOWN_RULE UINT32_C(8)
#define QL_EGRAPH_CHECK_CODE_WRONG_OPERATOR UINT32_C(9)
#define QL_EGRAPH_CHECK_CODE_WRONG_ARITY UINT32_C(10)
#define QL_EGRAPH_CHECK_CODE_SIDE_CONDITION UINT32_C(11)
#define QL_EGRAPH_CHECK_CODE_RESULT_MISMATCH UINT32_C(12)
#define QL_EGRAPH_CHECK_CODE_WIDTH_OUT_OF_RANGE UINT32_C(13)
#define QL_EGRAPH_CHECK_CODE_UNKNOWN_MERGE_KIND UINT32_C(14)
#define QL_EGRAPH_CHECK_CODE_CONGRUENCE_MISMATCH UINT32_C(15)

typedef struct ql_egraph_check_finding_v1 {
    uint64_t sequence;
    ql_egraph_check_verdict verdict;
    ql_egraph_check_code code;
    char rule_name[QL_EGRAPH_REASON_CAPACITY];
    char detail[QL_EGRAPH_CHECK_DETAIL_CAPACITY];
} ql_egraph_check_finding_v1;

typedef struct ql_egraph_check_report ql_egraph_check_report;

/* findings lists every record that is not JUSTIFIED, in sequence order, so a
   justified log produces an empty list. all_merges_justified is 1 only when
   the log has no rejection and no axiom; a log with axioms is consistent but
   assumption-relative, which is why the two counts are separate. */
typedef struct ql_egraph_check_report_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    uint32_t all_merges_justified;
    uint64_t record_count;
    uint64_t justified_count;
    uint64_t assumed_count;
    uint64_t rejected_count;
    const ql_egraph_check_finding_v1 *findings;
    size_t finding_count;
    ql_digest rule_catalogue_digest;
    uint64_t reserved[4];
} ql_egraph_check_report_view_v1;

QL_API void QL_CALL ql_egraph_check_input_init(
    ql_egraph_check_input_v1 *input);

/* Returns OK when the replay ran to completion, whatever its verdicts. A
   rejection is reported in the view, not as a status, because the caller must
   see which merge failed and why. A malformed input is a status error. */
QL_API ql_status QL_CALL ql_egraph_check_replay(
    const ql_allocator *allocator, const ql_egraph_check_input_v1 *input,
    ql_egraph_check_report **report, ql_error *error);

/* Harvests a snapshot from a live graph through the public API only, then
   replays it. The convenience does not weaken the independence: the snapshot
   is data, and the replay never calls back into the engine. The snapshot
   borrows the graph's symbol and constant storage, so the report is valid
   only while the graph is alive and unmutated. */
QL_API ql_status QL_CALL ql_egraph_check_graph(
    const ql_allocator *allocator, const ql_egraph *graph,
    ql_egraph_check_report **report, ql_error *error);

QL_API void QL_CALL ql_egraph_check_report_release(
    ql_egraph_check_report *report);
QL_API ql_status QL_CALL ql_egraph_check_report_get_view(
    const ql_egraph_check_report *report,
    ql_egraph_check_report_view_v1 *view, ql_error *error);

/* The equivalence the replay itself established, not the one the engine
   reports. A caller that wants to believe an e-graph PROVED_EQUAL asks this. */
QL_API ql_status QL_CALL ql_egraph_check_terms_equal(
    const ql_egraph_check_report *report, ql_egraph_term_id lhs,
    ql_egraph_term_id rhs, uint32_t *out_equal, ql_error *error);

QL_API const char *QL_CALL ql_egraph_check_verdict_string(
    ql_egraph_check_verdict verdict);
QL_API const char *QL_CALL ql_egraph_check_code_string(
    ql_egraph_check_code code);

QL_EXTERN_C_END

#endif
