#include "quodlibet/proof_method.h"

#include <stddef.h>
#include <string.h>

static int valid_family(ql_proof_method_family family) {
    return (family >= QL_PROOF_METHOD_FAMILY_REWRITE_EGRAPH &&
            family <= QL_PROOF_METHOD_FAMILY_CHC_PDR) ||
           family >= QL_PROOF_METHOD_FAMILY_EXTENSION_BASE;
}

static uint32_t relation_bit(ql_relation relation) {
    switch (relation) {
    case QL_RELATION_EQUIVALENCE:
        return QL_PROOF_RELATION_EQUIVALENCE;
    case QL_RELATION_LEFT_REFINES_RIGHT:
        return QL_PROOF_RELATION_LEFT_REFINEMENT;
    case QL_RELATION_RIGHT_REFINES_LEFT:
        return QL_PROOF_RELATION_RIGHT_REFINEMENT;
    default:
        return 0u;
    }
}

static uint32_t ub_policy_bit(ql_ub_policy policy) {
    switch (policy) {
    case QL_UB_MUST_MATCH:
        return QL_PROOF_UB_MUST_MATCH;
    case QL_UB_LANGUAGE_REFINEMENT:
        return QL_PROOF_UB_LANGUAGE_REFINEMENT;
    case QL_UB_COMPARE_WHERE_BOTH_DEFINED:
        return QL_PROOF_UB_BOTH_DEFINED;
    default:
        return 0u;
    }
}

void QL_CALL ql_proof_method_capability_init(
    ql_proof_method_capability_v1 *capability) {
    if (capability == NULL) {
        return;
    }
    memset(capability, 0, sizeof(*capability));
    capability->struct_size = sizeof(*capability);
    capability->abi_version = QL_ABI_VERSION;
}

void QL_CALL ql_proof_request_init(
    ql_proof_request_v1 *request,
    const ql_semantic_contract_v1 *contract) {
    if (request == NULL) {
        return;
    }
    memset(request, 0, sizeof(*request));
    request->struct_size = sizeof(*request);
    request->abi_version = QL_ABI_VERSION;
    request->contract = contract;
}

