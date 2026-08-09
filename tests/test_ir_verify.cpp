#include "quodlibet/c_lower.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace {

class BuilderHandle {
public:
    BuilderHandle() {
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_builder_create(nullptr, &builder_, &error))
            << error.message;
    }
    ~BuilderHandle() { ql_ir_builder_destroy(builder_); }

    ql_ir_builder *get() const { return builder_; }

private:
    ql_ir_builder *builder_ = nullptr;
};

class ModuleHandle {
public:
    ~ModuleHandle() {
        ql_ir_release(ir_);
        ql_artifact_release(artifact_);
    }

    ql_status Finish(ql_ir_builder *builder, ql_error *error) {
        ql_status status = ql_ir_builder_finish(builder, &artifact_, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return ql_ir_open(nullptr, artifact_, &ir_, error);
    }

    ql_ir *get() const { return ir_; }

private:
    ql_artifact *artifact_ = nullptr;
    ql_ir *ir_ = nullptr;
};

ql_ir_type_id AddType(ql_ir_builder *builder, ql_ir_type_kind kind,
                      uint32_t width = 0u,
                      ql_ir_type_id element = QL_IR_INVALID_TYPE_ID) {
    ql_ir_type_definition_v1 definition{};
    ql_ir_type_id id = QL_IR_INVALID_TYPE_ID;
    ql_error error{};
    ql_ir_type_definition_init(&definition, kind);
    definition.bit_width = width;
    definition.element_type = element;
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_type(builder, &definition, &id, &error))
        << error.message;
    return id;
}

ql_ir_block_id AddBlock(ql_ir_builder *builder, const char *label) {
    ql_ir_block_id id = QL_IR_INVALID_BLOCK_ID;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_block(builder, label, std::strlen(label), &id,
                                      &error))
        << error.message;
    return id;
}

ql_ir_value_id AddParameter(ql_ir_builder *builder, ql_ir_type_id type,
                            const char *name) {
    ql_ir_value_id id = QL_IR_INVALID_VALUE_ID;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_parameter(builder, type, name,
                                          std::strlen(name), &id, &error))
        << error.message;
    return id;
}

ql_ir_value_id AddIntConstant(ql_ir_builder *builder, ql_ir_type_id type,
                              uint32_t width, uint64_t value) {
    std::vector<uint8_t> bytes((width + 7u) / 8u, 0u);
    ql_ir_value_id id = QL_IR_INVALID_VALUE_ID;
    ql_error error{};
    for (std::size_t index = 0u; index < bytes.size() && index < 8u;
         ++index) {
        bytes[index] = static_cast<uint8_t>(value & 0xffu);
        value >>= 8u;
    }
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_add_constant(builder, type, bytes.data(),
                                         bytes.size(), &id, &error))
        << error.message;
    return id;
}

/* Appends an instruction and returns its single result, or the invalid value
   identifier when the opcode produces none. */
ql_ir_value_id Append(ql_ir_builder *builder, ql_ir_block_id block,
                      ql_ir_opcode opcode,
                      const std::vector<ql_ir_value_id> &operands,
                      ql_ir_type_id result_type = QL_IR_INVALID_TYPE_ID,
                      uint64_t effects = QL_IR_EFFECT_NONE) {
    ql_ir_instruction_definition_v1 definition{};
    ql_ir_instruction_id instruction = QL_IR_INVALID_INSTRUCTION_ID;
    ql_ir_value_id result = QL_IR_INVALID_VALUE_ID;
    ql_error error{};
    ql_ir_instruction_definition_init(&definition, opcode);
    definition.operands = operands.data();
    definition.operand_count = operands.size();
    definition.effects = effects;
    if (result_type != QL_IR_INVALID_TYPE_ID) {
        definition.result_types = &result_type;
        definition.result_count = 1u;
    }
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_append_instruction(
                  builder, block, &definition, &instruction,
                  result_type != QL_IR_INVALID_TYPE_ID ? &result : nullptr,
                  &error))
        << error.message;
    return result;
}

