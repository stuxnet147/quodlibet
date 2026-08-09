#include "quodlibet/ir.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace {

ql_ir_type_id add_type(ql_ir_builder *builder, ql_ir_type_kind kind,
                       uint32_t width = 0u,
                       ql_ir_type_id element = QL_IR_INVALID_TYPE_ID,
                       ql_ir_float_format format = QL_IR_FLOAT_INVALID) {
    ql_ir_type_definition_v1 definition{};
    ql_ir_type_id id = QL_IR_INVALID_TYPE_ID;
    ql_error error{};
    ql_ir_type_definition_init(&definition, kind);
    definition.bit_width = width;
    definition.element_type = element;
    definition.float_format = format;
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_type(builder, &definition, &id, &error))
        << error.message;
    return id;
}

ql_ir_block_id add_block(ql_ir_builder *builder, const char *label) {
    ql_ir_block_id id = QL_IR_INVALID_BLOCK_ID;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_block(builder, label, std::strlen(label),
                                      &id, &error))
        << error.message;
    return id;
}

void set_branch(ql_ir_builder *builder, ql_ir_block_id block,
                ql_ir_block_id target) {
    ql_ir_terminator_definition_v1 terminator{};
    ql_error error{};
    ql_ir_terminator_definition_init(&terminator, QL_IR_TERMINATOR_BRANCH);
    terminator.target = target;
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_terminator(builder, block, &terminator,
                                           &error))
        << error.message;
}

