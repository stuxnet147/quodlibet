#include "quodlibet/ir_verify.h"

#include "quodlibet/allocator.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Independent restatement of the IR schema v1 rules. Nothing here calls into
   the builder or the decoder; every fact is re-derived from the public reader
   views so that a defect in ir.c cannot hide behind the same defect here. */

/* Dominator sets are materialised as bit-vectors, which costs
   block_count * ceil(block_count / 64) words. The limit below keeps that
   bounded for adversarial artifacts. It is an explicit refusal, never a silent
   narrowing: a module above the limit is reported, not partially verified. */
#define VERIFY_MAX_BLOCKS 16384u

typedef struct verify_value {
    ql_ir_type_id type;
    ql_ir_value_definition_kind definition_kind;
    ql_ir_instruction_id instruction;
    uint32_t result_index;
    const void *constant_data;
    size_t constant_size;
    const char *name;
    size_t name_size;
    /* Highest linear key of a partial operation this value depends on that no
       intervening UB_GUARD has separated it from. Zero means none. */
    uint64_t pending_partial;
} verify_value;

typedef struct verify_instruction {
    ql_ir_block_id block;
    ql_ir_opcode opcode;
    uint32_t flags;
    uint64_t effects;
    const ql_ir_value_id *operands;
    size_t operand_count;
    const ql_ir_block_id *block_operands;
    size_t block_operand_count;
    const ql_ir_value_id *results;
    size_t result_count;
    size_t symbol_size;
    size_t image_size;
    size_t position;
    uint64_t key;
    uint8_t owned;
} verify_instruction;

typedef struct verify_block {
    const ql_ir_instruction_id *instructions;
    size_t instruction_count;
    ql_ir_terminator_definition_v1 terminator;
} verify_block;

typedef struct verify_context {
    ql_allocator allocator;
    const ql_ir *ir;
    ql_ir_verify_report_v1 *report;
    ql_ir_view_v1 view;
    ql_ir_type_view_v1 *types;
    verify_value *values;
    verify_instruction *instructions;
    verify_block *blocks;
    size_t *predecessor_offsets;
    ql_ir_block_id *predecessors;
    size_t *predecessor_cursor;
    ql_ir_block_id *order;
    size_t order_count;
    uint8_t *reachable;
    uint64_t *dominators;
    size_t dominator_words;
    uint64_t *block_guard;
    uint64_t *guard_at_entry;
    uint32_t *edge_stamp;
} verify_context;

/* ------------------------------------------------------------------ */
/* reporting                                                           */
/* ------------------------------------------------------------------ */

void QL_CALL ql_ir_verify_report_init(ql_ir_verify_report_v1 *report) {
    if (report == NULL) {
        return;
    }
    memset(report, 0, sizeof(*report));
    report->struct_size = sizeof(*report);
    report->schema_version = QL_IR_VERIFY_SCHEMA_VERSION;
    report->code = QL_IR_VERIFY_OK;
    report->block = QL_IR_INVALID_BLOCK_ID;
    report->instruction = QL_IR_INVALID_INSTRUCTION_ID;
    report->value = QL_IR_INVALID_VALUE_ID;
}

const char *QL_CALL ql_ir_verify_code_string(ql_ir_verify_code code) {
    switch (code) {
    case QL_IR_VERIFY_OK: return "ok";
    case QL_IR_VERIFY_MODULE: return "module";
    case QL_IR_VERIFY_TYPE_TABLE: return "type-table";
    case QL_IR_VERIFY_VALUE_TABLE: return "value-table";
    case QL_IR_VERIFY_REFERENCE: return "reference";
    case QL_IR_VERIFY_OWNERSHIP: return "ownership";
    case QL_IR_VERIFY_OPCODE: return "opcode";
    case QL_IR_VERIFY_ARITY: return "arity";
    case QL_IR_VERIFY_TYPE_RULE: return "type-rule";
    case QL_IR_VERIFY_EFFECT_RULE: return "effect-rule";
    case QL_IR_VERIFY_PHI_PLACEMENT: return "phi-placement";
    case QL_IR_VERIFY_PHI_EDGES: return "phi-edges";
    case QL_IR_VERIFY_DOMINANCE: return "dominance";
    case QL_IR_VERIFY_TERMINATOR: return "terminator";
    case QL_IR_VERIFY_UNREACHABLE: return "unreachable";
    case QL_IR_VERIFY_CFG_CYCLE: return "cfg-cycle";
    case QL_IR_VERIFY_UB_GUARD: return "ub-guard";
    default: return "unknown";
    }
}

static ql_status verify_code_status(ql_ir_verify_code code) {
    switch (code) {
    case QL_IR_VERIFY_TYPE_TABLE:
    case QL_IR_VERIFY_TYPE_RULE:
        return QL_STATUS_TYPE_MISMATCH;
    case QL_IR_VERIFY_CFG_CYCLE:
        return QL_STATUS_CYCLE;
    default:
        return QL_STATUS_INVALID_ARGUMENT;
    }
}