void SetReturn(ql_ir_builder *builder, ql_ir_block_id block,
               ql_ir_value_id value) {
    ql_ir_terminator_definition_v1 terminator{};
    ql_error error{};
    ql_ir_terminator_definition_init(&terminator, QL_IR_TERMINATOR_RETURN);
    terminator.return_value = value;
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_terminator(builder, block, &terminator,
                                           &error))
        << error.message;
}

void SetBranch(ql_ir_builder *builder, ql_ir_block_id block,
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

ql_ir_verify_report_v1 Verify(const ql_ir *ir, ql_status *status) {
    ql_ir_verify_report_v1 report{};
    ql_error error{};
    report.struct_size = sizeof(report);
    *status = ql_ir_verify(nullptr, ir, &report, &error);
    return report;
}

/* Shared skeleton: `int f(int a, int b)` whose entry block computes `b != 0`
   and a signed division. Callers decide where the guard goes. */
struct DivisionModule {
    ql_ir_type_id bool_type = QL_IR_INVALID_TYPE_ID;
    ql_ir_type_id bv32 = QL_IR_INVALID_TYPE_ID;
    ql_ir_block_id entry = QL_IR_INVALID_BLOCK_ID;
    ql_ir_value_id a = QL_IR_INVALID_VALUE_ID;
    ql_ir_value_id b = QL_IR_INVALID_VALUE_ID;
    ql_ir_value_id zero = QL_IR_INVALID_VALUE_ID;
};

DivisionModule StartDivisionModule(ql_ir_builder *builder) {
    DivisionModule module;
    ql_error error{};
    module.bool_type = AddType(builder, QL_IR_TYPE_BOOL, 1u);
    module.bv32 = AddType(builder, QL_IR_TYPE_BIT_VECTOR, 32u);
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(builder, "divide", 6u, module.bv32,
                                         &error))
        << error.message;
    module.a = AddParameter(builder, module.bv32, "a");
    module.b = AddParameter(builder, module.bv32, "b");
    module.zero = AddIntConstant(builder, module.bv32, 32u, 0u);
    module.entry = AddBlock(builder, "entry");
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(builder, module.entry, &error))
        << error.message;
    return module;
}

class LoweredFunction {
public:
    ~LoweredFunction() {
        ql_ir_release(ir_);
        ql_c_lower_result_destroy(result_);
        ql_c_frontend_unit_destroy(unit_);
    }

