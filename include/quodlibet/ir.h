#ifndef QUODLIBET_IR_H
#define QUODLIBET_IR_H

#include "quodlibet/artifact.h"
#include "quodlibet/semantics.h"

QL_EXTERN_C_BEGIN

/* An append-only, acyclic, typed SSA control-flow graph. v2 adds an
   instruction image payload, which MEMORY_IMAGE carries. */
#define QL_IR_ARTIFACT_SCHEMA_VERSION 2u

typedef uint32_t ql_ir_instruction_id;

#define QL_IR_INVALID_TYPE_ID UINT32_MAX
#define QL_IR_INVALID_VALUE_ID UINT32_MAX
#define QL_IR_INVALID_BLOCK_ID UINT32_MAX
#define QL_IR_INVALID_INSTRUCTION_ID UINT32_MAX

typedef struct ql_ir_builder ql_ir_builder;
typedef struct ql_ir ql_ir;

/* Bit-vectors are deliberately signless. Signedness is selected by opcodes
   such as SDIV and SLT; source-parameter signedness belongs to the separately
   versioned source-signature/precondition binding. */

typedef enum ql_ir_float_format {
    QL_IR_FLOAT_INVALID = 0,
    QL_IR_FLOAT_IEEE_BINARY16,
    QL_IR_FLOAT_BFLOAT16,
    QL_IR_FLOAT_IEEE_BINARY32,
    QL_IR_FLOAT_IEEE_BINARY64,
    QL_IR_FLOAT_X87_BINARY80,
    QL_IR_FLOAT_IEEE_BINARY128
} ql_ir_float_format;

typedef enum ql_ir_cfg_kind {
    QL_IR_CFG_ACYCLIC = 0
} ql_ir_cfg_kind;

typedef enum ql_ir_value_definition_kind {
    QL_IR_VALUE_PARAMETER = 1,
    QL_IR_VALUE_CONSTANT,
    QL_IR_VALUE_INSTRUCTION_RESULT
} ql_ir_value_definition_kind;

/* Built-in opcodes are stable. Values at or above EXTENSION_BASE are reserved
   for versioned consumers that agree on an external opcode vocabulary. */
typedef uint32_t ql_ir_opcode;

#define QL_IR_OPCODE_IDENTITY UINT32_C(1)
#define QL_IR_OPCODE_PHI UINT32_C(2)
#define QL_IR_OPCODE_BOOL_NOT UINT32_C(3)
#define QL_IR_OPCODE_BV_NOT UINT32_C(4)
#define QL_IR_OPCODE_BV_NEG UINT32_C(5)
#define QL_IR_OPCODE_ADD UINT32_C(6)
#define QL_IR_OPCODE_SUB UINT32_C(7)
#define QL_IR_OPCODE_MUL UINT32_C(8)
#define QL_IR_OPCODE_UDIV UINT32_C(9)
#define QL_IR_OPCODE_SDIV UINT32_C(10)
#define QL_IR_OPCODE_UREM UINT32_C(11)
#define QL_IR_OPCODE_SREM UINT32_C(12)
#define QL_IR_OPCODE_SHL UINT32_C(13)
#define QL_IR_OPCODE_LSHR UINT32_C(14)
#define QL_IR_OPCODE_ASHR UINT32_C(15)
#define QL_IR_OPCODE_BV_AND UINT32_C(16)
#define QL_IR_OPCODE_BV_OR UINT32_C(17)
#define QL_IR_OPCODE_BV_XOR UINT32_C(18)
#define QL_IR_OPCODE_EQ UINT32_C(19)
#define QL_IR_OPCODE_NE UINT32_C(20)
#define QL_IR_OPCODE_ULT UINT32_C(21)
#define QL_IR_OPCODE_ULE UINT32_C(22)
#define QL_IR_OPCODE_SLT UINT32_C(23)
#define QL_IR_OPCODE_SLE UINT32_C(24)
#define QL_IR_OPCODE_SELECT UINT32_C(25)
#define QL_IR_OPCODE_ZEXT UINT32_C(26)
#define QL_IR_OPCODE_SEXT UINT32_C(27)
#define QL_IR_OPCODE_TRUNC UINT32_C(28)
#define QL_IR_OPCODE_BITCAST UINT32_C(29)
#define QL_IR_OPCODE_PTR_TO_BV UINT32_C(30)
#define QL_IR_OPCODE_BV_TO_PTR UINT32_C(31)
#define QL_IR_OPCODE_PTR_ADD UINT32_C(32)
#define QL_IR_OPCODE_LOAD UINT32_C(33)
#define QL_IR_OPCODE_STORE UINT32_C(34)
#define QL_IR_OPCODE_CALL UINT32_C(35)
#define QL_IR_OPCODE_TRACE_APPEND UINT32_C(36)
#define QL_IR_OPCODE_ASSUME UINT32_C(37)
#define QL_IR_OPCODE_UB_GUARD UINT32_C(38)
#define QL_IR_OPCODE_FNEG UINT32_C(39)
#define QL_IR_OPCODE_FADD UINT32_C(40)
#define QL_IR_OPCODE_FSUB UINT32_C(41)
#define QL_IR_OPCODE_FMUL UINT32_C(42)
#define QL_IR_OPCODE_FDIV UINT32_C(43)
#define QL_IR_OPCODE_FREM UINT32_C(44)
#define QL_IR_OPCODE_FOEQ UINT32_C(45)
#define QL_IR_OPCODE_FONE UINT32_C(46)
#define QL_IR_OPCODE_FOLT UINT32_C(47)
#define QL_IR_OPCODE_FOLE UINT32_C(48)
#define QL_IR_OPCODE_FP_TO_SBV UINT32_C(49)
#define QL_IR_OPCODE_FP_TO_UBV UINT32_C(50)
#define QL_IR_OPCODE_SBV_TO_FP UINT32_C(51)
#define QL_IR_OPCODE_UBV_TO_FP UINT32_C(52)
#define QL_IR_OPCODE_FP_EXT UINT32_C(53)
#define QL_IR_OPCODE_FP_TRUNC UINT32_C(54)
/* Asks whether a run of bytes stands at an address in a memory state.
   Operands are the memory and the pointer; the bytes are the instruction's
   image payload. The result is a bool, so stating that the bytes are there
   is ASSUME of this, and nothing about the opcode itself decides whether it
   is a statement or a question.

   This exists so that bytes a program already states -- a string literal, an
   initialised array -- reach the IR as the constants they are, instead of as
   a store per byte with a bounds guard on each. The store chain said the same
   thing at a cost that grew with the byte count and with the object count at
   once, and it said it as a write, which it is not: nothing in the program
   performs those writes. */
