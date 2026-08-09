#ifndef QUODLIBET_INTERNAL_H
#define QUODLIBET_INTERNAL_H

#include "quodlibet/quodlibet.h"

char *ql_internal_strdup(const ql_allocator *allocator, const char *text);
ql_status ql_internal_registry_register_owned(ql_registry *registry,
                                              const ql_method_v1 *method,
                                              const void *owner,
                                              ql_error *error);
void ql_internal_registry_unregister_owner(ql_registry *registry,
                                           const void *owner);
const ql_allocator *ql_internal_registry_allocator(
    const ql_registry *registry);
uint64_t ql_internal_registry_generation(const ql_registry *registry);

#endif
