#include "quodlibet/cache.h"
#include "quodlibet/problem.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

/* Cache key completeness.

   A cache key that omits an axis does not fail loudly. It hands back the
   answer to a different question, and every layer above it believes that
   answer. These tests walk each axis, change exactly one thing, and require
   that the key moves and that the store therefore keeps the two answers
   apart.

   The key is built from an artifact digest, a semantic-problem digest, the
   method name and version, and the method's canonical options. Everything a
   run depends on has to reach one of those five, so each test also says which
   one carries the axis it is varying. */

struct CacheDeleter {
    void operator()(ql_cache *cache) const { ql_cache_close(cache); }
};
struct ArtifactDeleter {
    void operator()(ql_artifact *artifact) const {
        ql_artifact_release(artifact);
    }
};

using CachePtr = std::unique_ptr<ql_cache, CacheDeleter>;
using ArtifactPtr = std::unique_ptr<ql_artifact, ArtifactDeleter>;

class ScopedRoot {
public:
    ScopedRoot() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("quodlibet-cache-key-test-" + std::to_string(++counter));
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    ~ScopedRoot() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    std::string string() const { return path_.string(); }

private:
    fs::path path_;
};

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

/* The semantic contract reaches the key through the problem artifact, whose
   digest binds every contract field along with both sources. */
ql_digest problem_digest(const ql_problem_definition_v1 &definition) {
    ql_artifact *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_problem_artifact_create(nullptr, &definition, &raw, &error))
        << error.message;
    const ArtifactPtr artifact(raw);
    ql_artifact_view view{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(artifact.get(), &view, &error));
    return view.digest;
}

struct Identity {
    ql_digest artifact_digest{};
    ql_digest semantic_problem_digest{};
    std::string method_name = "prove.smt-product";
    std::string method_version = "2.1.0";
    std::string options = "{\"backend\":\"bitwuzla 0.9.1\",\"timeout_ms\":5000}";

    ql_cache_key_input_v1 input() const {
        ql_cache_key_input_v1 value{};
        value.struct_size = sizeof(value);
        value.artifact_digest = artifact_digest;
        value.semantic_problem_digest = semantic_problem_digest;
        value.method_name = method_name.c_str();
        value.method_version = method_version.c_str();
        value.canonical_options = options.data();
        value.canonical_options_size = options.size();
        return value;
    }

    ql_digest key() const {
        const ql_cache_key_input_v1 value = input();
        ql_digest digest{};
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_cache_key_compute(&value, &digest, &error))
            << error.message;
        return digest;
    }
};

Identity base_identity() {
    Identity identity;
    identity.artifact_digest = problem_digest(make_definition());
    ql_digest_data("semantic problem v2", 19u,
                   &identity.semantic_problem_digest);
    return identity;
}

CachePtr open_cache(const ScopedRoot &root, std::string &keepalive) {
    ql_cache_config_v1 config{};
    ql_cache_config_init(&config);
    keepalive = root.string();
    config.root_path = keepalive.c_str();
    ql_cache *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK, ql_cache_open(nullptr, &config, &raw, &error))
        << error.message;
    return CachePtr(raw);
}

ArtifactPtr make_answer(const char *payload) {
    ql_artifact *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_OUTCOME, 1u,
                                 payload, std::strlen(payload), &raw,
                                 &error));
    return ArtifactPtr(raw);
}

std::string payload_of(const ql_artifact *artifact) {
    ql_artifact_view view{};
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(artifact, &view, &error));
    return std::string(static_cast<const char *>(view.data), view.size);
}

/* Stores one answer under each key and requires that each lookup returns its
   own. A key that dropped the axis would make the second store collide with
   the first, and the collision is what a caller would never see. */
void expect_keys_separate_the_answers(const ql_digest &first,
                                      const ql_digest &second,
                                      const char *axis) {
    ASSERT_FALSE(ql_digest_equal(&first, &second))
        << "changing " << axis
        << " did not change the cache key, so the store would answer one "
           "question with the other's result";

    const ScopedRoot root;
    std::string keepalive;
    const CachePtr cache = open_cache(root, keepalive);
    const ArtifactPtr answer_a = make_answer("verdict for configuration A");
    const ArtifactPtr answer_b = make_answer("verdict for configuration B");
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_store_artifact(cache.get(), &first, answer_a.get(),
                                      &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_store_artifact(cache.get(), &second, answer_b.get(),
                                      &error))
        << error.message;

    ql_artifact *loaded = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_load_artifact(cache.get(), &first, &loaded, &error));
    EXPECT_EQ("verdict for configuration A", payload_of(loaded));
    ql_artifact_release(loaded);
    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_load_artifact(cache.get(), &second, &loaded, &error));
    EXPECT_EQ("verdict for configuration B", payload_of(loaded));
    ql_artifact_release(loaded);
}

