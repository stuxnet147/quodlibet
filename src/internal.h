#ifndef QUODLIBET_INTERNAL_H
#define QUODLIBET_INTERNAL_H

#include "quodlibet/quodlibet.h"
#include "quodlibet/ir.h"

char *ql_internal_strdup(const ql_allocator *allocator, const char *text);
ql_status ql_internal_registry_register_owned(ql_registry *registry,
                                              const ql_method_v1 *method,
                                              const void *owner,
                                              ql_error *error);
ql_status ql_internal_registry_register_proof_owned(
    ql_registry *registry, const ql_proof_method_v1 *proof_method,
    const void *owner, ql_error *error);
void ql_internal_registry_unregister_owner(ql_registry *registry,
                                           const void *owner);
const ql_allocator *ql_internal_registry_allocator(
    const ql_registry *registry);
uint64_t ql_internal_registry_generation(const ql_registry *registry);
/* Lowering may discover an auxiliary memory object only when it accesses
   pointer bits not already covered by the table. Artifacts permit parameters
   anywhere in the value table, while the public builder deliberately keeps
   its simpler parameters-first contract. */
ql_status ql_internal_ir_builder_add_late_parameter(
    ql_ir_builder *builder, ql_ir_type_id type, const char *name,
    size_t name_size, ql_ir_value_id *output, ql_error *error);
ql_status ql_internal_ir_builder_replace_entry_block(
    ql_ir_builder *builder, ql_ir_block_id expected,
    ql_ir_block_id replacement, ql_error *error);
/* Does either side thread an event trace? A body that calls takes the
   incoming history as a parameter. Every refutation path that replays a
   solver model concretely must gate on this: running a call needs a callee
   the witness does not carry, so a violation that threads a trace stays a
   claim about the encoding rather than becoming a counterexample. */
int ql_internal_ir_threads_event_trace(const ql_ir *left, const ql_ir *right);

#endif
