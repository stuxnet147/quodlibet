#include "quodlibet/plugin.h"

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
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL }
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
