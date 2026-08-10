#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

/* Lowers one C function and keeps the module open for repeated runs. */
class Module {
public:
  ~Module() {
    ql_ir_release(ir_);
    ql_c_lower_result_destroy(result_);
    ql_c_frontend_unit_destroy(unit_);
  }

  bool Open(const char *source, const char *name) {
    ql_c_function_view function{};
    ql_c_lower_result_view_v1 view{};
    ql_ir_verify_report_v1 report{};
    ql_error error{};
    const std::size_t source_size = std::strlen(source);

    if (ql_c_frontend_analyze(nullptr, source, source_size, &unit_, &error) !=
        QL_STATUS_OK) {
      ADD_FAILURE() << "analyze: " << error.message;
      return false;
    }
    function.struct_size = sizeof(function);
    if (ql_c_frontend_select_function(unit_, name, std::strlen(name), &function,
                                      &error) != QL_STATUS_OK) {
      ADD_FAILURE() << "select: " << error.message;
      return false;
    }
    if (ql_c_lower_selected_function(nullptr, source, source_size, unit_,
                                     &function, &result_,
                                     &error) != QL_STATUS_OK) {
      ADD_FAILURE() << "lower: " << error.message;
      return false;
    }
    view.struct_size = sizeof(view);
    if (ql_c_lower_result_get_view(result_, &view, &error) != QL_STATUS_OK) {
      ADD_FAILURE() << "view: " << error.message;
      return false;
    }
    if (view.support != QL_C_LOWER_SUPPORTED) {
      ql_c_lower_diagnostic_view_v1 diagnostic{};
      diagnostic.struct_size = sizeof(diagnostic);
      if (view.diagnostic_count != 0u &&
          ql_c_lower_result_diagnostic_at(result_, 0u, &diagnostic, &error) ==
              QL_STATUS_OK) {
        ADD_FAILURE() << "lowering did not accept the source: "
                      << diagnostic.message;
      } else {
        ADD_FAILURE() << "lowering did not accept the source";
      }
      return false;
    }
    if (ql_ir_open(nullptr, view.ir_artifact, &ir_, &error) != QL_STATUS_OK) {
      ADD_FAILURE() << "open: " << error.message;
      return false;
    }
    report.struct_size = sizeof(report);
    if (ql_ir_verify(nullptr, ir_, &report, &error) != QL_STATUS_OK) {
      ADD_FAILURE() << "verify: " << ql_ir_verify_code_string(report.code)
                    << ": " << report.message;
      return false;
    }
    return CollectParameters();
  }

  ql_ir *ir() const { return ir_; }
  const std::vector<ql_ir_value_id> &parameters() const { return parameters_; }
  const std::vector<uint32_t> &widths() const { return widths_; }

private:
  bool CollectParameters() {
    ql_ir_view_v1 view{};
    ql_error error{};
    view.struct_size = sizeof(view);
    if (ql_ir_get_view(ir_, &view, &error) != QL_STATUS_OK) {
      ADD_FAILURE() << error.message;
      return false;
    }
    for (std::size_t index = 0u; index < view.value_count; ++index) {
      ql_ir_value_view_v1 value{};
      ql_ir_type_view_v1 type{};
      value.struct_size = sizeof(value);
      if (ql_ir_value_at(ir_, index, &value, &error) != QL_STATUS_OK) {
        ADD_FAILURE() << error.message;
        return false;
      }
      if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
        continue;
      }
      type.struct_size = sizeof(type);
      if (ql_ir_type_at(ir_, value.type, &type, &error) != QL_STATUS_OK) {
        ADD_FAILURE() << error.message;
        return false;
      }
      parameters_.push_back(value.id);
      widths_.push_back(type.kind == QL_IR_TYPE_BOOL ? 1u : type.bit_width);
    }
    return true;
  }

  ql_c_frontend_unit *unit_ = nullptr;
  ql_c_lower_result *result_ = nullptr;
  ql_ir *ir_ = nullptr;
  std::vector<ql_ir_value_id> parameters_;
  std::vector<uint32_t> widths_;
};

std::vector<uint8_t> Encode(uint64_t value, uint32_t width) {
  std::vector<uint8_t> bytes((width + 7u) / 8u, 0u);
  for (std::size_t index = 0u; index < bytes.size() && index < 8u; ++index) {
    bytes[index] = static_cast<uint8_t>((value >> (index * 8u)) & 0xffu);
  }
  if (width % 8u != 0u && !bytes.empty()) {
    bytes.back() =
        static_cast<uint8_t>(bytes.back() & ((1u << (width % 8u)) - 1u));
  }
  return bytes;
}

