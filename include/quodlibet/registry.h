#ifndef QUODLIBET_REGISTRY_H
#define QUODLIBET_REGISTRY_H

#include "quodlibet/proof_method.h"

QL_EXTERN_C_BEGIN

typedef struct ql_registry ql_registry;

QL_API ql_status QL_CALL ql_registry_create(const ql_allocator *allocator,
                                            ql_registry **output,
                                            ql_error *error);
QL_API void QL_CALL ql_registry_destroy(ql_registry *registry);
QL_API ql_status QL_CALL ql_registry_register(ql_registry *registry,
                                              const ql_method_v1 *method,
                                              ql_error *error);
/* Decorates an already registered executable method. The registry borrows the
   descriptor and underlying method while their owner remains registered. */
QL_API ql_status QL_CALL ql_registry_register_proof_method(
    ql_registry *registry, const ql_proof_method_v1 *proof_method,
    ql_error *error);
QL_API const ql_method_v1 *QL_CALL ql_registry_find(
    const ql_registry *registry, const char *name);
QL_API const ql_proof_method_v1 *QL_CALL ql_registry_find_proof_method(
    const ql_registry *registry, const char *name);
QL_API size_t QL_CALL ql_registry_count(const ql_registry *registry);
QL_API const ql_method_v1 *QL_CALL ql_registry_at(
    const ql_registry *registry, size_t index);
QL_API size_t QL_CALL ql_registry_proof_method_count(
    const ql_registry *registry);
/* The index is relative to proof methods, not to ql_registry_at(). */
QL_API const ql_proof_method_v1 *QL_CALL ql_registry_proof_method_at(
    const ql_registry *registry, size_t index);

/* These calls keep a plugin-backed descriptor alive for the duration of its
   capability callback. options_json is borrowed and may be null. */
QL_API ql_status QL_CALL ql_registry_query_proof_capability(
    const ql_registry *registry, const char *method_name,
    const char *options_json, ql_proof_method_capability_v1 *capability,
    ql_error *error);
QL_API ql_status QL_CALL ql_registry_validate_proof_request(
    const ql_registry *registry, const char *method_name,
    const char *options_json, const ql_proof_request_v1 *request,
    ql_error *error);
QL_API ql_status QL_CALL ql_register_builtin_methods(ql_registry *registry,
                                                     ql_error *error);

QL_EXTERN_C_END

#endif
