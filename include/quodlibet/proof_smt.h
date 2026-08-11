#ifndef QUODLIBET_PROOF_SMT_H
#define QUODLIBET_PROOF_SMT_H

#include "quodlibet/evidence.h"
#include "quodlibet/registry.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

#define QL_SMT_PRODUCT_METHOD_NAME "prove.smt-product"
#define QL_SMT_PRODUCT_METHOD_VERSION "2"
#define QL_OUTCOME_SCHEMA_VERSION 1u

/* The boundary at which a solver UNSAT may become a Quodlibet proof.

   Bitwuzla 0.9.1 exposes no proof object, so there is no certificate for a
   checker to validate. Quodlibet therefore takes the second of the two
   admissible routes: an explicit, opt-in, recorded trust policy.

   QL_UNSAT_PROMOTION_NONE is the default. A raw UNSAT is retained as solver
   evidence and the verdict stays UNKNOWN.

   QL_UNSAT_PROMOTION_TRUSTED_BACKEND permits a PROVED_* verdict only when all
   of the following hold, and every one of them is written into the outcome
   envelope where a user can read it:

     1. the problem passes ql_problem_require_proof_binding, so both source
        signatures, the argument correspondence, and the typed-precondition
        digest are bound into one artifact identity;
     2. the miter covers exactly the contract's observation axes, relation
        direction, and UB policy;
     3. the comparison-domain query answered SAT, so the UNSAT is not vacuous;
     4. the violation query answered UNSAT;
     5. the backend is the pinned Bitwuzla, and its name, version, executable
        content digest, and query digest are recorded;
     6. the caller selected this policy explicitly.

   The recorded envelope always states checked_proof = 0 for this backend. The
   proof rests on trusting Bitwuzla, not on a validated certificate, and the
   result says so rather than implying otherwise. */
typedef enum ql_unsat_promotion_policy {
    QL_UNSAT_PROMOTION_NONE = 0,
    QL_UNSAT_PROMOTION_TRUSTED_BACKEND = 1
} ql_unsat_promotion_policy;

typedef enum ql_smt_product_answer {
    QL_SMT_PRODUCT_ANSWER_NOT_QUERIED = 0,
    QL_SMT_PRODUCT_ANSWER_SAT,
    QL_SMT_PRODUCT_ANSWER_UNSAT,
    QL_SMT_PRODUCT_ANSWER_UNKNOWN
} ql_smt_product_answer;

/* The cyclic-CFG dispatcher inside prove.smt-product is deliberately a
   narrow fast path.  These values describe which unbounded argument, if any,
   reached the solver.  CHC/PDR is a fallback boundary in v1, not a bundled
   backend; reaching it is reported separately from attempting it. */
typedef enum ql_loop_proof_strategy {
    QL_LOOP_PROOF_STRATEGY_NONE = 0,
    QL_LOOP_PROOF_STRATEGY_STRUCTURAL_INDUCTION,
    QL_LOOP_PROOF_STRATEGY_AFFINE_SUMMARY,
    QL_LOOP_PROOF_STRATEGY_CHC_PDR_UNAVAILABLE,
    /* Whole-IR identity plus a concrete defined execution discharges an
       exact self-pair without pretending that a canonical loop invariant was
       synthesized. */
    QL_LOOP_PROOF_STRATEGY_EXACT_REFLEXIVITY
} ql_loop_proof_strategy;

#define QL_LOOP_PROOF_STATS_SCHEMA_VERSION 1u
#define QL_LOOP_STAGE_DISCOVER (UINT64_C(1) << 0)
#define QL_LOOP_STAGE_CANONICALIZE (UINT64_C(1) << 1)
#define QL_LOOP_STAGE_PAIRING (UINT64_C(1) << 2)
#define QL_LOOP_STAGE_INVARIANT (UINT64_C(1) << 3)
#define QL_LOOP_STAGE_INDUCTION (UINT64_C(1) << 4)
#define QL_LOOP_STAGE_SUMMARY (UINT64_C(1) << 5)
#define QL_LOOP_STAGE_FALLBACK (UINT64_C(1) << 6)
#define QL_LOOP_STAGE_REFLEXIVITY (UINT64_C(1) << 7)

