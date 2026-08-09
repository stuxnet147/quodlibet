#include "quodlibet/registry.h"

#include <stddef.h>
#include <string.h>

#include "internal.h"
#include "uv.h"

typedef struct ql_registry_entry {
    const ql_method_v1 *method;
    const ql_proof_method_v1 *proof_method;
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
    registry->entries[registry->count].proof_method = NULL;
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

ql_status ql_internal_registry_register_proof_owned(
    ql_registry *registry, const ql_proof_method_v1 *proof_method,
    const void *owner, ql_error *error) {
    const char *name;
    size_t index;
    ql_status status;

    if (registry == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "registry is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = ql_proof_method_validate(proof_method, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    name = proof_method->method->name;

    uv_rwlock_wrlock(&registry->lock);
    for (index = 0u; index < registry->count; ++index) {
        ql_registry_entry *entry = &registry->entries[index];

        if (strcmp(entry->method->name, name) != 0) {
            continue;
        }
        if (entry->proof_method != NULL) {
            uv_rwlock_wrunlock(&registry->lock);
            ql_error_set(error, QL_STATUS_ALREADY_EXISTS,
                         "proof descriptor for method '%s' is already registered",
                         name);
            return QL_STATUS_ALREADY_EXISTS;
        }
        if (entry->method != proof_method->method || entry->owner != owner) {
            uv_rwlock_wrunlock(&registry->lock);
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "proof descriptor for method '%s' does not decorate its registered executable",
                         name);
            return QL_STATUS_TYPE_MISMATCH;
        }
        entry->proof_method = proof_method;
        ++registry->generation;
        if (registry->generation == 0u) {
            registry->generation = UINT64_C(1);
        }
        uv_rwlock_wrunlock(&registry->lock);
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    uv_rwlock_wrunlock(&registry->lock);
    ql_error_set(error, QL_STATUS_NOT_FOUND,
                 "proof descriptor references unregistered method '%s'", name);
    return QL_STATUS_NOT_FOUND;
}

ql_status QL_CALL ql_registry_register_proof_method(
    ql_registry *registry, const ql_proof_method_v1 *proof_method,
    ql_error *error) {
    return ql_internal_registry_register_proof_owned(
        registry, proof_method, NULL, error);
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

const ql_proof_method_v1 *QL_CALL ql_registry_find_proof_method(
    const ql_registry *registry, const char *name) {
    size_t index;
    const ql_proof_method_v1 *found = NULL;
    ql_registry *mutable_registry = (ql_registry *)registry;

    if (registry == NULL || name == NULL) {
        return NULL;
    }
    uv_rwlock_rdlock(&mutable_registry->lock);
    for (index = 0u; index < registry->count; ++index) {
        if (strcmp(registry->entries[index].method->name, name) == 0) {
            found = registry->entries[index].proof_method;
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

size_t QL_CALL ql_registry_proof_method_count(
    const ql_registry *registry) {
    size_t count = 0u;
    size_t index;
    ql_registry *mutable_registry = (ql_registry *)registry;

    if (registry == NULL) {
        return 0u;
    }
    uv_rwlock_rdlock(&mutable_registry->lock);
    for (index = 0u; index < registry->count; ++index) {
        if (registry->entries[index].proof_method != NULL) {
            ++count;
        }
    }
    uv_rwlock_rdunlock(&mutable_registry->lock);
    return count;
}

const ql_proof_method_v1 *QL_CALL ql_registry_proof_method_at(
    const ql_registry *registry, size_t index) {
    const ql_proof_method_v1 *proof_method = NULL;
    size_t entry_index;
    size_t proof_index = 0u;
    ql_registry *mutable_registry = (ql_registry *)registry;

    if (registry == NULL) {
        return NULL;
    }
    uv_rwlock_rdlock(&mutable_registry->lock);
    for (entry_index = 0u; entry_index < registry->count; ++entry_index) {
        if (registry->entries[entry_index].proof_method == NULL) {
            continue;
        }
        if (proof_index == index) {
            proof_method = registry->entries[entry_index].proof_method;
            break;
        }
        ++proof_index;
    }
    uv_rwlock_rdunlock(&mutable_registry->lock);
    return proof_method;
}

static const ql_registry_entry *find_entry_locked(
    const ql_registry *registry, const char *name) {
    size_t index;

    for (index = 0u; index < registry->count; ++index) {
        if (strcmp(registry->entries[index].method->name, name) == 0) {
            return &registry->entries[index];
        }
    }
    return NULL;
}

static ql_status require_proof_entry_locked(
    const ql_registry *registry, const char *name,
    const ql_registry_entry **output, ql_error *error) {
    const ql_registry_entry *entry = find_entry_locked(registry, name);

    if (entry == NULL) {
        ql_error_set(error, QL_STATUS_NOT_FOUND,
                     "method '%s' is not registered", name);
        return QL_STATUS_NOT_FOUND;
    }
    if (entry->proof_method == NULL) {
        ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                     "method '%s' has no proof descriptor", name);
        return QL_STATUS_TYPE_MISMATCH;
    }
    *output = entry;
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_registry_query_proof_capability(
    const ql_registry *registry, const char *method_name,
    const char *options_json, ql_proof_method_capability_v1 *capability,
    ql_error *error) {
    const ql_registry_entry *entry = NULL;
    ql_registry *mutable_registry = (ql_registry *)registry;
    ql_status status;

    if (registry == NULL || method_name == NULL || method_name[0] == '\0' ||
        capability == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "registry, method name, and capability output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    ql_proof_method_capability_init(capability);
    uv_rwlock_rdlock(&mutable_registry->lock);
    status = require_proof_entry_locked(registry, method_name, &entry, error);
    if (status == QL_STATUS_OK) {
        status = ql_proof_method_query_capability(
            entry->proof_method, options_json, capability, error);
    }
    uv_rwlock_rdunlock(&mutable_registry->lock);
    return status;
}

ql_status QL_CALL ql_registry_validate_proof_request(
    const ql_registry *registry, const char *method_name,
    const char *options_json, const ql_proof_request_v1 *request,
    ql_error *error) {
    ql_proof_method_capability_v1 capability;
    const ql_registry_entry *entry = NULL;
    ql_registry *mutable_registry = (ql_registry *)registry;
    ql_status status;

    if (registry == NULL || method_name == NULL || method_name[0] == '\0') {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "registry and method name are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    ql_proof_method_capability_init(&capability);
    uv_rwlock_rdlock(&mutable_registry->lock);
    status = require_proof_entry_locked(registry, method_name, &entry, error);
    if (status == QL_STATUS_OK) {
        status = ql_proof_method_query_capability(
            entry->proof_method, options_json, &capability, error);
    }
    if (status == QL_STATUS_OK) {
        status = ql_proof_method_validate_request(&capability, request, error);
    }
    uv_rwlock_rdunlock(&mutable_registry->lock);
    return status;
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
