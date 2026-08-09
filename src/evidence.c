#include "quodlibet/evidence.h"

#include <stdatomic.h>
#include <string.h>

#include "blake3.h"
#include "internal.h"

struct ql_evidence_envelope {
    atomic_uint reference_count;
    ql_allocator allocator;
    ql_evidence_class evidence_class;
    char *method_name;
    char *method_version;
    ql_digest cache_key;
    ql_digest artifact_digest;
    ql_digest semantic_problem_digest;
    ql_digest evidence_digest;
    ql_artifact *evidence_artifact;
};

static void update_u64(blake3_hasher *hasher, uint64_t value) {
    uint8_t bytes[8];
    size_t index;

    for (index = 0u; index < sizeof(bytes); ++index) {
        bytes[index] = (uint8_t)((value >> (index * 8u)) & UINT64_C(0xff));
    }
    blake3_hasher_update(hasher, bytes, sizeof(bytes));
}

static void update_sized_bytes(blake3_hasher *hasher, const void *data,
                               size_t size) {
    update_u64(hasher, (uint64_t)size);
    if (size != 0u) {
        blake3_hasher_update(hasher, data, size);
    }
}

static ql_status validate_cache_key_input(
    const ql_cache_key_input_v1 *input, ql_error *error) {
    if (input == NULL || input->struct_size < sizeof(*input)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "cache key input v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (input->method_name == NULL || input->method_name[0] == '\0' ||
        input->method_version == NULL || input->method_version[0] == '\0') {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "cache key requires a method name and version");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (input->canonical_options == NULL &&
        input->canonical_options_size != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "canonical option size requires option bytes");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

ql_status QL_CALL ql_cache_key_compute(const ql_cache_key_input_v1 *input,
                                       ql_digest *cache_key,
                                       ql_error *error) {
    static const char domain[] = "quodlibet-cache-key-v1";
    static const uint8_t schema_bytes[4] = {
        (uint8_t)(QL_CACHE_KEY_SCHEMA_VERSION & 0xffu),
        (uint8_t)((QL_CACHE_KEY_SCHEMA_VERSION >> 8u) & 0xffu),
        (uint8_t)((QL_CACHE_KEY_SCHEMA_VERSION >> 16u) & 0xffu),
        (uint8_t)((QL_CACHE_KEY_SCHEMA_VERSION >> 24u) & 0xffu)
    };
    blake3_hasher hasher;
    ql_status status;

    if (cache_key == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "cache key output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_cache_key_input(input, error);
    if (status != QL_STATUS_OK) {
        memset(cache_key, 0, sizeof(*cache_key));
        return status;
    }

    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, domain, sizeof(domain) - 1u);
    blake3_hasher_update(&hasher, schema_bytes, sizeof(schema_bytes));
    blake3_hasher_update(&hasher, input->artifact_digest.bytes,
                         QL_DIGEST_SIZE);
    blake3_hasher_update(&hasher, input->semantic_problem_digest.bytes,
                         QL_DIGEST_SIZE);
    update_sized_bytes(&hasher, input->method_name,
                       strlen(input->method_name));
    update_sized_bytes(&hasher, input->method_version,
                       strlen(input->method_version));
    update_sized_bytes(&hasher, input->canonical_options,
                       input->canonical_options_size);
    blake3_hasher_finalize(&hasher, cache_key->bytes, QL_DIGEST_SIZE);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

static int valid_evidence_class(ql_evidence_class evidence_class) {
    return evidence_class == QL_EVIDENCE_PROOF ||
           evidence_class == QL_EVIDENCE_COUNTEREXAMPLE ||
           evidence_class == QL_EVIDENCE_BOUNDED ||
           evidence_class == QL_EVIDENCE_UNKNOWN;
}

static const char *expected_artifact_kind(ql_evidence_class evidence_class) {
    switch (evidence_class) {
    case QL_EVIDENCE_PROOF: return QL_ARTIFACT_KIND_PROOF;
    case QL_EVIDENCE_COUNTEREXAMPLE:
        return QL_ARTIFACT_KIND_COUNTEREXAMPLE;
    case QL_EVIDENCE_BOUNDED:
    case QL_EVIDENCE_UNKNOWN: return QL_ARTIFACT_KIND_OUTCOME;
    default: return NULL;
    }
}

ql_status QL_CALL ql_evidence_envelope_create(
    const ql_allocator *allocator, const ql_cache_key_input_v1 *identity,
    ql_evidence_class evidence_class, ql_artifact *evidence_artifact,
    ql_evidence_envelope **output, ql_error *error) {
    const ql_allocator *selected = allocator;
    ql_artifact_view artifact_view;
    ql_digest cache_key;
    ql_evidence_envelope *envelope;
    const char *expected_kind;
    ql_status status;

    if (output == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "evidence envelope output is required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    *output = NULL;
    if (!valid_evidence_class(evidence_class)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "evidence class is invalid");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = ql_cache_key_compute(identity, &cache_key, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    expected_kind = expected_artifact_kind(evidence_class);
    if ((evidence_class == QL_EVIDENCE_PROOF ||
         evidence_class == QL_EVIDENCE_COUNTEREXAMPLE) &&
        evidence_artifact == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "%s evidence requires an artifact",
                     ql_evidence_class_string(evidence_class));
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&artifact_view, 0, sizeof(artifact_view));
    artifact_view.struct_size = sizeof(artifact_view);
    if (evidence_artifact != NULL) {
        status = ql_artifact_get_view(evidence_artifact, &artifact_view,
                                      error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        if (strcmp(artifact_view.kind, expected_kind) != 0) {
            ql_error_set(error, QL_STATUS_TYPE_MISMATCH,
                         "%s evidence requires artifact kind '%s'",
                         ql_evidence_class_string(evidence_class),
                         expected_kind);
            return QL_STATUS_TYPE_MISMATCH;
        }
    }

    if (selected == NULL) {
        selected = ql_default_allocator();
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    envelope = selected->allocate(selected->user_data, sizeof(*envelope));
    if (envelope == NULL) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }
    memset(envelope, 0, sizeof(*envelope));
    envelope->allocator = *selected;
    envelope->method_name =
        ql_internal_strdup(selected, identity->method_name);
    envelope->method_version =
        ql_internal_strdup(selected, identity->method_version);
    if (envelope->method_name == NULL || envelope->method_version == NULL) {
        selected->deallocate(selected->user_data, envelope->method_version);
        selected->deallocate(selected->user_data, envelope->method_name);
        selected->deallocate(selected->user_data, envelope);
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return QL_STATUS_OUT_OF_MEMORY;
    }

    envelope->evidence_class = evidence_class;
    envelope->cache_key = cache_key;
    envelope->artifact_digest = identity->artifact_digest;
    envelope->semantic_problem_digest = identity->semantic_problem_digest;
    envelope->evidence_artifact = evidence_artifact;
    if (evidence_artifact != NULL) {
        envelope->evidence_digest = artifact_view.digest;
        ql_artifact_retain(evidence_artifact);
    }
    atomic_init(&envelope->reference_count, 1u);
    *output = envelope;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_evidence_envelope_retain(ql_evidence_envelope *envelope) {
    if (envelope != NULL) {
        (void)atomic_fetch_add_explicit(&envelope->reference_count, 1u,
                                        memory_order_relaxed);
    }
}

void QL_CALL ql_evidence_envelope_release(ql_evidence_envelope *envelope) {
    ql_allocator allocator;

    if (envelope == NULL ||
        atomic_fetch_sub_explicit(&envelope->reference_count, 1u,
                                  memory_order_acq_rel) != 1u) {
        return;
    }
    allocator = envelope->allocator;
    ql_artifact_release(envelope->evidence_artifact);
    allocator.deallocate(allocator.user_data, envelope->method_version);
    allocator.deallocate(allocator.user_data, envelope->method_name);
    allocator.deallocate(allocator.user_data, envelope);
}

ql_status QL_CALL ql_evidence_envelope_get_view(
    const ql_evidence_envelope *envelope,
    ql_evidence_envelope_view_v1 *view, ql_error *error) {
    if (envelope == NULL || view == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "evidence envelope and view are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (view->struct_size != 0u && view->struct_size < sizeof(*view)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "evidence envelope view structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    memset(view, 0, sizeof(*view));
    view->struct_size = sizeof(*view);
    view->schema_version = QL_EVIDENCE_ENVELOPE_SCHEMA_VERSION;
    view->evidence_class = envelope->evidence_class;
    view->method_name = envelope->method_name;
    view->method_version = envelope->method_version;
    view->cache_key = envelope->cache_key;
    view->artifact_digest = envelope->artifact_digest;
    view->semantic_problem_digest = envelope->semantic_problem_digest;
    view->evidence_digest = envelope->evidence_digest;
    view->evidence_artifact = envelope->evidence_artifact;
    ql_error_clear(error);
    return QL_STATUS_OK;
}

const char *QL_CALL ql_evidence_class_string(
    ql_evidence_class evidence_class) {
    switch (evidence_class) {
    case QL_EVIDENCE_PROOF: return "PROOF";
    case QL_EVIDENCE_COUNTEREXAMPLE: return "COUNTEREXAMPLE";
    case QL_EVIDENCE_BOUNDED: return "BOUNDED";
    case QL_EVIDENCE_UNKNOWN: return "UNKNOWN";
    default: return "INVALID_EVIDENCE";
    }
}
