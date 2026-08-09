#include "quodlibet/proof_method.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

namespace {

ql_status QL_CALL unused_run(void *, const ql_run_context_v1 *,
                             ql_artifact *const *, std::size_t,
                             ql_artifact **, ql_error *) {
    return QL_STATUS_METHOD_ERROR;
}

ql_status QL_CALL query_smt(const char *,
                            ql_proof_method_capability_v1 *capability,
                            ql_error *) {
    capability->family = QL_PROOF_METHOD_FAMILY_SMT;
    capability->soundness_classes =
        QL_PROOF_SOUNDNESS_PROOF | QL_PROOF_SOUNDNESS_COUNTEREXAMPLE;
    capability->supported_relations = QL_PROOF_RELATION_ALL;
    capability->supported_observations = QL_OBSERVE_ALL;
    capability->supported_ub_policies = QL_PROOF_UB_ALL;
    capability->supported_memory_observations =
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FINAL_REACHABLE_STATE) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_ORDERED_WRITES) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FULL_TRACE);
    capability->supported_external_call_observations =
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_ORDERED_TRACE);
    capability->result_kinds = QL_PROOF_RESULT_PROOF |
                               QL_PROOF_RESULT_COUNTEREXAMPLE |
                               QL_PROOF_RESULT_UNKNOWN;
    capability->flags = QL_PROOF_CAPABILITY_PRECONDITIONS;
    return QL_STATUS_OK;
}

ql_status QL_CALL query_wrong_family(
    const char *, ql_proof_method_capability_v1 *capability, ql_error *) {
    const ql_status status = query_smt(nullptr, capability, nullptr);
    capability->family = QL_PROOF_METHOD_FAMILY_AIG_SAT;
    return status;
}

ql_method_v1 make_method() {
    ql_method_v1 method{};
    method.struct_size = sizeof(method);
    method.abi_version = QL_ABI_VERSION;
    method.name = "test.smt";
    method.description = "test only";
    method.output_kind = "quodlibet.outcome";
    method.flags = QL_METHOD_PROOF_PRODUCER |
                   QL_METHOD_COUNTEREXAMPLE_PRODUCER;
    method.minimum_inputs = 1u;
    method.maximum_inputs = 1u;
    method.run = unused_run;
    return method;
}

ql_proof_method_v1 make_proof_method(const ql_method_v1 *method) {
    ql_proof_method_v1 proof_method{};
    proof_method.struct_size = sizeof(proof_method);
    proof_method.abi_version = QL_ABI_VERSION;
    proof_method.family = QL_PROOF_METHOD_FAMILY_SMT;
    proof_method.method = method;
    proof_method.query_capability = query_smt;
    return proof_method;
}

TEST(ProofMethodABI, UsesFixedWidthCapabilityBitsAndAppendOnlyPrefixes) {
    static_assert(std::is_standard_layout_v<ql_proof_method_capability_v1>);
    static_assert(std::is_standard_layout_v<ql_proof_method_v1>);
    static_assert(std::is_standard_layout_v<ql_proof_request_v1>);
    static_assert(sizeof(ql_proof_method_family) == sizeof(std::uint32_t));
    static_assert(sizeof(decltype(ql_proof_method_capability_v1::
                                      supported_observations)) ==
                  sizeof(std::uint64_t));
    static_assert(offsetof(ql_proof_method_capability_v1, struct_size) == 0u);
    static_assert(offsetof(ql_proof_method_v1, struct_size) == 0u);
    static_assert(offsetof(ql_proof_request_v1, struct_size) == 0u);
    static_assert(offsetof(ql_proof_method_capability_v1, reserved) %
                      alignof(std::uint64_t) ==
                  0u);
    static_assert(sizeof(ql_proof_method_capability_v1::reserved) ==
                  6u * sizeof(std::uint64_t));

    EXPECT_EQ(UINT32_C(1024), QL_PROOF_METHOD_FAMILY_EXTENSION_BASE);
    EXPECT_EQ(0u, static_cast<std::uint32_t>(
                      QL_PROOF_RESULT_PROOF &
                      QL_PROOF_RESULT_COUNTEREXAMPLE));
    EXPECT_EQ(UINT32_C(15), static_cast<std::uint32_t>(QL_PROOF_RESULT_ALL));
    EXPECT_EQ(UINT32_C(7),
              static_cast<std::uint32_t>(QL_PROOF_RELATION_ALL));
    EXPECT_EQ(UINT32_C(7), static_cast<std::uint32_t>(QL_PROOF_UB_ALL));
    if constexpr (sizeof(void *) == 8u) {
        EXPECT_EQ(112u, sizeof(ql_proof_method_capability_v1));
        EXPECT_EQ(96u, sizeof(ql_proof_method_v1));
        EXPECT_EQ(80u, sizeof(ql_proof_request_v1));
        EXPECT_EQ(24u, offsetof(ql_proof_method_capability_v1,
                                supported_observations));
        EXPECT_EQ(24u, offsetof(ql_proof_request_v1, contract));
    }
}