void expect_contract_axis_reaches_the_key(
    const char *axis, void (*mutate)(ql_problem_definition_v1 *)) {
    SCOPED_TRACE(axis);
    Identity first = base_identity();
    ql_problem_definition_v1 changed = make_definition();
    mutate(&changed);
    Identity second = base_identity();
    second.artifact_digest = problem_digest(changed);
    expect_keys_separate_the_answers(first.key(), second.key(), axis);
}

TEST(CacheKeyCompleteness, TheRelationReachesTheKey) {
    expect_contract_axis_reaches_the_key(
        "relation", [](ql_problem_definition_v1 *definition) {
            definition->contract.relation = QL_RELATION_LEFT_REFINES_RIGHT;
        });
}

TEST(CacheKeyCompleteness, TheUndefinedBehaviourPolicyReachesTheKey) {
    expect_contract_axis_reaches_the_key(
        "ub_policy", [](ql_problem_definition_v1 *definition) {
            definition->contract.ub_policy = QL_UB_LANGUAGE_REFINEMENT;
        });
}

TEST(CacheKeyCompleteness, TheObservationProjectionReachesTheKey) {
    expect_contract_axis_reaches_the_key(
        "observations", [](ql_problem_definition_v1 *definition) {
            /* Dropping memory from the projection asks a strictly weaker
               question, and its answer must not be reused for the stronger
               one. */
            definition->contract.observations &= ~(uint64_t)QL_OBSERVE_MEMORY;
            definition->contract.memory_observation = QL_MEMORY_IGNORE;
        });
}

TEST(CacheKeyCompleteness, TheMemoryObservationModeReachesTheKey) {
    expect_contract_axis_reaches_the_key(
        "memory_observation", [](ql_problem_definition_v1 *definition) {
            definition->contract.memory_observation =
                QL_MEMORY_ORDERED_WRITES;
        });
}

TEST(CacheKeyCompleteness, TheExternalCallObservationReachesTheKey) {
    expect_contract_axis_reaches_the_key(
        "external_call_observation",
        [](ql_problem_definition_v1 *definition) {
            definition->contract.observations &=
                ~(uint64_t)QL_OBSERVE_EXTERNAL_CALLS;
            definition->contract.external_call_observation =
                QL_EXTERNAL_CALLS_IGNORE;
        });
}

TEST(CacheKeyCompleteness, TheCompilerAndCodegenProfileReachesTheKey) {
    expect_contract_axis_reaches_the_key(
        "compiler_families", [](ql_problem_definition_v1 *definition) {
            definition->contract.compiler_families =
                QL_COMPILER_FAMILY_CLANG;
        });
    expect_contract_axis_reaches_the_key(
        "codegen_modes", [](ql_problem_definition_v1 *definition) {
            definition->contract.codegen_modes = QL_CODEGEN_NON_PIC;
        });
}

TEST(CacheKeyCompleteness, TheTargetFeatureProfileReachesTheKey) {
    expect_contract_axis_reaches_the_key(
        "target_features", [](ql_problem_definition_v1 *definition) {
            definition->contract.target_features |=
                QL_TARGET_FEATURE_DOMAIN_BASELINE |
                QL_TARGET_FEATURE_BITMANIP;
        });
}

TEST(CacheKeyCompleteness, ThePreconditionReachesTheKey) {
    static constexpr char precondition[] =
        "{ \"schema_version\" : 1, \"expression\" : true }";
    expect_contract_axis_reaches_the_key(
        "precondition_json", [](ql_problem_definition_v1 *definition) {
            definition->contract.precondition_json = precondition;
            definition->contract.precondition_json_size =
                sizeof(precondition) - 1u;
        });
}

