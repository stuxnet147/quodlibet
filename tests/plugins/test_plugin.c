#include "quodlibet/plugin.h"

#include <string.h>

static ql_status QL_CALL echo_run(
    void *instance, const ql_run_context_v1 *context,
    ql_artifact *const *inputs, size_t input_count, ql_artifact **output,
    ql_error *error) {
    (void)instance;
    (void)error;
    if (context == NULL || context->host == NULL || inputs == NULL ||
        input_count != 1u || output == NULL) {
        return QL_STATUS_INVALID_ARGUMENT;
    }
    context->host->artifact_retain(inputs[0]);
    *output = inputs[0];
    return QL_STATUS_OK;
}

static const ql_method_v1 methods[] = {
    {
        sizeof(ql_method_v1),
        QL_ABI_VERSION,
        "test.echo",
        "Test plugin echo method",
        NULL,
        QL_METHOD_DETERMINISTIC,
        1u,
        1u,
        NULL,
        NULL,
        echo_run,
        NULL,
        { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
    },
    {
        sizeof(ql_method_v1),
        QL_ABI_VERSION,
        "test.prove",
        "Test plugin proof method",
        NULL,
        QL_METHOD_DETERMINISTIC | QL_METHOD_PROOF_PRODUCER |
            QL_METHOD_COUNTEREXAMPLE_PRODUCER,
        1u,
        1u,
        NULL,
        NULL,
        echo_run,
        NULL,
        { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
    },
    {
        sizeof(ql_method_v1),
        QL_ABI_VERSION,
        "test.bad-family",
        "Test plugin proof method with a mismatched capability family",
        NULL,
        QL_METHOD_DETERMINISTIC | QL_METHOD_PROOF_PRODUCER |
            QL_METHOD_COUNTEREXAMPLE_PRODUCER,
        1u,
        1u,
        NULL,
        NULL,
        echo_run,
        NULL,
        { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
    }
};

static void fill_capability(ql_proof_method_capability_v1 *capability,
                            ql_proof_method_family family) {
    capability->family = family;
    capability->soundness_classes =
        QL_PROOF_SOUNDNESS_PROOF | QL_PROOF_SOUNDNESS_COUNTEREXAMPLE;
    capability->supported_relations = QL_PROOF_RELATION_ALL;
    capability->supported_observations = QL_OBSERVE_ALL;
    capability->supported_ub_policies = QL_PROOF_UB_ALL;
    capability->supported_memory_observations =
        QL_PROOF_MEMORY_MODE(QL_MEMORY_IGNORE) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FINAL_REACHABLE_STATE) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_ORDERED_WRITES) |
        QL_PROOF_MEMORY_MODE(QL_MEMORY_FULL_TRACE);
    capability->supported_external_call_observations =
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_IGNORE) |
        QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_ORDERED_TRACE);
    capability->result_kinds = QL_PROOF_RESULT_PROOF |
                               QL_PROOF_RESULT_COUNTEREXAMPLE |
                               QL_PROOF_RESULT_UNKNOWN;
    capability->flags = QL_PROOF_CAPABILITY_PRECONDITIONS;
}

static ql_status QL_CALL query_capability(
    const char *options_json, ql_proof_method_capability_v1 *capability,
    ql_error *error) {
    (void)error;
    fill_capability(capability, QL_PROOF_METHOD_FAMILY_SMT);
    if (options_json != NULL &&
        strstr(options_json, "return_only") != NULL) {
        capability->supported_relations = QL_PROOF_RELATION_EQUIVALENCE;
        capability->supported_observations = QL_OBSERVE_RETURN_VALUE;
        capability->supported_memory_observations =
            QL_PROOF_MEMORY_MODE(QL_MEMORY_IGNORE);
        capability->supported_external_call_observations =
            QL_PROOF_EXTERNAL_CALL_MODE(QL_EXTERNAL_CALLS_IGNORE);
    }
    return QL_STATUS_OK;
}

static ql_status QL_CALL query_bad_family(
    const char *options_json, ql_proof_method_capability_v1 *capability,
    ql_error *error) {
    (void)options_json;
    (void)error;
    fill_capability(capability, QL_PROOF_METHOD_FAMILY_REWRITE_EGRAPH);
    return QL_STATUS_OK;
}

static const ql_proof_method_v1 proof_methods[] = {
    {
        sizeof(ql_proof_method_v1),
        QL_ABI_VERSION,
        QL_PROOF_METHOD_FAMILY_SMT,
        &methods[1],
        query_capability,
        { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
    },
    {
        sizeof(ql_proof_method_v1),
        QL_ABI_VERSION,
        QL_PROOF_METHOD_FAMILY_SMT,
        &methods[2],
        query_bad_family,
        { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
    }
};

static const ql_plugin_v1 plugin = {
    sizeof(ql_plugin_v1),
    QL_ABI_VERSION,
    "quodlibet-test-plugin",
    "1.0.0",
    methods,
    sizeof(methods) / sizeof(methods[0]),
    NULL,
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
    proof_methods,
    sizeof(proof_methods) / sizeof(proof_methods[0])
};

QL_PLUGIN_EXPORT ql_status QL_CALL quodlibet_plugin_init_v1(
    const ql_host_v1 *host, const ql_plugin_v1 **output, ql_error *error) {
    (void)error;
    if (host == NULL || host->abi_version != QL_ABI_VERSION || output == NULL) {
        return QL_STATUS_ABI_MISMATCH;
    }
    *output = &plugin;
    return QL_STATUS_OK;
}
