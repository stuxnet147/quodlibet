#ifndef QUODLIBET_PROOF_SMT_H
#define QUODLIBET_PROOF_SMT_H

#include "quodlibet/evidence.h"
#include "quodlibet/registry.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

#define QL_SMT_PRODUCT_METHOD_NAME "prove.smt-product"
#define QL_SMT_PRODUCT_METHOD_VERSION "1"
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
/* Borrowed canonical bytes of the embedded counterexample, or null when the
   outcome carries none. Valid until the artifact is released. */
QL_API ql_status QL_CALL ql_smt_product_outcome_counterexample(
    const ql_allocator *allocator, const ql_artifact *artifact,
    ql_artifact **output, ql_error *error);

QL_EXTERN_C_END

#endif