ql_status QL_CALL ql_proof_method_capability_validate(
    const ql_proof_method_capability_v1 *capability, ql_error *error) {
    const size_t minimum_size =
        offsetof(ql_proof_method_capability_v1, reserved);
    const uint32_t known_soundness =
        QL_PROOF_SOUNDNESS_PROOF | QL_PROOF_SOUNDNESS_COUNTEREXAMPLE |
        QL_PROOF_SOUNDNESS_BOUNDED;
    const uint32_t known_relations = QL_PROOF_RELATION_ALL;
    const uint32_t known_ub_policies = QL_PROOF_UB_ALL;
    const uint64_t known_observations = QL_OBSERVE_ALL;
    const uint32_t known_results = QL_PROOF_RESULT_ALL;
    const uint32_t known_flags = QL_PROOF_CAPABILITY_PRECONDITIONS;
    const uint32_t known_memory_modes =
        QL_PROOF_MEMORY_MODE(QL_MEMORY_IGNORE) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FINAL_REACHABLE_STATE) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_ORDERED_WRITES) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FULL_TRACE);
    const uint32_t known_external_call_modes =
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_IGNORE) |
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_ORDERED_TRACE);

    if (capability == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "proof-method capability is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (capability->abi_version != QL_ABI_VERSION ||
        capability->struct_size < minimum_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "proof-method capability uses ABI %u and structure size %zu; host requires ABI %u and at least %zu bytes",
                     capability->abi_version, capability->struct_size,
                     QL_ABI_VERSION, minimum_size);
        return QL_STATUS_ABI_MISMATCH;
    }
    if (!valid_family(capability->family)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "proof-method family %u is invalid", capability->family);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((capability->soundness_classes & ~known_soundness) != 0u ||
        capability->supported_relations == 0u ||
        (capability->supported_relations & ~known_relations) != 0u ||
        capability->supported_ub_policies == 0u ||
        (capability->supported_ub_policies & ~known_ub_policies) != 0u ||
        capability->supported_observations == 0u ||
        (capability->supported_observations & ~known_observations) != 0u ||
        capability->result_kinds == 0u ||
        (capability->result_kinds & ~known_results) != 0u ||
        (capability->flags & ~known_flags) != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "proof-method capability contains invalid support bits");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((capability->supported_memory_observations & ~known_memory_modes) !=
            0u ||
        (capability->supported_external_call_observations &
         ~known_external_call_modes) != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "proof-method capability contains invalid observation modes");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (((capability->supported_observations & QL_OBSERVE_MEMORY) != 0u) !=
        ((capability->supported_memory_observations &
          ~QL_PROOF_MEMORY_MODE(QL_MEMORY_IGNORE)) != 0u)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "memory observation axis and modes disagree");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (((capability->supported_observations &
          QL_OBSERVE_EXTERNAL_CALLS) != 0u) !=
        ((capability->supported_external_call_observations &
          ~QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_IGNORE)) != 0u)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "external-call observation axis and modes disagree");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (((capability->result_kinds & QL_PROOF_RESULT_PROOF) != 0u &&
         (capability->soundness_classes & QL_PROOF_SOUNDNESS_PROOF) == 0u) ||
        ((capability->result_kinds & QL_PROOF_RESULT_COUNTEREXAMPLE) != 0u &&
         (capability->soundness_classes &
          QL_PROOF_SOUNDNESS_COUNTEREXAMPLE) == 0u) ||
        ((capability->result_kinds & QL_PROOF_RESULT_BOUNDED) != 0u &&
         (capability->soundness_classes & QL_PROOF_SOUNDNESS_BOUNDED) == 0u)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a conclusive result kind lacks its soundness class");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_proof_method_validate(
    const ql_proof_method_v1 *proof_method, ql_error *error) {
    const size_t proof_minimum_size =
        offsetof(ql_proof_method_v1, reserved);
    const size_t method_minimum_size = offsetof(ql_method_v1, reserved);

    if (proof_method == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "proof-method descriptor is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (proof_method->abi_version != QL_ABI_VERSION ||
        proof_method->struct_size < proof_minimum_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "proof-method descriptor uses ABI %u and structure size %zu; host requires ABI %u and at least %zu bytes",
                     proof_method->abi_version, proof_method->struct_size,
                     QL_ABI_VERSION, proof_minimum_size);
        return QL_STATUS_ABI_MISMATCH;
    }
    if (!valid_family(proof_method->family) ||
        proof_method->query_capability == NULL ||
        proof_method->method == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "proof method requires a valid family, executable method, and capability query");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (proof_method->method->abi_version != QL_ABI_VERSION ||
        proof_method->method->struct_size < method_minimum_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "underlying method has an incompatible ABI");
        return QL_STATUS_ABI_MISMATCH;
    }
    if (proof_method->method->name == NULL ||
        proof_method->method->name[0] == '\0' ||
        proof_method->method->run == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "proof method requires a named executable method");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_proof_method_query_capability(
    const ql_proof_method_v1 *proof_method, const char *options_json,
    ql_proof_method_capability_v1 *capability, ql_error *error) {
    ql_status status;

    if (capability == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "capability output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = ql_proof_method_validate(proof_method, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    ql_proof_method_capability_init(capability);
    status = proof_method->query_capability(options_json, capability, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = ql_proof_method_capability_validate(capability, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (capability->family != proof_method->family) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "capability family %u does not match descriptor family %u",
                     capability->family, proof_method->family);
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (((capability->result_kinds & QL_PROOF_RESULT_PROOF) != 0u &&
         (proof_method->method->flags & QL_METHOD_PROOF_PRODUCER) == 0u) ||
        ((capability->result_kinds & QL_PROOF_RESULT_COUNTEREXAMPLE) != 0u &&
         (proof_method->method->flags &
          QL_METHOD_COUNTEREXAMPLE_PRODUCER) == 0u)) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "proof capability conflicts with underlying method flags");
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_proof_method_validate_request(
    const ql_proof_method_capability_v1 *capability,
    const ql_proof_request_v1 *request, ql_error *error) {
    const size_t request_minimum_size =
        offsetof(ql_proof_request_v1, reserved);
    ql_status status;
    uint32_t relation;
    uint32_t ub_policy;
    const uint32_t known_results = QL_PROOF_RESULT_ALL;
    const uint32_t known_soundness =
        QL_PROOF_SOUNDNESS_PROOF | QL_PROOF_SOUNDNESS_COUNTEREXAMPLE |
        QL_PROOF_SOUNDNESS_BOUNDED;

    status = ql_proof_method_capability_validate(capability, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (request == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "proof request is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (request->abi_version != QL_ABI_VERSION ||
        request->struct_size < request_minimum_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "proof request uses ABI %u and structure size %zu; host requires ABI %u and at least %zu bytes",
                     request->abi_version, request->struct_size,
                     QL_ABI_VERSION, request_minimum_size);
        return QL_STATUS_ABI_MISMATCH;
    }
    if (request->requested_result_kinds == 0u ||
        (request->requested_result_kinds & ~known_results) != 0u ||
        (request->required_soundness & ~known_soundness) != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "proof request contains invalid result or soundness bits");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (request->contract == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "proof request requires a semantic contract");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = ql_semantic_contract_validate(request->contract, error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    relation = relation_bit(request->contract->relation);
    ub_policy = ub_policy_bit(request->contract->ub_policy);
    if ((capability->supported_relations & relation) == 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "proof method does not support the requested relation");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if ((capability->supported_ub_policies & ub_policy) == 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "proof method does not support the requested UB policy");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if ((request->contract->observations &
         ~capability->supported_observations) != 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "proof method does not support every requested observation axis");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if ((request->contract->observations & QL_OBSERVE_MEMORY) != 0u &&
        (capability->supported_memory_observations &
         QL_PROOF_MEMORY_MODE(request->contract->memory_observation)) == 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "proof method does not support the requested memory observation mode");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if ((request->contract->observations & QL_OBSERVE_EXTERNAL_CALLS) != 0u &&
        (capability->supported_external_call_observations &
         QL_PROOF_EXTERNAL_CALL_MODE(
             request->contract->external_call_observation)) == 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "proof method does not support the requested external-call observation mode");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if (request->contract->precondition_json != NULL &&
        (capability->flags & QL_PROOF_CAPABILITY_PRECONDITIONS) == 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "proof method does not support input preconditions");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if ((request->requested_result_kinds & ~capability->result_kinds) != 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "proof method cannot produce every requested result kind");
        return QL_STATUS_TYPE_MISMATCH;
    }
    if ((request->required_soundness &
         ~capability->soundness_classes) != 0u) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "proof method does not satisfy the required soundness classes");
        return QL_STATUS_TYPE_MISMATCH;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

uint32_t QL_CALL ql_proof_result_kind_from_verdict(ql_verdict verdict) {
    switch (verdict) {
    case QL_VERDICT_PROVED_EQUIVALENT:
    case QL_VERDICT_PROVED_LEFT_REFINES_RIGHT:
    case QL_VERDICT_PROVED_RIGHT_REFINES_LEFT:
        return QL_PROOF_RESULT_PROOF;
    case QL_VERDICT_COUNTEREXAMPLE:
        return QL_PROOF_RESULT_COUNTEREXAMPLE;
    case QL_VERDICT_BOUNDED_CLEAN:
        return QL_PROOF_RESULT_BOUNDED;
    case QL_VERDICT_UNKNOWN:
        return QL_PROOF_RESULT_UNKNOWN;
    default:
        return 0u;
    }
}
