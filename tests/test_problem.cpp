#include "quodlibet/problem.h"

#include "w2_fixtures.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include <gtest/gtest.h>

namespace {

ql_problem_definition_v1 make_definition() {
    static constexpr char left[] = "int left(int x){return x+1;}";
    static constexpr char right[] = "int right(int x){return 1+x;}";
    ql_problem_definition_v1 definition{};
    ql_problem_definition_init(&definition);
    definition.left_source = left;
    definition.left_source_size = sizeof(left) - 1u;
    definition.left_function_name = "left";
    definition.left_function_name_size = 4u;
    definition.right_source = right;
    definition.right_source_size = sizeof(right) - 1u;
    definition.right_function_name = "right";
    definition.right_function_name_size = 5u;
    return definition;
}

TEST(Problem, CanonicalArtifactRoundTripsAsImmutableView) {
    constexpr char precondition[] =
        "{ \"schema_version\" : 1, \"expression\" : true }";
    ql_problem_definition_v1 definition = make_definition();
    ql_artifact *artifact = nullptr;
    ql_problem *problem = nullptr;
    ql_problem_view_v1 view{};
    ql_error error{};

    definition.contract.precondition_json = precondition;
    definition.contract.precondition_json_size = sizeof(precondition) - 1u;
    ASSERT_EQ(QL_STATUS_OK, ql_problem_artifact_create(
                                nullptr, &definition, &artifact, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_problem_open(nullptr, artifact, &problem, &error))
        << error.message;
    ql_artifact_release(artifact);
    artifact = nullptr;

    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_problem_get_view(problem, &view, &error));
    EXPECT_EQ(QL_PROBLEM_SCHEMA_VERSION, view.schema_version);
    EXPECT_EQ(0, std::memcmp("left", view.left_function_name,
                             view.left_function_name_size));
    EXPECT_EQ(0, std::memcmp("right", view.right_function_name,
                             view.right_function_name_size));
    EXPECT_EQ(0, std::memcmp(definition.left_source, view.left_source,
                             view.left_source_size));
    EXPECT_EQ(0, std::memcmp(definition.right_source, view.right_source,
                             view.right_source_size));
    EXPECT_STREQ("{\"schema_version\":1,\"expression\":true}",
                 view.contract.precondition_json);
    ql_problem_release(problem);
}

TEST(Problem, ViewCarriesTheArtifactCacheIdentity) {
    ql_problem_definition_v1 definition = make_definition();
    ql_artifact *artifact = nullptr;
    ql_problem *problem = nullptr;
    ql_problem_view_v1 problem_view{};
    ql_artifact_view artifact_view{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, ql_problem_artifact_create(
                                nullptr, &definition, &artifact, &error));
    artifact_view.struct_size = sizeof(artifact_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(artifact, &artifact_view, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_problem_open(nullptr, artifact, &problem, &error));
    problem_view.struct_size = sizeof(problem_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_problem_get_view(problem, &problem_view, &error));
    EXPECT_TRUE(ql_digest_equal(&artifact_view.digest,
                                &problem_view.artifact_digest));
    ql_problem_release(problem);
    ql_artifact_release(artifact);
}

TEST(Problem, ContractAndPreconditionChangeTheDigest) {
    constexpr char true_precondition[] =
        "{\"schema_version\":1,\"expression\":true}";
    constexpr char false_precondition[] =
        "{\"schema_version\":1,\"expression\":false}";
    ql_problem_definition_v1 definition = make_definition();
    ql_artifact *first = nullptr;
    ql_artifact *second = nullptr;
    ql_artifact *third = nullptr;
    ql_artifact_view first_view{};
    ql_artifact_view second_view{};
    ql_artifact_view third_view{};
    ql_error error{};

    definition.contract.precondition_json = true_precondition;
    definition.contract.precondition_json_size =
        sizeof(true_precondition) - 1u;
    ASSERT_EQ(QL_STATUS_OK, ql_problem_artifact_create(
                                nullptr, &definition, &first, &error));
    definition.contract.precondition_json = false_precondition;
    definition.contract.precondition_json_size =
        sizeof(false_precondition) - 1u;
    ASSERT_EQ(QL_STATUS_OK, ql_problem_artifact_create(
                                nullptr, &definition, &second, &error));
    definition.contract.relation = QL_RELATION_LEFT_REFINES_RIGHT;
    ASSERT_EQ(QL_STATUS_OK, ql_problem_artifact_create(
                                nullptr, &definition, &third, &error));
    first_view.struct_size = sizeof(first_view);
    second_view.struct_size = sizeof(second_view);
    third_view.struct_size = sizeof(third_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(first, &first_view, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(second, &second_view, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(third, &third_view, &error));
    EXPECT_FALSE(ql_digest_equal(&first_view.digest, &second_view.digest));
    EXPECT_FALSE(ql_digest_equal(&second_view.digest, &third_view.digest));
    ql_artifact_release(third);
    ql_artifact_release(second);
    ql_artifact_release(first);
}

TEST(Problem, EquivalentPreconditionWhitespaceHasOneIdentity) {
    constexpr char spaced[] =
        "{ \"schema_version\" : 1, \"expression\" : true }";
    constexpr char compact[] =
        "{\"schema_version\":1,\"expression\":true}";
    ql_problem_definition_v1 definition = make_definition();
    ql_artifact *first = nullptr;
    ql_artifact *second = nullptr;
    ql_artifact_view first_view{};
    ql_artifact_view second_view{};
    ql_error error{};

    definition.contract.precondition_json = spaced;
    definition.contract.precondition_json_size = sizeof(spaced) - 1u;
    ASSERT_EQ(QL_STATUS_OK, ql_problem_artifact_create(
                                nullptr, &definition, &first, &error));
    definition.contract.precondition_json = compact;
    definition.contract.precondition_json_size = sizeof(compact) - 1u;
    ASSERT_EQ(QL_STATUS_OK, ql_problem_artifact_create(
                                nullptr, &definition, &second, &error));
    first_view.struct_size = sizeof(first_view);
    second_view.struct_size = sizeof(second_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(first, &first_view, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(second, &second_view, &error));
    EXPECT_TRUE(ql_digest_equal(&first_view.digest, &second_view.digest));
    ql_artifact_release(second);
    ql_artifact_release(first);
}

TEST(Problem, RejectsMalformedArtifactJsonAndWrongKind) {
    ql_artifact *malformed = nullptr;
    ql_artifact *wrong_kind = nullptr;
    ql_problem *problem = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_PROBLEM,
                                 QL_PROBLEM_SCHEMA_VERSION, "{", 1u,
                                 &malformed, &error));
    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_problem_open(nullptr, malformed, &problem, &error));
    EXPECT_EQ(nullptr, problem);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_IR,
                                 QL_PROBLEM_SCHEMA_VERSION, "{}", 2u,
                                 &wrong_kind, &error));
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_problem_open(nullptr, wrong_kind, &problem, &error));
    EXPECT_EQ(nullptr, problem);
    ql_artifact_release(wrong_kind);
    ql_artifact_release(malformed);
}

