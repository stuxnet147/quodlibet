#ifndef QUODLIBET_IR_INTERP_H
#define QUODLIBET_IR_INTERP_H

#include "quodlibet/ir.h"

QL_EXTERN_C_BEGIN

#define QL_IR_INTERP_SCHEMA_VERSION 1u
/* Signed multiplication and signed left shift lower through a doubled-width
   operation, so a 128-bit C type already needs 256-bit intermediates. */
#define QL_IR_INTERP_MAX_BIT_WIDTH 256u
#define QL_IR_INTERP_VALUE_CAPACITY 32u
#define QL_IR_INTERP_DEFAULT_STEP_LIMIT UINT64_C(1000000)

/* Concrete execution of a lowered module on typed inputs.

   The interpreter models undefinedness the way the lowering produces it. A
   partial operation does not stop execution; it yields a value marked
   undefined, which propagates through pure operations. SELECT propagates only
   from its condition and the arm it selects, because the lowering evaluates
   both arms of `&&` and `||` eagerly and short-circuits definedness alone.
   Execution becomes undefined only where undefinedness is observed: a
   UB_GUARD whose predicate is false, a branch or a return that reads an
   undefined value, or an explicit undefined-behaviour terminator.

   That split is what makes this a check on the lowering rather than a second
   opinion about it. The verifier proves a guard stands between every partial
   operation and every observation of it. The interpreter proves the guard's
   predicate is strong enough: if every guard passed and an observation still
   read an undefined value, the predicate admitted an execution the operation
   leaves undefined, and the result says so. */

typedef enum ql_ir_interp_outcome {
    QL_IR_INTERP_OUTCOME_RETURN = 0,
    QL_IR_INTERP_OUTCOME_TRAP,
    QL_IR_INTERP_OUTCOME_TERMINATE,
    QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
    QL_IR_INTERP_OUTCOME_DIVERGE,
    /* An ASSUME rejected these inputs. The run says nothing about the
       function; the inputs were outside the assumed domain. */
    QL_IR_INTERP_OUTCOME_ASSUMPTION_VIOLATED,
    /* The module uses a construct this interpreter does not model. This is
       never a statement about the function, and no proof method may read it
       as one. */
    QL_IR_INTERP_OUTCOME_UNSUPPORTED,
    /* The step limit was reached. Schema v1 modules are acyclic, so this only
       fires on a module far larger than the limit allows. */
    QL_IR_INTERP_OUTCOME_STEP_LIMIT
} ql_ir_interp_outcome;

typedef enum ql_ir_interp_ub_reason {
    QL_IR_INTERP_UB_NONE = 0,
    /* A UB_GUARD predicate evaluated to false. */
    QL_IR_INTERP_UB_GUARD_FAILED,
    /* A UB_GUARD predicate was itself undefined. */
    QL_IR_INTERP_UB_GUARD_UNDEFINED,
    /* Every guard passed, yet an observation read a value that a partial
       operation left undefined. The guard predicate is too weak. */
    QL_IR_INTERP_UB_GUARD_INSUFFICIENT,
    /* An explicit undefined-behaviour terminator. */
    QL_IR_INTERP_UB_TERMINATOR
} ql_ir_interp_ub_reason;

/* Bytes use the same encoding as IR constants: exact width, little-endian,
   with the last byte's unused high bits zero. A boolean is one byte, 0 or 1. */
typedef struct ql_ir_interp_input_v1 {
    size_t struct_size;
    ql_ir_value_id value;
    const void *data;
    size_t size;
    uint64_t reserved[2];
} ql_ir_interp_input_v1;

/* The memory model is a flat 64-bit address space under the ASM2C_GNU_V1
   profile, where a pointer is an address and nothing more. One object is
   described per storage region the run can touch, and the caller is
   responsible for the model's three standing constraints:

     - distinct objects occupy disjoint byte ranges;
     - every object lies strictly above the first page, so address zero is in
       no object and dereferencing null is always undefined;
     - an object's range does not wrap the address space.

   ql_ir_interp_run checks all three and refuses the run rather than
   interpreting under a layout the model does not admit.

   This model admits more programs than ISO C provenance does: comparing and
   subtracting pointers into different objects is defined here. Verdicts are
   therefore relative to the profile, which ARCHITECTURE.md states. */
typedef struct ql_ir_interp_object_v1 {
    size_t struct_size;
    uint64_t base;
    uint64_t size;
    /* Bytes at `base`, or null for an object that starts zeroed. */
    const void *initial;
    /* Optional: receives `size` bytes of the final image when the run
       finishes. Null when the caller does not observe memory. */
    void *final_image;
    uint64_t reserved[2];
} ql_ir_interp_object_v1;

typedef struct ql_ir_interp_options_v1 {
    size_t struct_size;
    uint32_t schema_version;
    uint32_t reserved_alignment;
    /* Zero selects QL_IR_INTERP_DEFAULT_STEP_LIMIT. */
    uint64_t step_limit;
    /* The object table. A module that loads or stores with no objects
       declared can only reach undefined accesses. */
    const ql_ir_interp_object_v1 *objects;
    size_t object_count;
    uint64_t reserved[3];
} ql_ir_interp_options_v1;

typedef struct ql_ir_interp_result_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_ir_interp_outcome outcome;
    ql_ir_interp_ub_reason ub_reason;
    /* Where execution stopped, or the invalid sentinels when the outcome has
       no single site. */
    ql_ir_block_id block;
    ql_ir_instruction_id instruction;
    /* Set for a RETURN with a non-void return type and for a TERMINATE that
       carries a value. */
    uint32_t has_value;
    ql_ir_type_id value_type;
    size_t value_size;
    uint8_t value[QL_IR_INTERP_VALUE_CAPACITY];
    uint64_t steps;
    uint64_t reserved[4];
} ql_ir_interp_result_v1;

/* The first address any object may occupy. Everything below it, address zero
   included, belongs to no object, which is what makes a null dereference
   undefined rather than merely out of range. */
#define QL_IR_INTERP_FIRST_OBJECT_ADDRESS UINT64_C(0x1000)

QL_API void QL_CALL ql_ir_interp_object_init(
    ql_ir_interp_object_v1 *object);
QL_API void QL_CALL ql_ir_interp_options_init(
    ql_ir_interp_options_v1 *options);
QL_API void QL_CALL ql_ir_interp_input_init(ql_ir_interp_input_v1 *input);
QL_API const char *QL_CALL ql_ir_interp_outcome_string(
    ql_ir_interp_outcome outcome);
QL_API const char *QL_CALL ql_ir_interp_ub_reason_string(
    ql_ir_interp_ub_reason reason);

/* Every parameter of the module must be bound exactly once, and each input's
   bytes must match its parameter's type exactly. A status failure means the
   run could not start; it is never a statement about the function. */
QL_API ql_status QL_CALL ql_ir_interp_run(
    const ql_allocator *allocator, const ql_ir *ir,
    const ql_ir_interp_input_v1 *inputs, size_t input_count,
    const ql_ir_interp_options_v1 *options,
    ql_ir_interp_result_v1 *result, ql_error *error);

QL_EXTERN_C_END

#endif