TEST(Ir, TypedArtifactRoundTripsWithStableDigestAndImmutableViews) {
    ql_ir_builder *builder = nullptr;
    ql_artifact *first = nullptr;
    ql_artifact *second = nullptr;
    ql_ir *ir = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_create(nullptr, &builder, &error));

    const ql_ir_type_id void_type = add_type(builder, QL_IR_TYPE_VOID);
    const ql_ir_type_id bool_type = add_type(builder, QL_IR_TYPE_BOOL, 1u);
    const ql_ir_type_id bv32 = add_type(builder, QL_IR_TYPE_BIT_VECTOR, 32u);
    const ql_ir_type_id f32 =
        add_type(builder, QL_IR_TYPE_FLOAT, 32u, QL_IR_INVALID_TYPE_ID,
                 QL_IR_FLOAT_IEEE_BINARY32);
    const ql_ir_type_id pointer =
        add_type(builder, QL_IR_TYPE_POINTER, 64u, bv32);
    const ql_ir_type_id memory = add_type(builder, QL_IR_TYPE_MEMORY);
    const ql_ir_type_id trace = add_type(builder, QL_IR_TYPE_EVENT_TRACE);
    ASSERT_NE(QL_IR_INVALID_TYPE_ID, void_type);
    ASSERT_NE(QL_IR_INVALID_TYPE_ID, bool_type);
    ASSERT_NE(QL_IR_INVALID_TYPE_ID, f32);
    ASSERT_NE(QL_IR_INVALID_TYPE_ID, pointer);

    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(builder, "increment", 9u, bv32,
                                         &error));
    ql_ir_value_id x = QL_IR_INVALID_VALUE_ID;
    ql_ir_value_id initial_memory = QL_IR_INVALID_VALUE_ID;
    ql_ir_value_id initial_trace = QL_IR_INVALID_VALUE_ID;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_parameter(builder, bv32, "x", 1u, &x,
                                          &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_parameter(builder, memory, "memory", 6u,
                                          &initial_memory, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_parameter(builder, trace, "events", 6u,
                                          &initial_trace, &error));
    const uint8_t one_bytes[4] = {1u, 0u, 0u, 0u};
    ql_ir_value_id one = QL_IR_INVALID_VALUE_ID;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_constant(builder, bv32, one_bytes,
                                         sizeof(one_bytes), &one, &error));
    const ql_ir_block_id entry = add_block(builder, "entry");
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(builder, entry, &error));

    const ql_ir_value_id add_operands[2] = {x, one};
    const ql_ir_type_id add_results[1] = {bv32};
    ql_ir_instruction_definition_v1 add{};
    ql_ir_instruction_id add_id = QL_IR_INVALID_INSTRUCTION_ID;
    ql_ir_value_id sum = QL_IR_INVALID_VALUE_ID;
    ql_ir_instruction_definition_init(&add, QL_IR_OPCODE_ADD);
    add.operands = add_operands;
    add.operand_count = 2u;
    add.result_types = add_results;
    add.result_count = 1u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_append_instruction(builder, entry, &add, &add_id,
                                               &sum, &error))
        << error.message;

    const ql_ir_value_id trace_operands[2] = {initial_trace, sum};
    const ql_ir_type_id trace_results[1] = {trace};
    ql_ir_instruction_definition_v1 trace_append{};
    ql_ir_instruction_id trace_id = QL_IR_INVALID_INSTRUCTION_ID;
    ql_ir_value_id final_trace = QL_IR_INVALID_VALUE_ID;
    ql_ir_instruction_definition_init(&trace_append,
                                      QL_IR_OPCODE_TRACE_APPEND);
    trace_append.effects = QL_IR_EFFECT_IO;
    trace_append.operands = trace_operands;
    trace_append.operand_count = 2u;
    trace_append.result_types = trace_results;
    trace_append.result_count = 1u;
    trace_append.symbol = "return";
    trace_append.symbol_size = 6u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_append_instruction(builder, entry, &trace_append,
                                               &trace_id, &final_trace,
                                               &error))
        << error.message;

    ql_ir_terminator_definition_v1 terminator{};
    ql_ir_terminator_definition_init(&terminator, QL_IR_TERMINATOR_RETURN);
    terminator.return_value = sum;
    terminator.memory = initial_memory;
    terminator.event_trace = final_trace;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_terminator(builder, entry, &terminator,
                                           &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_finish(builder, &first, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_finish(builder, &second, &error))
        << error.message;

    ql_artifact_view first_artifact{};
    ql_artifact_view second_artifact{};
    first_artifact.struct_size = sizeof(first_artifact);
    second_artifact.struct_size = sizeof(second_artifact);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(first, &first_artifact, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(second, &second_artifact, &error));
    EXPECT_TRUE(ql_digest_equal(&first_artifact.digest,
                                &second_artifact.digest));
    ASSERT_EQ(first_artifact.size, second_artifact.size);
    EXPECT_EQ(0, std::memcmp(first_artifact.data, second_artifact.data,
                             first_artifact.size));

    ASSERT_EQ(QL_STATUS_OK, ql_ir_open(nullptr, first, &ir, &error))
        << error.message;
    ql_artifact_release(first);
    first = nullptr;
    ql_ir_view_v1 module_view{};
    module_view.struct_size = sizeof(module_view);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_get_view(ir, &module_view, &error));
    EXPECT_EQ(7u, module_view.type_count);
    EXPECT_EQ(6u, module_view.value_count);
    EXPECT_EQ(1u, module_view.block_count);
    EXPECT_EQ(2u, module_view.instruction_count);
    EXPECT_EQ(entry, module_view.entry_block);
    EXPECT_EQ(0, std::memcmp("increment", module_view.function_name,
                             module_view.function_name_size));
    EXPECT_TRUE(ql_digest_equal(&module_view.artifact_digest,
                                &first_artifact.digest));

    ql_ir_type_view_v1 pointer_view{};
    pointer_view.struct_size = sizeof(pointer_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_type_at(ir, pointer, &pointer_view, &error));
    EXPECT_EQ(QL_IR_TYPE_POINTER, pointer_view.kind);
    EXPECT_EQ(bv32, pointer_view.element_type);
    ql_ir_value_view_v1 sum_view{};
    sum_view.struct_size = sizeof(sum_view);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_value_at(ir, sum, &sum_view, &error));
    EXPECT_EQ(QL_IR_VALUE_INSTRUCTION_RESULT, sum_view.definition_kind);
    EXPECT_EQ(add_id, sum_view.instruction);
    ql_ir_block_view_v1 block_view{};
    block_view.struct_size = sizeof(block_view);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_block_at(ir, entry, &block_view, &error));
    EXPECT_EQ(QL_IR_TERMINATOR_RETURN, block_view.terminator.kind);
    EXPECT_EQ(final_trace, block_view.terminator.event_trace);

    ql_ir_release(ir);
    ql_artifact_release(second);
    ql_ir_builder_destroy(builder);
}

