#include "quodlibet/quodlibet.h"

#include <atomic>
#include <cstddef>
#include <cstring>

#include <gtest/gtest.h>

namespace {

const char *plugin_path = nullptr;

TEST(Core, Blake3Digest) {
    constexpr char expected[] =
        "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85";
    ql_digest digest{};
    char hex[QL_DIGEST_HEX_SIZE]{};

    ql_digest_data("abc", 3u, &digest);
    ql_digest_hex(&digest, hex);
    EXPECT_STREQ(expected, hex);
}

TEST(Core, NullHashInputIsEmptyInput) {
    ql_digest null_digest{};
    ql_digest empty_digest{};

    ql_digest_data(nullptr, 123u, &null_digest);
    ql_digest_data("", 0u, &empty_digest);
    EXPECT_TRUE(ql_digest_equal(&null_digest, &empty_digest));
    EXPECT_EQ(ql_fast_hash(nullptr, 123u, 7u),
              ql_fast_hash("", 0u, 7u));
}

TEST(Core, ArtifactIsImmutableCopy) {
    char bytes[] = "left";
    ql_artifact *artifact = nullptr;
    ql_artifact_view view{};
    ql_error error{};

    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_PROBLEM, 1u,
                                 bytes, 4u, &artifact, &error));
    bytes[0] = 'x';
    ASSERT_EQ(QL_STATUS_OK, ql_artifact_get_view(artifact, &view, &error));
    EXPECT_STREQ(QL_ARTIFACT_KIND_PROBLEM, view.kind);
    EXPECT_EQ(0, std::memcmp("left", view.data, 4u));
    ql_artifact_release(artifact);
}

TEST(Semantics, ConservativeAsm2cDefaultsAreValid) {
    ql_semantic_contract_v1 contract{};
    ql_error error{};

    ql_semantic_contract_init(&contract);
    EXPECT_EQ(QL_STATUS_OK, ql_semantic_contract_validate(&contract, &error));
    EXPECT_EQ(QL_RELATION_EQUIVALENCE, contract.relation);
    EXPECT_EQ(QL_UB_MUST_MATCH, contract.ub_policy);
    EXPECT_EQ(QL_OBSERVE_ALL, contract.observations);
    EXPECT_EQ(QL_MEMORY_FINAL_REACHABLE_STATE,
              contract.memory_observation);
    EXPECT_EQ(QL_EXTERNAL_CALLS_ORDERED_TRACE,
              contract.external_call_observation);
    EXPECT_EQ(QL_C_DIALECT_ASM2C_GNU_V1, contract.c_dialect);
    EXPECT_EQ(QL_TARGET_ABI_X86_64_LINUX_SYSV_LP64,
              contract.target_abi);
}

TEST(Semantics, AcceptsVersionedInputPrecondition) {
    constexpr char precondition[] =
        "{\"schema_version\":1,\"expression\":{"
        "\"op\":\"valid_range\",\"pointer\":{\"arg\":0},"
        "\"offset\":0,\"bytes\":{\"arg\":1},"
        "\"access\":\"read_write\",\"alignment\":4}}";
    ql_semantic_contract_v1 contract{};
    ql_error error{};

    ql_semantic_contract_init(&contract);
    contract.precondition_json = precondition;
    contract.precondition_json_size = sizeof(precondition) - 1u;
    EXPECT_EQ(QL_STATUS_OK, ql_semantic_contract_validate(&contract, &error));
}

TEST(Semantics, RejectsInconsistentObservationMode) {
    ql_semantic_contract_v1 contract{};
    ql_error error{};

    ql_semantic_contract_init(&contract);
    contract.observations &= ~QL_OBSERVE_MEMORY;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_semantic_contract_validate(&contract, &error));
    EXPECT_STREQ("memory observation bit and mode disagree", error.message);
}

TEST(Semantics, CallerCanNarrowEveryContractAxis) {
    ql_semantic_contract_v1 contract{};
    ql_error error{};

    ql_semantic_contract_init(&contract);
    contract.relation = QL_RELATION_RIGHT_REFINES_LEFT;
    contract.ub_policy = QL_UB_COMPARE_WHERE_BOTH_DEFINED;
    contract.observations = QL_OBSERVE_RETURN_VALUE | QL_OBSERVE_TERMINATION |
                            QL_OBSERVE_TRAPS;
    contract.memory_observation = QL_MEMORY_IGNORE;
    contract.external_call_observation = QL_EXTERNAL_CALLS_IGNORE;
    contract.compiler_families = QL_COMPILER_FAMILY_CLANG;
    contract.codegen_modes = QL_CODEGEN_NON_PIC;
    contract.target_features = QL_TARGET_FEATURE_X86_64_BASE |
                               QL_TARGET_FEATURE_DOMAIN_BASELINE |
                               QL_TARGET_FEATURE_CRYPTO;
    EXPECT_EQ(QL_STATUS_OK, ql_semantic_contract_validate(&contract, &error));
}