TEST(Problem, RejectsSizeOverflowBeforeReadingInput) {
    ql_problem_definition_v1 definition = make_definition();
    ql_artifact *artifact = nullptr;
    ql_error error{};

    definition.left_source = "x";
    definition.left_source_size = std::numeric_limits<std::size_t>::max();
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_problem_artifact_create(nullptr, &definition, &artifact,
                                         &error));
    EXPECT_EQ(nullptr, artifact);
    EXPECT_STREQ("problem text sizes overflow size_t", error.message);
}

TEST(Problem, RejectsEmbeddedNulAndInvalidUtf8) {
    const char embedded_nul[] = {'i', 'n', 't', '\0', 'x'};
    const char invalid_utf8[] = {static_cast<char>(0xff)};
    ql_problem_definition_v1 definition = make_definition();
    ql_artifact *artifact = nullptr;
    ql_error error{};

    definition.left_source = embedded_nul;
    definition.left_source_size = sizeof(embedded_nul);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_problem_artifact_create(nullptr, &definition, &artifact,
                                         &error));
    definition = make_definition();
    definition.left_source = invalid_utf8;
    definition.left_source_size = sizeof(invalid_utf8);
    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_problem_artifact_create(nullptr, &definition, &artifact,
                                         &error));
    EXPECT_EQ(nullptr, artifact);
}

