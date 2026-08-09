#include "quodlibet/problem.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

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

}  // namespace
