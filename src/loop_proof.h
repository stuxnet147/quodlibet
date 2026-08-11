#ifndef QUODLIBET_SRC_LOOP_PROOF_H
#define QUODLIBET_SRC_LOOP_PROOF_H

#include "loop_analysis.h"
#include "quodlibet/proof_smt.h"
#include "quodlibet/solver.h"

QL_EXTERN_C_BEGIN

#define QL_LOOP_PROOF_QUERY_SCHEMA_VERSION 1u

typedef struct ql_loop_proof_query ql_loop_proof_query;

/* A loop query is either ready for one of the two cheap symbolic checks, or
   has reached the deliberately unavailable CHC/PDR boundary.  Unsupported
   loop shapes are facts about this fast path, not transport failures. */
typedef enum ql_loop_proof_disposition {
  QL_LOOP_PROOF_NOT_APPLICABLE = 0,
  QL_LOOP_PROOF_QUERY_READY,
  QL_LOOP_PROOF_CHC_PDR_UNAVAILABLE
} ql_loop_proof_disposition;

typedef enum ql_loop_proof_unsupported_reason {
  QL_LOOP_PROOF_UNSUPPORTED_NONE = 0,
  QL_LOOP_PROOF_UNSUPPORTED_NOT_CYCLIC,
  QL_LOOP_PROOF_UNSUPPORTED_UNCLASSIFIED_CYCLE,
  QL_LOOP_PROOF_UNSUPPORTED_LOOP_COUNT_MISMATCH,
  QL_LOOP_PROOF_UNSUPPORTED_IRREDUCIBLE_OR_MULTI_ENTRY,
  QL_LOOP_PROOF_UNSUPPORTED_NESTED_LOOP,
  QL_LOOP_PROOF_UNSUPPORTED_MEMORY_STATE,
  QL_LOOP_PROOF_UNSUPPORTED_EVENT_TRACE,
  QL_LOOP_PROOF_UNSUPPORTED_CALL,
  QL_LOOP_PROOF_UNSUPPORTED_EFFECT,
  QL_LOOP_PROOF_UNSUPPORTED_NONSCALAR_STATE,
  QL_LOOP_PROOF_UNSUPPORTED_GUARD,
  QL_LOOP_PROOF_UNSUPPORTED_PAIRING,
  QL_LOOP_PROOF_UNSUPPORTED_ENTRY_TRANSITION_EXIT_MISMATCH,
  QL_LOOP_PROOF_UNSUPPORTED_AFFINE_SUMMARY
} ql_loop_proof_unsupported_reason;

/* Candidates state left = A*right+B modulo 2^bit_width.  EQUALITY fixes
   A=1,B=0; CONSTANT_OFFSET fixes A=1.  A candidate is selected only when its
   entry relation and analyzer recurrence equations justify it; the generated
   Step obligation additionally checks the separately serialized transitions. */
typedef enum ql_loop_relation_candidate_kind {
  QL_LOOP_RELATION_EQUALITY = 1,
  QL_LOOP_RELATION_CONSTANT_OFFSET,
  QL_LOOP_RELATION_AFFINE
} ql_loop_relation_candidate_kind;

typedef struct ql_loop_relation_candidate_v1 {
  size_t struct_size;
  ql_loop_relation_candidate_kind kind;
  uint32_t selected;
  uint32_t bit_width;
  size_t left_loop;
  size_t right_loop;
  size_t left_phi;
  size_t right_phi;
  ql_ir_value_id left_state;
  ql_ir_value_id right_state;
  /* Raw modular coefficients in the candidate's bit width. */
  uint64_t multiplier;
  uint64_t offset;
  ql_loop_recurrence_kind left_recurrence;
  ql_loop_recurrence_kind right_recurrence;
  uint64_t reserved[4];
} ql_loop_relation_candidate_v1;

typedef enum ql_loop_proof_check_target {
  QL_LOOP_PROOF_CHECK_INDUCTION = 1,
  QL_LOOP_PROOF_CHECK_SUMMARY,
  QL_LOOP_PROOF_CHECK_DOMAIN,
  QL_LOOP_PROOF_CHECK_REFLEXIVITY
} ql_loop_proof_check_target;

typedef struct ql_loop_proof_metrics_v1 {
  size_t struct_size;
  uint32_t schema_version;
  uint32_t reserved32;
  uint64_t left_loop_count;
  uint64_t right_loop_count;
  uint64_t paired_loop_count;
  uint64_t invariant_candidate_count;
  uint64_t invariant_generated_count;
  uint64_t summary_candidate_count;
  uint64_t summary_attempted_count;
  uint64_t fallback_reached_count;
  ql_smt_product_answer induction_answer;
  ql_smt_product_answer summary_answer;
  ql_smt_product_answer domain_answer;
  uint32_t reserved_answer;
  uint64_t stage_reached;
  uint64_t discover_ns;
  uint64_t canonicalize_ns;
  uint64_t pairing_ns;
  uint64_t invariant_ns;
  uint64_t summary_ns;
  uint64_t query_build_ns;
  uint64_t induction_solver_ns;
  uint64_t summary_solver_ns;
  uint64_t domain_solver_ns;
  uint64_t total_ns;
  ql_smt_product_answer reflexivity_answer;
  uint32_t concrete_domain_witness;
  uint64_t reflexivity_solver_ns;
  ql_digest domain_witness_digest;
  uint64_t reserved[2];
} ql_loop_proof_metrics_v1;

