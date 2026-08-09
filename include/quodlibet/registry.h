#ifndef QUODLIBET_REGISTRY_H
#define QUODLIBET_REGISTRY_H

#include "quodlibet/method.h"

QL_EXTERN_C_BEGIN

typedef struct ql_registry ql_registry;

QL_API ql_status QL_CALL ql_registry_create(const ql_allocator *allocator,
                                            ql_registry **output,
                                            ql_error *error);
QL_API void QL_CALL ql_registry_destroy(ql_registry *registry);
QL_API ql_status QL_CALL ql_registry_register(ql_registry *registry,
                                              const ql_method_v1 *method,
                                              ql_error *error);
QL_API const ql_method_v1 *QL_CALL ql_registry_find(
    const ql_registry *registry, const char *name);
QL_API size_t QL_CALL ql_registry_count(const ql_registry *registry);
QL_API const ql_method_v1 *QL_CALL ql_registry_at(
    const ql_registry *registry, size_t index);
QL_API ql_status QL_CALL ql_register_builtin_methods(ql_registry *registry,
                                                     ql_error *error);

QL_EXTERN_C_END

#endif
