#ifndef QUODLIBET_IR_VERIFY_H
#define QUODLIBET_IR_VERIFY_H

#include "quodlibet/ir.h"

QL_EXTERN_C_BEGIN

#define QL_IR_VERIFY_SCHEMA_VERSION 1u
#define QL_IR_VERIFY_MESSAGE_CAPACITY 256u

/* The verifier is a soundness device, so it shares no code with the builder
   and the decoder it checks. It re-derives the control-flow graph, the
   dominator tree, and every typing rule from the public reader API alone. A
   defect in one implementation therefore cannot mask the same defect in the
   other. */

typedef enum ql_ir_verify_code {
    QL_IR_VERIFY_OK = 0,
    /* Module-level metadata, entry block, or table extents are unusable. */
    QL_IR_VERIFY_MODULE,
    /* A type table entry violates the schema v1 type rules. */
    QL_IR_VERIFY_TYPE_TABLE,
    /* A value table entry violates its definition-kind rules. */
    QL_IR_VERIFY_VALUE_TABLE,
    /* An instruction, block, or terminator names an out-of-range entity. */
    QL_IR_VERIFY_REFERENCE,
    /* Block instruction lists do not partition the instruction table. */
    QL_IR_VERIFY_OWNERSHIP,
    /* The opcode is unknown, or is an extension opcode whose vocabulary the
       verifier cannot establish typing and effect rules for. */
    QL_IR_VERIFY_OPCODE,
    /* Operand, result, or block-operand counts are wrong for the opcode. */
    QL_IR_VERIFY_ARITY,
    /* Operand or result types are wrong for the opcode. */
    QL_IR_VERIFY_TYPE_RULE,
    /* Declared effect bits do not match what the opcode can perform. */
    QL_IR_VERIFY_EFFECT_RULE,
    /* A PHI does not lead its block. */
    QL_IR_VERIFY_PHI_PLACEMENT,
    /* PHI incoming blocks are not exactly the block's predecessors. */
    QL_IR_VERIFY_PHI_EDGES,
    /* A use is not dominated by its definition. */
    QL_IR_VERIFY_DOMINANCE,
    /* A block has no terminator, or the terminator's fields are invalid. */
    QL_IR_VERIFY_TERMINATOR,
    /* A block cannot be reached from the entry block. */
    QL_IR_VERIFY_UNREACHABLE,
    /* Schema v1 forbids control-flow cycles. */
    QL_IR_VERIFY_CFG_CYCLE,
    /* An observation depends on a partial operation that no dominating
       UB_GUARD separates it from. */
    QL_IR_VERIFY_UB_GUARD
} ql_ir_verify_code;

/* `block`, `instruction`, and `value` carry the failure site when they apply
   and hold the matching QL_IR_INVALID_* sentinel otherwise. */
typedef struct ql_ir_verify_report_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_ir_verify_code code;
    ql_ir_block_id block;
    ql_ir_instruction_id instruction;
    ql_ir_value_id value;
    uint32_t reserved_alignment;
    char message[QL_IR_VERIFY_MESSAGE_CAPACITY];
    uint64_t reserved[4];
} ql_ir_verify_report_v1;

QL_API void QL_CALL ql_ir_verify_report_init(ql_ir_verify_report_v1 *report);
QL_API const char *QL_CALL ql_ir_verify_code_string(ql_ir_verify_code code);

/* Returns QL_STATUS_OK exactly when the module verifies, and then leaves the
   report's code at QL_IR_VERIFY_OK. A module that fails verification returns a
   mapped status and fills the report with the precise rule and site; the
   report is the authoritative description of the failure. Status failures that
   are not about the module itself (null arguments, allocation) leave the
   report's code at QL_IR_VERIFY_OK, so callers distinguish "did not verify"
   from "could not run" by testing the report. */
QL_API ql_status QL_CALL ql_ir_verify(const ql_allocator *allocator,
                                      const ql_ir *ir,
                                      ql_ir_verify_report_v1 *report,
                                      ql_error *error);

QL_EXTERN_C_END

#endif