typedef struct ql_loop_proof_options_v1 {
  size_t struct_size;
  /* This is a fact supplied by the problem/precondition binding.  The
     default Quodlibet contract has the literal true precondition. */
  uint32_t precondition_is_true;
  /* Set only after the caller has checked identical contract axes, both
     source-signature/IR bindings, argument correspondence and the typed
     precondition digest.  Defaults to false so a bare IR query cannot claim
     promotion eligibility. */
  uint32_t contract_binding_match;
  uint64_t reserved[6];
} ql_loop_proof_options_v1;

typedef struct ql_loop_proof_query_view_v1 {
  size_t struct_size;
  uint32_t schema_version;
  ql_loop_proof_disposition disposition;
  ql_loop_proof_strategy strategy;
  ql_loop_proof_unsupported_reason unsupported_reason;
  ql_solver_logic logic;
  uint32_t maximum_bv_width;
  uint32_t self_pair;
  uint32_t ir_structural_match;
  /* Exact self-pairs may carry memory, definedness and event-trace PHIs as
     one opaque but exactly sorted state tuple.  This is congruence for an
     identical transition system, not a synthesized memory invariant. */
  uint32_t exact_self_pair_opaque_state;
  /* The builder sees IR only.  The caller must independently establish this
     before promotion by checking the problem's contract, signatures,
     argument correspondence and typed-precondition binding. */
  uint32_t requires_contract_binding_match;
  uint32_t canonical_transition_match;
  uint32_t canonical_entry_exit_match;
  uint32_t all_loops_paired;
  uint32_t structural_query_available;
  uint32_t affine_summary_available;
  /* The terminal is syntactically `true`.  It is a concrete inhabited-domain
     witness only for the narrowly checked true-precondition, scalar,
     assumption-free and UB-free actual path or exact-reflexive path below. */
  uint32_t domain_is_abstract_true;
  uint32_t requires_concrete_domain_check;
  /* Non-vacuity is intentionally narrow in v1: the entire IR must be scalar,
     effect-free, assumption-free and UB-free under the literal true
     precondition.  Promotion additionally requires the caller's binding gate.
   */
  uint32_t nonvacuity_eligible;
  uint32_t promotion_eligible;
  uint32_t has_ub_guard_or_terminator;
  uint32_t chc_pdr_reached;
  uint32_t chc_pdr_available;
  uint32_t candidate_sat_is_counterexample;
  size_t candidate_count;
  ql_digest canonical_digest;
  ql_digest prefix_digest;
  ql_digest induction_terminal_digest;
  ql_digest summary_terminal_digest;
  ql_digest domain_terminal_digest;
  ql_digest base_obligation_digest;
  ql_digest guard_obligation_digest;
  ql_digest step_obligation_digest;
  ql_digest exit_obligation_digest;
  ql_digest summary_obligation_digest;
  ql_digest reflexivity_obligation_digest;
  ql_digest reflexivity_terminal_digest;
  ql_loop_proof_metrics_v1 metrics;
  char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
  uint64_t reserved[4];
} ql_loop_proof_query_view_v1;

void ql_loop_proof_options_init(ql_loop_proof_options_v1 *options);

/* Builds no bounded execution trace.  The relational scalar path serializes
   left/right entry, guard, transition and exit expressions for one induction
   step and consumes the selected equality/offset/affine relation.  An exact
   whole-IR self-pair may instead use shared symbols as a reflexivity fast path.
   Disconnected affine summaries are recorded as candidates but are not exposed
   as proof terminals in schema v1. */
ql_status ql_loop_proof_query_build(const ql_allocator *allocator,
                                    const ql_ir *left_ir, const ql_ir *right_ir,
                                    const ql_loop_proof_options_v1 *options,
                                    ql_loop_proof_query **output,
                                    ql_error *error);
void ql_loop_proof_query_destroy(ql_loop_proof_query *query);

ql_status ql_loop_proof_query_get_view(const ql_loop_proof_query *query,
                                       ql_loop_proof_query_view_v1 *view,
                                       ql_error *error);
ql_status
ql_loop_proof_query_candidate_at(const ql_loop_proof_query *query, size_t index,
                                 ql_loop_relation_candidate_v1 *candidate,
                                 ql_error *error);

const ql_artifact *
ql_loop_proof_query_prefix_artifact(const ql_loop_proof_query *query);
const ql_artifact *
ql_loop_proof_query_induction_artifact(const ql_loop_proof_query *query);
const ql_artifact *
ql_loop_proof_query_reflexivity_artifact(const ql_loop_proof_query *query);
const ql_artifact *
ql_loop_proof_query_summary_artifact(const ql_loop_proof_query *query);
const ql_artifact *
ql_loop_proof_query_domain_artifact(const ql_loop_proof_query *query);

/* Records candidate-check telemetry and advances only among proof strategies.
   In particular, SAT for induction or summary rejects that candidate and
   never becomes a program counterexample. */
ql_status ql_loop_proof_query_record_check(
    ql_loop_proof_query *query, ql_loop_proof_check_target target,
    ql_solver_check_kind answer, ql_solver_unknown_reason unknown_reason,
    uint64_t elapsed_ns, ql_error *error);

QL_EXTERN_C_END

#endif
