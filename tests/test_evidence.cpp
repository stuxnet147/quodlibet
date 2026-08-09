#include "quodlibet/evidence.h"

#include <array>
#include <cstddef>
#include <cstring>

#include <gtest/gtest.h>

namespace {

ql_cache_key_input_v1 make_identity(const char *options,
                                    std::size_t options_size) {
    ql_cache_key_input_v1 identity{};
    identity.struct_size = sizeof(identity);
    ql_digest_data("input artifact", 14u, &identity.artifact_digest);
    ql_digest_data("semantic problem", 16u,
                   &identity.semantic_problem_digest);
    identity.method_name = "smt.product-program";
    identity.method_version = "2.1.0";
    identity.canonical_options = options;
    identity.canonical_options_size = options_size;
    return identity;
}

TEST(Evidence, CacheKeyIsDeterministicAndUsesEveryIdentityField) {
    constexpr char options[] = "{\"solver\":\"z3\",\"timeout_ms\":5000}";
    ql_cache_key_input_v1 base =
        make_identity(options, sizeof(options) - 1u);
    ql_digest expected{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_key_compute(&base, &expected, &error));

    auto expect_change = [&](ql_cache_key_input_v1 changed) {
        ql_digest actual{};
        ASSERT_EQ(QL_STATUS_OK,
                  ql_cache_key_compute(&changed, &actual, &error));
        EXPECT_FALSE(ql_digest_equal(&expected, &actual));
    };

    ql_cache_key_input_v1 same = base;
    ql_digest same_key{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_key_compute(&same, &same_key, &error));
    EXPECT_TRUE(ql_digest_equal(&expected, &same_key));

    ql_cache_key_input_v1 changed = base;
    ql_digest_data("other artifact", 14u, &changed.artifact_digest);
    expect_change(changed);

    changed = base;
    ql_digest_data("other problem", 13u,
                   &changed.semantic_problem_digest);
    expect_change(changed);

    changed = base;
    changed.method_name = "egraph.normalize";
    expect_change(changed);

    changed = base;
    changed.method_version = "2.1.1";
    expect_change(changed);

    constexpr char other_options[] =
        "{\"solver\":\"z3\",\"timeout_ms\":6000}";
    changed = base;
    changed.canonical_options = other_options;
    changed.canonical_options_size = sizeof(other_options) - 1u;
    expect_change(changed);
}

TEST(Evidence, CacheKeyHasStableBlake3Encoding) {
    constexpr char options[] = "{\"solver\":\"z3\",\"timeout_ms\":5000}";
    ql_cache_key_input_v1 identity =
        make_identity(options, sizeof(options) - 1u);
    ql_digest cache_key{};
    char hex[QL_DIGEST_HEX_SIZE]{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_key_compute(&identity, &cache_key, &error));
    ql_digest_hex(&cache_key, hex);
    EXPECT_STREQ(
        "0acfb91d9da9e68623e93dd4ef100fcbc21ba33b9b0e2b68cb2154860428fb9e",
        hex);
}

TEST(Evidence, CacheKeyRejectsMissingCanonicalOptionBytes) {
    ql_cache_key_input_v1 identity = make_identity(nullptr, 1u);
    ql_digest cache_key{};
    ql_error error{};

    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_cache_key_compute(&identity, &cache_key, &error));
    EXPECT_STREQ("canonical option size requires option bytes",
                 error.message);
}