TEST(ProofMethod, QueriesOptionDependentCapability) {
    ql_method_v1 method = make_method();
    ql_proof_method_v1 proof_method = make_proof_method(&method);
    ql_proof_method_capability_v1 capability{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_proof_method_query_capability(
                  &proof_method, "{\"logic\":\"QF_BV\"}", &capability,
                  &error))
        << error.message;
    EXPECT_EQ(QL_PROOF_METHOD_FAMILY_SMT, capability.family);
    EXPECT_EQ(QL_PROOF_RELATION_ALL, capability.supported_relations);
    EXPECT_EQ(static_cast<std::uint64_t>(QL_OBSERVE_ALL),
              capability.supported_observations);
    EXPECT_NE(0u, capability.result_kinds & QL_PROOF_RESULT_PROOF);
}

TEST(ProofMethod, RejectsDescriptorAndCapabilityFamilyMismatch) {
    ql_method_v1 method = make_method();
    ql_proof_method_v1 proof_method = make_proof_method(&method);
    ql_proof_method_capability_v1 capability{};
    ql_error error{};

    proof_method.query_capability = query_wrong_family;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_proof_method_query_capability(
                  &proof_method, nullptr, &capability, &error));
}

TEST(ProofMethod, RejectsTruncatedABIAndUnknownBits) {
    ql_proof_method_capability_v1 capability{};
    ql_error error{};

    ql_proof_method_capability_init(&capability);
    capability.family = QL_PROOF_METHOD_FAMILY_REWRITE_EGRAPH;
    capability.soundness_classes = QL_PROOF_SOUNDNESS_PROOF;
    capability.supported_relations = QL_PROOF_RELATION_EQUIVALENCE;
    capability.supported_observations = QL_OBSERVE_RETURN_VALUE;
    capability.supported_ub_policies = QL_PROOF_UB_MUST_MATCH;
    capability.result_kinds = QL_PROOF_RESULT_PROOF;

    capability.struct_size =
        offsetof(ql_proof_method_capability_v1, reserved) - 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_proof_method_capability_validate(&capability, &error));

    capability.struct_size = sizeof(capability);
    capability.result_kinds |= UINT32_C(1) << 31;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_proof_method_capability_validate(&capability, &error));
}

TEST(ProofMethod, AcceptsCatalogueAndExtensionFamilies) {
    constexpr ql_proof_method_family families[] = {
        QL_PROOF_METHOD_FAMILY_REWRITE_EGRAPH,
        QL_PROOF_METHOD_FAMILY_SMT,
        QL_PROOF_METHOD_FAMILY_AIG_SAT,
        QL_PROOF_METHOD_FAMILY_BOUNDED_EXECUTION,
        QL_PROOF_METHOD_FAMILY_CONCRETE_DIFFERENTIAL,
        QL_PROOF_METHOD_FAMILY_CHC_PDR,
        QL_PROOF_METHOD_FAMILY_EXTENSION_BASE,
    };

    for (const ql_proof_method_family family : families) {
        ql_proof_method_capability_v1 capability{};
        ql_error error{};

        ql_proof_method_capability_init(&capability);
        capability.family = family;
        capability.supported_relations = QL_PROOF_RELATION_EQUIVALENCE;
        capability.supported_observations = QL_OBSERVE_RETURN_VALUE;
        capability.supported_ub_policies = QL_PROOF_UB_MUST_MATCH;
        capability.result_kinds = QL_PROOF_RESULT_UNKNOWN;
        EXPECT_EQ(QL_STATUS_OK,
                  ql_proof_method_capability_validate(&capability, &error))
            << "family=" << family << ": " << error.message;
    }
}