struct CounterTask {
    std::atomic<unsigned> *counter;
};

ql_status QL_CALL increment_counter(void *user_data, ql_error *error) {
    auto *task = static_cast<CounterTask *>(user_data);
    (void)error;
    task->counter->fetch_add(1u, std::memory_order_relaxed);
    return QL_STATUS_OK;
}

ql_status QL_CALL fail_without_diagnostic(void *user_data, ql_error *error) {
    (void)user_data;
    (void)error;
    return QL_STATUS_METHOD_ERROR;
}

TEST(Scheduler, ExecutesTaskGroup) {
    ql_scheduler *scheduler = nullptr;
    ql_task_group *group = nullptr;
    ql_error error{};
    std::atomic<unsigned> counter{0u};
    CounterTask task{&counter};

    ASSERT_EQ(QL_STATUS_OK,
              ql_scheduler_create(nullptr, 4u, &scheduler, &error));
    EXPECT_EQ(4u, ql_scheduler_worker_count(scheduler));
    ASSERT_EQ(QL_STATUS_OK,
              ql_task_group_create(nullptr, &group, &error));
    for (std::size_t index = 0u; index < 100u; ++index) {
        ASSERT_EQ(QL_STATUS_OK,
                  ql_scheduler_submit(scheduler, group, increment_counter,
                                      &task, &error));
    }
    EXPECT_EQ(QL_STATUS_OK, ql_task_group_wait(group, &error));
    EXPECT_EQ(100u, counter.load(std::memory_order_relaxed));
    ql_task_group_destroy(group);
    ql_scheduler_destroy(scheduler);
}