uint64_t Decode(const ql_ir_interp_result_v1 &result) {
  uint64_t value = 0u;
  for (std::size_t index = 0u; index < result.value_size && index < 8u;
       ++index) {
    value |= static_cast<uint64_t>(result.value[index]) << (index * 8u);
  }
  return value;
}

int64_t DecodeSigned(const ql_ir_interp_result_v1 &result, uint32_t width) {
  uint64_t raw = Decode(result);
  if (width < 64u && (raw >> (width - 1u)) != 0u) {
    raw |= ~((UINT64_C(1) << width) - UINT64_C(1));
  }
  return static_cast<int64_t>(raw);
}

ql_ir_interp_result_v1 Run(const Module &module,
                           const std::vector<uint64_t> &arguments) {
  std::vector<std::vector<uint8_t>> storage;
  std::vector<ql_ir_interp_input_v1> inputs;
  ql_ir_interp_result_v1 result{};
  ql_error error{};

  EXPECT_EQ(module.parameters().size(), arguments.size());
  storage.reserve(arguments.size());
  for (std::size_t index = 0u; index < arguments.size(); ++index) {
    storage.push_back(Encode(arguments[index], module.widths()[index]));
  }
  for (std::size_t index = 0u; index < arguments.size(); ++index) {
    ql_ir_interp_input_v1 input{};
    ql_ir_interp_input_init(&input);
    input.value = module.parameters()[index];
    input.data = storage[index].data();
    input.size = storage[index].size();
    inputs.push_back(input);
  }
  result.struct_size = sizeof(result);
  EXPECT_EQ(QL_STATUS_OK,
            ql_ir_interp_run(nullptr, module.ir(), inputs.data(), inputs.size(),
                             nullptr, &result, &error))
      << error.message;
  return result;
}

int64_t ExpectReturn(const Module &module,
                     const std::vector<uint64_t> &arguments, uint32_t width) {
  const ql_ir_interp_result_v1 result = Run(module, arguments);
  EXPECT_EQ(QL_IR_INTERP_OUTCOME_RETURN, result.outcome)
      << ql_ir_interp_outcome_string(result.outcome) << " / "
      << ql_ir_interp_ub_reason_string(result.ub_reason);
  return DecodeSigned(result, width);
}

void ExpectUndefined(const Module &module,
                     const std::vector<uint64_t> &arguments,
                     ql_ir_interp_ub_reason reason) {
  const ql_ir_interp_result_v1 result = Run(module, arguments);
  EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR, result.outcome)
      << ql_ir_interp_outcome_string(result.outcome);
  EXPECT_EQ(reason, result.ub_reason)
      << ql_ir_interp_ub_reason_string(result.ub_reason);
}

TEST(IrInterp, EvaluatesUnsignedArithmeticModulo) {
  Module module;
  ASSERT_TRUE(
      module.Open("unsigned int wrap(unsigned int a, unsigned int b) {\n"
                  "  return a * b + 7u;\n"
                  "}",
                  "wrap"));
  EXPECT_EQ(7, ExpectReturn(module, {0u, 5u}, 32u));
  EXPECT_EQ(static_cast<int64_t>(static_cast<int32_t>(0xffffffffu * 3u + 7u)),
            ExpectReturn(module, {0xffffffffu, 3u}, 32u));
}

TEST(IrInterp, TreatsSignedOverflowAsUndefined) {
  Module module;
  ASSERT_TRUE(module.Open("int sum(int a, int b) { return a + b; }", "sum"));
  EXPECT_EQ(3, ExpectReturn(module, {1u, 2u}, 32u));
  EXPECT_EQ(-1, ExpectReturn(module, {static_cast<uint64_t>(-3), 2u}, 32u));
  ExpectUndefined(module, {0x7fffffffu, 1u}, QL_IR_INTERP_UB_GUARD_FAILED);
  ExpectUndefined(module, {0x80000000u, static_cast<uint64_t>(-1)},
                  QL_IR_INTERP_UB_GUARD_FAILED);
}

