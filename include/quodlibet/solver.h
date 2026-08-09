#ifndef QUODLIBET_SOLVER_H
#define QUODLIBET_SOLVER_H

#include "quodlibet/artifact.h"

QL_EXTERN_C_BEGIN

#define QL_SOLVER_ABI_VERSION 1u
#define QL_SMTLIB2_SCHEMA_VERSION 1u
#define QL_SOLVER_ARTIFACT_SCHEMA_VERSION 1u
#define QL_SOLVER_MAX_SMTLIB2_BYTES (UINT64_C(64) * 1024u * 1024u)
#define QL_SOLVER_MAX_STDOUT_BYTES (UINT64_C(16) * 1024u * 1024u)
#define QL_SOLVER_MAX_STDERR_BYTES (UINT64_C(4) * 1024u * 1024u)

#define QL_ARTIFACT_KIND_SMTLIB2 "quodlibet.smtlib2"
#define QL_ARTIFACT_KIND_SOLVER_MODEL "quodlibet.solver-model"
#define QL_ARTIFACT_KIND_SOLVER_PROOF "quodlibet.solver-proof"
#define QL_ARTIFACT_KIND_SOLVER_UNSAT_METADATA \
    "quodlibet.solver-unsat-metadata"
#define QL_ARTIFACT_KIND_SOLVER_DIAGNOSTICS \
    "quodlibet.solver-diagnostics"

typedef uint64_t ql_solver_logic;

#define QL_SOLVER_LOGIC_INVALID UINT64_C(0)
#define QL_SOLVER_LOGIC_QF_BV (UINT64_C(1) << 0)
#define QL_SOLVER_LOGIC_QF_ABV (UINT64_C(1) << 1)
#define QL_SOLVER_LOGIC_QF_FP (UINT64_C(1) << 2)
#define QL_SOLVER_LOGIC_QF_BVFP (UINT64_C(1) << 3)
#define QL_SOLVER_LOGIC_QF_AUFBV (UINT64_C(1) << 4)
#define QL_SOLVER_LOGIC_ALL                                            \
    (QL_SOLVER_LOGIC_QF_BV | QL_SOLVER_LOGIC_QF_ABV |                 \
     QL_SOLVER_LOGIC_QF_FP | QL_SOLVER_LOGIC_QF_BVFP |               \
     QL_SOLVER_LOGIC_QF_AUFBV)

typedef enum ql_solver_availability {
    QL_SOLVER_UNAVAILABLE = 0,
    QL_SOLVER_AVAILABLE = 1
} ql_solver_availability;

typedef enum ql_solver_feature {
    QL_SOLVER_FEATURE_ARRAYS = UINT64_C(1) << 0,
    QL_SOLVER_FEATURE_FLOATING_POINT = UINT64_C(1) << 1,
    QL_SOLVER_FEATURE_INCREMENTAL = UINT64_C(1) << 2,
    QL_SOLVER_FEATURE_MODELS = UINT64_C(1) << 3,
    QL_SOLVER_FEATURE_PROOFS = UINT64_C(1) << 4,
    QL_SOLVER_FEATURE_CANCELLATION = UINT64_C(1) << 5,
    QL_SOLVER_FEATURE_TIMEOUT = UINT64_C(1) << 6,
    QL_SOLVER_FEATURE_PROCESS_ISOLATION = UINT64_C(1) << 7,
    QL_SOLVER_FEATURE_MEMORY_LIMIT = UINT64_C(1) << 8
} ql_solver_feature;

#define QL_SOLVER_FEATURE_ALL                                          \
    (UINT64_C(0) | QL_SOLVER_FEATURE_ARRAYS |                         \
     QL_SOLVER_FEATURE_FLOATING_POINT | QL_SOLVER_FEATURE_INCREMENTAL | \
     QL_SOLVER_FEATURE_MODELS | QL_SOLVER_FEATURE_PROOFS |            \
     QL_SOLVER_FEATURE_CANCELLATION | QL_SOLVER_FEATURE_TIMEOUT |     \
     QL_SOLVER_FEATURE_PROCESS_ISOLATION |                            \
     QL_SOLVER_FEATURE_MEMORY_LIMIT)

typedef struct ql_solver_capability_v1 {
    size_t struct_size;
    uint32_t abi_version;
    ql_solver_availability availability;
    uint64_t supported_logics;
    uint32_t minimum_bv_width;
    uint32_t maximum_bv_width;
    uint64_t features;
    uint64_t reserved[6];
} ql_solver_capability_v1;