    ql_status Lower(const char *source, const char *name, ql_error *error) {
        ql_c_function_view function{};
        ql_c_lower_result_view_v1 view{};
        const std::size_t source_size = std::strlen(source);
        ql_status status = ql_c_frontend_analyze(nullptr, source, source_size,
                                                 &unit_, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        function.struct_size = sizeof(function);
        status = ql_c_frontend_select_function(unit_, name, std::strlen(name),
                                               &function, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_c_lower_selected_function(nullptr, source, source_size,
                                              unit_, &function, &result_,
                                              error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        view.struct_size = sizeof(view);
        status = ql_c_lower_result_get_view(result_, &view, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        support_ = view.support;
        if (view.support != QL_C_LOWER_SUPPORTED) {
            return QL_STATUS_OK;
        }
        return ql_ir_open(nullptr, view.ir_artifact, &ir_, error);
    }

    ql_c_lower_support support() const { return support_; }
    ql_ir *ir() const { return ir_; }

private:
    ql_c_frontend_unit *unit_ = nullptr;
    ql_c_lower_result *result_ = nullptr;
    ql_ir *ir_ = nullptr;
    ql_c_lower_support support_ = QL_C_LOWER_UNKNOWN;
};

TEST(IrVerify, AcceptsAGuardedPartialOperation) {
    BuilderHandle builder;
    ModuleHandle module;
    ql_error error{};
    ql_status status = QL_STATUS_INTERNAL_ERROR;

    DivisionModule skeleton = StartDivisionModule(builder.get());
    const ql_ir_value_id defined =
        Append(builder.get(), skeleton.entry, QL_IR_OPCODE_NE,
               {skeleton.b, skeleton.zero}, skeleton.bool_type);
    const ql_ir_value_id quotient =
        Append(builder.get(), skeleton.entry, QL_IR_OPCODE_SDIV,
               {skeleton.a, skeleton.b}, skeleton.bv32);
    Append(builder.get(), skeleton.entry, QL_IR_OPCODE_UB_GUARD, {defined},
           QL_IR_INVALID_TYPE_ID, QL_IR_EFFECT_UNDEFINED_BEHAVIOR);
    SetReturn(builder.get(), skeleton.entry, quotient);

    ASSERT_EQ(QL_STATUS_OK, module.Finish(builder.get(), &error))
        << error.message;
    const ql_ir_verify_report_v1 report = Verify(module.get(), &status);
    EXPECT_EQ(QL_STATUS_OK, status) << report.message;
    EXPECT_EQ(QL_IR_VERIFY_OK, report.code);
}

TEST(IrVerify, RejectsAnObservationOfAnUnguardedPartialOperation) {
    BuilderHandle builder;
    ModuleHandle module;
    ql_error error{};
    ql_status status = QL_STATUS_OK;

    DivisionModule skeleton = StartDivisionModule(builder.get());
    const ql_ir_value_id quotient =
        Append(builder.get(), skeleton.entry, QL_IR_OPCODE_SDIV,
               {skeleton.a, skeleton.b}, skeleton.bv32);
    SetReturn(builder.get(), skeleton.entry, quotient);

    /* The builder accepts this module: nothing in schema v1 makes a guard a
       structural requirement. The verifier is what makes it one. */
    ASSERT_EQ(QL_STATUS_OK, module.Finish(builder.get(), &error))
        << error.message;
    const ql_ir_verify_report_v1 report = Verify(module.get(), &status);
    EXPECT_NE(QL_STATUS_OK, status);
    EXPECT_EQ(QL_IR_VERIFY_UB_GUARD, report.code);
    EXPECT_EQ(skeleton.entry, report.block);
    EXPECT_EQ(quotient, report.value);
}

TEST(IrVerify, RejectsAGuardThatPrecedesThePartialOperation) {
    BuilderHandle builder;
    ModuleHandle module;
    ql_error error{};
    ql_status status = QL_STATUS_OK;

    DivisionModule skeleton = StartDivisionModule(builder.get());
    const ql_ir_value_id defined =
        Append(builder.get(), skeleton.entry, QL_IR_OPCODE_NE,
               {skeleton.b, skeleton.zero}, skeleton.bool_type);
    Append(builder.get(), skeleton.entry, QL_IR_OPCODE_UB_GUARD, {defined},
           QL_IR_INVALID_TYPE_ID, QL_IR_EFFECT_UNDEFINED_BEHAVIOR);
    const ql_ir_value_id quotient =
        Append(builder.get(), skeleton.entry, QL_IR_OPCODE_SDIV,
               {skeleton.a, skeleton.b}, skeleton.bv32);
    SetReturn(builder.get(), skeleton.entry, quotient);

    ASSERT_EQ(QL_STATUS_OK, module.Finish(builder.get(), &error))
        << error.message;
    const ql_ir_verify_report_v1 report = Verify(module.get(), &status);
    EXPECT_NE(QL_STATUS_OK, status);
    EXPECT_EQ(QL_IR_VERIFY_UB_GUARD, report.code);
}

TEST(IrVerify, RequiresEachPhiEdgeToBeGuardedInItsOwnPredecessor) {
    BuilderHandle builder;
    ModuleHandle module;
    ql_error error{};
    ql_status status = QL_STATUS_OK;

    const ql_ir_type_id bool_type = AddType(builder.get(), QL_IR_TYPE_BOOL,
                                            1u);
    const ql_ir_type_id bv32 = AddType(builder.get(), QL_IR_TYPE_BIT_VECTOR,
                                       32u);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(builder.get(), "merge", 5u, bv32,
                                         &error))
        << error.message;
    const ql_ir_value_id a = AddParameter(builder.get(), bv32, "a");
    const ql_ir_value_id b = AddParameter(builder.get(), bv32, "b");
    const ql_ir_value_id condition = AddParameter(builder.get(), bool_type,
                                                  "c");
    const ql_ir_value_id zero = AddIntConstant(builder.get(), bv32, 32u, 0u);
    const ql_ir_block_id entry = AddBlock(builder.get(), "entry");
    const ql_ir_block_id then_block = AddBlock(builder.get(), "then");
    const ql_ir_block_id else_block = AddBlock(builder.get(), "else");
    const ql_ir_block_id join = AddBlock(builder.get(), "join");
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(builder.get(), entry, &error))
        << error.message;

    ql_ir_terminator_definition_v1 branch{};
    ql_ir_terminator_definition_init(&branch, QL_IR_TERMINATOR_COND_BRANCH);
    branch.condition = condition;
    branch.target = then_block;
    branch.false_target = else_block;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_terminator(builder.get(), entry, &branch,
                                           &error))
        << error.message;

    const ql_ir_value_id quotient =
        Append(builder.get(), then_block, QL_IR_OPCODE_SDIV, {a, b}, bv32);
    SetBranch(builder.get(), then_block, join);
    SetBranch(builder.get(), else_block, join);

    ql_ir_instruction_definition_v1 phi{};
    ql_ir_instruction_id phi_id = QL_IR_INVALID_INSTRUCTION_ID;
    ql_ir_value_id merged = QL_IR_INVALID_VALUE_ID;
    const ql_ir_value_id phi_operands[2] = {quotient, zero};
    const ql_ir_block_id phi_blocks[2] = {then_block, else_block};
    ql_ir_type_id phi_result = bv32;
    ql_ir_instruction_definition_init(&phi, QL_IR_OPCODE_PHI);
    phi.operands = phi_operands;
    phi.operand_count = 2u;
    phi.block_operands = phi_blocks;
    phi.block_operand_count = 2u;
    phi.result_types = &phi_result;
    phi.result_count = 1u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_append_instruction(builder.get(), join, &phi,
                                               &phi_id, &merged, &error))
        << error.message;
    SetReturn(builder.get(), join, merged);