TEST(Scheduler, SuppliesFallbackDiagnostic) {
    ql_scheduler *scheduler = nullptr;
    ql_task_group *group = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_scheduler_create(nullptr, 1u, &scheduler, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_task_group_create(nullptr, &group, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_scheduler_submit(scheduler, group, fail_without_diagnostic,
                                  nullptr, &error));
    EXPECT_EQ(QL_STATUS_METHOD_ERROR, ql_task_group_wait(group, &error));
    EXPECT_EQ(QL_STATUS_METHOD_ERROR, error.code);
    EXPECT_STREQ("method error", error.message);
    ql_task_group_destroy(group);
    ql_scheduler_destroy(scheduler);
}

TEST(Pipeline, RunsParallelBranches) {
    constexpr char json[] =
        "{\"schema_version\":1,\"nodes\":["
        "{\"name\":\"normalize\",\"method\":\"builtin.identity\","
        " \"options\":{}},"
        "{\"name\":\"prove\",\"method\":\"builtin.identity\","
        " \"depends_on\":[\"normalize\"]},"
        "{\"name\":\"refute\",\"method\":\"builtin.identity\","
        " \"depends_on\":[\"normalize\"]}]}";
    ql_registry *registry = nullptr;
    ql_pipeline *pipeline = nullptr;
    ql_scheduler *scheduler = nullptr;
    ql_pipeline_result *result = nullptr;
    ql_artifact *input = nullptr;
    ql_artifact_view view{};
    ql_error error{};

    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_registry_create(nullptr, &registry, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_register_builtin_methods(registry, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_from_json(registry, nullptr, json, sizeof(json) - 1u,
                                    &pipeline, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_scheduler_create(nullptr, 2u, &scheduler, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_PROBLEM, 1u,
                                 "pair", 4u, &input, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_run(pipeline, scheduler, input, nullptr, &result,
                              &error));
    ASSERT_EQ(2u, ql_pipeline_result_count(result));
    for (std::size_t index = 0u;
         index < ql_pipeline_result_count(result); ++index) {
        ASSERT_EQ(QL_STATUS_OK,
                  ql_artifact_get_view(
                      ql_pipeline_result_artifact(result, index), &view,
                      &error));
        EXPECT_EQ(0, std::memcmp("pair", view.data, 4u));
    }
    ql_pipeline_result_destroy(result);
    ql_artifact_release(input);
    ql_scheduler_destroy(scheduler);
    ql_pipeline_destroy(pipeline);
    ql_registry_destroy(registry);
}

TEST(Pipeline, RejectsCycle) {
    constexpr char json[] =
        "{\"schema_version\":1,\"nodes\":["
        "{\"name\":\"a\",\"method\":\"builtin.identity\","
        " \"depends_on\":[\"b\"]},"
        "{\"name\":\"b\",\"method\":\"builtin.identity\","
        " \"depends_on\":[\"a\"]}]}";
    ql_registry *registry = nullptr;
    ql_pipeline *pipeline = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, ql_registry_create(nullptr, &registry, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_register_builtin_methods(registry, &error));
    EXPECT_EQ(QL_STATUS_CYCLE,
              ql_pipeline_from_json(registry, nullptr, json, sizeof(json) - 1u,
                                    &pipeline, &error));
    EXPECT_EQ(nullptr, pipeline);
    ql_registry_destroy(registry);
}

TEST(Pipeline, CancelledRunDoesNotStart) {
    constexpr char json[] =
        "{\"schema_version\":1,\"nodes\":["
        "{\"name\":\"only\",\"method\":\"builtin.identity\"}]}";
    ql_registry *registry = nullptr;
    ql_pipeline *pipeline = nullptr;
    ql_scheduler *scheduler = nullptr;
    ql_cancel_token *cancel = nullptr;
    ql_pipeline_result *result = nullptr;
    ql_artifact *input = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, ql_registry_create(nullptr, &registry, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_register_builtin_methods(registry, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_from_json(registry, nullptr, json, sizeof(json) - 1u,
                                    &pipeline, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_scheduler_create(nullptr, 1u, &scheduler, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_cancel_token_create(nullptr, &cancel, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_PROBLEM, 1u,
                                 "x", 1u, &input, &error));
    ql_cancel_token_request(cancel);
    EXPECT_EQ(QL_STATUS_CANCELLED,
              ql_pipeline_run(pipeline, scheduler, input, cancel, &result,
                              &error));
    EXPECT_EQ(nullptr, result);
    ql_artifact_release(input);
    ql_cancel_token_destroy(cancel);
    ql_scheduler_destroy(scheduler);
    ql_pipeline_destroy(pipeline);
    ql_registry_destroy(registry);
}

TEST(Plugin, RunsAndUnloadInvalidatesBinding) {
    constexpr char json[] =
        "{\"schema_version\":1,\"nodes\":["
        "{\"name\":\"echo\",\"method\":\"test.echo\"}]}";
    ql_registry *registry = nullptr;
    ql_plugin_handle *plugin = nullptr;
    ql_pipeline *pipeline = nullptr;
    ql_scheduler *scheduler = nullptr;
    ql_pipeline_result *result = nullptr;
    ql_artifact *input = nullptr;
    ql_error error{};

    ASSERT_NE(nullptr, plugin_path);
    ASSERT_EQ(QL_STATUS_OK, ql_registry_create(nullptr, &registry, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_plugin_load(registry, plugin_path, &plugin, &error))
        << error.message;
    EXPECT_STREQ("quodlibet-test-plugin", ql_plugin_name(plugin));
    ASSERT_NE(nullptr, ql_registry_find(registry, "test.echo"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_from_json(registry, nullptr, json, sizeof(json) - 1u,
                                    &pipeline, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_scheduler_create(nullptr, 1u, &scheduler, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_PROBLEM, 1u,
                                 "plugin", 6u, &input, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_run(pipeline, scheduler, input, nullptr, &result,
                              &error));
    EXPECT_EQ(1u, ql_pipeline_result_count(result));
    ql_pipeline_result_destroy(result);
    result = nullptr;
    ql_plugin_unload(plugin);
    EXPECT_EQ(nullptr, ql_registry_find(registry, "test.echo"));
    EXPECT_EQ(QL_STATUS_NOT_FOUND,
              ql_pipeline_run(pipeline, scheduler, input, nullptr, &result,
                              &error));
    EXPECT_EQ(nullptr, result);
    ql_artifact_release(input);
    ql_scheduler_destroy(scheduler);
    ql_pipeline_destroy(pipeline);
    ql_registry_destroy(registry);
}

}  // namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    plugin_path = argc > 1 ? argv[1] : nullptr;
    return RUN_ALL_TESTS();
}
