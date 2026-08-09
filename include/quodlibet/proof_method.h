#ifndef QUODLIBET_PROOF_METHOD_H
#define QUODLIBET_PROOF_METHOD_H

#include "quodlibet/method.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

/* Families describe an algorithmic integration point, not a built-in engine.
   Values at or above the extension base are available to third-party methods. */
typedef uint32_t ql_proof_method_family;

#define QL_PROOF_METHOD_FAMILY_INVALID UINT32_C(0)
#define QL_PROOF_METHOD_FAMILY_REWRITE_EGRAPH UINT32_C(1)
#define QL_PROOF_METHOD_FAMILY_SMT UINT32_C(2)
#define QL_PROOF_METHOD_FAMILY_AIG_SAT UINT32_C(3)
#define QL_PROOF_METHOD_FAMILY_BOUNDED_EXECUTION UINT32_C(4)
#define QL_PROOF_METHOD_FAMILY_CONCRETE_DIFFERENTIAL UINT32_C(5)
#define QL_PROOF_METHOD_FAMILY_CHC_PDR UINT32_C(6)
#define QL_PROOF_METHOD_FAMILY_EXTENSION_BASE UINT32_C(1024)

/* These bits state which logical claims made by the method are sound. A zero
   value is suitable only for methods that produce UNKNOWN. */
typedef enum ql_proof_soundness_class {
    QL_PROOF_SOUNDNESS_NONE = 0,
    QL_PROOF_SOUNDNESS_PROOF = UINT32_C(1) << 0,
    QL_PROOF_SOUNDNESS_COUNTEREXAMPLE = UINT32_C(1) << 1,
    QL_PROOF_SOUNDNESS_BOUNDED = UINT32_C(1) << 2
} ql_proof_soundness_class;

typedef enum ql_proof_relation_capability {
    QL_PROOF_RELATION_EQUIVALENCE = UINT32_C(1) << 0,
    QL_PROOF_RELATION_LEFT_REFINEMENT = UINT32_C(1) << 1,
    QL_PROOF_RELATION_RIGHT_REFINEMENT = UINT32_C(1) << 2
} ql_proof_relation_capability;

#define QL_PROOF_RELATION_ALL                                            \
    (UINT32_C(0) | QL_PROOF_RELATION_EQUIVALENCE |                       \
     QL_PROOF_RELATION_LEFT_REFINEMENT |                                 \
     QL_PROOF_RELATION_RIGHT_REFINEMENT)

typedef enum ql_proof_ub_capability {
    QL_PROOF_UB_MUST_MATCH = UINT32_C(1) << 0,
    QL_PROOF_UB_LANGUAGE_REFINEMENT = UINT32_C(1) << 1,
    QL_PROOF_UB_BOTH_DEFINED = UINT32_C(1) << 2
} ql_proof_ub_capability;

#define QL_PROOF_UB_ALL                                             \
    (UINT32_C(0) | QL_PROOF_UB_MUST_MATCH |                         \
     QL_PROOF_UB_LANGUAGE_REFINEMENT |                              \
     QL_PROOF_UB_BOTH_DEFINED)

typedef enum ql_proof_result_kind {
    QL_PROOF_RESULT_PROOF = UINT32_C(1) << 0,
    QL_PROOF_RESULT_COUNTEREXAMPLE = UINT32_C(1) << 1,
    QL_PROOF_RESULT_BOUNDED = UINT32_C(1) << 2,
    QL_PROOF_RESULT_UNKNOWN = UINT32_C(1) << 3
} ql_proof_result_kind;

#define QL_PROOF_RESULT_ALL                                           \
    (UINT32_C(0) | QL_PROOF_RESULT_PROOF |                            \
     QL_PROOF_RESULT_COUNTEREXAMPLE |                                 \
     QL_PROOF_RESULT_BOUNDED | QL_PROOF_RESULT_UNKNOWN)

typedef enum ql_proof_capability_flag {
    QL_PROOF_CAPABILITY_PRECONDITIONS = UINT32_C(1) << 0
} ql_proof_capability_flag;

#define QL_PROOF_MEMORY_MODE(mode) (UINT32_C(1) << (uint32_t)(mode))
#define QL_PROOF_EXTERNAL_CALL_MODE(mode) \
    (UINT32_C(1) << (uint32_t)(mode))

typedef struct ql_proof_method_capability_v1 {
    size_t struct_size;
    uint32_t abi_version;
    ql_proof_method_family family;
    uint32_t soundness_classes;
    uint32_t supported_relations;
    uint64_t supported_observations;
    uint32_t supported_ub_policies;
    uint32_t supported_memory_observations;
    uint32_t supported_external_call_observations;
    uint32_t result_kinds;
    uint32_t flags;
    uint32_t reserved32[3];
    uint64_t reserved[6];
} ql_proof_method_capability_v1;

/* The query may make capabilities depend on the node's method options. It
   receives an initialized v1 capability structure and must fill every
   non-reserved field. The options string is borrowed and may be null. */
typedef ql_status (QL_CALL *ql_proof_capability_query_v1)(
    const char *options_json, ql_proof_method_capability_v1 *capability,
    ql_error *error);

/* A proof descriptor decorates an ordinary executable method. Registration
   and scheduling remain the responsibility of ql_registry and ql_pipeline. */
typedef struct ql_proof_method_v1 {
    size_t struct_size;
    uint32_t abi_version;
    ql_proof_method_family family;
    const ql_method_v1 *method;
    ql_proof_capability_query_v1 query_capability;
    void *reserved[8];
} ql_proof_method_v1;

/* The contract is borrowed. requested_result_kinds and required_soundness are
   both subset requirements, so a caller can select proof-only, refutation-only,
   bounded, or mixed methods without changing the pipeline ABI. */
typedef struct ql_proof_request_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t requested_result_kinds;
    uint32_t required_soundness;
    uint32_t reserved32;
    const ql_semantic_contract_v1 *contract;
    uint64_t reserved[6];
} ql_proof_request_v1;

QL_API void QL_CALL ql_proof_method_capability_init(
    ql_proof_method_capability_v1 *capability);
QL_API void QL_CALL ql_proof_request_init(
    ql_proof_request_v1 *request,
    const ql_semantic_contract_v1 *contract);

QL_API ql_status QL_CALL ql_proof_method_capability_validate(
    const ql_proof_method_capability_v1 *capability, ql_error *error);
QL_API ql_status QL_CALL ql_proof_method_validate(
    const ql_proof_method_v1 *proof_method, ql_error *error);
QL_API ql_status QL_CALL ql_proof_method_query_capability(
    const ql_proof_method_v1 *proof_method, const char *options_json,
    ql_proof_method_capability_v1 *capability, ql_error *error);
QL_API ql_status QL_CALL ql_proof_method_validate_request(
    const ql_proof_method_capability_v1 *capability,
    const ql_proof_request_v1 *request, ql_error *error);

/* Returns one QL_PROOF_RESULT_* bit, or zero for an invalid verdict. */
QL_API uint32_t QL_CALL ql_proof_result_kind_from_verdict(
    ql_verdict verdict);

QL_EXTERN_C_END

#endif