typedef enum ql_solver_check_kind {
    QL_SOLVER_CHECK_INVALID = 0,
    QL_SOLVER_CHECK_SAT,
    QL_SOLVER_CHECK_UNSAT,
    QL_SOLVER_CHECK_UNKNOWN
} ql_solver_check_kind;

typedef enum ql_solver_unknown_reason {
    QL_SOLVER_UNKNOWN_NONE = 0,
    QL_SOLVER_UNKNOWN_CANCELLED,
    QL_SOLVER_UNKNOWN_TIMEOUT,
    QL_SOLVER_UNKNOWN_RESOURCE_LIMIT,
    QL_SOLVER_UNKNOWN_INCOMPLETE,
    QL_SOLVER_UNKNOWN_BACKEND
} ql_solver_unknown_reason;

typedef enum ql_solver_artifact_request {
    QL_SOLVER_REQUEST_MODEL = UINT32_C(1) << 0,
    QL_SOLVER_REQUEST_PROOF = UINT32_C(1) << 1,
    QL_SOLVER_REQUEST_UNSAT_METADATA = UINT32_C(1) << 2,
    QL_SOLVER_REQUEST_DIAGNOSTICS = UINT32_C(1) << 3
} ql_solver_artifact_request;

#define QL_SOLVER_REQUEST_ALL                                          \
    (UINT32_C(0) | QL_SOLVER_REQUEST_MODEL | QL_SOLVER_REQUEST_PROOF | \
     QL_SOLVER_REQUEST_UNSAT_METADATA | QL_SOLVER_REQUEST_DIAGNOSTICS)

typedef uint32_t (QL_CALL *ql_solver_is_cancelled_v1)(
    const void *cancel_state);

typedef struct ql_solver_check_request_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t artifact_requests;
    uint32_t maximum_bv_width;
    ql_solver_logic logic;
    uint64_t required_features;
    uint64_t timeout_ms;
    /* Backend-native per-check limit in MiB. Zero leaves it unset. This is
       not an operating-system RSS hard cap. */
    uint64_t memory_limit_mb;
    const void *cancel_state;
    ql_solver_is_cancelled_v1 is_cancelled;
    uint64_t stdout_limit_bytes;
    uint64_t stderr_limit_bytes;
    uint64_t reserved[4];
} ql_solver_check_request_v1;

/* Artifacts are owned by the result and are released by
   ql_solver_check_result_clear. Backend identity strings are borrowed from the
   descriptor and remain valid for the descriptor's lifetime. */
typedef struct ql_solver_check_result_v1 {
    size_t struct_size;
    uint32_t abi_version;
    ql_solver_check_kind kind;
    ql_solver_unknown_reason unknown_reason;
    uint32_t reserved32;
    const char *backend_name;
    const char *backend_version;
    ql_digest backend_binary_digest;
    ql_digest query_digest;
    ql_artifact *model_artifact;
    ql_artifact *proof_artifact;
    ql_artifact *unsat_metadata_artifact;
    ql_artifact *diagnostics_artifact;
    uint64_t reserved[4];
} ql_solver_check_result_v1;

typedef ql_status (QL_CALL *ql_solver_create_v1)(
    const ql_allocator *allocator, const char *options_json,
    void **backend_state, ql_error *error);
typedef void (QL_CALL *ql_solver_destroy_v1)(void *backend_state);
typedef ql_status (QL_CALL *ql_solver_add_smt2_v1)(
    void *backend_state, const char *commands, size_t size,
    ql_error *error);
typedef ql_status (QL_CALL *ql_solver_stack_v1)(
    void *backend_state, uint32_t levels, ql_error *error);
typedef ql_status (QL_CALL *ql_solver_check_v1)(
    void *backend_state, const ql_solver_check_request_v1 *request,
    ql_solver_check_result_v1 *result, ql_error *error);

/* Descriptor and all pointed-to strings and callbacks must outlive every
   ql_solver instance created from it. No callback may invoke a shell. */
typedef struct ql_solver_descriptor_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t reserved32;
    const char *name;
    const char *version;
    const char *description;
    ql_solver_capability_v1 capability;
    ql_solver_create_v1 create;
    ql_solver_destroy_v1 destroy;
    ql_solver_add_smt2_v1 add_smt2;
    ql_solver_stack_v1 push;
    ql_solver_stack_v1 pop;
    ql_solver_check_v1 check;
    void *reserved[8];
} ql_solver_descriptor_v1;

typedef struct ql_solver ql_solver;
typedef struct ql_smt2_builder ql_smt2_builder;