#define QL_IR_OPCODE_MEMORY_IMAGE UINT32_C(55)
#define QL_IR_OPCODE_EXTENSION_BASE UINT32_C(65536)

typedef enum ql_ir_terminator_kind {
    QL_IR_TERMINATOR_RETURN = 1,
    QL_IR_TERMINATOR_BRANCH,
    QL_IR_TERMINATOR_COND_BRANCH,
    QL_IR_TERMINATOR_TRAP,
    QL_IR_TERMINATOR_UNDEFINED_BEHAVIOR,
    QL_IR_TERMINATOR_TERMINATE,
    QL_IR_TERMINATOR_DIVERGE
} ql_ir_terminator_kind;

typedef struct ql_ir_type_definition_v1 {
    size_t struct_size;
    ql_ir_type_kind kind;
    ql_ir_float_format float_format;
    uint32_t bit_width;
    uint32_t address_space;
    ql_ir_type_id element_type;
    uint64_t element_count;
    uint64_t reserved[4];
} ql_ir_type_definition_v1;

typedef struct ql_ir_instruction_definition_v1 {
    size_t struct_size;
    ql_ir_opcode opcode;
    uint32_t flags;
    uint64_t effects;
    const ql_ir_value_id *operands;
    size_t operand_count;
    const ql_ir_block_id *block_operands;
    size_t block_operand_count;
    const ql_ir_type_id *result_types;
    size_t result_count;
    uint64_t immediate;
    const char *symbol;
    size_t symbol_size;
    /* An instruction's constant byte payload. `symbol` is text and may not
       contain NUL; this may contain anything, because the bytes a program
       states about memory are bytes and not a name. MEMORY_IMAGE is what
       carries one today. */
    const void *image;
    size_t image_size;
    uint64_t reserved[2];
} ql_ir_instruction_definition_v1;

/* `condition` is used only by COND_BRANCH. RETURN uses `return_value` unless
   the function returns void. Memory and event_trace are optional terminal
   observable states. A true UB_GUARD predicate means execution is defined. */
typedef struct ql_ir_terminator_definition_v1 {
    size_t struct_size;
    ql_ir_terminator_kind kind;
    ql_ir_value_id condition;
    ql_ir_value_id return_value;
    ql_ir_value_id memory;
    ql_ir_value_id event_trace;
    ql_ir_block_id target;
    ql_ir_block_id false_target;
    uint64_t code;
    const char *reason;
    size_t reason_size;
    uint64_t reserved[4];
} ql_ir_terminator_definition_v1;

typedef struct ql_ir_view_v1 {
    size_t struct_size;
    uint32_t schema_version;
    ql_ir_cfg_kind cfg_kind;
    const char *function_name;
    size_t function_name_size;
    ql_ir_type_id return_type;
    ql_ir_block_id entry_block;
    size_t type_count;
    size_t value_count;
    size_t block_count;
    size_t instruction_count;
    ql_digest artifact_digest;
    uint64_t reserved[4];
} ql_ir_view_v1;

/* Every pointer returned through a reader view is borrowed from the immutable
   ql_ir and remains valid until its final ql_ir_release(). */

typedef struct ql_ir_type_view_v1 {
    size_t struct_size;
    ql_ir_type_id id;
    ql_ir_type_kind kind;
    ql_ir_float_format float_format;
    uint32_t bit_width;
    uint32_t address_space;
    ql_ir_type_id element_type;
    uint64_t element_count;
    uint64_t reserved[3];
} ql_ir_type_view_v1;