TEST(CacheKeyCompleteness, EitherSideOfTheProgramReachesTheKey) {
    expect_contract_axis_reaches_the_key(
        "left_source", [](ql_problem_definition_v1 *definition) {
            static constexpr char left[] = "int left(int x){return x+2;}";
            definition->left_source = left;
            definition->left_source_size = sizeof(left) - 1u;
        });
    expect_contract_axis_reaches_the_key(
        "right_source", [](ql_problem_definition_v1 *definition) {
            static constexpr char right[] = "int right(int x){return 2+x;}";
            definition->right_source = right;
            definition->right_source_size = sizeof(right) - 1u;
        });
}

TEST(CacheKeyCompleteness, TheSemanticProblemDigestReachesTheKey) {
    const Identity first = base_identity();
    Identity second = base_identity();
    ql_digest_data("semantic problem v3", 19u,
                   &second.semantic_problem_digest);
    expect_keys_separate_the_answers(first.key(), second.key(),
                                     "semantic_problem_digest");
}

TEST(CacheKeyCompleteness, TheMethodIdentityReachesTheKey) {
    const Identity first = base_identity();
    Identity second = base_identity();
    second.method_name = "prove.aig-sat";
    expect_keys_separate_the_answers(first.key(), second.key(),
                                     "method_name");
}

TEST(CacheKeyCompleteness, TheMethodVersionReachesTheKey) {
    const Identity first = base_identity();
    Identity second = base_identity();
    /* A method that changed its encoding between versions answers a
       different question with the same name. */
    second.method_version = "2.1.1";
    expect_keys_separate_the_answers(first.key(), second.key(),
                                     "method_version");
}

TEST(CacheKeyCompleteness, TheMethodOptionsReachTheKey) {
    const Identity first = base_identity();
    Identity second = base_identity();
    second.options = "{\"backend\":\"bitwuzla 0.9.1\",\"timeout_ms\":9000}";
    expect_keys_separate_the_answers(first.key(), second.key(),
                                     "canonical_options");
}

/* The backend has no field of its own. It has to ride in the method version
   or the canonical options, and this test states that requirement by showing
   what happens on both sides of it. */
TEST(CacheKeyCompleteness, TheBackendVersionMustRideInVersionOrOptions) {
    const Identity carried = base_identity();
    Identity other_backend = base_identity();
    other_backend.options =
        "{\"backend\":\"bitwuzla 0.8.0\",\"timeout_ms\":5000}";
    expect_keys_separate_the_answers(carried.key(), other_backend.key(),
                                     "backend identity inside the options");

    /* And the failure mode it prevents: two runs on different solver builds
       whose options never named the solver produce one key, so the second run
       would be served the first one's answer. */
    Identity omitted_a = base_identity();
    Identity omitted_b = base_identity();
    omitted_a.options = "{\"timeout_ms\":5000}";
    omitted_b.options = "{\"timeout_ms\":5000}";
    const ql_digest first = omitted_a.key();
    const ql_digest second = omitted_b.key();
    EXPECT_TRUE(ql_digest_equal(&first, &second))
        << "the two runs differ only in a backend the key never saw, so a "
           "collision here is exactly the defect the requirement exists to "
           "prevent";
}

TEST(CacheKeyCompleteness, TheStoreRefusesTwoAnswersUnderOneKey) {
    /* The last line of defence when a key is incomplete after all: the store
       will not overwrite, so the disagreement surfaces instead of one answer
       quietly replacing the other. */
    const ScopedRoot root;
    std::string keepalive;
    const CachePtr cache = open_cache(root, keepalive);
    const ql_digest key = base_identity().key();
    const ArtifactPtr first = make_answer("proved equivalent");
    const ArtifactPtr second = make_answer("counterexample");
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_cache_store_artifact(cache.get(), &key, first.get(),
                                      &error));
    EXPECT_EQ(QL_STATUS_ALREADY_EXISTS,
              ql_cache_store_artifact(cache.get(), &key, second.get(),
                                      &error));
}

TEST(CacheKeyCompleteness, TheSameQuestionKeepsTheSameKey) {
    const Identity first = base_identity();
    const Identity second = base_identity();
    const ql_digest a = first.key();
    const ql_digest b = second.key();
    /* Completeness would be trivial if the key changed on nothing at all. */
    EXPECT_TRUE(ql_digest_equal(&a, &b));
}

}  // namespace