TEST(IrInterp, ExecutesSwitchFallthroughBreakAndDefaultExactly) {
  Module module;
  ASSERT_TRUE(module.Open("int dispatch(int x) {\n"
                          "  int result = 1;\n"
                          "  switch (x) {\n"
                          "  case 0: result += 2;\n"
                          "  case 1:\n"
                          "    if (x == 1) { result = 11; break; }\n"
                          "    result += 4; break;\n"
                          "  case 2: return 20;\n"
                          "  default: result = 9;\n"
                          "  }\n"
                          "  return result;\n"
                          "}",
                          "dispatch"));
  EXPECT_EQ(7, ExpectReturn(module, {0u}, 32u));
  EXPECT_EQ(11, ExpectReturn(module, {1u}, 32u));
  EXPECT_EQ(20, ExpectReturn(module, {2u}, 32u));
  EXPECT_EQ(9, ExpectReturn(module, {3u}, 32u));
}

TEST(IrInterp, ExecutesDefaultInSourceOrderAndAnAllReturningSwitch) {
  Module middle_default;
  Module all_return;
  ASSERT_TRUE(middle_default.Open("int dispatch(int x) {\n"
                                  "  int result = 0;\n"
                                  "  switch (x) {\n"
                                  "  case 0: return 10;\n"
                                  "  default: result = 5;\n"
                                  "  case 2: result += 3; break;\n"
                                  "  }\n"
                                  "  return result;\n"
                                  "}",
                                  "dispatch"));
  EXPECT_EQ(10, ExpectReturn(middle_default, {0u}, 32u));
  EXPECT_EQ(3, ExpectReturn(middle_default, {2u}, 32u));
  EXPECT_EQ(8, ExpectReturn(middle_default, {7u}, 32u));

  ASSERT_TRUE(all_return.Open("int dispatch(int x) {\n"
                              "  switch (x) {\n"
                              "  case 'a': return 1;\n"
                              "  case 2 + 3: return 2;\n"
                              "  default: return 3;\n"
                              "  }\n"
                              "}",
                              "dispatch"));
  EXPECT_EQ(1, ExpectReturn(all_return, {'a'}, 32u));
  EXPECT_EQ(2, ExpectReturn(all_return, {5u}, 32u));
  EXPECT_EQ(3, ExpectReturn(all_return, {9u}, 32u));
}

TEST(IrInterp, DividesAndRemaindersTowardZero) {
  Module module;
  Module remainder;
  ASSERT_TRUE(
      module.Open("int quotient(int a, int b) { return a / b; }", "quotient"));
  ASSERT_TRUE(
      remainder.Open("int rest(int a, int b) { return a % b; }", "rest"));

  EXPECT_EQ(3, ExpectReturn(module, {7u, 2u}, 32u));
  EXPECT_EQ(-3, ExpectReturn(module, {static_cast<uint64_t>(-7), 2u}, 32u));
  EXPECT_EQ(-3, ExpectReturn(module, {7u, static_cast<uint64_t>(-2)}, 32u));
  EXPECT_EQ(3,
            ExpectReturn(module,
                         {static_cast<uint64_t>(-7), static_cast<uint64_t>(-2)},
                         32u));

  EXPECT_EQ(1, ExpectReturn(remainder, {7u, 2u}, 32u));
  EXPECT_EQ(-1, ExpectReturn(remainder, {static_cast<uint64_t>(-7), 2u}, 32u));
  EXPECT_EQ(1, ExpectReturn(remainder, {7u, static_cast<uint64_t>(-2)}, 32u));

  ExpectUndefined(module, {7u, 0u}, QL_IR_INTERP_UB_GUARD_FAILED);
  ExpectUndefined(remainder, {7u, 0u}, QL_IR_INTERP_UB_GUARD_FAILED);
  ExpectUndefined(module, {0x80000000u, static_cast<uint64_t>(-1)},
                  QL_IR_INTERP_UB_GUARD_FAILED);
}

TEST(IrInterp, AppliesShiftRangeAndSignRules) {
  Module logical;
  Module arithmetic;
  ASSERT_TRUE(logical.Open(
      "unsigned int left(unsigned int a, int b) { return a << b; }", "left"));
  ASSERT_TRUE(
      arithmetic.Open("int right(int a, int b) { return a >> b; }", "right"));

  EXPECT_EQ(static_cast<int64_t>(static_cast<int32_t>(0x80000000u)),
            ExpectReturn(logical, {1u, 31u}, 32u));
  ExpectUndefined(logical, {1u, 32u}, QL_IR_INTERP_UB_GUARD_FAILED);
  ExpectUndefined(logical, {1u, static_cast<uint64_t>(-1)},
                  QL_IR_INTERP_UB_GUARD_FAILED);

  EXPECT_EQ(-1, ExpectReturn(arithmetic, {static_cast<uint64_t>(-1), 3u}, 32u));
  EXPECT_EQ(-2,
            ExpectReturn(arithmetic, {static_cast<uint64_t>(-16), 3u}, 32u));
  EXPECT_EQ(2, ExpectReturn(arithmetic, {16u, 3u}, 32u));
}

