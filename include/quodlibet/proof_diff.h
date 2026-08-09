#ifndef QUODLIBET_PROOF_DIFF_H
#define QUODLIBET_PROOF_DIFF_H

#include "quodlibet/evidence.h"
#include "quodlibet/proof_method.h"
#include "quodlibet/registry.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

#define QL_DIFF_METHOD_NAME "refute.concrete-differential"
#define QL_DIFF_METHOD_VERSION "1"
#define QL_DIFF_OUTCOME_SCHEMA_VERSION 1u

#define QL_DIFF_DEFAULT_TESTS UINT64_C(512)
#define QL_DIFF_DEFAULT_SEED UINT64_C(0x9e3779b97f4a7c15)
#define QL_DIFF_MAX_TESTS UINT64_C(4000000)

/* Concrete differential refutation.

   This method never solves anything. It generates concrete inputs, runs both
   lowered functions through the IR interpreter, re-evaluates the typed
   precondition concretely, and re-derives the declared relation on the
   results. That is exactly the replay path a solver model must survive before
   it becomes a counterexample, so a mismatch found here is replay-confirmed by
   construction and needs no second opinion.

   It is a refutation method and nothing else.

     - A confirmed mismatch is QL_VERDICT_COUNTEREXAMPLE.
     - Finding nothing is QL_VERDICT_UNKNOWN. It is not BOUNDED_CLEAN.

   METHODS.md permits BOUNDED_CLEAN for a differential runner in general, and
   this implementation deliberately does not take it. BOUNDED_CLEAN states that
   a bound was exhausted. Boundary-and-random sampling exhausts no bound over a
   64-bit input space: it covers an unmeasured fraction of it. Reporting a
   number of passing tests as a clean bound would name a guarantee the search
   does not provide, so the search parameters are recorded as diagnostics on an
   UNKNOWN verdict instead.

   The generator is deterministic. The same seed, test count, and problem
   produce the same inputs in the same order on every platform, which is what
   makes the outcome cacheable and a reported counterexample reproducible. */

typedef struct ql_diff_outcome_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_verdict verdict;
    ql_evidence_class evidence_class;
    /* Always zero. This method checks no proof certificate because it never
       claims a proof. */
    uint32_t checked_proof;
    uint32_t replay_confirmed;
    uint64_t seed;
    /* Inputs generated and handed to the replay path. */
    uint64_t tests_executed;
    /* Runs where both sides were modelled and the precondition held, so the
       relation could actually be decided. */
    uint64_t tests_conclusive;
    /* Runs rejected by the precondition. A large share here means the
       generator is mostly sampling outside the compared domain. */
    uint64_t tests_precondition_rejected;
    ql_digest problem_digest;
    ql_digest cache_key;
    ql_digest counterexample_digest;
    char diagnostic[QL_ERROR_MESSAGE_CAPACITY];
    uint64_t reserved[4];
} ql_diff_outcome_view_v1;

/* Method options, all optional:

     {"seed":6364136223846793005,
      "tests":512}

   An unknown key, a non-integer value, or a test count above
   QL_DIFF_MAX_TESTS is rejected before the run starts. */
QL_API const ql_method_v1 *QL_CALL ql_diff_method(void);
QL_API const ql_proof_method_v1 *QL_CALL ql_diff_proof_method(void);
QL_API ql_status QL_CALL ql_register_diff_method(ql_registry *registry,
                                                 ql_error *error);

QL_API ql_status QL_CALL ql_diff_outcome_read(
    const ql_artifact *artifact, ql_diff_outcome_view_v1 *view,
    ql_error *error);
/* The embedded counterexample as its own artifact, or null when the outcome
   carries none. */
QL_API ql_status QL_CALL ql_diff_outcome_counterexample(
    const ql_allocator *allocator, const ql_artifact *artifact,
    ql_artifact **output, ql_error *error);

QL_EXTERN_C_END

#endif
