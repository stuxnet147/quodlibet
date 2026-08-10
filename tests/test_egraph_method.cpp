#include "quodlibet/egraph_method.h"

#include "quodlibet/ir.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"
#include "quodlibet/pipeline.h"
#include "quodlibet/scheduler.h"

#include <cstdint>
#include <cstring>
#include <memory>

#include <gtest/gtest.h>

namespace {

struct ArtifactDeleter {
  void operator()(ql_artifact *artifact) const {
    ql_artifact_release(artifact);
  }
};

struct IrDeleter {
  void operator()(ql_ir *ir) const { ql_ir_release(ir); }
};

struct RegistryDeleter {
  void operator()(ql_registry *registry) const {
    ql_registry_destroy(registry);
  }
};

struct PipelineDeleter {
  void operator()(ql_pipeline *pipeline) const {
    ql_pipeline_destroy(pipeline);
  }
};

struct SchedulerDeleter {
  void operator()(ql_scheduler *scheduler) const {
    ql_scheduler_destroy(scheduler);
  }
};

struct PipelineResultDeleter {
  void operator()(ql_pipeline_result *result) const {
    ql_pipeline_result_destroy(result);
  }
};

using ArtifactPtr = std::unique_ptr<ql_artifact, ArtifactDeleter>;
using IrPtr = std::unique_ptr<ql_ir, IrDeleter>;
using RegistryPtr = std::unique_ptr<ql_registry, RegistryDeleter>;
using PipelinePtr = std::unique_ptr<ql_pipeline, PipelineDeleter>;
using SchedulerPtr = std::unique_ptr<ql_scheduler, SchedulerDeleter>;
using PipelineResultPtr =
    std::unique_ptr<ql_pipeline_result, PipelineResultDeleter>;

ArtifactPtr BuildAddZero() {
  ql_ir_builder *builder = nullptr;
  ql_artifact *artifact = nullptr;
  ql_error error{};
  ql_ir_type_definition_v1 type{};
  ql_ir_type_id bv32 = QL_IR_INVALID_TYPE_ID;
  ql_ir_value_id x = QL_IR_INVALID_VALUE_ID;
  ql_ir_value_id zero = QL_IR_INVALID_VALUE_ID;
  ql_ir_value_id sum = QL_IR_INVALID_VALUE_ID;
  ql_ir_block_id block = QL_IR_INVALID_BLOCK_ID;
  ql_ir_instruction_id instruction = QL_IR_INVALID_INSTRUCTION_ID;
  const std::uint8_t zero_bytes[4] = {0u, 0u, 0u, 0u};

  EXPECT_EQ(QL_STATUS_OK, ql_ir_builder_create(nullptr, &builder, &error))
      << error.message;
  if (builder == nullptr) {
    return ArtifactPtr();
  }
  ql_ir_type_definition_init(&type, QL_IR_TYPE_BIT_VECTOR);
  type.bit_width = 32u;
  EXPECT_EQ(QL_STATUS_OK, ql_ir_builder_add_type(builder, &type, &bv32, &error))
      << error.message;
  EXPECT_EQ(QL_STATUS_OK,
            ql_ir_builder_set_function(builder, "add_zero", 8u, bv32, &error))
      << error.message;
  EXPECT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_parameter(builder, bv32, "x", 1u, &x, &error))
      << error.message;
  EXPECT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_constant(builder, bv32, zero_bytes,
                                       sizeof(zero_bytes), &zero, &error))
      << error.message;
  EXPECT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_block(builder, "entry", 5u, &block, &error))
      << error.message;
  EXPECT_EQ(QL_STATUS_OK, ql_ir_builder_set_entry_block(builder, block, &error))
      << error.message;

  const ql_ir_value_id operands[2] = {x, zero};
  ql_ir_instruction_definition_v1 add{};
  ql_ir_instruction_definition_init(&add, QL_IR_OPCODE_ADD);
  add.operands = operands;
  add.operand_count = 2u;
  add.result_types = &bv32;
  add.result_count = 1u;
  EXPECT_EQ(QL_STATUS_OK, ql_ir_builder_append_instruction(
                              builder, block, &add, &instruction, &sum, &error))
      << error.message;

  ql_ir_terminator_definition_v1 terminator{};
  ql_ir_terminator_definition_init(&terminator, QL_IR_TERMINATOR_RETURN);
  terminator.return_value = sum;
  EXPECT_EQ(QL_STATUS_OK,
            ql_ir_builder_set_terminator(builder, block, &terminator, &error))
      << error.message;
  EXPECT_EQ(QL_STATUS_OK, ql_ir_builder_finish(builder, &artifact, &error))
      << error.message;
  ql_ir_builder_destroy(builder);
  return ArtifactPtr(artifact);
}

RegistryPtr Builtins() {
  ql_registry *registry = nullptr;
  ql_error error{};
  EXPECT_EQ(QL_STATUS_OK, ql_registry_create(nullptr, &registry, &error))
      << error.message;
  if (registry != nullptr) {
    EXPECT_EQ(QL_STATUS_OK, ql_register_builtin_methods(registry, &error))
        << error.message;
  }
  return RegistryPtr(registry);
}

