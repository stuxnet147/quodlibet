#ifndef QUODLIBET_POLICY_H
#define QUODLIBET_POLICY_H

#include "quodlibet/allocator.h"
#include "quodlibet/semantics.h"
#include "quodlibet/status.h"

#define QL_POLICY_ABI_VERSION 1u
#define QL_POLICY_SCHEMA_VERSION 1u
#define QL_POLICY_RESULT_SCHEMA_VERSION 1u
#define QL_POLICY_MAX_NAME 63u
#define QL_POLICY_MAX_CLASSES 16u
#define QL_POLICY_MAX_TRUSTED_BACKENDS 16u

QL_EXTERN_C_BEGIN

typedef struct ql_policy ql_policy;

/* What the caller counts a class as. This is the caller's vocabulary; it says
   nothing about what the core proved. */
typedef enum ql_policy_disposition {
    QL_POLICY_DISPOSITION_PASS = 0,
    QL_POLICY_DISPOSITION_FAIL = 1,
    QL_POLICY_DISPOSITION_ABSTAIN = 2
} ql_policy_disposition;

/* How strong a claim the caller attaches to a class. A class may only claim
   QL_POLICY_CLAIMS_PROOF for verdicts the core actually proved. */
typedef enum ql_policy_claims {
    QL_POLICY_CLAIMS_NONE = 0,
    QL_POLICY_CLAIMS_HEURISTIC = 1,
    QL_POLICY_CLAIMS_EVIDENCE = 2,
    QL_POLICY_CLAIMS_PROOF = 3
} ql_policy_claims;

/* How a class that accepts a PROVED_* verdict justifies trusting it. A class
   accepting PROVED_* must name one of these; there is no third option and no
   default, because the missing case is exactly "raw solver UNSAT promoted with
   no checker and no stated trusted backend". */
typedef enum ql_policy_proof_trust {
    QL_POLICY_PROOF_TRUST_UNSET = 0,
    QL_POLICY_PROOF_TRUST_CHECKED = 1,
    QL_POLICY_PROOF_TRUST_TRUSTED_BACKEND = 2
} ql_policy_proof_trust;

typedef struct ql_policy_class_view_v1 {
    size_t struct_size;
    uint32_t abi_version;
    ql_policy_disposition disposition;
    ql_policy_claims claims;
    ql_policy_proof_trust proof_trust;
    uint32_t require_replayed_witness;
    uint32_t verdict_count;
    const char *name;
    const ql_verdict *verdicts;
    double score;
    uint64_t reserved[4];
} ql_policy_class_view_v1;

/* Facts the core knows about how a verdict was produced. The policy consumes
   them; it cannot invent them. */
typedef struct ql_policy_evidence_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t budget_exhausted;
    uint32_t witness_replayed;
    uint32_t proof_checked;
    const char *backend_identity;
    const char *checker_identity;
    uint64_t reserved[4];
} ql_policy_evidence_v1;

/* Self-contained so it can be copied, serialized and handed to a binding
   without owning any pointer into the policy. */
typedef struct ql_policy_result_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t schema_version;
    ql_verdict reported_verdict;
    ql_verdict effective_verdict;
    ql_policy_disposition disposition;
    ql_policy_claims claims;
    uint32_t weakened;
    uint32_t gated;
    double score;
    uint64_t checked_bound;
    char policy_name[QL_POLICY_MAX_NAME + 1u];
    char class_name[QL_POLICY_MAX_NAME + 1u];
    char gate_reason[QL_POLICY_MAX_NAME + 1u];
    uint64_t reserved[4];
} ql_policy_result_v1;

QL_API void QL_CALL ql_policy_evidence_init(ql_policy_evidence_v1 *evidence);
QL_API void QL_CALL ql_policy_result_init(ql_policy_result_v1 *result);

QL_API const char *QL_CALL ql_policy_disposition_string(
    ql_policy_disposition disposition);
QL_API const char *QL_CALL ql_policy_claims_string(ql_policy_claims claims);
QL_API const char *QL_CALL ql_policy_proof_trust_string(
    ql_policy_proof_trust trust);