TEST(IrInterp, CarriesSignedMultiplicationThroughDoubleWidth) {
  Module module;
  ASSERT_TRUE(module.Open(
      "long long product(long long a, long long b) { return a * b; }",
      "product"));
  EXPECT_EQ(6, ExpectReturn(module, {2u, 3u}, 64u));
  EXPECT_EQ(INT64_C(-6),
            ExpectReturn(module, {static_cast<uint64_t>(-2), 3u}, 64u));
  EXPECT_EQ(
      INT64_C(4611686014132420609),
      ExpectReturn(module, {UINT64_C(2147483647), UINT64_C(2147483647)}, 64u));
  /* 3037000500^2 exceeds the 64-bit signed range by one step. */
  ExpectUndefined(module, {UINT64_C(3037000500), UINT64_C(3037000500)},
                  QL_IR_INTERP_UB_GUARD_FAILED);
}

TEST(IrInterp, ShortCircuitDefinednessDoesNotInventUndefinedBehaviour) {
  Module module;
  ASSERT_TRUE(module.Open(
      "int safe(int a, int b) { return b != 0 && a / b > 1; }", "safe"));
  /* The lowering evaluates a / b eagerly and short-circuits only its
     definedness. A zero divisor must therefore still return 0, not report
     undefined behaviour. */
  EXPECT_EQ(0, ExpectReturn(module, {5u, 0u}, 32u));
  EXPECT_EQ(1, ExpectReturn(module, {5u, 2u}, 32u));
  EXPECT_EQ(0, ExpectReturn(module, {1u, 2u}, 32u));
}

TEST(IrInterp, FollowsBranchesAndMergesThroughPhi) {
  Module module;
  ASSERT_TRUE(module.Open("int classify(int a) {\n"
                          "  int x;\n"
                          "  if (a < 3) { x = 0; }\n"
                          "  else if (a < 6) { x = 1; }\n"
                          "  else { x = 4; }\n"
                          "  return x;\n"
                          "}",
                          "classify"));
  EXPECT_EQ(0, ExpectReturn(module, {0u}, 32u));
  EXPECT_EQ(0, ExpectReturn(module, {2u}, 32u));
  EXPECT_EQ(1, ExpectReturn(module, {3u}, 32u));
  EXPECT_EQ(1, ExpectReturn(module, {5u}, 32u));
  EXPECT_EQ(4, ExpectReturn(module, {6u}, 32u));
  EXPECT_EQ(0, ExpectReturn(module, {static_cast<uint64_t>(-1)}, 32u));
}

TEST(IrInterp, ExecutesForWhileAndDoLoopsWithBreakAndContinue) {
  Module for_loop;
  ASSERT_TRUE(for_loop.Open("int sum_for(int n) {\n"
                            "  int sum = 0;\n"
                            "  for (int i = 0; i < n; ++i) { sum += i; }\n"
                            "  return sum;\n"
                            "}",
                            "sum_for"));
  EXPECT_EQ(0, ExpectReturn(for_loop, {0u}, 32u));
  EXPECT_EQ(10, ExpectReturn(for_loop, {5u}, 32u));

  Module while_loop;
  ASSERT_TRUE(while_loop.Open("int sum_while(int n) {\n"
                              "  int i = 0;\n"
                              "  int sum = 0;\n"
                              "  while (i < n) {\n"
                              "    ++i;\n"
                              "    if (i == 2) continue;\n"
                              "    if (i == 5) break;\n"
                              "    sum += i;\n"
                              "  }\n"
                              "  return sum;\n"
                              "}",
                              "sum_while"));
  EXPECT_EQ(0, ExpectReturn(while_loop, {0u}, 32u));
  EXPECT_EQ(8, ExpectReturn(while_loop, {4u}, 32u));
  EXPECT_EQ(8, ExpectReturn(while_loop, {9u}, 32u));

  Module do_loop;
  ASSERT_TRUE(do_loop.Open("int sum_do(int n) {\n"
                           "  int i = 0;\n"
                           "  int sum = 0;\n"
                           "  do { sum += i; ++i; } while (i < n);\n"
                           "  return sum;\n"
                           "}",
                           "sum_do"));
  EXPECT_EQ(0, ExpectReturn(do_loop, {0u}, 32u));
  EXPECT_EQ(10, ExpectReturn(do_loop, {5u}, 32u));
}