TEST(Ir, DiamondCfgUsesTypedPhiIncomingEdges) {
    ql_ir_builder *builder = nullptr;
    ql_artifact *artifact = nullptr;
    ql_ir *ir = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_create(nullptr, &builder, &error));
    const ql_ir_type_id bool_type = add_type(builder, QL_IR_TYPE_BOOL, 1u);
    const ql_ir_type_id bv8 = add_type(builder, QL_IR_TYPE_BIT_VECTOR, 8u);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(builder, "choose", 6u, bv8,
                                         &error));
    ql_ir_value_id condition = QL_IR_INVALID_VALUE_ID;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_parameter(builder, bool_type, "condition",
                                          9u, &condition, &error));
    const uint8_t one_byte = 1u;
    const uint8_t two_byte = 2u;
    ql_ir_value_id one = QL_IR_INVALID_VALUE_ID;
    ql_ir_value_id two = QL_IR_INVALID_VALUE_ID;
    ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_add_constant(
                                builder, bv8, &one_byte, 1u, &one, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_add_constant(
                                builder, bv8, &two_byte, 1u, &two, &error));

    const ql_ir_block_id entry = add_block(builder, "entry");
    const ql_ir_block_id left = add_block(builder, "left");
    const ql_ir_block_id right = add_block(builder, "right");
    const ql_ir_block_id merge = add_block(builder, "merge");
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(builder, entry, &error));
    ql_ir_terminator_definition_v1 branch{};
    ql_ir_terminator_definition_init(&branch,
                                     QL_IR_TERMINATOR_COND_BRANCH);
    branch.condition = condition;
    branch.target = left;
    branch.false_target = right;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_terminator(builder, entry, &branch, &error));
    set_branch(builder, left, merge);
    set_branch(builder, right, merge);

    const ql_ir_value_id incoming_values[2] = {one, two};
    const ql_ir_block_id incoming_blocks[2] = {left, right};
    const ql_ir_type_id phi_results[1] = {bv8};
    ql_ir_instruction_definition_v1 phi{};
    ql_ir_instruction_id phi_id = QL_IR_INVALID_INSTRUCTION_ID;
    ql_ir_value_id selected = QL_IR_INVALID_VALUE_ID;
    ql_ir_instruction_definition_init(&phi, QL_IR_OPCODE_PHI);
    phi.operands = incoming_values;
    phi.operand_count = 2u;
    phi.block_operands = incoming_blocks;
    phi.block_operand_count = 2u;
    phi.result_types = phi_results;
    phi.result_count = 1u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_append_instruction(builder, merge, &phi, &phi_id,
                                               &selected, &error));
    ql_ir_instruction_id second_phi_id = QL_IR_INVALID_INSTRUCTION_ID;
    ql_ir_value_id second_selected = QL_IR_INVALID_VALUE_ID;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_append_instruction(
                  builder, merge, &phi, &second_phi_id, &second_selected,
                  &error));
    ql_ir_terminator_definition_v1 result{};
    ql_ir_terminator_definition_init(&result, QL_IR_TERMINATOR_RETURN);
    result.return_value = second_selected;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_terminator(builder, merge, &result, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_finish(builder, &artifact, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, ql_ir_open(nullptr, artifact, &ir, &error))
        << error.message;
    ql_ir_instruction_view_v1 phi_view{};
    phi_view.struct_size = sizeof(phi_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_instruction_at(ir, phi_id, &phi_view, &error));
    ASSERT_EQ(2u, phi_view.block_operand_count);
    EXPECT_EQ(left, phi_view.block_operands[0]);
    EXPECT_EQ(right, phi_view.block_operands[1]);
    EXPECT_EQ(one, phi_view.operands[0]);
    EXPECT_EQ(two, phi_view.operands[1]);
    ql_ir_release(ir);
    ql_artifact_release(artifact);
    ql_ir_builder_destroy(builder);
}