static ql_status verify_fail(verify_context *context, ql_ir_verify_code code,
                             ql_ir_block_id block,
                             ql_ir_instruction_id instruction,
                             ql_ir_value_id value, const char *format, ...) {
    va_list arguments;

    context->report->code = code;
    context->report->block = block;
    context->report->instruction = instruction;
    context->report->value = value;
    va_start(arguments, format);
    (void)vsnprintf(context->report->message,
                    sizeof(context->report->message), format, arguments);
    va_end(arguments);
    return verify_code_status(code);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int verify_mul_size(size_t left, size_t right, size_t *result) {
    if (left != 0u && right > (size_t)-1 / left) {
        return 0;
    }
    *result = left * right;
    return 1;
}

static void *verify_allocate(verify_context *context, size_t count,
                             size_t item_size) {
    size_t bytes;
    void *memory;

    if (count == 0u) {
        return NULL;
    }
    if (!verify_mul_size(count, item_size, &bytes)) {
        return NULL;
    }
    memory = context->allocator.allocate(context->allocator.user_data, bytes);
    if (memory != NULL) {
        memset(memory, 0, bytes);
    }
    return memory;
}

static void verify_free(verify_context *context, void *memory) {
    if (memory != NULL) {
        context->allocator.deallocate(context->allocator.user_data, memory);
    }
}

static ql_ir_type_kind verify_type_kind(const verify_context *context,
                                        ql_ir_type_id type) {
    return context->types[type].kind;
}

static ql_ir_type_id verify_value_type(const verify_context *context,
                                       ql_ir_value_id value) {
    return context->values[value].type;
}

static ql_ir_type_kind verify_value_kind(const verify_context *context,
                                         ql_ir_value_id value) {
    return verify_type_kind(context, verify_value_type(context, value));
}

static uint32_t verify_type_width(const verify_context *context,
                                  ql_ir_type_id type) {
    return context->types[type].bit_width;
}

static uint32_t verify_float_width(ql_ir_float_format format) {
    switch (format) {
    case QL_IR_FLOAT_IEEE_BINARY16:
    case QL_IR_FLOAT_BFLOAT16:
        return 16u;
    case QL_IR_FLOAT_IEEE_BINARY32:
        return 32u;
    case QL_IR_FLOAT_IEEE_BINARY64:
        return 64u;
    case QL_IR_FLOAT_X87_BINARY80:
        return 80u;
    case QL_IR_FLOAT_IEEE_BINARY128:
        return 128u;
    default:
        return 0u;
    }
}

/* Operations whose IR semantics are partial: some operand assignments leave
   the result undefined. Every observation that depends on one of these must be
   separated from it by a dominating UB_GUARD. */
/* A memory access is partial too, but its definedness is a precondition on
   the address rather than a fact about the result, so the guard stands before
   the access instead of between it and the observation. That is the only
   place it can stand: nothing can dereference first and check afterwards. */
static int verify_opcode_is_memory_access(ql_ir_opcode opcode) {
    return opcode == QL_IR_OPCODE_LOAD || opcode == QL_IR_OPCODE_STORE;
}

static int verify_opcode_is_partial(ql_ir_opcode opcode) {
    switch (opcode) {
    case QL_IR_OPCODE_UDIV:
    case QL_IR_OPCODE_SDIV:
    case QL_IR_OPCODE_UREM:
    case QL_IR_OPCODE_SREM:
    case QL_IR_OPCODE_SHL:
    case QL_IR_OPCODE_LSHR:
    case QL_IR_OPCODE_ASHR:
    case QL_IR_OPCODE_LOAD:
    case QL_IR_OPCODE_STORE:
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* bit-vector helpers for dominator sets                               */
/* ------------------------------------------------------------------ */

static uint64_t *verify_dominator_row(verify_context *context,
                                      ql_ir_block_id block) {
    return context->dominators + (size_t)block * context->dominator_words;
}

static void verify_bits_set(uint64_t *row, size_t index) {
    row[index / 64u] |= UINT64_C(1) << (index % 64u);
}

static int verify_bits_test(const uint64_t *row, size_t index) {
    return (row[index / 64u] & (UINT64_C(1) << (index % 64u))) != 0u;
}

static int verify_block_dominates(verify_context *context,
                                  ql_ir_block_id dominator,
                                  ql_ir_block_id block) {
    return verify_bits_test(verify_dominator_row(context, block), dominator);
}

/* ------------------------------------------------------------------ */
/* table loading                                                       */
/* ------------------------------------------------------------------ */

static ql_status verify_load_types(verify_context *context, ql_error *error) {
    size_t index;

    for (index = 0u; index < context->view.type_count; ++index) {
        ql_ir_type_view_v1 *type = &context->types[index];
        ql_status status;
        memset(type, 0, sizeof(*type));
        type->struct_size = sizeof(*type);
        status = ql_ir_type_at(context->ir, index, type, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

static ql_status verify_load_values(verify_context *context, ql_error *error) {
    size_t index;

    for (index = 0u; index < context->view.value_count; ++index) {
        ql_ir_value_view_v1 view;
        verify_value *value = &context->values[index];
        ql_status status;
        memset(&view, 0, sizeof(view));
        view.struct_size = sizeof(view);
        status = ql_ir_value_at(context->ir, index, &view, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        value->type = view.type;
        value->definition_kind = view.definition_kind;
        value->instruction = view.instruction;
        value->result_index = view.result_index;
        value->constant_data = view.constant_data;
        value->constant_size = view.constant_size;
        value->name = view.name;
        value->name_size = view.name_size;
        value->pending_partial = 0u;
    }
    return QL_STATUS_OK;
}

static ql_status verify_load_instructions(verify_context *context,
                                          ql_error *error) {
    size_t index;

    for (index = 0u; index < context->view.instruction_count; ++index) {
        ql_ir_instruction_view_v1 view;
        verify_instruction *instruction = &context->instructions[index];
        ql_status status;
        memset(&view, 0, sizeof(view));
        view.struct_size = sizeof(view);
        status = ql_ir_instruction_at(context->ir, index, &view, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        instruction->block = view.block;
        instruction->opcode = view.opcode;
        instruction->flags = view.flags;
        instruction->effects = view.effects;
        instruction->operands = view.operands;
        instruction->operand_count = view.operand_count;
        instruction->block_operands = view.block_operands;
        instruction->block_operand_count = view.block_operand_count;
        instruction->results = view.results;
        instruction->result_count = view.result_count;
        instruction->symbol_size = view.symbol_size;
        instruction->image_size = view.image_size;
        instruction->position = 0u;
        instruction->key = 0u;
        instruction->owned = 0u;
    }
    return QL_STATUS_OK;
}

static ql_status verify_load_blocks(verify_context *context, ql_error *error) {
    size_t index;

    for (index = 0u; index < context->view.block_count; ++index) {
        ql_ir_block_view_v1 view;
        verify_block *block = &context->blocks[index];
        ql_status status;
        memset(&view, 0, sizeof(view));
        view.struct_size = sizeof(view);
        status = ql_ir_block_at(context->ir, index, &view, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        block->instructions = view.instructions;
        block->instruction_count = view.instruction_count;
        block->terminator = view.terminator;
    }
    return QL_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* stage 1: tables                                                     */
/* ------------------------------------------------------------------ */

static ql_status verify_types_table(verify_context *context) {
    size_t index;

    for (index = 0u; index < context->view.type_count; ++index) {
        const ql_ir_type_view_v1 *type = &context->types[index];
        int scalar_fields_clear =
            type->element_type == QL_IR_INVALID_TYPE_ID &&
            type->element_count == 0u;

        if (type->id != (ql_ir_type_id)index) {
            return verify_fail(context, QL_IR_VERIFY_TYPE_TABLE,
                               QL_IR_INVALID_BLOCK_ID,
                               QL_IR_INVALID_INSTRUCTION_ID,
                               QL_IR_INVALID_VALUE_ID,
                               "type %zu reports identifier %u", index,
                               type->id);
        }
        switch (type->kind) {
        case QL_IR_TYPE_VOID:
        case QL_IR_TYPE_MEMORY:
        case QL_IR_TYPE_EVENT_TRACE:
            if (type->float_format == QL_IR_FLOAT_INVALID &&
                type->bit_width == 0u && type->address_space == 0u &&
                scalar_fields_clear) {
                continue;
            }
            break;
        case QL_IR_TYPE_BOOL:
            if (type->float_format == QL_IR_FLOAT_INVALID &&
                type->bit_width == 1u && type->address_space == 0u &&
                scalar_fields_clear) {
                continue;
            }
            break;
        case QL_IR_TYPE_BIT_VECTOR:
            if (type->float_format == QL_IR_FLOAT_INVALID &&
                type->bit_width != 0u && type->address_space == 0u &&
                scalar_fields_clear) {
                continue;
            }
            break;
        case QL_IR_TYPE_FLOAT:
            if (type->bit_width != 0u &&
                verify_float_width(type->float_format) == type->bit_width &&
                type->address_space == 0u && scalar_fields_clear) {
                continue;
            }
            break;
        case QL_IR_TYPE_POINTER:
            /* The element type must already be defined, which keeps the type
               table acyclic without a separate traversal. */
            if (type->float_format == QL_IR_FLOAT_INVALID &&
                type->bit_width != 0u && type->element_count == 0u &&
                type->element_type < index &&
                context->types[type->element_type].kind !=
                    QL_IR_TYPE_MEMORY &&
                context->types[type->element_type].kind !=
                    QL_IR_TYPE_EVENT_TRACE) {
                continue;
            }
            break;
        case QL_IR_TYPE_ARRAY:
        case QL_IR_TYPE_TUPLE:
        default:
            return verify_fail(context, QL_IR_VERIFY_TYPE_TABLE,
                               QL_IR_INVALID_BLOCK_ID,
                               QL_IR_INVALID_INSTRUCTION_ID,
                               QL_IR_INVALID_VALUE_ID,
                               "type %zu uses kind %u, which schema v1 does "
                               "not define", index, (unsigned)type->kind);
        }
        return verify_fail(context, QL_IR_VERIFY_TYPE_TABLE,
                           QL_IR_INVALID_BLOCK_ID,
                           QL_IR_INVALID_INSTRUCTION_ID,
                           QL_IR_INVALID_VALUE_ID,
                           "type %zu has fields its kind %u forbids", index,
                           (unsigned)type->kind);
    }
    return QL_STATUS_OK;
}

static size_t verify_constant_size(const verify_context *context,
                                   ql_ir_type_id type) {
    uint32_t width = verify_type_kind(context, type) == QL_IR_TYPE_BOOL
                         ? 1u
                         : verify_type_width(context, type);
    return (size_t)(width / 8u) + (width % 8u == 0u ? 0u : 1u);
}

static ql_status verify_constant_bytes(verify_context *context,
                                       ql_ir_value_id id) {
    const verify_value *value = &context->values[id];
    const uint8_t *bytes = (const uint8_t *)value->constant_data;
    ql_ir_type_kind kind = verify_value_kind(context, id);
    size_t expected;
    uint32_t width;
    uint32_t unused_bits;

    if (kind != QL_IR_TYPE_BOOL && kind != QL_IR_TYPE_BIT_VECTOR &&
        kind != QL_IR_TYPE_FLOAT && kind != QL_IR_TYPE_POINTER) {
        return verify_fail(context, QL_IR_VERIFY_VALUE_TABLE,
                           QL_IR_INVALID_BLOCK_ID,
                           QL_IR_INVALID_INSTRUCTION_ID, id,
                           "type kind %u cannot carry a constant",
                           (unsigned)kind);
    }
    expected = verify_constant_size(context, value->type);
    if (bytes == NULL || value->constant_size != expected) {
        return verify_fail(context, QL_IR_VERIFY_VALUE_TABLE,
                           QL_IR_INVALID_BLOCK_ID,
                           QL_IR_INVALID_INSTRUCTION_ID, id,
                           "constant needs exactly %zu bytes but carries %zu",
                           expected, value->constant_size);
    }
    if (kind == QL_IR_TYPE_BOOL && bytes[0] > 1u) {
        return verify_fail(context, QL_IR_VERIFY_VALUE_TABLE,
                           QL_IR_INVALID_BLOCK_ID,
                           QL_IR_INVALID_INSTRUCTION_ID, id,
                           "boolean constant byte is %u, not zero or one",
                           (unsigned)bytes[0]);
    }
    width = kind == QL_IR_TYPE_BOOL ? 1u
                                    : verify_type_width(context, value->type);
    unused_bits = (uint32_t)(expected * 8u) - width;
    if (unused_bits != 0u &&
        (bytes[expected - 1u] >> (8u - unused_bits)) != 0u) {
        return verify_fail(context, QL_IR_VERIFY_VALUE_TABLE,
                           QL_IR_INVALID_BLOCK_ID,
                           QL_IR_INVALID_INSTRUCTION_ID, id,
                           "constant sets bits above its %u-bit width", width);
    }
    if (kind == QL_IR_TYPE_POINTER) {
        size_t index;
        for (index = 0u; index < value->constant_size; ++index) {
            if (bytes[index] != 0u) {
                return verify_fail(context, QL_IR_VERIFY_VALUE_TABLE,
                                   QL_IR_INVALID_BLOCK_ID,
                                   QL_IR_INVALID_INSTRUCTION_ID, id,
                                   "schema v1 admits only the null pointer "
                                   "constant");
            }
        }
    }
    return QL_STATUS_OK;
}

static ql_status verify_values_table(verify_context *context) {
    size_t index;

    for (index = 0u; index < context->view.value_count; ++index) {
        verify_value *value = &context->values[index];
        ql_ir_type_kind kind;
        ql_status status;

        if (value->type >= context->view.type_count) {
            return verify_fail(context, QL_IR_VERIFY_REFERENCE,
                               QL_IR_INVALID_BLOCK_ID,
                               QL_IR_INVALID_INSTRUCTION_ID,
                               (ql_ir_value_id)index,
                               "value names missing type %u", value->type);
        }
        kind = verify_type_kind(context, value->type);
        if (kind == QL_IR_TYPE_VOID) {
            return verify_fail(context, QL_IR_VERIFY_VALUE_TABLE,
                               QL_IR_INVALID_BLOCK_ID,
                               QL_IR_INVALID_INSTRUCTION_ID,
                               (ql_ir_value_id)index,
                               "a value cannot have void type");
        }
        switch (value->definition_kind) {
        case QL_IR_VALUE_PARAMETER:
            if (value->constant_data != NULL || value->constant_size != 0u ||
                value->instruction != QL_IR_INVALID_INSTRUCTION_ID) {
                return verify_fail(context, QL_IR_VERIFY_VALUE_TABLE,
                                   QL_IR_INVALID_BLOCK_ID,
                                   QL_IR_INVALID_INSTRUCTION_ID,
                                   (ql_ir_value_id)index,
                                   "parameter carries constant or "
                                   "instruction fields");
            }
            break;
        case QL_IR_VALUE_CONSTANT:
            if (value->name != NULL || value->name_size != 0u ||
                value->instruction != QL_IR_INVALID_INSTRUCTION_ID) {
                return verify_fail(context, QL_IR_VERIFY_VALUE_TABLE,
                                   QL_IR_INVALID_BLOCK_ID,
                                   QL_IR_INVALID_INSTRUCTION_ID,
                                   (ql_ir_value_id)index,
                                   "constant carries name or instruction "
                                   "fields");
            }
            status = verify_constant_bytes(context, (ql_ir_value_id)index);
            if (status != QL_STATUS_OK) {
                return status;
            }
            break;
        case QL_IR_VALUE_INSTRUCTION_RESULT:
            if (value->name != NULL || value->name_size != 0u ||
                value->constant_data != NULL || value->constant_size != 0u) {
                return verify_fail(context, QL_IR_VERIFY_VALUE_TABLE,
                                   QL_IR_INVALID_BLOCK_ID,
                                   QL_IR_INVALID_INSTRUCTION_ID,
                                   (ql_ir_value_id)index,
                                   "instruction result carries name or "
                                   "constant fields");
            }
            if (value->instruction >= context->view.instruction_count) {
                return verify_fail(context, QL_IR_VERIFY_REFERENCE,
                                   QL_IR_INVALID_BLOCK_ID,
                                   QL_IR_INVALID_INSTRUCTION_ID,
                                   (ql_ir_value_id)index,
                                   "result names missing instruction %u",
                                   value->instruction);
            }
            break;
        default:
            return verify_fail(context, QL_IR_VERIFY_VALUE_TABLE,
                               QL_IR_INVALID_BLOCK_ID,
                               QL_IR_INVALID_INSTRUCTION_ID,
                               (ql_ir_value_id)index,
                               "unknown value definition kind %u",
                               (unsigned)value->definition_kind);
        }
    }
    return QL_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* stage 2: block ownership and references                             */
/* ------------------------------------------------------------------ */

static ql_status verify_instruction_references(verify_context *context,
                                               ql_ir_instruction_id id) {
    const verify_instruction *instruction = &context->instructions[id];
    size_t index;

    if (instruction->block >= context->view.block_count) {
        return verify_fail(context, QL_IR_VERIFY_REFERENCE,
                           QL_IR_INVALID_BLOCK_ID, id,
                           QL_IR_INVALID_VALUE_ID,
                           "instruction names missing block %u",
                           instruction->block);
    }
    for (index = 0u; index < instruction->operand_count; ++index) {
        if (instruction->operands[index] >= context->view.value_count) {
            return verify_fail(context, QL_IR_VERIFY_REFERENCE,
                               instruction->block, id,
                               instruction->operands[index],
                               "operand %zu names a missing value", index);
        }
    }
    for (index = 0u; index < instruction->block_operand_count; ++index) {
        if (instruction->block_operands[index] >= context->view.block_count) {
            return verify_fail(context, QL_IR_VERIFY_REFERENCE,
                               instruction->block, id,
                               QL_IR_INVALID_VALUE_ID,
                               "block operand %zu names a missing block",
                               index);
        }
    }
    for (index = 0u; index < instruction->result_count; ++index) {
        ql_ir_value_id result = instruction->results[index];
        const verify_value *value;
        if (result >= context->view.value_count) {
            return verify_fail(context, QL_IR_VERIFY_REFERENCE,
                               instruction->block, id, result,
                               "result %zu names a missing value", index);
        }
        value = &context->values[result];
        if (value->definition_kind != QL_IR_VALUE_INSTRUCTION_RESULT ||
            value->instruction != id ||
            value->result_index != (uint32_t)index) {
            return verify_fail(context, QL_IR_VERIFY_VALUE_TABLE,
                               instruction->block, id, result,
                               "result %zu does not point back at this "
                               "instruction", index);
        }
    }
    return QL_STATUS_OK;
}

static ql_status verify_block_ownership(verify_context *context) {
    size_t block_index;
    size_t index;

    for (block_index = 0u; block_index < context->view.block_count;
         ++block_index) {
        const verify_block *block = &context->blocks[block_index];
        int saw_non_phi = 0;
        for (index = 0u; index < block->instruction_count; ++index) {
            ql_ir_instruction_id id = block->instructions[index];
            verify_instruction *instruction;
            if (id >= context->view.instruction_count) {
                return verify_fail(context, QL_IR_VERIFY_REFERENCE,
                                   (ql_ir_block_id)block_index,
                                   QL_IR_INVALID_INSTRUCTION_ID,
                                   QL_IR_INVALID_VALUE_ID,
                                   "block slot %zu names a missing "
                                   "instruction", index);
            }
            instruction = &context->instructions[id];
            if (instruction->owned != 0u ||
                instruction->block != (ql_ir_block_id)block_index) {
                return verify_fail(context, QL_IR_VERIFY_OWNERSHIP,
                                   (ql_ir_block_id)block_index, id,
                                   QL_IR_INVALID_VALUE_ID,
                                   "instruction is listed by a block that "
                                   "does not own it, or is listed twice");
            }
            instruction->owned = 1u;
            instruction->position = index;
            if (instruction->opcode == QL_IR_OPCODE_PHI) {
                if (saw_non_phi != 0) {
                    return verify_fail(context, QL_IR_VERIFY_PHI_PLACEMENT,
                                       (ql_ir_block_id)block_index, id,
                                       QL_IR_INVALID_VALUE_ID,
                                       "a PHI follows a non-PHI instruction");
                }
            } else {
                saw_non_phi = 1;
            }
        }
    }
    for (index = 0u; index < context->view.instruction_count; ++index) {
        if (context->instructions[index].owned == 0u) {
            return verify_fail(context, QL_IR_VERIFY_OWNERSHIP,
                               QL_IR_INVALID_BLOCK_ID,
                               (ql_ir_instruction_id)index,
                               QL_IR_INVALID_VALUE_ID,
                               "instruction belongs to no block list");
        }
    }
    return QL_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* stage 3: terminators and the control-flow graph                     */
/* ------------------------------------------------------------------ */

static size_t verify_successor_count(const ql_ir_terminator_definition_v1 *t) {
    if (t->kind == QL_IR_TERMINATOR_BRANCH) {
        return 1u;
    }
    if (t->kind == QL_IR_TERMINATOR_COND_BRANCH) {
        return 2u;
    }
    return 0u;
}

static ql_ir_block_id verify_successor_at(
    const ql_ir_terminator_definition_v1 *t, size_t index) {
    return index == 0u ? t->target : t->false_target;
}

static ql_status verify_state_value(verify_context *context,
                                    ql_ir_block_id block,
                                    ql_ir_value_id value,
                                    ql_ir_type_kind required,
                                    const char *name) {
    if (value == QL_IR_INVALID_VALUE_ID) {
        return QL_STATUS_OK;
    }
    if (value >= context->view.value_count) {
        return verify_fail(context, QL_IR_VERIFY_REFERENCE, block,
                           QL_IR_INVALID_INSTRUCTION_ID, value,
                           "terminator %s names a missing value", name);
    }
    if (verify_value_kind(context, value) != required) {
        return verify_fail(context, QL_IR_VERIFY_TYPE_RULE, block,
                           QL_IR_INVALID_INSTRUCTION_ID, value,
                           "terminator %s has the wrong type kind", name);
    }
    return QL_STATUS_OK;
}

static ql_status verify_terminator(verify_context *context,
                                   ql_ir_block_id block) {
    const ql_ir_terminator_definition_v1 *t =
        &context->blocks[block].terminator;
    ql_ir_type_kind return_kind;
    ql_status status;

    status = verify_state_value(context, block, t->memory, QL_IR_TYPE_MEMORY,
                                "memory");
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = verify_state_value(context, block, t->event_trace,
                                QL_IR_TYPE_EVENT_TRACE, "event trace");
    if (status != QL_STATUS_OK) {
        return status;
    }
    switch (t->kind) {
    case QL_IR_TERMINATOR_RETURN:
        if (t->condition != QL_IR_INVALID_VALUE_ID ||
            t->target != QL_IR_INVALID_BLOCK_ID ||
            t->false_target != QL_IR_INVALID_BLOCK_ID) {
            break;
        }
        return_kind = verify_type_kind(context, context->view.return_type);
        if (return_kind == QL_IR_TYPE_VOID) {
            if (t->return_value == QL_IR_INVALID_VALUE_ID) {
                return QL_STATUS_OK;
            }
            break;
        }
        if (t->return_value < context->view.value_count &&
            verify_value_type(context, t->return_value) ==
                context->view.return_type) {
            return QL_STATUS_OK;
        }
        break;
    case QL_IR_TERMINATOR_BRANCH:
        if (t->target < context->view.block_count &&
            t->false_target == QL_IR_INVALID_BLOCK_ID &&
            t->condition == QL_IR_INVALID_VALUE_ID &&
            t->return_value == QL_IR_INVALID_VALUE_ID &&
            t->memory == QL_IR_INVALID_VALUE_ID &&
            t->event_trace == QL_IR_INVALID_VALUE_ID) {
            return QL_STATUS_OK;
        }
        break;
    case QL_IR_TERMINATOR_COND_BRANCH:
        if (t->condition < context->view.value_count &&
            verify_value_kind(context, t->condition) == QL_IR_TYPE_BOOL &&
            t->target < context->view.block_count &&
            t->false_target < context->view.block_count &&
            t->target != t->false_target &&
            t->return_value == QL_IR_INVALID_VALUE_ID &&
            t->memory == QL_IR_INVALID_VALUE_ID &&
            t->event_trace == QL_IR_INVALID_VALUE_ID) {
            return QL_STATUS_OK;
        }
        break;
    case QL_IR_TERMINATOR_TRAP:
    case QL_IR_TERMINATOR_UNDEFINED_BEHAVIOR:
    case QL_IR_TERMINATOR_DIVERGE:
        if (t->condition == QL_IR_INVALID_VALUE_ID &&
            t->return_value == QL_IR_INVALID_VALUE_ID &&
            t->target == QL_IR_INVALID_BLOCK_ID &&
            t->false_target == QL_IR_INVALID_BLOCK_ID) {
            return QL_STATUS_OK;
        }
        break;
    case QL_IR_TERMINATOR_TERMINATE:
        if (t->condition == QL_IR_INVALID_VALUE_ID &&
            t->target == QL_IR_INVALID_BLOCK_ID &&
            t->false_target == QL_IR_INVALID_BLOCK_ID &&
            (t->return_value == QL_IR_INVALID_VALUE_ID ||
             (t->return_value < context->view.value_count &&
              (verify_value_kind(context, t->return_value) ==
                   QL_IR_TYPE_BOOL ||
               verify_value_kind(context, t->return_value) ==
                   QL_IR_TYPE_BIT_VECTOR)))) {
            return QL_STATUS_OK;
        }
        break;
    default:
        return verify_fail(context, QL_IR_VERIFY_TERMINATOR, block,
                           QL_IR_INVALID_INSTRUCTION_ID,
                           QL_IR_INVALID_VALUE_ID,
                           "unknown terminator kind %u", (unsigned)t->kind);
    }
    return verify_fail(context, QL_IR_VERIFY_TERMINATOR, block,
                       QL_IR_INVALID_INSTRUCTION_ID, QL_IR_INVALID_VALUE_ID,
                       "terminator kind %u carries fields it forbids",
                       (unsigned)t->kind);
}

static ql_status verify_build_cfg(verify_context *context) {
    size_t block_index;
    size_t index;
    size_t cursor = 0u;
    size_t queue_begin;
    size_t queue_end;
    size_t edge_count = 0u;
    ql_status status;

    for (block_index = 0u; block_index < context->view.block_count;
         ++block_index) {
        status = verify_terminator(context, (ql_ir_block_id)block_index);
        if (status != QL_STATUS_OK) {
            return status;
        }
        edge_count += verify_successor_count(
            &context->blocks[block_index].terminator);
    }

    context->predecessor_offsets = (size_t *)verify_allocate(
        context, context->view.block_count + 1u, sizeof(size_t));
    context->predecessor_cursor = (size_t *)verify_allocate(
        context, context->view.block_count, sizeof(size_t));
    context->predecessors = (ql_ir_block_id *)verify_allocate(
        context, edge_count, sizeof(ql_ir_block_id));
    context->order = (ql_ir_block_id *)verify_allocate(
        context, context->view.block_count, sizeof(ql_ir_block_id));
    context->reachable = (uint8_t *)verify_allocate(
        context, context->view.block_count, sizeof(uint8_t));
    if (context->predecessor_offsets == NULL ||
        context->predecessor_cursor == NULL || context->order == NULL ||
        context->reachable == NULL ||
        (edge_count != 0u && context->predecessors == NULL)) {
        return QL_STATUS_OUT_OF_MEMORY;
    }

    for (block_index = 0u; block_index < context->view.block_count;
         ++block_index) {
        const ql_ir_terminator_definition_v1 *t =
            &context->blocks[block_index].terminator;
        size_t successors = verify_successor_count(t);
        for (index = 0u; index < successors; ++index) {
            ++context->predecessor_cursor[verify_successor_at(t, index)];
        }
    }
    for (block_index = 0u; block_index < context->view.block_count;
         ++block_index) {
        context->predecessor_offsets[block_index] = cursor;
        cursor += context->predecessor_cursor[block_index];
        context->predecessor_cursor[block_index] = 0u;
    }
    context->predecessor_offsets[context->view.block_count] = cursor;
    for (block_index = 0u; block_index < context->view.block_count;
         ++block_index) {
        const ql_ir_terminator_definition_v1 *t =
            &context->blocks[block_index].terminator;
        size_t successors = verify_successor_count(t);
        for (index = 0u; index < successors; ++index) {
            ql_ir_block_id successor = verify_successor_at(t, index);
            size_t slot = context->predecessor_offsets[successor] +
                          context->predecessor_cursor[successor]++;
            context->predecessors[slot] = (ql_ir_block_id)block_index;
        }
    }

    queue_begin = 0u;
    queue_end = 0u;
    context->order[queue_end++] = context->view.entry_block;
    context->reachable[context->view.entry_block] = 1u;
    while (queue_begin < queue_end) {
        ql_ir_block_id block = context->order[queue_begin++];
        const ql_ir_terminator_definition_v1 *t =
            &context->blocks[block].terminator;
        size_t successors = verify_successor_count(t);
        for (index = 0u; index < successors; ++index) {
            ql_ir_block_id successor = verify_successor_at(t, index);
            if (context->reachable[successor] == 0u) {
                context->reachable[successor] = 1u;
                context->order[queue_end++] = successor;
            }
        }
    }
    for (block_index = 0u; block_index < context->view.block_count;
         ++block_index) {
        if (context->reachable[block_index] == 0u) {
            return verify_fail(context, QL_IR_VERIFY_UNREACHABLE,
                               (ql_ir_block_id)block_index,
                               QL_IR_INVALID_INSTRUCTION_ID,
                               QL_IR_INVALID_VALUE_ID,
                               "block cannot be reached from the entry block");
        }
    }
    return QL_STATUS_OK;
}

/* Kahn ordering doubles as the schema v1 acyclicity check. */
static ql_status verify_topological_order(verify_context *context) {
    size_t block_index;
    size_t index;
    size_t queue_begin = 0u;
    size_t queue_end = 0u;
    size_t *indegree;

    indegree = (size_t *)verify_allocate(context, context->view.block_count,
                                         sizeof(size_t));
    if (indegree == NULL) {
        return QL_STATUS_OUT_OF_MEMORY;
    }
    for (block_index = 0u; block_index < context->view.block_count;
         ++block_index) {
        indegree[block_index] =
            context->predecessor_offsets[block_index + 1u] -
            context->predecessor_offsets[block_index];
        if (indegree[block_index] == 0u) {
            context->order[queue_end++] = (ql_ir_block_id)block_index;
        }
    }
    while (queue_begin < queue_end) {
        ql_ir_block_id block = context->order[queue_begin++];
        const ql_ir_terminator_definition_v1 *t =
            &context->blocks[block].terminator;
        size_t successors = verify_successor_count(t);
        for (index = 0u; index < successors; ++index) {
            ql_ir_block_id successor = verify_successor_at(t, index);
            if (--indegree[successor] == 0u) {
                context->order[queue_end++] = successor;
            }
        }
    }
    verify_free(context, indegree);
    context->order_count = queue_end;
    if (queue_end != context->view.block_count) {
        return verify_fail(context, QL_IR_VERIFY_CFG_CYCLE,
                           QL_IR_INVALID_BLOCK_ID,
                           QL_IR_INVALID_INSTRUCTION_ID,
                           QL_IR_INVALID_VALUE_ID,
                           "schema v1 forbids control-flow cycles, but %zu of "
                           "%zu blocks could not be ordered",
                           context->view.block_count - queue_end,
                           context->view.block_count);
    }
    if (context->order[0] != context->view.entry_block) {
        return verify_fail(context, QL_IR_VERIFY_MODULE,
                           context->order[0],
                           QL_IR_INVALID_INSTRUCTION_ID,
                           QL_IR_INVALID_VALUE_ID,
                           "a block other than the entry block has no "
                           "predecessor");
    }
    return QL_STATUS_OK;
}

/* Dominator sets by forward intersection over the topological order. The CFG
   is acyclic in schema v1, so one pass reaches the fixed point. */
static ql_status verify_dominators(verify_context *context) {
    size_t index;
    size_t word;

    context->dominator_words = (context->view.block_count + 63u) / 64u;
    context->dominators = (uint64_t *)verify_allocate(
        context, context->view.block_count * context->dominator_words,
        sizeof(uint64_t));
    if (context->dominators == NULL) {
        return QL_STATUS_OUT_OF_MEMORY;
    }
    for (index = 0u; index < context->order_count; ++index) {
        ql_ir_block_id block = context->order[index];
        uint64_t *row = verify_dominator_row(context, block);
        size_t begin = context->predecessor_offsets[block];
        size_t end = context->predecessor_offsets[block + 1u];
        size_t predecessor_index;

        if (begin == end) {
            verify_bits_set(row, block);
            continue;
        }
        memcpy(row,
               verify_dominator_row(context, context->predecessors[begin]),
               context->dominator_words * sizeof(uint64_t));
        for (predecessor_index = begin + 1u; predecessor_index < end;
             ++predecessor_index) {
            const uint64_t *other = verify_dominator_row(
                context, context->predecessors[predecessor_index]);
            for (word = 0u; word < context->dominator_words; ++word) {
                row[word] &= other[word];
            }
        }
        verify_bits_set(row, block);
    }
    return QL_STATUS_OK;
}

/* The key orders instructions along any single dominator path: blocks on such
   a path have strictly increasing dominator-set sizes. */
static uint64_t verify_instruction_key(verify_context *context,
                                       ql_ir_instruction_id id) {
    const verify_instruction *instruction = &context->instructions[id];
    const uint64_t *row = verify_dominator_row(context, instruction->block);
    uint64_t depth = 0u;
    size_t word;

    for (word = 0u; word < context->dominator_words; ++word) {
        uint64_t bits = row[word];
        while (bits != 0u) {
            bits &= bits - 1u;
            ++depth;
        }
    }
    return (depth << 32u) | (uint64_t)(instruction->position + 1u);
}

/* ------------------------------------------------------------------ */
/* stage 4: instruction typing, effects, and dominance                 */
/* ------------------------------------------------------------------ */

static int verify_same_operand_type(const verify_context *context,
                                    const verify_instruction *instruction,
                                    ql_ir_type_id type) {
    size_t index;
    for (index = 0u; index < instruction->operand_count; ++index) {
        if (verify_value_type(context, instruction->operands[index]) != type) {
            return 0;
        }
    }
    return 1;
}

static ql_status verify_effects(verify_context *context,
                                ql_ir_instruction_id id) {
    const verify_instruction *instruction = &context->instructions[id];
    const uint64_t known = QL_IR_EFFECT_MEMORY | QL_IR_EFFECT_CALL |
                           QL_IR_EFFECT_VOLATILE | QL_IR_EFFECT_ATOMIC |
                           QL_IR_EFFECT_IO |
                           QL_IR_EFFECT_UNDEFINED_BEHAVIOR;
    const uint64_t observable = QL_IR_EFFECT_VOLATILE | QL_IR_EFFECT_ATOMIC |
                                QL_IR_EFFECT_IO;
    uint64_t effects = instruction->effects;
    uint64_t required = 0u;
    uint64_t allowed = 0u;

    if ((effects & ~known) != 0u) {
        return verify_fail(context, QL_IR_VERIFY_EFFECT_RULE,
                           instruction->block, id, QL_IR_INVALID_VALUE_ID,
                           "instruction declares effect bits outside the "
                           "schema v1 vocabulary");
    }
    switch (instruction->opcode) {
    case QL_IR_OPCODE_LOAD:
        required = QL_IR_EFFECT_MEMORY;
        allowed = QL_IR_EFFECT_MEMORY | observable;
        break;
    case QL_IR_OPCODE_STORE:
        required = QL_IR_EFFECT_MEMORY;
        allowed = QL_IR_EFFECT_MEMORY | observable;
        break;
    case QL_IR_OPCODE_CALL:
        required = QL_IR_EFFECT_CALL;
        allowed = QL_IR_EFFECT_CALL | QL_IR_EFFECT_MEMORY | observable |
                  QL_IR_EFFECT_UNDEFINED_BEHAVIOR;
        break;
    case QL_IR_OPCODE_TRACE_APPEND:
        required = 0u;
        allowed = QL_IR_EFFECT_CALL | QL_IR_EFFECT_MEMORY | observable;
        if (effects == 0u) {
            return verify_fail(context, QL_IR_VERIFY_EFFECT_RULE,
                               instruction->block, id,
                               QL_IR_INVALID_VALUE_ID,
                               "an event-trace append with no effect bit "
                               "records nothing observable");
        }
        break;
    case QL_IR_OPCODE_UB_GUARD:
        required = QL_IR_EFFECT_UNDEFINED_BEHAVIOR;
        allowed = QL_IR_EFFECT_UNDEFINED_BEHAVIOR;
        break;
    default:
        required = 0u;
        allowed = 0u;
        break;
    }
    if ((effects & required) != required || (effects & ~allowed) != 0u) {
        return verify_fail(context, QL_IR_VERIFY_EFFECT_RULE,
                           instruction->block, id, QL_IR_INVALID_VALUE_ID,
                           "opcode %u cannot declare effects 0x%llx",
                           instruction->opcode,
                           (unsigned long long)effects);
    }
    return QL_STATUS_OK;
}

static ql_status verify_shape(verify_context *context,
                              ql_ir_instruction_id id) {
    const verify_instruction *instruction = &context->instructions[id];
    ql_ir_block_id block = instruction->block;
    ql_ir_type_id first_type = QL_IR_INVALID_TYPE_ID;
    ql_ir_type_id result_type = QL_IR_INVALID_TYPE_ID;
    ql_ir_type_kind first_kind = QL_IR_TYPE_VOID;
    ql_ir_type_kind result_kind = QL_IR_TYPE_VOID;

    if (instruction->opcode >= QL_IR_OPCODE_EXTENSION_BASE) {
        return verify_fail(context, QL_IR_VERIFY_OPCODE, block, id,
                           QL_IR_INVALID_VALUE_ID,
                           "extension opcode %u has no vocabulary this "
                           "verifier can establish rules for",
                           instruction->opcode);
    }
    if (instruction->opcode == 0u ||
        instruction->opcode > QL_IR_OPCODE_MEMORY_IMAGE) {
        return verify_fail(context, QL_IR_VERIFY_OPCODE, block, id,
                           QL_IR_INVALID_VALUE_ID, "unknown opcode %u",
                           instruction->opcode);
    }
    if (instruction->flags != 0u) {
        return verify_fail(context, QL_IR_VERIFY_OPCODE, block, id,
                           QL_IR_INVALID_VALUE_ID,
                           "built-in opcode %u defines no flags",
                           instruction->opcode);
    }
    if (instruction->opcode != QL_IR_OPCODE_PHI &&
        instruction->block_operand_count != 0u) {
        return verify_fail(context, QL_IR_VERIFY_ARITY, block, id,
                           QL_IR_INVALID_VALUE_ID,
                           "only PHI accepts block operands");
    }
    if (instruction->operand_count != 0u) {
        first_type = verify_value_type(context, instruction->operands[0]);
        first_kind = verify_type_kind(context, first_type);
    }
    if (instruction->result_count != 0u) {
        result_type = verify_value_type(context, instruction->results[0]);
        result_kind = verify_type_kind(context, result_type);
    }

#define VERIFY_ARITY(operands_, results_)                                     \
    do {                                                                      \
        if (instruction->operand_count != (size_t)(operands_) ||              \
            instruction->result_count != (size_t)(results_)) {                \
            return verify_fail(context, QL_IR_VERIFY_ARITY, block, id,        \
                               QL_IR_INVALID_VALUE_ID,                        \
                               "opcode %u needs %d operands and %d results",  \
                               instruction->opcode, (int)(operands_),         \
                               (int)(results_));                              \
        }                                                                     \
    } while (0)
#define VERIFY_TYPES(condition_, rule_)                                       \
    do {                                                                      \
        if (!(condition_)) {                                                  \
            return verify_fail(context, QL_IR_VERIFY_TYPE_RULE, block, id,    \
                               QL_IR_INVALID_VALUE_ID,                        \
                               "instruction violates the %s type rule",       \
                               (rule_));                                      \
        }                                                                     \
    } while (0)

    switch (instruction->opcode) {
    case QL_IR_OPCODE_IDENTITY:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_type == result_type, "identity");
        break;
    case QL_IR_OPCODE_PHI:
        if (instruction->operand_count == 0u ||
            instruction->result_count != 1u ||
            instruction->block_operand_count != instruction->operand_count) {
            return verify_fail(context, QL_IR_VERIFY_ARITY, block, id,
                               QL_IR_INVALID_VALUE_ID,
                               "a PHI needs one result and matching operand "
                               "and block-operand counts");
        }
        VERIFY_TYPES(verify_same_operand_type(context, instruction,
                                              result_type), "PHI");
        break;
    case QL_IR_OPCODE_BOOL_NOT:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_BOOL && first_type ==
                         result_type, "boolean negation");
        break;
    case QL_IR_OPCODE_BV_NOT:
    case QL_IR_OPCODE_BV_NEG:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_BIT_VECTOR &&
                         first_type == result_type, "bit-vector unary");
        break;
    case QL_IR_OPCODE_ADD:
    case QL_IR_OPCODE_SUB:
    case QL_IR_OPCODE_MUL:
    case QL_IR_OPCODE_UDIV:
    case QL_IR_OPCODE_SDIV:
    case QL_IR_OPCODE_UREM:
    case QL_IR_OPCODE_SREM:
    case QL_IR_OPCODE_SHL:
    case QL_IR_OPCODE_LSHR:
    case QL_IR_OPCODE_ASHR:
    case QL_IR_OPCODE_BV_AND:
    case QL_IR_OPCODE_BV_OR:
    case QL_IR_OPCODE_BV_XOR:
        VERIFY_ARITY(2, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_BIT_VECTOR &&
                         verify_same_operand_type(context, instruction,
                                                  first_type) &&
                         result_type == first_type, "bit-vector binary");
        break;
    case QL_IR_OPCODE_EQ:
    case QL_IR_OPCODE_NE:
        VERIFY_ARITY(2, 1);
        VERIFY_TYPES(verify_same_operand_type(context, instruction,
                                              first_type) &&
                         result_kind == QL_IR_TYPE_BOOL &&
                         first_kind != QL_IR_TYPE_VOID &&
                         first_kind != QL_IR_TYPE_MEMORY &&
                         first_kind != QL_IR_TYPE_EVENT_TRACE, "equality");
        break;
    case QL_IR_OPCODE_ULT:
    case QL_IR_OPCODE_ULE:
    case QL_IR_OPCODE_SLT:
    case QL_IR_OPCODE_SLE:
        VERIFY_ARITY(2, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_BIT_VECTOR &&
                         verify_same_operand_type(context, instruction,
                                                  first_type) &&
                         result_kind == QL_IR_TYPE_BOOL,
                     "integer comparison");
        break;
    case QL_IR_OPCODE_SELECT:
        VERIFY_ARITY(3, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_BOOL &&
                         verify_value_type(context,
                                           instruction->operands[1]) ==
                             result_type &&
                         verify_value_type(context,
                                           instruction->operands[2]) ==
                             result_type, "select");
        break;
    case QL_IR_OPCODE_ZEXT:
    case QL_IR_OPCODE_SEXT:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_BIT_VECTOR &&
                         result_kind == QL_IR_TYPE_BIT_VECTOR &&
                         verify_type_width(context, result_type) >
                             verify_type_width(context, first_type),
                     "integer extension");
        break;
    case QL_IR_OPCODE_TRUNC:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_BIT_VECTOR &&
                         result_kind == QL_IR_TYPE_BIT_VECTOR &&
                         verify_type_width(context, result_type) <
                             verify_type_width(context, first_type),
                     "integer truncation");
        break;
    case QL_IR_OPCODE_BITCAST:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_type != result_type &&
                         (first_kind == QL_IR_TYPE_BIT_VECTOR ||
                          first_kind == QL_IR_TYPE_FLOAT) &&
                         (result_kind == QL_IR_TYPE_BIT_VECTOR ||
                          result_kind == QL_IR_TYPE_FLOAT) &&
                         verify_type_width(context, first_type) ==
                             verify_type_width(context, result_type),
                     "bitcast");
        break;
    case QL_IR_OPCODE_PTR_TO_BV:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_POINTER &&
                         result_kind == QL_IR_TYPE_BIT_VECTOR &&
                         verify_type_width(context, first_type) ==
                             verify_type_width(context, result_type),
                     "pointer to bit-vector");
        break;
    case QL_IR_OPCODE_BV_TO_PTR:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_BIT_VECTOR &&
                         result_kind == QL_IR_TYPE_POINTER &&
                         verify_type_width(context, first_type) ==
                             verify_type_width(context, result_type),
                     "bit-vector to pointer");
        break;
    case QL_IR_OPCODE_PTR_ADD:
        VERIFY_ARITY(2, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_POINTER &&
                         verify_value_kind(context,
                                           instruction->operands[1]) ==
                             QL_IR_TYPE_BIT_VECTOR &&
                         result_type == first_type, "pointer addition");
        break;
    case QL_IR_OPCODE_LOAD:
        VERIFY_ARITY(2, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_MEMORY &&
                         verify_value_kind(context,
                                           instruction->operands[1]) ==
                             QL_IR_TYPE_POINTER &&
                         context->types[verify_value_type(
                                            context,
                                            instruction->operands[1])]
                                 .element_type == result_type, "load");
        break;
    case QL_IR_OPCODE_STORE:
        VERIFY_ARITY(3, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_MEMORY &&
                         verify_value_kind(context,
                                           instruction->operands[1]) ==
                             QL_IR_TYPE_POINTER &&
                         context->types[verify_value_type(
                                            context,
                                            instruction->operands[1])]
                                 .element_type ==
                             verify_value_type(context,
                                               instruction->operands[2]) &&
                         result_type == first_type, "store");
        break;
    case QL_IR_OPCODE_CALL: {
        /* A call threads the observable states it was handed. Producing a
           memory or event-trace result it never consumed would be conjuring
           a state from nothing, and everything downstream would read it as
           the continuation of a history that never happened. */
        size_t consumed_memory = 0u;
        size_t consumed_trace = 0u;
        size_t produced_memory = 0u;
        size_t produced_trace = 0u;
        size_t slot;

        if (instruction->symbol_size == 0u) {
            return verify_fail(context, QL_IR_VERIFY_TYPE_RULE, block, id,
                               QL_IR_INVALID_VALUE_ID,
                               "a call must name its callee symbol");
        }
        for (slot = 0u; slot < instruction->operand_count; ++slot) {
            ql_ir_type_kind kind =
                verify_value_kind(context, instruction->operands[slot]);
            if (kind == QL_IR_TYPE_MEMORY) {
                ++consumed_memory;
            } else if (kind == QL_IR_TYPE_EVENT_TRACE) {
                ++consumed_trace;
            }
        }
        for (slot = 0u; slot < instruction->result_count; ++slot) {
            ql_ir_type_kind kind =
                verify_value_kind(context, instruction->results[slot]);
            if (kind == QL_IR_TYPE_MEMORY) {
                ++produced_memory;
            } else if (kind == QL_IR_TYPE_EVENT_TRACE) {
                ++produced_trace;
            }
        }
        if (produced_memory > consumed_memory ||
            produced_trace > consumed_trace) {
            return verify_fail(context, QL_IR_VERIFY_TYPE_RULE, block, id,
                               QL_IR_INVALID_VALUE_ID,
                               "a call produces an observable state it never "
                               "consumed");
        }
        if (produced_memory != 0u &&
            (instruction->effects & QL_IR_EFFECT_MEMORY) == 0u) {
            return verify_fail(context, QL_IR_VERIFY_EFFECT_RULE, block, id,
                               QL_IR_INVALID_VALUE_ID,
                               "a call that hands on a memory state must "
                               "declare the memory effect");
        }
        break;
    }
    case QL_IR_OPCODE_TRACE_APPEND:
        if (instruction->operand_count == 0u ||
            instruction->result_count != 1u) {
            return verify_fail(context, QL_IR_VERIFY_ARITY, block, id,
                               QL_IR_INVALID_VALUE_ID,
                               "an event-trace append needs the incoming "
                               "trace and one result");
        }
        VERIFY_TYPES(first_kind == QL_IR_TYPE_EVENT_TRACE &&
                         result_type == first_type, "event-trace append");
        break;
    case QL_IR_OPCODE_ASSUME:
        VERIFY_ARITY(1, 0);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_BOOL, "assume");
        break;
    case QL_IR_OPCODE_MEMORY_IMAGE:
        VERIFY_ARITY(2, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_MEMORY &&
                         verify_value_kind(context, instruction->operands[1]) ==
                             QL_IR_TYPE_POINTER &&
                         result_kind == QL_IR_TYPE_BOOL,
                     "memory image");
        if (instruction->image_size == 0u) {
            return verify_fail(context, QL_IR_VERIFY_TYPE_RULE, block, id,
                               QL_IR_INVALID_VALUE_ID,
                               "a memory image states no bytes");
        }
        if (instruction->effects != 0u) {
            /* An image asks a question about a memory value. If it declared
               an effect it would be an access, and the rule below would then
               demand a guard for something that accesses nothing. */
            return verify_fail(context, QL_IR_VERIFY_EFFECT_RULE, block, id,
                               QL_IR_INVALID_VALUE_ID,
                               "a memory image declares an effect though it "
                               "performs no access");
        }
        break;
    case QL_IR_OPCODE_UB_GUARD:
        VERIFY_ARITY(1, 0);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_BOOL, "UB guard");
        break;
    case QL_IR_OPCODE_FNEG:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_FLOAT && first_type ==
                         result_type, "floating unary");
        break;
    case QL_IR_OPCODE_FADD:
    case QL_IR_OPCODE_FSUB:
    case QL_IR_OPCODE_FMUL:
    case QL_IR_OPCODE_FDIV:
    case QL_IR_OPCODE_FREM:
        VERIFY_ARITY(2, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_FLOAT &&
                         verify_same_operand_type(context, instruction,
                                                  first_type) &&
                         result_type == first_type, "floating binary");
        break;
    case QL_IR_OPCODE_FOEQ:
    case QL_IR_OPCODE_FONE:
    case QL_IR_OPCODE_FOLT:
    case QL_IR_OPCODE_FOLE:
        VERIFY_ARITY(2, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_FLOAT &&
                         verify_same_operand_type(context, instruction,
                                                  first_type) &&
                         result_kind == QL_IR_TYPE_BOOL,
                     "floating comparison");
        break;
    case QL_IR_OPCODE_FP_TO_SBV:
    case QL_IR_OPCODE_FP_TO_UBV:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_FLOAT &&
                         result_kind == QL_IR_TYPE_BIT_VECTOR,
                     "float to integer");
        break;
    case QL_IR_OPCODE_SBV_TO_FP:
    case QL_IR_OPCODE_UBV_TO_FP:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_BIT_VECTOR &&
                         result_kind == QL_IR_TYPE_FLOAT,
                     "integer to float");
        break;
    case QL_IR_OPCODE_FP_EXT:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_FLOAT &&
                         result_kind == QL_IR_TYPE_FLOAT &&
                         verify_type_width(context, result_type) >
                             verify_type_width(context, first_type),
                     "float extension");
        break;
    case QL_IR_OPCODE_FP_TRUNC:
        VERIFY_ARITY(1, 1);
        VERIFY_TYPES(first_kind == QL_IR_TYPE_FLOAT &&
                         result_kind == QL_IR_TYPE_FLOAT &&
                         verify_type_width(context, result_type) <
                             verify_type_width(context, first_type),
                     "float truncation");
        break;
    default:
        return verify_fail(context, QL_IR_VERIFY_OPCODE, block, id,
                           QL_IR_INVALID_VALUE_ID, "unhandled opcode %u",
                           instruction->opcode);
    }

