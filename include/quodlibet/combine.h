#ifndef QUODLIBET_COMBINE_H
#define QUODLIBET_COMBINE_H

#include "quodlibet/allocator.h"
#include "quodlibet/policy.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

/* Combining the results of several methods that ran in parallel.

   Parallel methods produce evidence, not votes. This combiner implements the
   rules METHODS.md states, and the shape of the API is chosen so the unsound
   moves are not expressible: there is no count of agreeing methods anywhere,
   no method priority, and no way to resolve a proof against a replayed
   counterexample.

   The one case that must not be swallowed is exactly that: a valid proof and
   a valid counterexample for the same contract. That is evidence of a defect
   in the encoding, the checker, or the trust boundary, so the result is
   inconsistent and names both inputs. */

#define QL_COMBINE_SCHEMA_VERSION 1u
#define QL_COMBINE_NO_INPUT ((size_t)-1)
#define QL_COMBINE_BOUND_NAME_CAPACITY 48u
#define QL_COMBINE_DETAIL_CAPACITY 192u

/* One dimension of a bounded search. The vector is kept whole: bounds with
   different dimensions are never collapsed into a scalar, because a caller
   cannot tell from one number which state space was covered. */
typedef struct ql_combine_bound_v1 {
    char dimension[QL_COMBINE_BOUND_NAME_CAPACITY];
    uint64_t value;
} ql_combine_bound_v1;

typedef uint32_t ql_combine_role;
#define QL_COMBINE_ROLE_INVALID UINT32_C(0)
/* Produces a verdict about the requested relation. */
#define QL_COMBINE_ROLE_DECIDER UINT32_C(1)
/* Transforms the problem and carries transformation evidence. A decider that
   ran on its output can only be transferred back when the normalization's own
   proof is present and valid. */
#define QL_COMBINE_ROLE_NORMALIZER UINT32_C(2)

/* Every input restates the contract it answered so a mismatch is caught
   instead of silently combining answers to different questions. */
typedef struct ql_combine_input_v1 {
    size_t struct_size;
    const char *method_name;
    const char *method_version;
    ql_digest problem_digest;
    ql_digest contract_digest;
    uint32_t ir_semantics_version;
    ql_relation relation;
    ql_ub_policy ub_policy;
    uint64_t observations;
    ql_memory_observation memory_observation;
    ql_external_call_observation external_call_observation;
    ql_combine_role role;
    /* Index of the normalizer this result was produced on top of, or
       QL_COMBINE_NO_INPUT. A normalizer may not itself depend on one. */
    size_t depends_on;
    /* Pipeline declaration order. Ties break on the evidence digest. */
    uint32_t declaration_order;
    /* Recorded for diagnostics and never consulted. Cancellation after a
       decisive result is a runtime optimization and must not change which
       evidence counts. */
    uint32_t completion_order;
    ql_outcome_v1 outcome;
    ql_policy_evidence_v1 evidence;
    const ql_combine_bound_v1 *bounds;
    size_t bound_count;
    uint64_t reserved[4];
} ql_combine_input_v1;

typedef struct ql_combine_request_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t schema_version;
    ql_digest problem_digest;
    ql_digest contract_digest;
    uint32_t ir_semantics_version;
    ql_relation relation;
    ql_ub_policy ub_policy;
    uint32_t reserved32;
    uint64_t observations;
    ql_memory_observation memory_observation;
    ql_external_call_observation external_call_observation;
    /* Two checked opposite refinements establish equivalence only when the
       combiner applies that rule explicitly. Everything else the rule
       requires, identical precondition, UB policy, behaviour universe, and
       observation projection, is already forced by the contract match. */
    uint32_t allow_refinement_composition;
    const char *const *trusted_backends;
    size_t trusted_backend_count;
    uint64_t reserved[4];
} ql_combine_request_v1;

typedef uint32_t ql_combine_disposition;
#define QL_COMBINE_DISPOSITION_INVALID UINT32_C(0)
/* The logical verdict survived validation and counts. */
#define QL_COMBINE_DISPOSITION_ACCEPTED UINT32_C(1)
/* The verdict was withdrawn to UNKNOWN because its evidence did not hold up.
   This is not a rejection of the method, and it never lowers a proof to a
   weaker positive claim. */
#define QL_COMBINE_DISPOSITION_WITHDRAWN UINT32_C(2)
/* Reported UNKNOWN. Neutral. */
#define QL_COMBINE_DISPOSITION_NEUTRAL UINT32_C(3)
/* A bounded-clean result, kept with its whole bound vector and never
   promoted. */
#define QL_COMBINE_DISPOSITION_BOUNDED UINT32_C(4)

typedef uint32_t ql_combine_code;
#define QL_COMBINE_CODE_NONE UINT32_C(0)
#define QL_COMBINE_CODE_BUDGET_EXHAUSTED UINT32_C(1)
#define QL_COMBINE_CODE_UNCHECKED_PROOF UINT32_C(2)
#define QL_COMBINE_CODE_UNTRUSTED_BACKEND UINT32_C(3)
#define QL_COMBINE_CODE_UNREPLAYED_WITNESS UINT32_C(4)
#define QL_COMBINE_CODE_BROKEN_NORMALIZATION UINT32_C(5)
#define QL_COMBINE_CODE_WEAKER_RELATION UINT32_C(6)

typedef struct ql_combine_finding_v1 {
    size_t input_index;
    ql_combine_disposition disposition;
    ql_combine_code code;
    ql_verdict reported_verdict;
    ql_verdict effective_verdict;
    char detail[QL_COMBINE_DETAIL_CAPACITY];
} ql_combine_finding_v1;

typedef struct ql_combine_result ql_combine_result;

/* findings has one entry per input, in input order, so the caller can always
   say why any single method did or did not count. */
typedef struct ql_combine_result_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    /* Set when a valid proof and a valid counterexample answer the same
       contract. verdict is UNKNOWN in that case and must not be resolved. */
    uint32_t inconsistent;
    ql_verdict verdict;
    uint32_t refinement_composition_applied;
    uint32_t reserved32;
    size_t deciding_input;
    size_t supporting_input;
    size_t conflicting_input;
    uint64_t accepted_count;
    uint64_t withdrawn_count;
    uint64_t bounded_count;
    const ql_combine_finding_v1 *findings;
    size_t finding_count;
    const ql_combine_bound_v1 *bounds;
    size_t bound_count;
    uint64_t reserved[4];
} ql_combine_result_view_v1;

QL_API void QL_CALL ql_combine_request_init(ql_combine_request_v1 *request);
QL_API void QL_CALL ql_combine_input_init(ql_combine_input_v1 *input);

/* Returns OK whenever the combination ran, including the inconsistent case,
   which is a result and not a call error. A contract mismatch between inputs
   is a status error, because those answers are not about one question. */
QL_API ql_status QL_CALL ql_combine_evaluate(
    const ql_allocator *allocator, const ql_combine_request_v1 *request,
    const ql_combine_input_v1 *inputs, size_t input_count,
    ql_combine_result **result, ql_error *error);

QL_API void QL_CALL ql_combine_result_release(ql_combine_result *result);
QL_API ql_status QL_CALL ql_combine_result_get_view(
    const ql_combine_result *result, ql_combine_result_view_v1 *view,
    ql_error *error);

QL_API const char *QL_CALL ql_combine_disposition_string(
    ql_combine_disposition disposition);
QL_API const char *QL_CALL ql_combine_code_string(ql_combine_code code);

QL_EXTERN_C_END

#endif