TEST(IrInterp, StepLimitStopsAnEmptyCyclicLoop) {
  Module module;
  ql_ir_interp_options_v1 options{};
  ql_ir_interp_result_v1 result{};
  ql_error error{};

  ASSERT_TRUE(module.Open("void spin(void) { for (;;) { } }", "spin"));
  ql_ir_interp_options_init(&options);
  options.step_limit = 8u;
  result.struct_size = sizeof(result);
  ASSERT_EQ(QL_STATUS_OK, ql_ir_interp_run(nullptr, module.ir(), nullptr, 0u,
                                           &options, &result, &error))
      << error.message;
  EXPECT_EQ(QL_IR_INTERP_OUTCOME_STEP_LIMIT, result.outcome);
  EXPECT_EQ(9u, result.steps);
}

TEST(IrInterp, DistinguishesAConstPointeeFromAConstPointerObject) {
  Module module;
  ASSERT_TRUE(module.Open("int const_pointee(int a) {\n"
                          "  const int *p = (void *)0;\n"
                          "  p = (void *)0;\n"
                          "  const int unused;\n"
                          "  return a;\n"
                          "}",
                          "const_pointee"));
  EXPECT_EQ(7, ExpectReturn(module, {7u}, 32u));
}

TEST(IrInterp, NarrowsAndPromotesAcrossIntegerRanks) {
  Module module;
  ASSERT_TRUE(module.Open("short narrow(short a, short b) { return a + b; }",
                          "narrow"));
  EXPECT_EQ(3, ExpectReturn(module, {1u, 2u}, 16u));
  /* Both operands promote to int, so this sum does not overflow; the
     conversion back to short is implementation-defined truncation, not
     undefined behaviour. */
  EXPECT_EQ(static_cast<int64_t>(static_cast<int16_t>(0x7fff + 1)),
            ExpectReturn(module, {0x7fffu, 1u}, 16u));
}

/* A guard that checks the wrong predicate is exactly what the verifier cannot
   see. The interpreter must catch it on the input that exposes it. */