/* --- Schema v2: binding what is being proved ------------------------------ */

constexpr char kLeftSource[] = "int add(int x, int y){ return x + y; }";
constexpr char kRightSource[] = "int sum(int a, int b){ return b + a; }";

class ArtifactRef {
public:
    ArtifactRef() = default;
    ArtifactRef(const ArtifactRef &) = delete;
    ArtifactRef &operator=(const ArtifactRef &) = delete;
    ~ArtifactRef() { ql_artifact_release(artifact_); }

    ql_artifact **output() { return &artifact_; }
    ql_artifact *get() const { return artifact_; }

private:
    ql_artifact *artifact_ = nullptr;
};

class ProblemRef {
public:
    ProblemRef() = default;
    ProblemRef(const ProblemRef &) = delete;
    ProblemRef &operator=(const ProblemRef &) = delete;
    ~ProblemRef() { ql_problem_release(problem_); }

    ql_problem **output() { return &problem_; }
    ql_problem *get() const { return problem_; }

private:
    ql_problem *problem_ = nullptr;
};

ql_problem_argument_binding_v1 MakeBinding(std::uint32_t left,
                                           std::uint32_t right) {
    ql_problem_argument_binding_v1 binding{};
    binding.struct_size = sizeof(binding);
    binding.left_index = left;
    binding.right_index = right;
    return binding;
}

ql_problem_definition_v2 MakeDefinitionV2(const w2::CFunction &left,
                                          const w2::CFunction &right) {
    ql_problem_definition_v2 definition{};
    ql_problem_definition_v2_init(&definition);
    definition.left_source = left.source().c_str();
    definition.left_source_size = left.source().size();
    definition.left_function_name = "add";
    definition.left_function_name_size = 3u;
    definition.right_source = right.source().c_str();
    definition.right_source_size = right.source().size();
    definition.right_function_name = "sum";
    definition.right_function_name_size = 3u;
    definition.left_signature = left.signature_artifact();
    definition.right_signature = right.signature_artifact();
    return definition;
}

TEST(ProblemV2, BindsBothSignatureDigestsTheCorrespondenceAndThePrecondition) {
    constexpr char precondition[] =
        "{\"schema_version\":1,\"expression\":{\"op\":\"slt\","
        "\"left\":{\"op\":\"arg\",\"index\":0},"
        "\"right\":{\"op\":\"int\",\"signed\":true,\"width\":32,"
        "\"value\":\"100\"}}}";
    w2::CFunction left;
    w2::CFunction right;
    ArtifactRef artifact;
    ProblemRef problem;
    ql_problem_view_v2 view{};
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&left, kLeftSource, "add"));
    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&right, kRightSource, "sum"));

    ql_problem_definition_v2 definition = MakeDefinitionV2(left, right);
    const ql_problem_argument_binding_v1 bindings[] = {MakeBinding(0u, 1u),
                                                       MakeBinding(1u, 0u)};
    definition.argument_bindings = bindings;
    definition.argument_binding_count = 2u;
    definition.contract.precondition_json = precondition;
    definition.contract.precondition_json_size = sizeof(precondition) - 1u;

    ASSERT_EQ(QL_STATUS_OK,
              ql_problem_artifact_create_v2(nullptr, &definition,
                                            artifact.output(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, ql_problem_open(nullptr, artifact.get(),
                                            problem.output(), &error))
        << error.message;

    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_problem_get_view_v2(problem.get(), &view, &error))
        << error.message;
    EXPECT_EQ(QL_PROBLEM_SCHEMA_VERSION_2, view.schema_version);
    EXPECT_EQ(2u, view.argument_binding_count);

    ql_digest zero{};
    EXPECT_EQ(0u, ql_digest_equal(&view.left_signature_digest, &zero));
    EXPECT_EQ(0u, ql_digest_equal(&view.right_signature_digest, &zero));
    EXPECT_EQ(0u, ql_digest_equal(&view.precondition_digest, &zero));
    EXPECT_EQ(0u, ql_digest_equal(&view.left_signature_digest,
                                  &view.right_signature_digest));

    ql_problem_argument_binding_v1 read{};
    ASSERT_EQ(QL_STATUS_OK, ql_problem_argument_binding_at(problem.get(), 0u,
                                                           &read, &error));
    EXPECT_EQ(0u, read.left_index);
    EXPECT_EQ(1u, read.right_index);
    EXPECT_EQ(QL_STATUS_NOT_FOUND, ql_problem_argument_binding_at(
                                       problem.get(), 2u, &read, &error));
    EXPECT_NE(nullptr, ql_problem_left_signature_artifact(problem.get()));
    EXPECT_NE(nullptr, ql_problem_right_signature_artifact(problem.get()));
    EXPECT_EQ(QL_STATUS_OK,
              ql_problem_require_proof_binding(problem.get(), &error));
}