#undef VERIFY_TYPES
#undef VERIFY_ARITY
    return QL_STATUS_OK;
}

/* A definition reaches a use when it is a parameter or a constant, when it
   sits earlier in the same block, or when its block strictly dominates the
   using block. */
static int verify_reaches(verify_context *context, ql_ir_value_id value,
                          ql_ir_block_id block, size_t position) {
    const verify_value *entry = &context->values[value];
    const verify_instruction *definition;

    if (entry->definition_kind != QL_IR_VALUE_INSTRUCTION_RESULT) {
        return 1;
    }
    definition = &context->instructions[entry->instruction];
    if (definition->block == block) {
        return definition->position < position;
    }
    return verify_block_dominates(context, definition->block, block);
}

static ql_status verify_phi_edges(verify_context *context,
                                  ql_ir_instruction_id id) {
    const verify_instruction *instruction = &context->instructions[id];
    ql_ir_block_id block = instruction->block;
    size_t begin = context->predecessor_offsets[block];
    size_t end = context->predecessor_offsets[block + 1u];
    uint32_t stamp = id + 1u;
    size_t index;

    if (instruction->block_operand_count != end - begin) {
        return verify_fail(context, QL_IR_VERIFY_PHI_EDGES, block, id,
                           QL_IR_INVALID_VALUE_ID,
                           "PHI carries %zu incoming edges but the block has "
                           "%zu predecessors",
                           instruction->block_operand_count, end - begin);
    }
    for (index = begin; index < end; ++index) {
        context->edge_stamp[context->predecessors[index]] = stamp;
    }
    for (index = 0u; index < instruction->block_operand_count; ++index) {
        ql_ir_block_id incoming = instruction->block_operands[index];
        if (context->edge_stamp[incoming] != stamp) {
            return verify_fail(context, QL_IR_VERIFY_PHI_EDGES, block, id,
                               QL_IR_INVALID_VALUE_ID,
                               "PHI edge %zu names block %u, which is not a "
                               "distinct predecessor", index, incoming);
        }
        /* Consuming the stamp rejects a repeated predecessor. */
        context->edge_stamp[incoming] = 0u;
        if (!verify_reaches(context, instruction->operands[index], incoming,
                            context->blocks[incoming].instruction_count)) {
            return verify_fail(context, QL_IR_VERIFY_DOMINANCE, block, id,
                               instruction->operands[index],
                               "PHI incoming value does not reach the end of "
                               "predecessor %u", incoming);
        }
    }
    return QL_STATUS_OK;
}