/* Optional structured telemetry read from an SMT-product outcome.  Counts are
   raw counts, never percentages, so a report can retain its exact numerator
   and denominator.  A stage duration is meaningful only when its matching
   bit is set in stage_reached; skipped stages do not contribute zero-latency
   samples to p50 or p95. The unavailable fallback is a reached boundary but
   has no timed implementation, so fallback_ns == 0 is explicitly unmeasured.

   None of these fields is proof authority.  Verdict promotion still follows
   the ordinary trusted-backend/checker boundary and problem binding. */
typedef struct ql_loop_proof_stats_v1 {
    size_t struct_size;
    uint32_t schema_version;
    uint32_t applicable;
    uint32_t cyclic;
    uint32_t noncanonical_cycle;
    uint32_t all_loops_paired;
    uint32_t fallback_reached;
    uint32_t fallback_attempted;
    uint32_t proof_eligible;
    ql_loop_proof_strategy strategy;
    ql_smt_product_answer induction_answer;
    ql_smt_product_answer summary_answer;
    uint32_t reserved32;
    uint64_t left_loop_count;
    uint64_t right_loop_count;
    uint64_t natural_loop_count;
    uint64_t paired_loop_count;
    uint64_t invariant_generated_count;
    uint64_t induction_proved_count;
    uint64_t summary_attempted_count;
    uint64_t summary_proved_count;
    /* Bits, in order: discover, canonicalize, pairing, invariant generation,
       induction solver check, affine summary, CHC/PDR fallback, exact whole-IR
       reflexivity. */
    uint64_t stage_reached;
    uint64_t discover_ns;
    uint64_t canonicalize_ns;
    uint64_t pairing_ns;
    uint64_t invariant_ns;
    uint64_t induction_ns;
    uint64_t summary_ns;
    uint64_t fallback_ns;
    ql_digest canonical_digest;
    ql_digest base_obligation_digest;
    ql_digest guard_obligation_digest;
    ql_digest step_obligation_digest;
    ql_digest exit_obligation_digest;
    char failure_reason[QL_ERROR_MESSAGE_CAPACITY];
    ql_smt_product_answer reflexivity_answer;
    uint32_t concrete_domain_witness;
    uint64_t reflexivity_proved_count;
    uint64_t reflexivity_ns;
    ql_digest reflexivity_obligation_digest;
    ql_digest domain_witness_digest;
    uint64_t reserved[2];
} ql_loop_proof_stats_v1;

/* Read back from a quodlibet.outcome artifact this method produced. */
typedef struct ql_smt_product_outcome_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    ql_unsat_promotion_policy unsat_promotion;
    ql_smt_product_answer violation_answer;
    ql_smt_product_answer domain_answer;
    uint32_t checked_proof;
    uint32_t replay_confirmed;
    ql_digest problem_digest;
    ql_digest solver_query_digest;
    ql_digest solver_binary_digest;
    ql_digest cache_key;
    ql_digest counterexample_digest;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
    uint64_t reserved[4];
} ql_smt_product_outcome_view_v1;

/* Method options, all optional:

     {"unsat_promotion":"none"|"trusted-backend",
      "timeout_ms":30000,
      "memory_limit_mb":0,
      "solver_options":"{\"executable\":\"/absolute/path\"}"}

   An unknown key or an unknown unsat_promotion spelling is rejected before
   the run starts. */
QL_API const ql_method_v1 *QL_CALL ql_smt_product_method(void);
QL_API const ql_proof_method_v1 *QL_CALL ql_smt_product_proof_method(void);
QL_API ql_status QL_CALL ql_register_smt_product_method(
    ql_registry *registry, ql_error *error);

QL_API ql_status QL_CALL ql_smt_product_outcome_read(
    const ql_artifact *artifact, ql_smt_product_outcome_view_v1 *view,
    ql_error *error);
/* Reads the optional loop-proof object.  A loop-free outcome returns OK with
   applicable == 0.  The view is versioned independently of the outcome's
   long-standing v1 summary view, so adding telemetry does not change that
   ABI. */
QL_API ql_status QL_CALL ql_smt_product_outcome_loop_stats(
    const ql_artifact *artifact, ql_loop_proof_stats_v1 *view,
    ql_error *error);
/* Borrowed canonical bytes of the embedded counterexample, or null when the
   outcome carries none. Valid until the artifact is released. */
QL_API ql_status QL_CALL ql_smt_product_outcome_counterexample(
    const ql_allocator *allocator, const ql_artifact *artifact,
    ql_artifact **output, ql_error *error);

QL_EXTERN_C_END

#endif