    ASSERT_EQ(QL_STATUS_OK, module.Finish(builder.get(), &error))
        << error.message;
    const ql_ir_verify_report_v1 report = Verify(module.get(), &status);
    EXPECT_NE(QL_STATUS_OK, status);
    EXPECT_EQ(QL_IR_VERIFY_UB_GUARD, report.code);
    /* A guard in the join block could not have covered this edge, so the
       report names the branch the value came from. */
    EXPECT_EQ(then_block, report.block);
}

TEST(IrVerify, RejectsEffectBitsAnOpcodeCannotPerform) {
    BuilderHandle builder;
    ModuleHandle module;
    ql_error error{};
    ql_status status = QL_STATUS_OK;

    const ql_ir_type_id bv32 = AddType(builder.get(), QL_IR_TYPE_BIT_VECTOR,
                                       32u);
    const ql_ir_type_id trace = AddType(builder.get(),
                                        QL_IR_TYPE_EVENT_TRACE);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(builder.get(), "emit", 4u, bv32,
                                         &error))
        << error.message;
    const ql_ir_value_id value = AddParameter(builder.get(), bv32, "value");
    const ql_ir_value_id events = AddParameter(builder.get(), trace,
                                               "events");
    const ql_ir_block_id entry = AddBlock(builder.get(), "entry");
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(builder.get(), entry, &error))
        << error.message;
    Append(builder.get(), entry, QL_IR_OPCODE_TRACE_APPEND, {events, value},
           trace, QL_IR_EFFECT_UNDEFINED_BEHAVIOR);
    SetReturn(builder.get(), entry, value);

    ASSERT_EQ(QL_STATUS_OK, module.Finish(builder.get(), &error))
        << error.message;
    const ql_ir_verify_report_v1 report = Verify(module.get(), &status);
    EXPECT_NE(QL_STATUS_OK, status);
    EXPECT_EQ(QL_IR_VERIFY_EFFECT_RULE, report.code);
}