TEST(Evidence, EnvelopeOwnsMethodIdentityAndEvidenceArtifact) {
    constexpr char options[] = "{}";
    char method_name[] = "smt.product-program";
    char method_version[] = "2.1.0";
    char proof_bytes[] = "proof payload";
    ql_cache_key_input_v1 identity =
        make_identity(options, sizeof(options) - 1u);
    ql_artifact *proof = nullptr;
    ql_evidence_envelope *envelope = nullptr;
    ql_evidence_envelope_view_v1 view{};
    ql_artifact_view proof_view{};
    ql_digest expected_cache_key{};
    ql_error error{};

    identity.method_name = method_name;
    identity.method_version = method_version;
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_key_compute(&identity, &expected_cache_key, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_PROOF, 1u,
                                 proof_bytes, sizeof(proof_bytes) - 1u,
                                 &proof, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_evidence_envelope_create(nullptr, &identity,
                                          QL_EVIDENCE_PROOF, proof,
                                          &envelope, &error));
    method_name[0] = 'x';
    method_version[0] = '9';
    proof_bytes[0] = 'x';
    ql_artifact_release(proof);

    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_evidence_envelope_get_view(envelope, &view, &error));
    EXPECT_EQ(QL_EVIDENCE_ENVELOPE_SCHEMA_VERSION, view.schema_version);
    EXPECT_EQ(QL_EVIDENCE_PROOF, view.evidence_class);
    EXPECT_STREQ("smt.product-program", view.method_name);
    EXPECT_STREQ("2.1.0", view.method_version);
    EXPECT_TRUE(ql_digest_equal(&identity.artifact_digest,
                                &view.artifact_digest));
    EXPECT_TRUE(ql_digest_equal(&identity.semantic_problem_digest,
                                &view.semantic_problem_digest));
    EXPECT_TRUE(ql_digest_equal(&expected_cache_key, &view.cache_key));
    ASSERT_NE(nullptr, view.evidence_artifact);
    proof_view.struct_size = sizeof(proof_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(view.evidence_artifact, &proof_view,
                                   &error));
    EXPECT_STREQ(QL_ARTIFACT_KIND_PROOF, proof_view.kind);
    EXPECT_EQ(0, std::memcmp("proof payload", proof_view.data,
                             sizeof(proof_bytes) - 1u));
    EXPECT_TRUE(ql_digest_equal(&proof_view.digest,
                                &view.evidence_digest));

    ql_evidence_envelope_release(envelope);
}

TEST(Evidence, ProofAndCounterexampleRequireMatchingArtifacts) {
    ql_cache_key_input_v1 identity = make_identity(nullptr, 0u);
    ql_evidence_envelope *envelope = nullptr;
    ql_artifact *outcome = nullptr;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_evidence_envelope_create(nullptr, &identity,
                                          QL_EVIDENCE_PROOF, nullptr,
                                          &envelope, &error));
    EXPECT_EQ(nullptr, envelope);

    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_OUTCOME, 1u,
                                 nullptr, 0u, &outcome, &error));
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_evidence_envelope_create(
                  nullptr, &identity, QL_EVIDENCE_COUNTEREXAMPLE, outcome,
                  &envelope, &error));
    EXPECT_EQ(nullptr, envelope);
    ql_artifact_release(outcome);
}

TEST(Evidence, CounterexampleAndBoundedClassesCanBeRepresented) {
    ql_cache_key_input_v1 identity = make_identity(nullptr, 0u);
    ql_artifact *counterexample = nullptr;
    ql_artifact *outcome = nullptr;
    ql_evidence_envelope *envelope = nullptr;
    ql_evidence_envelope_view_v1 view{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_COUNTEREXAMPLE,
                                 1u, "x=7", 3u, &counterexample, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_evidence_envelope_create(
                  nullptr, &identity, QL_EVIDENCE_COUNTEREXAMPLE,
                  counterexample, &envelope, &error));
    ql_artifact_release(counterexample);
    ASSERT_EQ(QL_STATUS_OK,
              ql_evidence_envelope_get_view(envelope, &view, &error));
    EXPECT_EQ(QL_EVIDENCE_COUNTEREXAMPLE, view.evidence_class);
    EXPECT_STREQ("COUNTEREXAMPLE",
                 ql_evidence_class_string(view.evidence_class));
    ql_evidence_envelope_release(envelope);
    envelope = nullptr;

    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_OUTCOME, 1u,
                                 "bound=64", 8u, &outcome, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_evidence_envelope_create(nullptr, &identity,
                                          QL_EVIDENCE_BOUNDED, outcome,
                                          &envelope, &error));
    ql_artifact_release(outcome);
    ASSERT_EQ(QL_STATUS_OK,
              ql_evidence_envelope_get_view(envelope, &view, &error));
    EXPECT_EQ(QL_EVIDENCE_BOUNDED, view.evidence_class);
    EXPECT_STREQ("BOUNDED",
                 ql_evidence_class_string(view.evidence_class));
    ql_evidence_envelope_release(envelope);
}

TEST(Evidence, UnknownEnvelopeCanOmitArtifact) {
    ql_cache_key_input_v1 identity = make_identity(nullptr, 0u);
    ql_evidence_envelope *envelope = nullptr;
    ql_evidence_envelope_view_v1 view{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_evidence_envelope_create(nullptr, &identity,
                                          QL_EVIDENCE_UNKNOWN, nullptr,
                                          &envelope, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_evidence_envelope_get_view(envelope, &view, &error));
    EXPECT_EQ(QL_EVIDENCE_UNKNOWN, view.evidence_class);
    EXPECT_EQ(nullptr, view.evidence_artifact);
    EXPECT_STREQ("UNKNOWN",
                 ql_evidence_class_string(view.evidence_class));
    ql_evidence_envelope_release(envelope);
}

}  // namespace