static ql_status verify_dominance(verify_context *context,
                                  ql_ir_instruction_id id) {
    const verify_instruction *instruction = &context->instructions[id];
    size_t index;

    if (instruction->opcode == QL_IR_OPCODE_PHI) {
        return verify_phi_edges(context, id);
    }
    for (index = 0u; index < instruction->operand_count; ++index) {
        if (!verify_reaches(context, instruction->operands[index],
                            instruction->block, instruction->position)) {
            return verify_fail(context, QL_IR_VERIFY_DOMINANCE,
                               instruction->block, id,
                               instruction->operands[index],
                               "operand %zu is not defined on every path to "
                               "this instruction", index);
        }
    }
    return QL_STATUS_OK;
}

static ql_status verify_terminator_dominance(verify_context *context,
                                             ql_ir_block_id block) {
    const ql_ir_terminator_definition_v1 *t =
        &context->blocks[block].terminator;
    ql_ir_value_id values[4];
    size_t index;

    values[0] = t->condition;
    values[1] = t->return_value;
    values[2] = t->memory;
    values[3] = t->event_trace;
    for (index = 0u; index < 4u; ++index) {
        if (values[index] == QL_IR_INVALID_VALUE_ID) {
            continue;
        }
        if (!verify_reaches(context, values[index], block,
                            context->blocks[block].instruction_count)) {
            return verify_fail(context, QL_IR_VERIFY_DOMINANCE, block,
                               QL_IR_INVALID_INSTRUCTION_ID, values[index],
                               "terminator value is not defined on every path "
                               "to this block");
        }
    }
    return QL_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* stage 5: undefined-behaviour guard obligations                      */
/* ------------------------------------------------------------------ */

/* The verifier establishes placement, not sufficiency: it proves that no
   observation can depend on a partial operation without a UB_GUARD standing
   between them on every path. Whether a guard's predicate is the right
   predicate is settled concretely by the IR interpreter, which knows each
   opcode's partiality independently and reports a guard that admits an
   execution the opcode leaves undefined.

   A guard's own operand is deliberately not an observation. Definedness
   predicates legitimately read partial results: the signed left-shift lowering
   compares a wide SHL against its re-extension precisely to decide whether the
   narrow shift is defined. */

static uint64_t verify_guard_before(verify_context *context,
                                    ql_ir_block_id block, size_t position) {
    uint64_t best = context->guard_at_entry[block];
    const verify_block *entry = &context->blocks[block];
    size_t index;

    for (index = 0u; index < entry->instruction_count && index < position;
         ++index) {
        const verify_instruction *instruction =
            &context->instructions[entry->instructions[index]];
        if (instruction->opcode == QL_IR_OPCODE_UB_GUARD &&
            instruction->key > best) {
            best = instruction->key;
        }
    }
    return best;
}

static ql_status verify_observation(verify_context *context,
                                    ql_ir_block_id block, size_t position,
                                    ql_ir_instruction_id instruction,
                                    ql_ir_value_id value, const char *what) {
    uint64_t pending;

    if (value == QL_IR_INVALID_VALUE_ID) {
        return QL_STATUS_OK;
    }
    pending = context->values[value].pending_partial;
    if (pending == 0u) {
        return QL_STATUS_OK;
    }
    if (verify_guard_before(context, block, position) > pending) {
        return QL_STATUS_OK;
    }
    return verify_fail(context, QL_IR_VERIFY_UB_GUARD, block, instruction,
                       value,
                       "%s depends on a partial operation that no dominating "
                       "UB_GUARD separates it from", what);
}

static ql_status verify_ub_obligations(verify_context *context) {
    size_t order_index;
    size_t index;
    ql_status status;

    context->block_guard = (uint64_t *)verify_allocate(
        context, context->view.block_count, sizeof(uint64_t));
    context->guard_at_entry = (uint64_t *)verify_allocate(
        context, context->view.block_count, sizeof(uint64_t));
    if (context->block_guard == NULL || context->guard_at_entry == NULL) {
        return QL_STATUS_OUT_OF_MEMORY;
    }
    for (order_index = 0u; order_index < context->view.block_count;
         ++order_index) {
        const verify_block *block = &context->blocks[order_index];
        uint64_t best = 0u;
        for (index = 0u; index < block->instruction_count; ++index) {
            const verify_instruction *instruction =
                &context->instructions[block->instructions[index]];
            if (instruction->opcode == QL_IR_OPCODE_UB_GUARD &&
                instruction->key > best) {
                best = instruction->key;
            }
        }
        context->block_guard[order_index] = best;
    }
    for (order_index = 0u; order_index < context->order_count; ++order_index) {
        ql_ir_block_id block = context->order[order_index];
        const uint64_t *row = verify_dominator_row(context, block);
        uint64_t best = 0u;
        size_t candidate;
        for (candidate = 0u; candidate < context->view.block_count;
             ++candidate) {
            if (candidate == (size_t)block ||
                !verify_bits_test(row, candidate)) {
                continue;
            }
            if (context->block_guard[candidate] > best) {
                best = context->block_guard[candidate];
            }
        }
        context->guard_at_entry[block] = best;
    }

    for (order_index = 0u; order_index < context->order_count; ++order_index) {
        ql_ir_block_id block = context->order[order_index];
        const verify_block *entry = &context->blocks[block];
        const ql_ir_terminator_definition_v1 *t = &entry->terminator;

        for (index = 0u; index < entry->instruction_count; ++index) {
            ql_ir_instruction_id id = entry->instructions[index];
            const verify_instruction *instruction = &context->instructions[id];
            uint64_t pending = 0u;
            size_t operand;

            if (instruction->opcode == QL_IR_OPCODE_LOAD ||
                instruction->opcode == QL_IR_OPCODE_STORE ||
                instruction->opcode == QL_IR_OPCODE_CALL ||
                instruction->opcode == QL_IR_OPCODE_TRACE_APPEND) {
                for (operand = 0u; operand < instruction->operand_count;
                     ++operand) {
                    status = verify_observation(
                        context, block, index, id,
                        instruction->operands[operand],
                        "an effectful operand");
                    if (status != QL_STATUS_OK) {
                        return status;
                    }
                }
                if (verify_opcode_is_memory_access(instruction->opcode) &&
                    verify_guard_before(context, block, index) == 0u) {
                    return verify_fail(
                        context, QL_IR_VERIFY_UB_GUARD, block, id,
                        QL_IR_INVALID_VALUE_ID,
                        "a memory access has no guard standing before it to "
                        "establish that it is in bounds and aligned");
                }
            } else if (instruction->opcode == QL_IR_OPCODE_PHI) {
                /* Each incoming obligation must already be discharged at the
                   end of the predecessor it arrives from, because no single
                   guard in the join block dominates the branches. */
                for (operand = 0u; operand < instruction->operand_count;
                     ++operand) {
                    ql_ir_block_id incoming =
                        instruction->block_operands[operand];
                    status = verify_observation(
                        context, incoming,
                        context->blocks[incoming].instruction_count, id,
                        instruction->operands[operand],
                        "a PHI incoming value");
                    if (status != QL_STATUS_OK) {
                        return status;
                    }
                }
            } else if (instruction->opcode != QL_IR_OPCODE_UB_GUARD) {
                for (operand = 0u; operand < instruction->operand_count;
                     ++operand) {
                    uint64_t operand_pending =
                        context->values[instruction->operands[operand]]
                            .pending_partial;
                    if (operand_pending > pending) {
                        pending = operand_pending;
                    }
                }
            }
            if (verify_opcode_is_memory_access(instruction->opcode)) {
                /* The guard checked above discharges the access, so nothing
                   downstream inherits an obligation from it. */
                pending = 0u;
            } else if (verify_opcode_is_partial(instruction->opcode) &&
                       instruction->key > pending) {
                pending = instruction->key;
            }
            for (operand = 0u; operand < instruction->result_count;
                 ++operand) {
                context->values[instruction->results[operand]]
                    .pending_partial = pending;
            }
        }

        status = verify_observation(context, block, entry->instruction_count,
                                    QL_IR_INVALID_INSTRUCTION_ID,
                                    t->condition, "a branch condition");
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = verify_observation(context, block, entry->instruction_count,
                                    QL_IR_INVALID_INSTRUCTION_ID,
                                    t->return_value, "a returned value");
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = verify_observation(context, block, entry->instruction_count,
                                    QL_IR_INVALID_INSTRUCTION_ID, t->memory,
                                    "an observed memory state");
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = verify_observation(context, block, entry->instruction_count,
                                    QL_IR_INVALID_INSTRUCTION_ID,
                                    t->event_trace, "an observed event trace");
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return QL_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* driver                                                              */
/* ------------------------------------------------------------------ */

static ql_status verify_module(verify_context *context, ql_error *error) {
    size_t index;
    ql_status status;

    memset(&context->view, 0, sizeof(context->view));
    context->view.struct_size = sizeof(context->view);
    status = ql_ir_get_view(context->ir, &context->view, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (context->view.schema_version != QL_IR_ARTIFACT_SCHEMA_VERSION) {
        return verify_fail(context, QL_IR_VERIFY_MODULE,
                           QL_IR_INVALID_BLOCK_ID,
                           QL_IR_INVALID_INSTRUCTION_ID,
                           QL_IR_INVALID_VALUE_ID,
                           "module reports schema version %u",
                           context->view.schema_version);
    }
    if (context->view.cfg_kind != QL_IR_CFG_ACYCLIC) {
        return verify_fail(context, QL_IR_VERIFY_MODULE,
                           QL_IR_INVALID_BLOCK_ID,
                           QL_IR_INVALID_INSTRUCTION_ID,
                           QL_IR_INVALID_VALUE_ID,
                           "module reports an unknown CFG kind %u",
                           (unsigned)context->view.cfg_kind);
    }
    if (context->view.function_name == NULL ||
        context->view.function_name_size == 0u ||
        memchr(context->view.function_name, '\0',
               context->view.function_name_size) != NULL) {
        return verify_fail(context, QL_IR_VERIFY_MODULE,
                           QL_IR_INVALID_BLOCK_ID,
                           QL_IR_INVALID_INSTRUCTION_ID,
                           QL_IR_INVALID_VALUE_ID,
                           "module has no usable function name");
    }
    if (context->view.block_count == 0u ||
        context->view.type_count == 0u ||
        context->view.entry_block >= context->view.block_count ||
        context->view.return_type >= context->view.type_count) {
        return verify_fail(context, QL_IR_VERIFY_MODULE,
                           QL_IR_INVALID_BLOCK_ID,
                           QL_IR_INVALID_INSTRUCTION_ID,
                           QL_IR_INVALID_VALUE_ID,
                           "module metadata names a missing entry block or "
                           "return type");
    }
    if (context->view.block_count > VERIFY_MAX_BLOCKS) {
        return verify_fail(context, QL_IR_VERIFY_MODULE,
                           QL_IR_INVALID_BLOCK_ID,
                           QL_IR_INVALID_INSTRUCTION_ID,
                           QL_IR_INVALID_VALUE_ID,
                           "module has %zu blocks, above this verifier's "
                           "limit of %u; it is refused, not partly verified",
                           context->view.block_count, VERIFY_MAX_BLOCKS);
    }

    context->types = (ql_ir_type_view_v1 *)verify_allocate(
        context, context->view.type_count, sizeof(ql_ir_type_view_v1));
    context->values = (verify_value *)verify_allocate(
        context, context->view.value_count, sizeof(verify_value));
    context->instructions = (verify_instruction *)verify_allocate(
        context, context->view.instruction_count, sizeof(verify_instruction));
    context->blocks = (verify_block *)verify_allocate(
        context, context->view.block_count, sizeof(verify_block));
    context->edge_stamp = (uint32_t *)verify_allocate(
        context, context->view.block_count, sizeof(uint32_t));
    if (context->types == NULL || context->blocks == NULL ||
        context->edge_stamp == NULL ||
        (context->view.value_count != 0u && context->values == NULL) ||
        (context->view.instruction_count != 0u &&
         context->instructions == NULL)) {
        return QL_STATUS_OUT_OF_MEMORY;
    }

    status = verify_load_types(context, error);
    if (status == QL_STATUS_OK) {
        status = verify_load_values(context, error);
    }
    if (status == QL_STATUS_OK) {
        status = verify_load_instructions(context, error);
    }
    if (status == QL_STATUS_OK) {
        status = verify_load_blocks(context, error);
    }
    if (status != QL_STATUS_OK) {
        return status;
    }

    status = verify_types_table(context);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = verify_values_table(context);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < context->view.instruction_count; ++index) {
        status = verify_instruction_references(
            context, (ql_ir_instruction_id)index);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    status = verify_block_ownership(context);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = verify_build_cfg(context);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = verify_topological_order(context);
    if (status != QL_STATUS_OK) {
        return status;
    }
    status = verify_dominators(context);
    if (status != QL_STATUS_OK) {
        return status;
    }
    for (index = 0u; index < context->view.instruction_count; ++index) {
        context->instructions[index].key =
            verify_instruction_key(context, (ql_ir_instruction_id)index);
    }
    for (index = 0u; index < context->view.instruction_count; ++index) {
        status = verify_shape(context, (ql_ir_instruction_id)index);
        if (status == QL_STATUS_OK) {
            status = verify_effects(context, (ql_ir_instruction_id)index);
        }
        if (status == QL_STATUS_OK) {
            status = verify_dominance(context, (ql_ir_instruction_id)index);
        }
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    for (index = 0u; index < context->view.block_count; ++index) {
        status = verify_terminator_dominance(context, (ql_ir_block_id)index);
        if (status != QL_STATUS_OK) {
            return status;
        }
    }
    return verify_ub_obligations(context);
}

static void verify_context_dispose(verify_context *context) {
    verify_free(context, context->guard_at_entry);
    verify_free(context, context->block_guard);
    verify_free(context, context->dominators);
    verify_free(context, context->reachable);
    verify_free(context, context->order);
    verify_free(context, context->predecessors);
    verify_free(context, context->predecessor_cursor);
    verify_free(context, context->predecessor_offsets);
    verify_free(context, context->edge_stamp);
    verify_free(context, context->blocks);
    verify_free(context, context->instructions);
    verify_free(context, context->values);
    verify_free(context, context->types);
    memset(context, 0, sizeof(*context));
}

ql_status QL_CALL ql_ir_verify(const ql_allocator *allocator,
                               const ql_ir *ir,
                               ql_ir_verify_report_v1 *report,
                               ql_error *error) {
    const ql_allocator *selected = allocator;
    verify_context context;
    ql_status status;

    if (ir == NULL || report == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "IR module and verification report are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (report->struct_size != 0u && report->struct_size < sizeof(*report)) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "IR verification report structure is too small");
        return QL_STATUS_ABI_MISMATCH;
    }
    ql_ir_verify_report_init(report);
    if (selected == NULL) {
        selected = ql_default_allocator();
    }
    if (!ql_allocator_is_valid(selected)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT, "invalid allocator");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memset(&context, 0, sizeof(context));
    context.allocator = *selected;
    context.ir = ir;
    context.report = report;
    status = verify_module(&context, error);
    verify_context_dispose(&context);
    if (status == QL_STATUS_OUT_OF_MEMORY) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, NULL);
        return status;
    }
    if (status != QL_STATUS_OK) {
        if (report->code != QL_IR_VERIFY_OK) {
            ql_error_set(error, status, "IR verification failed (%s): %s",
                         ql_ir_verify_code_string(report->code),
                         report->message);
        }
        return status;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}
