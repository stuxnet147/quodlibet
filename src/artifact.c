#include "quodlibet/artifact.h"

#include <stdatomic.h>
#include <string.h>

#include "blake3.h"
#include "internal.h"

struct ql_artifact {
    atomic_uint reference_count;
    ql_allocator allocator;
    char *kind;
    uint32_t schema_version;
    void *data;
    size_t size;
    ql_digest digest;
};

static void artifact_digest(const char *kind, uint32_t schema_version,
                            const void *data, size_t size, ql_digest *digest) {
    static const char domain[] = "quodlibet-artifact-v1";
    uint8_t schema_bytes[4];
    blake3_hasher hasher;

    schema_bytes[0] = (uint8_t)(schema_version & 0xffu);
    schema_bytes[1] = (uint8_t)((schema_version >> 8u) & 0xffu);
    schema_bytes[2] = (uint8_t)((schema_version >> 16u) & 0xffu);
    schema_bytes[3] = (uint8_t)((schema_version >> 24u) & 0xffu);
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, domain, sizeof(domain));
    blake3_hasher_update(&hasher, kind, strlen(kind) + 1u);
    blake3_hasher_update(&hasher, schema_bytes, sizeof(schema_bytes));
    if (size != 0u) {
        blake3_hasher_update(&hasher, data, size);
    }
    blake3_hasher_finalize(&hasher, digest->bytes, QL_DIGEST_SIZE);
}

ql_status QL_CALL ql_artifact_create(
    const ql_allocator *allocator, const char *kind, uint32_t schema_version,
    const void *data, size_t size, ql_artifact **output, ql_error *error) {
    ql_artifact *artifact;
    const ql_allocator *selected = allocator;

    if (output == NULL || kind == NULL || kind[0] == '\0' ||
        (data == NULL && size != 0u)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "artifact creation requires a kind, valid data, and output");
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

    artifact = selected->allocate(selected->user_data, sizeof(*artifact));
    if (artifact == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(artifact, 0, sizeof(*artifact));
    artifact->allocator = *selected;
    artifact->kind = ql_internal_strdup(selected, kind);
    if (artifact->kind == NULL) {
        selected->deallocate(selected->user_data, artifact);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    if (size != 0u) {
        artifact->data = selected->allocate(selected->user_data, size);
        if (artifact->data == NULL) {
            selected->deallocate(selected->user_data, artifact->kind);
            selected->deallocate(selected->user_data, artifact);
            ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
            return QL_STATUS_OUT_OF_MEMORY;
        }
        memcpy(artifact->data, data, size);
    }
    artifact->schema_version = schema_version;
    artifact->size = size;
    artifact_digest(kind, schema_version, data, size, &artifact->digest);
    atomic_init(&artifact->reference_count, 1u);
    *output = artifact;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_artifact_retain(ql_artifact *artifact) {
    if (artifact != NULL) {
        (void)atomic_fetch_add_explicit(&artifact->reference_count, 1u,
                                        memory_order_relaxed);
    }
}

void QL_CALL ql_artifact_release(ql_artifact *artifact) {
    ql_allocator allocator;

    if (artifact == NULL ||
        atomic_fetch_sub_explicit(&artifact->reference_count, 1u,
                                  memory_order_acq_rel) != 1u) {
        return;
    }
    allocator = artifact->allocator;
    if (artifact->data != NULL) {
        allocator.deallocate(allocator.user_data, artifact->data);
    }
    allocator.deallocate(allocator.user_data, artifact->kind);
    allocator.deallocate(allocator.user_data, artifact);
}

ql_status QL_CALL ql_artifact_get_view(const ql_artifact *artifact,
                                       ql_artifact_view *view,
                                       ql_error *error) {
    if (artifact == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "artifact and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view->struct_size != 0u &&
        view->struct_size < sizeof(ql_artifact_view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "artifact view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    view->struct_size = sizeof(*view);
    view->kind = artifact->kind;
    view->schema_version = artifact->schema_version;
    view->data = artifact->data;
    view->size = artifact->size;
    view->digest = artifact->digest;
    ql_error_clear(error);
    return QL_STATUS_OK;
}