typedef struct ql_ir_value_view_v1 {
    size_t struct_size;
    ql_ir_value_id id;
    ql_ir_type_id type;
    ql_ir_value_definition_kind definition_kind;
    uint32_t result_index;
    ql_ir_instruction_id instruction;
    const void *constant_data;
    size_t constant_size;
    const char *name;
    size_t name_size;
    uint64_t reserved[3];
} ql_ir_value_view_v1;

typedef struct ql_ir_instruction_view_v1 {
    size_t struct_size;
    ql_ir_instruction_id id;
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
    uint64_t immediate;
    const char *symbol;
    size_t symbol_size;
    const void *image;
    size_t image_size;
    uint64_t reserved[1];
} ql_ir_instruction_view_v1;

typedef struct ql_ir_block_view_v1 {
    size_t struct_size;
    ql_ir_block_id id;
    const char *label;
    size_t label_size;
    const ql_ir_instruction_id *instructions;
    size_t instruction_count;
    ql_ir_terminator_definition_v1 terminator;
    uint64_t reserved[3];
} ql_ir_block_view_v1;

QL_API void QL_CALL ql_ir_type_definition_init(
    ql_ir_type_definition_v1 *definition, ql_ir_type_kind kind);
QL_API void QL_CALL ql_ir_instruction_definition_init(
    ql_ir_instruction_definition_v1 *definition, ql_ir_opcode opcode);
QL_API void QL_CALL ql_ir_terminator_definition_init(
    ql_ir_terminator_definition_v1 *definition,
    ql_ir_terminator_kind kind);

QL_API ql_status QL_CALL ql_ir_builder_create(
    const ql_allocator *allocator, ql_ir_builder **output, ql_error *error);
QL_API void QL_CALL ql_ir_builder_destroy(ql_ir_builder *builder);
QL_API ql_status QL_CALL ql_ir_builder_add_type(
    ql_ir_builder *builder, const ql_ir_type_definition_v1 *definition,
    ql_ir_type_id *output, ql_error *error);
QL_API ql_status QL_CALL ql_ir_builder_set_function(
    ql_ir_builder *builder, const char *name, size_t name_size,
    ql_ir_type_id return_type, ql_error *error);
QL_API ql_status QL_CALL ql_ir_builder_add_parameter(
    ql_ir_builder *builder, ql_ir_type_id type, const char *name,
    size_t name_size, ql_ir_value_id *output, ql_error *error);
/* Constants use exact-width, canonical little-endian bytes. Booleans are one
   byte (0 or 1); unused high bits of a bit-vector's last byte must be zero. */
QL_API ql_status QL_CALL ql_ir_builder_add_constant(
    ql_ir_builder *builder, ql_ir_type_id type, const void *data,
    size_t size, ql_ir_value_id *output, ql_error *error);
QL_API ql_status QL_CALL ql_ir_builder_add_block(
    ql_ir_builder *builder, const char *label, size_t label_size,
    ql_ir_block_id *output, ql_error *error);
QL_API ql_status QL_CALL ql_ir_builder_set_entry_block(
    ql_ir_builder *builder, ql_ir_block_id block, ql_error *error);
QL_API ql_status QL_CALL ql_ir_builder_append_instruction(
    ql_ir_builder *builder, ql_ir_block_id block,
    const ql_ir_instruction_definition_v1 *definition,
    ql_ir_instruction_id *instruction, ql_ir_value_id *results,
    ql_error *error);
QL_API ql_status QL_CALL ql_ir_builder_set_terminator(
    ql_ir_builder *builder, ql_ir_block_id block,
    const ql_ir_terminator_definition_v1 *definition, ql_error *error);
QL_API ql_status QL_CALL ql_ir_builder_finish(
    const ql_ir_builder *builder, ql_artifact **output, ql_error *error);

/* Opening validates the kind, schema, every table extent and reference, type
   rules, SSA dominance, reachability, and the v1 acyclic-CFG restriction. */
QL_API ql_status QL_CALL ql_ir_open(const ql_allocator *allocator,
                                    const ql_artifact *artifact,
                                    ql_ir **output, ql_error *error);
QL_API void QL_CALL ql_ir_retain(ql_ir *ir);
QL_API void QL_CALL ql_ir_release(ql_ir *ir);
QL_API ql_status QL_CALL ql_ir_get_view(const ql_ir *ir,
                                        ql_ir_view_v1 *view,
                                        ql_error *error);
QL_API ql_status QL_CALL ql_ir_type_at(const ql_ir *ir, size_t index,
                                       ql_ir_type_view_v1 *view,
                                       ql_error *error);
QL_API ql_status QL_CALL ql_ir_value_at(const ql_ir *ir, size_t index,
                                        ql_ir_value_view_v1 *view,
                                        ql_error *error);
QL_API ql_status QL_CALL ql_ir_block_at(const ql_ir *ir, size_t index,
                                        ql_ir_block_view_v1 *view,
                                        ql_error *error);
QL_API ql_status QL_CALL ql_ir_instruction_at(
    const ql_ir *ir, size_t index, ql_ir_instruction_view_v1 *view,
    ql_error *error);

QL_EXTERN_C_END

#endif