QL_API void QL_CALL ql_solver_capability_init(
    ql_solver_capability_v1 *capability);
QL_API void QL_CALL ql_solver_check_request_init(
    ql_solver_check_request_v1 *request, ql_solver_logic logic);
QL_API void QL_CALL ql_solver_check_result_init(
    ql_solver_check_result_v1 *result);
QL_API void QL_CALL ql_solver_check_result_clear(
    ql_solver_check_result_v1 *result);
/* Classifies process transport observations. With neither observation, an
   exact raw solver `unknown` is conservatively a backend unknown. */
QL_API ql_solver_unknown_reason QL_CALL ql_solver_unknown_reason_classify(
    uint32_t cancellation_observed, uint32_t watchdog_timeout_observed);

QL_API ql_status QL_CALL ql_solver_capability_validate(
    const ql_solver_capability_v1 *capability, ql_error *error);
QL_API ql_status QL_CALL ql_solver_descriptor_validate(
    const ql_solver_descriptor_v1 *descriptor, ql_error *error);
QL_API ql_status QL_CALL ql_solver_check_request_validate(
    const ql_solver_capability_v1 *capability,
    const ql_solver_check_request_v1 *request, ql_error *error);
QL_API ql_status QL_CALL ql_solver_check_result_validate(
    const ql_solver_check_request_v1 *request,
    const ql_solver_check_result_v1 *result, ql_error *error);

QL_API ql_status QL_CALL ql_solver_create(
    const ql_allocator *allocator, const ql_solver_descriptor_v1 *descriptor,
    const char *options_json, ql_solver **output, ql_error *error);
QL_API void QL_CALL ql_solver_destroy(ql_solver *solver);
QL_API ql_status QL_CALL ql_solver_add_smt2(
    ql_solver *solver, const ql_artifact *commands, ql_error *error);
QL_API ql_status QL_CALL ql_solver_push(
    ql_solver *solver, uint32_t levels, ql_error *error);
QL_API ql_status QL_CALL ql_solver_pop(
    ql_solver *solver, uint32_t levels, ql_error *error);
QL_API ql_status QL_CALL ql_solver_check(
    ql_solver *solver, const ql_solver_check_request_v1 *request,
    ql_solver_check_result_v1 *result, ql_error *error);

QL_API ql_status QL_CALL ql_smt2_builder_create(
    const ql_allocator *allocator, ql_solver_logic logic,
    ql_smt2_builder **output, ql_error *error);
QL_API void QL_CALL ql_smt2_builder_destroy(ql_smt2_builder *builder);
QL_API ql_status QL_CALL ql_smt2_builder_declare_bool(
    ql_smt2_builder *builder, const char *symbol, ql_error *error);
QL_API ql_status QL_CALL ql_smt2_builder_declare_bv(
    ql_smt2_builder *builder, const char *symbol, uint32_t width,
    ql_error *error);
QL_API ql_status QL_CALL ql_smt2_builder_assert(
    ql_smt2_builder *builder, const char *boolean_term, ql_error *error);
QL_API ql_status QL_CALL ql_smt2_builder_build(
    const ql_smt2_builder *builder, ql_artifact **output, ql_error *error);

/* Caller-supplied metadata that structurally binds a claimed checker artifact
   to exact solver, query, and raw-proof digests. Neither this structure nor
   successful validation establishes that the named checker ran or is trusted. */
typedef struct ql_solver_checked_proof_binding_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t reserved32;
    ql_digest backend_binary_digest;
    ql_digest query_digest;
    ql_digest raw_proof_digest;
    const char *checker_name;
    const char *checker_version;
    const ql_artifact *checked_proof_artifact;
    uint64_t reserved[4];
} ql_solver_checked_proof_binding_v1;

/* Performs structural kind and digest-binding validation only. It grants no
   authority to emit a PROVED verdict. Trusted proof-method code must invoke
   and authenticate its checker independently before any such promotion. */
QL_API ql_status QL_CALL ql_solver_checked_proof_binding_validate(
    const ql_solver_check_result_v1 *result,
    const ql_solver_checked_proof_binding_v1 *binding, ql_error *error);

/* Canonical backend. The descriptor is always present but advertises
   UNAVAILABLE when Bitwuzla support was disabled at build time. In an enabled
   build, create returns NOT_FOUND when no adjacent, build-default, or absolute
   options_json executable is available. */
QL_API const ql_solver_descriptor_v1 *QL_CALL
ql_bitwuzla_solver_descriptor(void);
QL_API const char *QL_CALL ql_bitwuzla_executable_path(void);

QL_EXTERN_C_END

#endif
