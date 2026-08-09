#include "quodlibet/quodlibet.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct LegacyPluginV1Prefix {
    std::size_t struct_size;
    std::uint32_t abi_version;
    const char *name;
    const char *version;
    const ql_method_v1 *methods;
    std::size_t method_count;
    void (QL_CALL *shutdown)(void);
    void *reserved[8];
};

static_assert(sizeof(LegacyPluginV1Prefix) ==
              offsetof(ql_plugin_v1, proof_methods));

std::string test_plugin_path() {
    const std::vector<std::string> arguments =
        ::testing::internal::GetArgvs();
    for (std::size_t index = 1u; index < arguments.size(); ++index) {
        if (arguments[index].rfind("--", 0u) != 0u) {
            return arguments[index];
        }
    }
    return {};
}

ql_plugin_handle *load_test_plugin(ql_registry *registry, ql_error *error) {
    const std::string path = test_plugin_path();
    ql_plugin_handle *plugin = nullptr;

    if (path.empty()) {
        return nullptr;
    }
    if (ql_plugin_load(registry, path.c_str(), &plugin, error) !=
        QL_STATUS_OK) {
        return nullptr;
    }
    return plugin;
}

void initialize_proof_request(ql_semantic_contract_v1 *contract,
                              ql_proof_request_v1 *request) {
    ql_semantic_contract_init(contract);
    ql_proof_request_init(request, contract);
    request->requested_result_kinds = QL_PROOF_RESULT_PROOF;
    request->required_soundness = QL_PROOF_SOUNDNESS_PROOF;
}

TEST(ProofRegistryABI, ProofDescriptorsFollowTheLegacyPluginPrefix) {
    EXPECT_EQ(sizeof(LegacyPluginV1Prefix),
              offsetof(ql_plugin_v1, proof_methods));
    EXPECT_LT(offsetof(ql_plugin_v1, proof_methods),
              sizeof(ql_plugin_v1));
}

TEST(ProofRegistry, PluginDescriptorsAreDiscoveredAlongsideOrdinaryMethods) {
    ql_registry *registry = nullptr;
    ql_plugin_handle *plugin = nullptr;
    const ql_proof_method_v1 *proof_method = nullptr;
    ql_proof_method_capability_v1 capability{};
    ql_error error{};

    ASSERT_FALSE(test_plugin_path().empty());
    ASSERT_EQ(QL_STATUS_OK, ql_registry_create(nullptr, &registry, &error));
    plugin = load_test_plugin(registry, &error);
    ASSERT_NE(nullptr, plugin) << error.message;

    EXPECT_EQ(3u, ql_registry_count(registry));
    EXPECT_EQ(2u, ql_registry_proof_method_count(registry));
    ASSERT_NE(nullptr, ql_registry_find(registry, "test.echo"));
    EXPECT_EQ(nullptr,
              ql_registry_find_proof_method(registry, "test.echo"));
    proof_method = ql_registry_find_proof_method(registry, "test.prove");
    ASSERT_NE(nullptr, proof_method);
    EXPECT_STREQ("test.prove", proof_method->method->name);
    ASSERT_NE(nullptr, ql_registry_proof_method_at(registry, 0u));
    EXPECT_EQ(nullptr, ql_registry_proof_method_at(registry, 2u));

    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_registry_query_proof_capability(
                  registry, "test.echo", nullptr, &capability, &error));
    EXPECT_STREQ("method 'test.echo' has no proof descriptor", error.message);
    EXPECT_EQ(QL_STATUS_ALREADY_EXISTS,
              ql_registry_register_proof_method(registry, proof_method,
                                                &error));

    ql_plugin_unload(plugin);
    plugin = nullptr;
    EXPECT_EQ(0u, ql_registry_count(registry));
    EXPECT_EQ(0u, ql_registry_proof_method_count(registry));
    EXPECT_EQ(nullptr, ql_registry_find_proof_method(registry, "test.prove"));
    EXPECT_EQ(QL_STATUS_NOT_FOUND,
              ql_registry_query_proof_capability(
                  registry, "test.prove", nullptr, &capability, &error));
    ql_registry_destroy(registry);
}

TEST(ProofRegistry, OptionsSelectCapabilityBeforeContractValidation) {
    ql_registry *registry = nullptr;
    ql_plugin_handle *plugin = nullptr;
    ql_semantic_contract_v1 contract{};
    ql_proof_request_v1 request{};
    ql_proof_method_capability_v1 capability{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, ql_registry_create(nullptr, &registry, &error));
    plugin = load_test_plugin(registry, &error);
    ASSERT_NE(nullptr, plugin) << error.message;
    initialize_proof_request(&contract, &request);

    ASSERT_EQ(QL_STATUS_OK,
              ql_registry_query_proof_capability(
                  registry, "test.prove", nullptr, &capability, &error));
    EXPECT_EQ(QL_PROOF_METHOD_FAMILY_SMT, capability.family);
    EXPECT_EQ(QL_OBSERVE_ALL, capability.supported_observations);
    EXPECT_EQ(QL_STATUS_OK,
              ql_registry_validate_proof_request(
                  registry, "test.prove", nullptr, &request, &error));

    constexpr char return_only[] = "{\"profile\":\"return_only\"}";
    ASSERT_EQ(QL_STATUS_OK,
              ql_registry_query_proof_capability(
                  registry, "test.prove", return_only, &capability, &error));
    EXPECT_EQ(QL_OBSERVE_RETURN_VALUE,
              capability.supported_observations);
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_registry_validate_proof_request(
                  registry, "test.prove", return_only, &request, &error));

    contract.observations = QL_OBSERVE_RETURN_VALUE;
    contract.memory_observation = QL_MEMORY_IGNORE;
    contract.external_call_observation = QL_EXTERNAL_CALLS_IGNORE;
    EXPECT_EQ(QL_STATUS_OK,
              ql_registry_validate_proof_request(
                  registry, "test.prove", return_only, &request, &error));

    ql_plugin_unload(plugin);
    ql_registry_destroy(registry);
}

TEST(ProofRegistry, RejectsCapabilityFamilyMismatchAtQueryTime) {
    ql_registry *registry = nullptr;
    ql_plugin_handle *plugin = nullptr;
    ql_proof_method_capability_v1 capability{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, ql_registry_create(nullptr, &registry, &error));
    plugin = load_test_plugin(registry, &error);
    ASSERT_NE(nullptr, plugin) << error.message;

    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_registry_query_proof_capability(
                  registry, "test.bad-family", nullptr, &capability,
                  &error));
    EXPECT_STREQ(
        "capability family 1 does not match descriptor family 2",
        error.message);

    ql_plugin_unload(plugin);
    ql_registry_destroy(registry);
}

}  // namespace