TEST(ProblemV2, NullPreconditionStillGetsASignatureBoundDigest) {
    w2::CFunction left;
    w2::CFunction right;
    ArtifactRef artifact;
    ProblemRef problem;
    ql_problem_view_v2 view{};
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&left, kLeftSource, "add"));
    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&right, kRightSource, "sum"));

    ql_problem_definition_v2 definition = MakeDefinitionV2(left, right);
    const ql_problem_argument_binding_v1 bindings[] = {MakeBinding(0u, 0u),
                                                       MakeBinding(1u, 1u)};
    definition.argument_bindings = bindings;
    definition.argument_binding_count = 2u;

    ASSERT_EQ(QL_STATUS_OK,
              ql_problem_artifact_create_v2(nullptr, &definition,
                                            artifact.output(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, ql_problem_open(nullptr, artifact.get(),
                                            problem.output(), &error))
        << error.message;
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_problem_get_view_v2(problem.get(), &view, &error));
    ql_digest zero{};
    EXPECT_EQ(0u, ql_digest_equal(&view.precondition_digest, &zero));
    EXPECT_EQ(nullptr, view.contract.precondition_json);
}

TEST(ProblemV2, RejectsAPartialOrRepeatedArgumentCorrespondence) {
    w2::CFunction left;
    w2::CFunction right;
    ql_artifact *artifact = nullptr;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&left, kLeftSource, "add"));
    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&right, kRightSource, "sum"));

    ql_problem_definition_v2 definition = MakeDefinitionV2(left, right);
    const ql_problem_argument_binding_v1 partial[] = {MakeBinding(0u, 0u)};
    definition.argument_bindings = partial;
    definition.argument_binding_count = 1u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_problem_artifact_create_v2(nullptr, &definition, &artifact,
                                            &error));
    EXPECT_EQ(nullptr, artifact);

    const ql_problem_argument_binding_v1 repeated[] = {MakeBinding(0u, 0u),
                                                       MakeBinding(1u, 0u)};
    definition.argument_bindings = repeated;
    definition.argument_binding_count = 2u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_problem_artifact_create_v2(nullptr, &definition, &artifact,
                                            &error));
    EXPECT_EQ(nullptr, artifact);
}

TEST(ProblemV2, RejectsCorrespondingArgumentsWithDifferentSourceTypes) {
    w2::CFunction left;
    w2::CFunction right;
    ql_artifact *artifact = nullptr;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&left, kLeftSource, "add"));
    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(
        &right, "int sum(unsigned a, int b){ return b + a; }", "sum"));

    ql_problem_definition_v2 definition = MakeDefinitionV2(left, right);
    const ql_problem_argument_binding_v1 bindings[] = {MakeBinding(0u, 0u),
                                                       MakeBinding(1u, 1u)};
    definition.argument_bindings = bindings;
    definition.argument_binding_count = 2u;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_problem_artifact_create_v2(nullptr, &definition, &artifact,
                                            &error));
    EXPECT_EQ(nullptr, artifact);
}