/* Lowercase, hyphen-free verdict names used by the JSON schemas, distinct from
   the upper-case display names ql_verdict_string returns. */
QL_API const char *QL_CALL ql_policy_verdict_name(ql_verdict verdict);
QL_API ql_status QL_CALL ql_policy_verdict_parse(const char *text,
                                                 ql_verdict *verdict,
                                                 ql_error *error);

/* Parses and fully validates schema v1 before any run. Every rejection names
   the offending location: a JSON pointer for a semantic error, a byte offset
   for a syntax error.

   Schema v1:

     {
       "schema_version": 1,
       "name": "rl-reward",
       "description": "optional",
       "classes": [
         {
           "name": "accept",
           "disposition": "pass" | "fail" | "abstain",
           "claims": "none" | "heuristic" | "evidence" | "proof",
           "verdicts": ["proved_equivalent", ...],
           "proof_trust": "checked" | "trusted_backend",
           "require_replayed_witness": true,
           "score": 1.0
         }
       ],
       "default_class": "open",
       "weaken": [ {"from": "bounded_clean", "to": "unknown"} ],
       "trust": { "trusted_backends": ["bitwuzla 0.9.1"] }
     }

   A policy decides which already-justified verdicts count as a pass and what
   to call them. It cannot manufacture a verdict, so the parser rejects:

     - a class with "claims": "proof" that lists anything but a proved_*
       verdict, which is how a policy would try to call BOUNDED_CLEAN a proof;
     - a "weaken" entry whose "to" is neither "unknown" nor equal to its
       "from", which is how a policy would try to promote a verdict outright;
     - a class listing "counterexample" without
       "require_replayed_witness": true, which is how an unreplayed SAT model
       would become a confirmed counterexample;
     - a class listing a proved_* verdict without "proof_trust", and a
       "proof_trust": "trusted_backend" with no entry in
       trust.trusted_backends, which is how a raw solver UNSAT would become a
       proof with neither a checker nor a stated trusted backend. */
QL_API ql_status QL_CALL ql_policy_parse(const ql_allocator *allocator,
                                         const char *json, size_t json_size,
                                         ql_policy **output, ql_error *error);
QL_API void QL_CALL ql_policy_destroy(ql_policy *policy);

QL_API const char *QL_CALL ql_policy_name(const ql_policy *policy);
QL_API const char *QL_CALL ql_policy_description(const ql_policy *policy);
QL_API size_t QL_CALL ql_policy_class_count(const ql_policy *policy);
QL_API ql_status QL_CALL ql_policy_class_at(const ql_policy *policy,
                                            size_t index,
                                            ql_policy_class_view_v1 *view,
                                            ql_error *error);

/* Applies the runtime half of the same discipline. Before classification the
   effective verdict is withdrawn to UNKNOWN when the run exhausted its budget,
   when a COUNTEREXAMPLE has no replayed witness, or when a PROVED_* verdict
   has neither a checked proof nor an accepted trusted backend. Then the
   policy's weakening map is applied, and the surviving verdict selects a
   class. */
QL_API ql_status QL_CALL ql_policy_evaluate(
    const ql_policy *policy, const ql_outcome_v1 *outcome,
    const ql_policy_evidence_v1 *evidence, ql_policy_result_v1 *result,
    ql_error *error);

/* Canonical serialization. Both writers report the exact size they need in
   `written` and return QL_STATUS_INVALID_ARGUMENT without touching the buffer
   when capacity is short, so a caller can size then write. Output of
   ql_policy_serialize parses back through ql_policy_parse, and output of
   ql_policy_result_serialize parses back through ql_policy_result_parse. */
QL_API ql_status QL_CALL ql_policy_serialize(const ql_policy *policy,
                                             char *buffer, size_t capacity,
                                             size_t *written,
                                             ql_error *error);
QL_API ql_status QL_CALL ql_policy_result_serialize(
    const ql_policy_result_v1 *result, char *buffer, size_t capacity,
    size_t *written, ql_error *error);
QL_API ql_status QL_CALL ql_policy_result_parse(const char *json,
                                                size_t json_size,
                                                ql_policy_result_v1 *result,
                                                ql_error *error);

QL_EXTERN_C_END

#endif