TEST(ProofMethod, ValidatesFullSemanticRequest) {
    ql_method_v1 method = make_method();
    ql_proof_method_v1 proof_method = make_proof_method(&method);
    ql_proof_method_capability_v1 capability{};
    ql_semantic_contract_v1 contract{};
    ql_proof_request_v1 request{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_proof_method_query_capability(
                  &proof_method, nullptr, &capability, &error));
    ql_semantic_contract_init(&contract);
    ql_proof_request_init(&request, &contract);
    request.requested_result_kinds =
        QL_PROOF_RESULT_PROOF | QL_PROOF_RESULT_COUNTEREXAMPLE;
    request.required_soundness =
        QL_PROOF_SOUNDNESS_PROOF | QL_PROOF_SOUNDNESS_COUNTEREXAMPLE;

    EXPECT_EQ(QL_STATUS_OK,
              ql_proof_method_validate_request(&capability, &request,
                                               &error))
        << error.message;
}

TEST(ProofMethod, RejectsUnsupportedRelationObservationAndResult) {
    ql_proof_method_capability_v1 capability{};
    ql_semantic_contract_v1 contract{};
    ql_proof_request_v1 request{};
    ql_error error{};

    ql_proof_method_capability_init(&capability);
    capability.family = QL_PROOF_METHOD_FAMILY_BOUNDED_EXECUTION;
    capability.soundness_classes = QL_PROOF_SOUNDNESS_COUNTEREXAMPLE |
                                   QL_PROOF_SOUNDNESS_BOUNDED;
    capability.supported_relations = QL_PROOF_RELATION_EQUIVALENCE;
    capability.supported_observations = QL_OBSERVE_RETURN_VALUE;
    capability.supported_ub_policies = QL_PROOF_UB_BOTH_DEFINED;
    capability.result_kinds = QL_PROOF_RESULT_COUNTEREXAMPLE |
                              QL_PROOF_RESULT_BOUNDED |
                              QL_PROOF_RESULT_UNKNOWN;
    ASSERT_EQ(QL_STATUS_OK,
              ql_proof_method_capability_validate(&capability, &error));

    ql_semantic_contract_init(&contract);
    contract.relation = QL_RELATION_LEFT_REFINES_RIGHT;
    contract.ub_policy = QL_UB_COMPARE_WHERE_BOTH_DEFINED;
    contract.observations = QL_OBSERVE_RETURN_VALUE;
    contract.memory_observation = QL_MEMORY_IGNORE;
    contract.external_call_observation = QL_EXTERNAL_CALLS_IGNORE;
    ql_proof_request_init(&request, &contract);
    request.requested_result_kinds = QL_PROOF_RESULT_BOUNDED;
    request.required_soundness = QL_PROOF_SOUNDNESS_BOUNDED;

    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_proof_method_validate_request(&capability, &request,
                                               &error));

    contract.relation = QL_RELATION_EQUIVALENCE;
    contract.observations |= QL_OBSERVE_TRAPS;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_proof_method_validate_request(&capability, &request,
                                               &error));

    contract.observations = QL_OBSERVE_RETURN_VALUE;
    request.requested_result_kinds = QL_PROOF_RESULT_PROOF;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_proof_method_validate_request(&capability, &request,
                                               &error));
}

TEST(ProofMethod, MapsVerdictsWithoutPromotingBoundedClean) {
    EXPECT_EQ(QL_PROOF_RESULT_PROOF,
              ql_proof_result_kind_from_verdict(
                  QL_VERDICT_PROVED_EQUIVALENT));
    EXPECT_EQ(QL_PROOF_RESULT_COUNTEREXAMPLE,
              ql_proof_result_kind_from_verdict(
                  QL_VERDICT_COUNTEREXAMPLE));
    EXPECT_EQ(QL_PROOF_RESULT_BOUNDED,
              ql_proof_result_kind_from_verdict(
                  QL_VERDICT_BOUNDED_CLEAN));
    EXPECT_EQ(QL_PROOF_RESULT_UNKNOWN,
              ql_proof_result_kind_from_verdict(QL_VERDICT_UNKNOWN));
    EXPECT_EQ(0u, ql_proof_result_kind_from_verdict(
                      static_cast<ql_verdict>(999)));
}

}  // namespace