TEST(Ir, RejectsBadReferencesBeforeReadingThem) {
    ql_ir_builder *builder = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_create(nullptr, &builder, &error));
    const ql_ir_type_id void_type = add_type(builder, QL_IR_TYPE_VOID);
    const ql_ir_type_id bv8 = add_type(builder, QL_IR_TYPE_BIT_VECTOR, 8u);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(builder, "bad", 3u, void_type,
                                         &error));
    const ql_ir_block_id entry = add_block(builder, "entry");
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(builder, entry, &error));

    const ql_ir_value_id missing_operand = 99u;
    const ql_ir_type_id result_type = bv8;
    ql_ir_instruction_definition_v1 identity{};
    ql_ir_instruction_id instruction = QL_IR_INVALID_INSTRUCTION_ID;
    ql_ir_value_id result = QL_IR_INVALID_VALUE_ID;
    ql_ir_instruction_definition_init(&identity, QL_IR_OPCODE_IDENTITY);
    identity.operands = &missing_operand;
    identity.operand_count = 1u;
    identity.result_types = &result_type;
    identity.result_count = 1u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_ir_builder_append_instruction(builder, entry, &identity,
                                               &instruction, &result,
                                               &error));
    EXPECT_EQ(QL_IR_INVALID_INSTRUCTION_ID, instruction);

    identity.operands = reinterpret_cast<const ql_ir_value_id *>(1u);
    identity.operand_count = std::numeric_limits<std::size_t>::max();
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_ir_builder_append_instruction(builder, entry, &identity,
                                               &instruction, &result,
                                               &error));
    ql_ir_builder_destroy(builder);

    ql_ir_builder *text_builder = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_create(nullptr, &text_builder, &error));
    const ql_ir_type_id text_void = add_type(text_builder, QL_IR_TYPE_VOID);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_ir_builder_set_function(
                  text_builder, reinterpret_cast<const char *>(1u),
                  std::numeric_limits<std::size_t>::max(), text_void,
                  &error));
    ql_ir_builder_destroy(text_builder);
}

TEST(Ir, RejectsCycleAndNonDominatingBranchValue) {
    ql_error error{};
    ql_ir_builder *cycle_builder = nullptr;
    ql_artifact *artifact = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_create(nullptr, &cycle_builder, &error));
    const ql_ir_type_id cycle_void = add_type(cycle_builder, QL_IR_TYPE_VOID);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(cycle_builder, "cycle", 5u,
                                         cycle_void, &error));
    const ql_ir_block_id first = add_block(cycle_builder, "first");
    const ql_ir_block_id second = add_block(cycle_builder, "second");
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(cycle_builder, first, &error));
    set_branch(cycle_builder, first, second);
    set_branch(cycle_builder, second, first);
    EXPECT_EQ(QL_STATUS_CYCLE,
              ql_ir_builder_finish(cycle_builder, &artifact, &error));
    EXPECT_EQ(nullptr, artifact);
    ql_ir_builder_destroy(cycle_builder);

    ql_ir_builder *builder = nullptr;
    ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_create(nullptr, &builder, &error));
    const ql_ir_type_id bool_type = add_type(builder, QL_IR_TYPE_BOOL, 1u);
    const ql_ir_type_id bv8 = add_type(builder, QL_IR_TYPE_BIT_VECTOR, 8u);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(builder, "bad_dom", 7u, bv8,
                                         &error));
    ql_ir_value_id condition = QL_IR_INVALID_VALUE_ID;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_parameter(builder, bool_type, "c", 1u,
                                          &condition, &error));
    const uint8_t constant_byte = 7u;
    ql_ir_value_id constant = QL_IR_INVALID_VALUE_ID;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_constant(builder, bv8, &constant_byte, 1u,
                                         &constant, &error));
    const ql_ir_block_id entry = add_block(builder, "entry");
    const ql_ir_block_id left = add_block(builder, "left");
    const ql_ir_block_id right = add_block(builder, "right");
    const ql_ir_block_id merge = add_block(builder, "merge");
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(builder, entry, &error));
    ql_ir_terminator_definition_v1 conditional{};
    ql_ir_terminator_definition_init(&conditional,
                                     QL_IR_TERMINATOR_COND_BRANCH);
    conditional.condition = condition;
    conditional.target = left;
    conditional.false_target = right;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_terminator(builder, entry, &conditional,
                                           &error));
    const ql_ir_value_id identity_operand = constant;
    const ql_ir_type_id identity_result = bv8;
    ql_ir_instruction_definition_v1 identity{};
    ql_ir_instruction_id left_instruction = QL_IR_INVALID_INSTRUCTION_ID;
    ql_ir_value_id left_value = QL_IR_INVALID_VALUE_ID;
    ql_ir_instruction_definition_init(&identity, QL_IR_OPCODE_IDENTITY);
    identity.operands = &identity_operand;
    identity.operand_count = 1u;
    identity.result_types = &identity_result;
    identity.result_count = 1u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_append_instruction(builder, left, &identity,
                                               &left_instruction, &left_value,
                                               &error));
    set_branch(builder, left, merge);
    set_branch(builder, right, merge);
    identity.operands = &left_value;
    ql_ir_instruction_id merge_instruction = QL_IR_INVALID_INSTRUCTION_ID;
    ql_ir_value_id merged_value = QL_IR_INVALID_VALUE_ID;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_append_instruction(
                  builder, merge, &identity, &merge_instruction,
                  &merged_value, &error));
    ql_ir_terminator_definition_v1 result_terminator{};
    ql_ir_terminator_definition_init(&result_terminator,
                                     QL_IR_TERMINATOR_RETURN);
    result_terminator.return_value = merged_value;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_terminator(builder, merge,
                                           &result_terminator, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_ir_builder_finish(builder, &artifact, &error));
    EXPECT_EQ(nullptr, artifact);
    ql_ir_builder_destroy(builder);
}

