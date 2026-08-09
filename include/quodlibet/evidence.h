#ifndef QUODLIBET_EVIDENCE_H
#define QUODLIBET_EVIDENCE_H

#include "quodlibet/artifact.h"

QL_EXTERN_C_BEGIN

#define QL_CACHE_KEY_SCHEMA_VERSION 1u
#define QL_EVIDENCE_ENVELOPE_SCHEMA_VERSION 1u

/* The options bytes are method-defined, canonical bytes. Semantically equal
   option sets must be encoded identically before this API is called. */
typedef struct ql_cache_key_input_v1 {
    size_t struct_size;
    ql_digest artifact_digest;
    ql_digest semantic_problem_digest;
    const char *method_name;
    const char *method_version;
    const void *canonical_options;
    size_t canonical_options_size;
    uint64_t reserved[4];
} ql_cache_key_input_v1;

QL_API ql_status QL_CALL ql_cache_key_compute(
    const ql_cache_key_input_v1 *input, ql_digest *cache_key,
    ql_error *error);

typedef enum ql_evidence_class {
    QL_EVIDENCE_PROOF = 0,
    QL_EVIDENCE_COUNTEREXAMPLE,
    /* No counterexample was found within a finite, method-defined bound. */
    QL_EVIDENCE_BOUNDED,
    QL_EVIDENCE_UNKNOWN
} ql_evidence_class;

typedef struct ql_evidence_envelope ql_evidence_envelope;

/* All pointers in this view are borrowed from the envelope and remain valid
   until the envelope is released. */
typedef struct ql_evidence_envelope_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_evidence_class evidence_class;
    const char *method_name;
    const char *method_version;
    ql_digest cache_key;
    ql_digest artifact_digest;
    ql_digest semantic_problem_digest;
    ql_digest evidence_digest;
    const ql_artifact *evidence_artifact;
    uint64_t reserved[4];
} ql_evidence_envelope_view_v1;

/* Proof and counterexample envelopes require a matching evidence artifact.
   Bounded and unknown envelopes may omit it or carry a quodlibet.outcome. */
QL_API ql_status QL_CALL ql_evidence_envelope_create(
    const ql_allocator *allocator, const ql_cache_key_input_v1 *identity,
    ql_evidence_class evidence_class, ql_artifact *evidence_artifact,
    ql_evidence_envelope **output, ql_error *error);
QL_API void QL_CALL ql_evidence_envelope_retain(
    ql_evidence_envelope *envelope);
QL_API void QL_CALL ql_evidence_envelope_release(
    ql_evidence_envelope *envelope);
QL_API ql_status QL_CALL ql_evidence_envelope_get_view(
    const ql_evidence_envelope *envelope,
    ql_evidence_envelope_view_v1 *view, ql_error *error);
QL_API const char *QL_CALL ql_evidence_class_string(
    ql_evidence_class evidence_class);

QL_EXTERN_C_END

#endif