TEST(ProblemV2, RejectsDifferentReturnTypes) {
    w2::CFunction left;
    w2::CFunction right;
    ql_artifact *artifact = nullptr;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&left, kLeftSource, "add"));
    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(
        &right, "long sum(int a, int b){ return b + a; }", "sum"));

    ql_problem_definition_v2 definition = MakeDefinitionV2(left, right);
    const ql_problem_argument_binding_v1 bindings[] = {MakeBinding(0u, 0u),
                                                       MakeBinding(1u, 1u)};
    definition.argument_bindings = bindings;
    definition.argument_binding_count = 2u;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_problem_artifact_create_v2(nullptr, &definition, &artifact,
                                            &error));
    EXPECT_EQ(nullptr, artifact);
}

TEST(ProblemV2, RejectsASignatureThatNamesAnotherFunction) {
    w2::CFunction left;
    w2::CFunction right;
    ql_artifact *artifact = nullptr;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&left, kLeftSource, "add"));
    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&right, kRightSource, "sum"));

    ql_problem_definition_v2 definition = MakeDefinitionV2(left, right);
    const ql_problem_argument_binding_v1 bindings[] = {MakeBinding(0u, 0u),
                                                       MakeBinding(1u, 1u)};
    definition.argument_bindings = bindings;
    definition.argument_binding_count = 2u;
    definition.left_signature = right.signature_artifact();
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_problem_artifact_create_v2(nullptr, &definition, &artifact,
                                            &error));
    EXPECT_EQ(nullptr, artifact);
}

TEST(ProblemV2, RecomputesTheRecordedDigestsWhenOpeningAnEditedArtifact) {
    w2::CFunction left;
    w2::CFunction right;
    ArtifactRef original;
    ArtifactRef edited;
    ql_problem *problem = nullptr;
    ql_artifact_view view{};
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&left, kLeftSource, "add"));
    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&right, kRightSource, "sum"));

    ql_problem_definition_v2 definition = MakeDefinitionV2(left, right);
    const ql_problem_argument_binding_v1 bindings[] = {MakeBinding(0u, 0u),
                                                       MakeBinding(1u, 1u)};
    definition.argument_bindings = bindings;
    definition.argument_binding_count = 2u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_problem_artifact_create_v2(nullptr, &definition,
                                            original.output(), &error))
        << error.message;

    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(original.get(), &view, &error));
    std::string json(static_cast<const char *>(view.data), view.size);
    const std::string key = "\"precondition_digest\":\"";
    const std::size_t position = json.find(key);
    ASSERT_NE(std::string::npos, position);
    json.replace(position + key.size(), QL_DIGEST_HEX_SIZE - 1u,
                 std::string(QL_DIGEST_HEX_SIZE - 1u, '0'));

    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_PROBLEM,
                                 QL_PROBLEM_SCHEMA_VERSION_2, json.data(),
                                 json.size(), edited.output(), &error));
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_problem_open(nullptr, edited.get(), &problem, &error));
    EXPECT_EQ(nullptr, problem);
}

TEST(ProblemV2, SchemaV1NeverJustifiesAProvedVerdict) {
    constexpr char precondition[] =
        "{\"schema_version\":1,\"expression\":true}";
    ql_problem_definition_v1 with_precondition = make_definition();
    ql_problem_definition_v1 without = make_definition();
    ArtifactRef bound;
    ArtifactRef unbound;
    ProblemRef bound_problem;
    ProblemRef unbound_problem;
    ql_problem_view_v2 view{};
    ql_error error{};

    with_precondition.contract.precondition_json = precondition;
    with_precondition.contract.precondition_json_size =
        sizeof(precondition) - 1u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_problem_artifact_create(nullptr, &with_precondition,
                                         bound.output(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, ql_problem_open(nullptr, bound.get(),
                                            bound_problem.output(), &error));
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_problem_require_proof_binding(bound_problem.get(), &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "configuration"));
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_problem_get_view_v2(bound_problem.get(), &view, &error));

    ASSERT_EQ(QL_STATUS_OK, ql_problem_artifact_create(
                                nullptr, &without, unbound.output(), &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_problem_open(nullptr, unbound.get(),
                              unbound_problem.output(), &error));
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_problem_require_proof_binding(unbound_problem.get(), &error));
    EXPECT_EQ(nullptr,
              ql_problem_left_signature_artifact(unbound_problem.get()));
}

}  // namespace