TEST(IrVerify, RefusesExtensionOpcodesInsteadOfAssumingTheirRules) {
    BuilderHandle builder;
    ModuleHandle module;
    ql_error error{};
    ql_status status = QL_STATUS_OK;

    const ql_ir_type_id bv32 = AddType(builder.get(), QL_IR_TYPE_BIT_VECTOR,
                                       32u);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_function(builder.get(), "extended", 8u, bv32,
                                         &error))
        << error.message;
    const ql_ir_value_id value = AddParameter(builder.get(), bv32, "value");
    const ql_ir_block_id entry = AddBlock(builder.get(), "entry");
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_builder_set_entry_block(builder.get(), entry, &error))
        << error.message;
    const ql_ir_value_id extended =
        Append(builder.get(), entry, QL_IR_OPCODE_EXTENSION_BASE, {value},
               bv32);
    SetReturn(builder.get(), entry, extended);

    ASSERT_EQ(QL_STATUS_OK, module.Finish(builder.get(), &error))
        << error.message;
    const ql_ir_verify_report_v1 report = Verify(module.get(), &status);
    EXPECT_NE(QL_STATUS_OK, status);
    EXPECT_EQ(QL_IR_VERIFY_OPCODE, report.code);
}

TEST(IrVerify, VerifiesEveryModuleTheCLoweringAccepts) {
    struct Case {
        const char *source;
        const char *name;
    };
    const Case cases[] = {
        {"int identity(int a) { return a; }", "identity"},
        {"int divide(int a, int b) { return a / b; }", "divide"},
        {"unsigned int shift(unsigned int a, int b) { return a << b; }",
         "shift"},
        {"int shift_signed(int a, int b) { return a << b; }", "shift_signed"},
        {"int remainder(int a, int b) { return a % b; }", "remainder"},
        {"int product(int a, int b) { return a * b; }", "product"},
        {"int sum(int a, int b) { return a + b - 1; }", "sum"},
        {"int guarded(int a, int b) { if (b) { return a / b; } return 0; }",
         "guarded"},
        {"int merged(int a, int b, int c) {\n"
         "  int x;\n"
         "  if (c) { x = a / b; } else { x = 0; }\n"
         "  return x;\n"
         "}",
         "merged"},
        {"int chained(int a, int b) {\n"
         "  int q = a / b;\n"
         "  int r = a % b;\n"
         "  return q + r;\n"
         "}",
         "chained"},
        {"int shortcut(int a, int b) { return b != 0 && a / b > 1; }",
         "shortcut"},
        {"_Bool compare(int a, int b) { return a < b; }", "compare"},
        {"int nested(int a) {\n"
         "  if (a < 3) { return 0; }\n"
         "  else if (a < 6) { return 1; }\n"
         "  return 4;\n"
         "}",
         "nested"},
    };

    for (const Case &item : cases) {
        LoweredFunction lowered;
        ql_error error{};
        ql_status status = QL_STATUS_INTERNAL_ERROR;
        SCOPED_TRACE(item.name);
        ASSERT_EQ(QL_STATUS_OK,
                  lowered.Lower(item.source, item.name, &error))
            << error.message;
        ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
        ASSERT_NE(nullptr, lowered.ir());
        const ql_ir_verify_report_v1 report = Verify(lowered.ir(), &status);
        EXPECT_EQ(QL_STATUS_OK, status)
            << ql_ir_verify_code_string(report.code) << ": " << report.message;
    }
}

TEST(IrVerify, ReportsArgumentFailuresWithoutClaimingAVerdict) {
    ql_ir_verify_report_v1 report{};
    ql_error error{};

    report.struct_size = sizeof(report);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_ir_verify(nullptr, nullptr, &report, &error));
    EXPECT_EQ(QL_IR_VERIFY_OK, report.code);

    BuilderHandle builder;
    ModuleHandle module;
    DivisionModule skeleton = StartDivisionModule(builder.get());
    SetReturn(builder.get(), skeleton.entry, skeleton.a);
    ASSERT_EQ(QL_STATUS_OK, module.Finish(builder.get(), &error))
        << error.message;

    ql_ir_verify_report_v1 undersized{};
    undersized.struct_size = sizeof(undersized) - 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_ir_verify(nullptr, module.get(), &undersized, &error));

    /* A zero struct_size keeps the older ABI convention of "unspecified". */
    ql_ir_verify_report_v1 unspecified{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_verify(nullptr, module.get(), &unspecified, &error))
        << error.message;
    EXPECT_EQ(QL_IR_VERIFY_SCHEMA_VERSION, unspecified.schema_version);
}

}  // namespace
