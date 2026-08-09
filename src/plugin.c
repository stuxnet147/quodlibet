#include "quodlibet/plugin.h"

#include <stddef.h>
#include <string.h>

#include "internal.h"
#include "uv.h"

struct ql_plugin_handle {
    ql_allocator allocator;
    ql_registry *registry;
    uv_lib_t library;
    const ql_plugin_v1 *descriptor;
};

ql_status QL_CALL ql_plugin_load(ql_registry *registry, const char *path,
                                 ql_plugin_handle **output, ql_error *error) {
    const ql_allocator *allocator;
    ql_plugin_handle *handle;
    ql_plugin_init_v1_fn initialize = NULL;
    const ql_plugin_v1 *descriptor = NULL;
    void *symbol = NULL;
    ql_status status;
    size_t index;
    int uv_status;

    if (registry == NULL || path == NULL || path[0] == '\0' || output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "registry, plugin path, and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    allocator = ql_internal_registry_allocator(registry);
    handle = allocator->allocate(allocator->user_data, sizeof(*handle));
    if (handle == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(handle, 0, sizeof(*handle));
    handle->allocator = *allocator;
    handle->registry = registry;

    uv_status = uv_dlopen(path, &handle->library);
    if (uv_status != 0) {
        ql_error_set(error, QL_STATUS_PLUGIN_ERROR,
                     "could not load plugin '%s': %s", path,
                     uv_dlerror(&handle->library));
        uv_dlclose(&handle->library);
        allocator->deallocate(allocator->user_data, handle);
        return QL_STATUS_PLUGIN_ERROR;
    }
    uv_status = uv_dlsym(&handle->library, QL_PLUGIN_ENTRY_SYMBOL, &symbol);
    if (uv_status != 0 || symbol == NULL) {
        ql_error_set(error, QL_STATUS_PLUGIN_ERROR,
                     "plugin '%s' does not export %s: %s", path,
                     QL_PLUGIN_ENTRY_SYMBOL, uv_dlerror(&handle->library));
        uv_dlclose(&handle->library);
        allocator->deallocate(allocator->user_data, handle);
        return QL_STATUS_PLUGIN_ERROR;
    }
    if (sizeof(initialize) != sizeof(symbol)) {
        ql_error_set(error, QL_STATUS_PLUGIN_ERROR,
                     "platform function pointer size is unsupported");
        uv_dlclose(&handle->library);
        allocator->deallocate(allocator->user_data, handle);
        return QL_STATUS_PLUGIN_ERROR;
    }
    memcpy(&initialize, &symbol, sizeof(initialize));
    status = initialize(ql_default_host(), &descriptor, error);
    if (status != QL_STATUS_OK) {
        if (error != NULL && error->message[0] == '\0') {
            ql_error_set(error, status, "plugin initialization failed");
        }
        uv_dlclose(&handle->library);
        allocator->deallocate(allocator->user_data, handle);
        return status;
    }
    if (descriptor == NULL || descriptor->struct_size <
            offsetof(ql_plugin_v1, reserved) ||
        descriptor->abi_version != QL_ABI_VERSION ||
        descriptor->name == NULL || descriptor->name[0] == '\0' ||
        (descriptor->method_count != 0u && descriptor->methods == NULL)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "plugin '%s' returned an invalid ABI descriptor", path);
        if (descriptor != NULL &&
            descriptor->struct_size >= offsetof(ql_plugin_v1, reserved) &&
            descriptor->shutdown != NULL) {
            descriptor->shutdown();
        }
        uv_dlclose(&handle->library);
        allocator->deallocate(allocator->user_data, handle);
        return QL_STATUS_ABI_MISMATCH;
    }
    handle->descriptor = descriptor;
    for (index = 0u; index < descriptor->method_count; ++index) {
        status = ql_internal_registry_register_owned(
            registry, &descriptor->methods[index], handle, error);
        if (status != QL_STATUS_OK) {
            ql_internal_registry_unregister_owner(registry, handle);
            if (descriptor->shutdown != NULL) {
                descriptor->shutdown();
            }
            uv_dlclose(&handle->library);
            allocator->deallocate(allocator->user_data, handle);
            return status;
        }
    }
    *output = handle;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

const char *QL_CALL ql_plugin_name(const ql_plugin_handle *plugin) {
    return plugin != NULL && plugin->descriptor != NULL
               ? plugin->descriptor->name
               : NULL;
}

const char *QL_CALL ql_plugin_version(const ql_plugin_handle *plugin) {
    return plugin != NULL && plugin->descriptor != NULL
               ? plugin->descriptor->version
               : NULL;
}

void QL_CALL ql_plugin_unload(ql_plugin_handle *plugin) {
    ql_allocator allocator;

    if (plugin == NULL) {
        return;
    }
    allocator = plugin->allocator;
    ql_internal_registry_unregister_owner(plugin->registry, plugin);
    if (plugin->descriptor != NULL && plugin->descriptor->shutdown != NULL) {
        plugin->descriptor->shutdown();
    }
    uv_dlclose(&plugin->library);
    allocator.deallocate(allocator.user_data, plugin);
}