TEST(IrInterp, ReportsAGuardThatIsTooWeakToCoverItsOperation) {
  ql_ir_builder *builder = nullptr;
  ql_artifact *artifact = nullptr;
  ql_ir *ir = nullptr;
  ql_error error{};
  ql_ir_verify_report_v1 report{};
  ql_ir_interp_result_v1 result{};
  ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_create(nullptr, &builder, &error));

  ql_ir_type_definition_v1 type{};
  ql_ir_type_id bool_type = QL_IR_INVALID_TYPE_ID;
  ql_ir_type_id bv32 = QL_IR_INVALID_TYPE_ID;
  ql_ir_type_definition_init(&type, QL_IR_TYPE_BOOL);
  type.bit_width = 1u;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_type(builder, &type, &bool_type, &error));
  ql_ir_type_definition_init(&type, QL_IR_TYPE_BIT_VECTOR);
  type.bit_width = 32u;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_type(builder, &type, &bv32, &error));
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_set_function(builder, "weak", 4u, bv32, &error));

  ql_ir_value_id a = QL_IR_INVALID_VALUE_ID;
  ql_ir_value_id b = QL_IR_INVALID_VALUE_ID;
  ql_ir_value_id zero = QL_IR_INVALID_VALUE_ID;
  const uint8_t zero_bytes[4] = {0u, 0u, 0u, 0u};
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_parameter(builder, bv32, "a", 1u, &a, &error));
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_parameter(builder, bv32, "b", 1u, &b, &error));
  ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_add_constant(builder, bv32, zero_bytes,
                                                     4u, &zero, &error));
  ql_ir_block_id entry = QL_IR_INVALID_BLOCK_ID;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_add_block(builder, "entry", 5u, &entry, &error));
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_set_entry_block(builder, entry, &error));

  ql_ir_instruction_definition_v1 definition{};
  ql_ir_instruction_id instruction = QL_IR_INVALID_INSTRUCTION_ID;
  ql_ir_value_id quotient = QL_IR_INVALID_VALUE_ID;
  ql_ir_value_id wrong = QL_IR_INVALID_VALUE_ID;
  ql_ir_value_id operands[2] = {a, b};
  ql_ir_type_id result_type = bv32;
  ql_ir_instruction_definition_init(&definition, QL_IR_OPCODE_SDIV);
  definition.operands = operands;
  definition.operand_count = 2u;
  definition.result_types = &result_type;
  definition.result_count = 1u;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_append_instruction(builder, entry, &definition,
                                             &instruction, &quotient, &error));

  /* The guard asks whether the dividend is non-zero, which says nothing
     about the divisor. */
  ql_ir_value_id wrong_operands[2] = {a, zero};
  result_type = bool_type;
  ql_ir_instruction_definition_init(&definition, QL_IR_OPCODE_NE);
  definition.operands = wrong_operands;
  definition.operand_count = 2u;
  definition.result_types = &result_type;
  definition.result_count = 1u;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_append_instruction(builder, entry, &definition,
                                             &instruction, &wrong, &error));
  ql_ir_instruction_definition_init(&definition, QL_IR_OPCODE_UB_GUARD);
  definition.operands = &wrong;
  definition.operand_count = 1u;
  definition.effects = QL_IR_EFFECT_UNDEFINED_BEHAVIOR;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_append_instruction(builder, entry, &definition,
                                             &instruction, nullptr, &error));
  ql_ir_terminator_definition_v1 terminator{};
  ql_ir_terminator_definition_init(&terminator, QL_IR_TERMINATOR_RETURN);
  terminator.return_value = quotient;
  ASSERT_EQ(QL_STATUS_OK,
            ql_ir_builder_set_terminator(builder, entry, &terminator, &error));
  ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_finish(builder, &artifact, &error));
  ql_ir_builder_destroy(builder);
  ASSERT_EQ(QL_STATUS_OK, ql_ir_open(nullptr, artifact, &ir, &error));

  /* The verifier is satisfied: a guard does stand between the division and
     the return. Only execution can show the predicate is the wrong one. */
  report.struct_size = sizeof(report);
  EXPECT_EQ(QL_STATUS_OK, ql_ir_verify(nullptr, ir, &report, &error))
      << report.message;

  const uint8_t one_bytes[4] = {1u, 0u, 0u, 0u};
  ql_ir_interp_input_v1 inputs[2];
  ql_ir_interp_input_init(&inputs[0]);
  inputs[0].value = a;
  inputs[0].data = one_bytes;
  inputs[0].size = 4u;
  ql_ir_interp_input_init(&inputs[1]);
  inputs[1].value = b;
  inputs[1].data = zero_bytes;
  inputs[1].size = 4u;
  result.struct_size = sizeof(result);
  EXPECT_EQ(QL_STATUS_OK,
            ql_ir_interp_run(nullptr, ir, inputs, 2u, nullptr, &result, &error))
      << error.message;
  EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR, result.outcome);
  EXPECT_EQ(QL_IR_INTERP_UB_GUARD_INSUFFICIENT, result.ub_reason);

  ql_ir_release(ir);
  ql_artifact_release(artifact);
}

TEST(IrInterp, RefusesRunsItCannotJustify) {
  Module module;
  ql_ir_interp_result_v1 result{};
  ql_error error{};
  ASSERT_TRUE(module.Open("int pair(int a, int b) { return a - b; }", "pair"));
  ASSERT_EQ(2u, module.parameters().size());

  const std::vector<uint8_t> four = Encode(1u, 32u);
  ql_ir_interp_input_v1 single{};
  ql_ir_interp_input_init(&single);
  single.value = module.parameters()[0];
  single.data = four.data();
  single.size = four.size();

  result.struct_size = sizeof(result);
  EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
            ql_ir_interp_run(nullptr, module.ir(), &single, 1u, nullptr,
                             &result, &error));

  ql_ir_interp_input_v1 twice[2] = {single, single};
  result.struct_size = sizeof(result);
  EXPECT_EQ(QL_STATUS_ALREADY_EXISTS,
            ql_ir_interp_run(nullptr, module.ir(), twice, 2u, nullptr, &result,
                             &error));

  ql_ir_interp_input_v1 wrong_size[2] = {single, single};
  wrong_size[1].value = module.parameters()[1];
  wrong_size[1].size = 3u;
  result.struct_size = sizeof(result);
  EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
            ql_ir_interp_run(nullptr, module.ir(), wrong_size, 2u, nullptr,
                             &result, &error));
}

} // namespace