TEST(EGraphMethod, IsRegisteredAsACheckedNonProofNormalizer) {
  const RegistryPtr registry = Builtins();
  ASSERT_NE(nullptr, registry);
  const ql_method_v1 *method =
      ql_registry_find(registry.get(), QL_EGRAPH_METHOD_NAME);
  ASSERT_NE(nullptr, method);
  EXPECT_STREQ(QL_ARTIFACT_KIND_IR, method->output_kind);
  EXPECT_EQ(QL_METHOD_DETERMINISTIC | QL_METHOD_CACHEABLE, method->flags);
  EXPECT_EQ(1u, method->minimum_inputs);
  EXPECT_EQ(1u, method->maximum_inputs);
  EXPECT_EQ(nullptr, ql_registry_find_proof_method(registry.get(),
                                                   QL_EGRAPH_METHOD_NAME));
}

TEST(EGraphMethod, PipelineEliminatesAddZeroAndPreservesExecution) {
  constexpr char pipeline_json[] =
      "{\"schema_version\":1,\"nodes\":[{\"name\":\"normalize\","
      "\"method\":\"normalize.egraph\",\"options\":{"
      "\"rewrite_set\":\"pure-bitvector-v1\","
      "\"node_limit\":1024,\"iteration_limit\":8,"
      "\"reconstruct_proof\":true}}]}";
  const ArtifactPtr input = BuildAddZero();
  const RegistryPtr registry = Builtins();
  ql_pipeline *raw_pipeline = nullptr;
  ql_scheduler *raw_scheduler = nullptr;
  ql_pipeline_result *raw_result = nullptr;
  ql_error error{};
  ASSERT_NE(nullptr, input);
  ASSERT_NE(nullptr, registry);
  ASSERT_EQ(QL_STATUS_OK,
            ql_pipeline_from_json(registry.get(), nullptr, pipeline_json,
                                  sizeof(pipeline_json) - 1u, &raw_pipeline,
                                  &error))
      << error.message;
  const PipelinePtr pipeline(raw_pipeline);
  ASSERT_EQ(QL_STATUS_OK,
            ql_scheduler_create(nullptr, 1u, &raw_scheduler, &error))
      << error.message;
  const SchedulerPtr scheduler(raw_scheduler);
  ASSERT_EQ(QL_STATUS_OK,
            ql_pipeline_run(pipeline.get(), scheduler.get(), input.get(),
                            nullptr, &raw_result, &error))
      << error.message;
  const PipelineResultPtr result(raw_result);
  ASSERT_EQ(1u, ql_pipeline_result_count(result.get()));
  const ql_artifact *borrowed = ql_pipeline_result_artifact(result.get(), 0u);
  ASSERT_NE(nullptr, borrowed);

  ql_ir *raw_ir = nullptr;
  ASSERT_EQ(QL_STATUS_OK, ql_ir_open(nullptr, borrowed, &raw_ir, &error))
      << error.message;
  const IrPtr ir(raw_ir);
  ql_ir_view_v1 view{};
  view.struct_size = sizeof(view);
  ASSERT_EQ(QL_STATUS_OK, ql_ir_get_view(ir.get(), &view, &error))
      << error.message;
  EXPECT_EQ(0u, view.instruction_count);

  ql_ir_verify_report_v1 report{};
  ql_ir_verify_report_init(&report);
  ASSERT_EQ(QL_STATUS_OK, ql_ir_verify(nullptr, ir.get(), &report, &error))
      << report.message;

  ql_ir_interp_input_v1 argument{};
  ql_ir_interp_input_init(&argument);
  const std::uint8_t input_bytes[4] = {0x78u, 0x56u, 0x34u, 0x12u};
  argument.value = 0u;
  argument.data = input_bytes;
  argument.size = sizeof(input_bytes);
  ql_ir_interp_result_v1 execution{};
  execution.struct_size = sizeof(execution);
  ASSERT_EQ(QL_STATUS_OK, ql_ir_interp_run(nullptr, ir.get(), &argument, 1u,
                                           nullptr, &execution, &error))
      << error.message;
  ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, execution.outcome);
  ASSERT_EQ(sizeof(input_bytes), execution.value_size);
  EXPECT_EQ(0, std::memcmp(input_bytes, execution.value, sizeof(input_bytes)));
}

TEST(EGraphMethod, RefusesUncheckedAndUnknownOptionModes) {
  const ql_method_v1 *method;
  const RegistryPtr registry = Builtins();
  void *instance = nullptr;
  ql_error error{};
  ASSERT_NE(nullptr, registry);
  method = ql_registry_find(registry.get(), QL_EGRAPH_METHOD_NAME);
  ASSERT_NE(nullptr, method);
  EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
            method->create(ql_default_host(), "{\"reconstruct_proof\":false}",
                           &instance, &error));
  EXPECT_EQ(nullptr, instance);
  EXPECT_EQ(
      QL_STATUS_INVALID_ARGUMENT,
      method->create(ql_default_host(), "{\"mystery\":1}", &instance, &error));
  EXPECT_EQ(nullptr, instance);
}

} // namespace