TEST(Ir, OpenRejectsTruncationAndCorruptedTypeReference) {
    ql_ir_builder *builder = nullptr;
    ql_artifact *artifact = nullptr;
    ql_artifact *corrupt = nullptr;
    ql_ir *ir = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_create(nullptr, &builder, &error));
    const ql_ir_type_id void_type = add_type(builder, QL_IR_TYPE_VOID);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(builder, "empty", 5u, void_type,
                                         &error));
    const ql_ir_block_id entry = add_block(builder, "entry");
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(builder, entry, &error));
    ql_ir_terminator_definition_v1 terminator{};
    ql_ir_terminator_definition_init(&terminator, QL_IR_TERMINATOR_RETURN);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_terminator(builder, entry, &terminator,
                                           &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_finish(builder, &artifact, &error));
    ql_artifact_view artifact_view{};
    artifact_view.struct_size = sizeof(artifact_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(artifact, &artifact_view, &error));
    ASSERT_GT(artifact_view.size, 40u);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_IR,
                                 QL_IR_ARTIFACT_SCHEMA_VERSION,
                                 artifact_view.data, 7u, &corrupt, &error));
    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_ir_open(nullptr, corrupt, &ir, &error));
    EXPECT_EQ(nullptr, ir);
    ql_artifact_release(corrupt);
    corrupt = nullptr;

    std::vector<uint8_t> bytes(
        static_cast<const uint8_t *>(artifact_view.data),
        static_cast<const uint8_t *>(artifact_view.data) + artifact_view.size);
    ASSERT_GE(bytes.size(), 36u);
    bytes[16] = 0xffu;
    bytes[17] = 0xffu;
    bytes[18] = 0xffu;
    bytes[19] = 0x7fu;
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_IR,
                                 QL_IR_ARTIFACT_SCHEMA_VERSION, bytes.data(),
                                 bytes.size(), &corrupt, &error));
    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_ir_open(nullptr, corrupt, &ir, &error));
    EXPECT_EQ(nullptr, ir);
    ql_artifact_release(corrupt);
    corrupt = nullptr;

    bytes.assign(static_cast<const uint8_t *>(artifact_view.data),
                 static_cast<const uint8_t *>(artifact_view.data) +
                     artifact_view.size);
    bytes[32] = 0xffu;
    bytes[33] = 0xffu;
    bytes[34] = 0xffu;
    bytes[35] = 0x7fu;
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_IR,
                                 QL_IR_ARTIFACT_SCHEMA_VERSION, bytes.data(),
                                 bytes.size(), &corrupt, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_ir_open(nullptr, corrupt, &ir, &error));
    EXPECT_EQ(nullptr, ir);
    ql_artifact_release(corrupt);
    ql_artifact_release(artifact);
    ql_ir_builder_destroy(builder);
}

}  // namespace
