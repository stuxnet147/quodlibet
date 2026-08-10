/* The smallest Quodlibet plugin that is still a real one.
 *
 * It registers a single method, `example.normalize`, which rewrites an
 * artifact's bytes by collapsing runs of ASCII whitespace to one space and
 * trimming the ends. The transformation itself is deliberately dull. What the
 * example is for is the four things a plugin has to get right, and each is
 * marked below:
 *
 *   1. the ABI handshake in quodlibet_plugin_init_v1;
 *   2. struct_size and abi_version on every descriptor;
 *   3. allocation and ownership across the host boundary;
 *   4. declaring only capabilities the method actually has.
 *
 * examples/plugin/README.md explains each in prose. tests/test_example_plugin.cpp
 * loads this module and checks it still does what the README says, so the
 * example cannot quietly stop working.
 */

#include "quodlibet/plugin.h"

#include <stddef.h>
#include <string.h>

#define QL_EXAMPLE_OUTPUT_KIND "example.normalized"

/* The plugin's own bookkeeping. A method with no configuration would not need
 * an instance at all; this one keeps the host so that `run` allocates through
 * the same allocator the host handed it, which is point 3. */
typedef struct example_instance {
    const ql_host_v1 *host;
} example_instance;

static int is_ascii_space(unsigned char byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' ||
           byte == '\v' || byte == '\f';
}

static ql_status QL_CALL example_create(const ql_host_v1 *host,
                                        const char *options_json,
                                        void **instance, ql_error *error) {
    example_instance *state;

    (void)options_json;
    if (host == NULL || instance == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the host and an instance output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    /* Point 3. Every allocation the plugin makes comes from the host's
     * allocator. The host and the plugin may be linked against different C
     * runtimes, and a block one runtime allocated cannot be freed by the
     * other. */
    state = host->allocator.allocate(host->allocator.user_data,
                                     sizeof(*state));
    if (state == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate the example instance");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    state->host = host;
    *instance = state;
    return QL_STATUS_OK;
}

static void QL_CALL example_destroy(void *instance) {
    example_instance *state = (example_instance *)instance;

    if (state != NULL) {
        const ql_host_v1 *host = state->host;
        host->allocator.deallocate(host->allocator.user_data, state);
    }
}

/* Optional. The host already enforces the arity declared below, so this only
 * checks what the arity cannot express. Rejecting here rather than inside
 * `run` keeps a bad input from being half-processed. */
static ql_status QL_CALL example_validate(void *instance,
                                          ql_artifact *const *inputs,
                                          size_t input_count,
                                          ql_error *error) {
    ql_artifact_view view;

    (void)instance;
    if (inputs == NULL || input_count != 1u || inputs[0] == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "example.normalize takes exactly one artifact");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    return ql_artifact_get_view(inputs[0], &view, error);
}

static ql_status QL_CALL example_run(void *instance,
                                     const ql_run_context_v1 *context,
                                     ql_artifact *const *inputs,
                                     size_t input_count,
                                     ql_artifact **output, ql_error *error) {
    example_instance *state = (example_instance *)instance;
    ql_artifact_view view;
    const unsigned char *source;
    unsigned char *buffer;
    size_t read_index = 0u;
    size_t written = 0u;
    ql_status status;

    if (state == NULL || context == NULL || context->host == NULL ||
        inputs == NULL || input_count != 1u || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "example.normalize was called with an invalid context");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    /* A long-running method polls this so a cancelled pipeline stops promptly.
     * This one is not long running, so checking once on entry is honest. */
    if (context->is_cancelled != NULL &&
        context->is_cancelled(context->cancel_state) != 0u) {
        ql_error_set(error, QL_STATUS_CANCELLED, "the run was cancelled");
        return QL_STATUS_CANCELLED;
    }

    memset(&view, 0, sizeof(view));
    view.struct_size = sizeof(view);
    status = ql_artifact_get_view(inputs[0], &view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    /* The result is never longer than the input, so one allocation of the
     * input's size is enough and there is no growth loop to get wrong. */
    buffer = view.size == 0u
                 ? NULL
                 : (unsigned char *)state->host->allocator.allocate(
                       state->host->allocator.user_data, view.size);
    if (view.size != 0u && buffer == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                     "could not allocate the normalized output");
        return QL_STATUS_OUT_OF_MEMORY;
    }

    source = (const unsigned char *)view.data;
    while (read_index < view.size && is_ascii_space(source[read_index])) {
        ++read_index;
    }
    for (; read_index < view.size; ++read_index) {
        if (!is_ascii_space(source[read_index])) {
            buffer[written++] = source[read_index];
            continue;
        }
        /* One space for a run of any length, and none at all if the run turns
         * out to be trailing. */
        while (read_index + 1u < view.size &&
               is_ascii_space(source[read_index + 1u])) {
            ++read_index;
        }
        if (read_index + 1u < view.size) {
            buffer[written++] = ' ';
        }
    }

    /* Point 3 again, and the ownership rule with it. `inputs` is borrowed and
     * is not released here. The artifact created below is transferred to the
     * caller through `*output`, which is why the plugin does not retain it. */
    status = context->host->artifact_create(
        &context->host->allocator, QL_EXAMPLE_OUTPUT_KIND, view.schema_version,
        buffer, written, output, error);
    if (buffer != NULL) {
        state->host->allocator.deallocate(state->host->allocator.user_data,
                                          buffer);
    }
    return status;
}

/* Point 2 and point 4. `struct_size` and `abi_version` come from the headers
 * this file was compiled against, so a later host can tell exactly how much of
 * the structure exists. The flags are claims the host will act on:
 * DETERMINISTIC and CACHEABLE are both true of a pure byte rewrite, and
 * nothing about proofs or counterexamples is claimed, because this method
 * produces neither. */
static const ql_method_v1 example_methods[] = {
    {
        sizeof(ql_method_v1),
        QL_ABI_VERSION,
        "example.normalize",
        "Collapses runs of ASCII whitespace and trims the ends",
        QL_EXAMPLE_OUTPUT_KIND,
        QL_METHOD_DETERMINISTIC | QL_METHOD_CACHEABLE,
        1u,
        1u,
        example_create,
        example_validate,
        example_run,
        example_destroy,
        {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}
    }
};

static const ql_plugin_v1 example_plugin = {
    sizeof(ql_plugin_v1),
    QL_ABI_VERSION,
    "quodlibet-example-plugin",
    "1.0.0",
    example_methods,
    sizeof(example_methods) / sizeof(example_methods[0]),
    NULL, /* no shutdown hook: this plugin holds no process-wide state */
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    NULL, /* no proof methods, so the extension pair stays empty */
    0u
};

/* Point 1. The single exported symbol, and the whole ABI handshake.
 *
 * Refusing a host whose abi_version is not the one this plugin was built
 * against is what makes it safe for the host to load a module it did not
 * compile. The descriptor returned is static and outlives the call, because
 * the host borrows it until ql_plugin_unload. */
QL_PLUGIN_EXPORT ql_status QL_CALL quodlibet_plugin_init_v1(
    const ql_host_v1 *host, const ql_plugin_v1 **output, ql_error *error) {
    if (host == NULL || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "the host and a descriptor output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (host->abi_version != QL_ABI_VERSION ||
        host->struct_size < offsetof(ql_host_v1, reserved)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "this plugin was built for Quodlibet ABI %u",
                     (unsigned)QL_ABI_VERSION);
        return QL_STATUS_ABI_MISMATCH;
    }
    *output = &example_plugin;
    return QL_STATUS_OK;
}
