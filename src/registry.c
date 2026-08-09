#include "quodlibet/registry.h"

#include <stddef.h>
#include <string.h>

#include "internal.h"
#include "uv.h"

typedef struct ql_registry_entry {
    const ql_method_v1 *method;
    const void *owner;
} ql_registry_entry;

struct ql_registry {
    ql_allocator allocator;
    uv_rwlock_t lock;
    ql_registry_entry *entries;
    size_t count;
    size_t capacity;
    uint64_t generation;
};

static ql_status validate_method(const ql_method_v1 *method,
                                 ql_error *error) {
    const size_t minimum_size = offsetof(ql_method_v1, reserved);

    if (method == NULL || method->name == NULL || method->name[0] == '\0' ||
        method->run == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a method requires a name and run callback");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (method->abi_version != QL_ABI_VERSION ||
        method->struct_size < minimum_size) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "method '%s' uses ABI %u and structure size %zu; host requires ABI %u and at least %zu bytes",
                     method->name, method->abi_version, method->struct_size,
                     QL_ABI_VERSION, minimum_size);
        return QL_STATUS_ABI_MISMATCH;
    }
    if (method->minimum_inputs > method->maximum_inputs) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "method '%s' has an invalid input arity", method->name);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_registry_create(const ql_allocator *allocator,
                                     ql_registry **output, ql_error *error) {
    const ql_allocator *selected = allocator;
    ql_registry *registry;
    int uv_status;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "registry output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (selected == NULL) {
        selected = ql_default_allocator();
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    registry = selected->allocate(selected->user_data, sizeof(*registry));
    if (registry == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(registry, 0, sizeof(*registry));
    registry->allocator = *selected;
    registry->generation = UINT64_C(1);
    uv_status = uv_rwlock_init(&registry->lock);
    if (uv_status != 0) {
        selected->deallocate(selected->user_data, registry);
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "could not initialize registry lock: %s",
                     uv_strerror(uv_status));
        return QL_STATUS_INTERNAL_ERROR;
    }
    *output = registry;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_registry_destroy(ql_registry *registry) {
    if (registry == NULL) {
        return;
    }
    uv_rwlock_destroy(&registry->lock);
    registry->allocator.deallocate(registry->allocator.user_data,
                                   registry->entries);
    registry->allocator.deallocate(registry->allocator.user_data, registry);
}

ql_status ql_internal_registry_register_owned(ql_registry *registry,
                                              const ql_method_v1 *method,
                                              const void *owner,
                                              ql_error *error) {
    ql_registry_entry *entries;
    size_t capacity;
    size_t index;
    ql_status status;

    if (registry == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "registry is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_method(method, error);
    if (status != QL_STATUS_OK) {
        return status;
    }

    uv_rwlock_wrlock(&registry->lock);
    for (index = 0u; index < registry->count; ++index) {
        if (strcmp(registry->entries[index].method->name, method->name) == 0) {
            uv_rwlock_wrunlock(&registry->lock);
            ql_error_set(error, QL_STATUS_ALREADY_EXISTS,
                         "method '%s' is already registered", method->name);
            return QL_STATUS_ALREADY_EXISTS;
        }
    }
    if (registry->count == registry->capacity) {
        capacity = registry->capacity == 0u ? 8u : registry->capacity * 2u;
        if (capacity < registry->capacity ||
            capacity > SIZE_MAX / sizeof(*entries)) {
            uv_rwlock_wrunlock(&registry->lock);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY,
                         "registry capacity overflow");
            return QL_STATUS_OUT_OF_MEMORY;
        }
        entries = registry->allocator.reallocate(
            registry->allocator.user_data, registry->entries,
            capacity * sizeof(*entries));
        if (entries == NULL) {
            uv_rwlock_wrunlock(&registry->lock);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        registry->entries = entries;
        registry->capacity = capacity;
    }
    registry->entries[registry->count].method = method;
    registry->entries[registry->count].owner = owner;
    ++registry->count;
    ++registry->generation;
    if (registry->generation == 0u) {
        registry->generation = UINT64_C(1);
    }
    uv_rwlock_wrunlock(&registry->lock);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_registry_register(ql_registry *registry,
                                       const ql_method_v1 *method,
                                       ql_error *error) {
    return ql_internal_registry_register_owned(registry, method, NULL, error);
}

const ql_method_v1 *QL_CALL ql_registry_find(const ql_registry *registry,
                                             const char *name) {
    size_t index;
    const ql_method_v1 *found = NULL;
    ql_registry *mutable_registry = (ql_registry *)registry;

    if (registry == NULL || name == NULL) {
        return NULL;
    }
    uv_rwlock_rdlock(&mutable_registry->lock);
    for (index = 0u; index < registry->count; ++index) {
        if (strcmp(registry->entries[index].method->name, name) == 0) {
            found = registry->entries[index].method;
            break;
        }
    }
    uv_rwlock_rdunlock(&mutable_registry->lock);
    return found;
}

size_t QL_CALL ql_registry_count(const ql_registry *registry) {
    size_t count;
    ql_registry *mutable_registry = (ql_registry *)registry;

    if (registry == NULL) {
        return 0u;
    }
    uv_rwlock_rdlock(&mutable_registry->lock);
    count = registry->count;
    uv_rwlock_rdunlock(&mutable_registry->lock);
    return count;
}

const ql_method_v1 *QL_CALL ql_registry_at(const ql_registry *registry,
                                           size_t index) {
    const ql_method_v1 *method = NULL;
    ql_registry *mutable_registry = (ql_registry *)registry;

    if (registry == NULL) {
        return NULL;
    }
    uv_rwlock_rdlock(&mutable_registry->lock);
    if (index < registry->count) {
        method = registry->entries[index].method;
    }
    uv_rwlock_rdunlock(&mutable_registry->lock);
    return method;
}

void ql_internal_registry_unregister_owner(ql_registry *registry,
                                           const void *owner) {
    size_t read_index;
    size_t write_index = 0u;
    size_t old_count;

    if (registry == NULL || owner == NULL) {
        return;
    }
    uv_rwlock_wrlock(&registry->lock);
    old_count = registry->count;
    for (read_index = 0u; read_index < registry->count; ++read_index) {
        if (registry->entries[read_index].owner != owner) {
            if (write_index != read_index) {
                registry->entries[write_index] = registry->entries[read_index];
            }
            ++write_index;
        }
    }
    registry->count = write_index;
    if (registry->count != old_count) {
        ++registry->generation;
        if (registry->generation == 0u) {
            registry->generation = UINT64_C(1);
        }
    }
    uv_rwlock_wrunlock(&registry->lock);
}

const ql_allocator *ql_internal_registry_allocator(
    const ql_registry *registry) {
    return registry != NULL ? &registry->allocator : NULL;
}

uint64_t ql_internal_registry_generation(const ql_registry *registry) {
    uint64_t generation;
    ql_registry *mutable_registry = (ql_registry *)registry;

    if (registry == NULL) {
        return 0u;
    }
    uv_rwlock_rdlock(&mutable_registry->lock);
    generation = registry->generation;
    uv_rwlock_rdunlock(&mutable_registry->lock);
    return generation;
}
