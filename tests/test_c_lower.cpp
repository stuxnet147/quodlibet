#include "quodlibet/c_lower.h"

#include <cstddef>
#include <cstring>

#include <gtest/gtest.h>

namespace {

class LoweredFunction {
public:
    ~LoweredFunction() {
        ql_c_lower_result_destroy(result_);
        ql_c_frontend_unit_destroy(unit_);
    }

    ql_status Lower(const char *source, const char *name, ql_error *error) {
        ql_c_function_view function{};
        const std::size_t source_size = std::strlen(source);

        ql_status status = ql_c_frontend_analyze(
            nullptr, source, source_size, &unit_, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        function.struct_size = sizeof(function);
        status = ql_c_frontend_select_function(
            unit_, name, std::strlen(name), &function, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        return ql_c_lower_selected_function(
            nullptr, source, source_size, unit_, &function, &result_, error);
    }

    ql_c_lower_result *get() const { return result_; }

private:
    ql_c_frontend_unit *unit_ = nullptr;
    ql_c_lower_result *result_ = nullptr;
};

ql_c_lower_result_view_v1 ResultView(const ql_c_lower_result *result) {
    ql_c_lower_result_view_v1 view{};
    ql_error error{};
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK,
              ql_c_lower_result_get_view(result, &view, &error))
        << error.message;
    return view;
}

ql_c_lower_diagnostic_view_v1 FirstDiagnostic(
    const ql_c_lower_result *result) {
    ql_c_lower_diagnostic_view_v1 diagnostic{};
    ql_error error{};
    diagnostic.struct_size = sizeof(diagnostic);
    EXPECT_EQ(QL_STATUS_OK,
              ql_c_lower_result_diagnostic_at(result, 0u, &diagnostic,
                                              &error))
        << error.message;
    return diagnostic;
}

class IrHandle {
public:
    ~IrHandle() { ql_ir_release(ir_); }
    ql_ir **output() { return &ir_; }
    ql_ir *get() const { return ir_; }

private:
    ql_ir *ir_ = nullptr;
};

std::size_t CountOpcode(const ql_ir *ir, std::size_t instruction_count,
                        ql_ir_opcode opcode) {
    std::size_t count = 0u;
    for (std::size_t index = 0u; index < instruction_count; ++index) {
        ql_ir_instruction_view_v1 instruction{};
        ql_error error{};
        instruction.struct_size = sizeof(instruction);
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_instruction_at(ir, index, &instruction, &error))
            << error.message;
        if (instruction.opcode == opcode) {
            ++count;
        }
    }
    return count;
}

TEST(CLower, LowersBranchesAssignmentsAndMultipleReturnsToTypedSsa) {
    constexpr char source[] =
        "int choose(int a, int b) {\n"
        "  int value = a + 1;\n"
        "  if (b) { value = value / b; }\n"
        "  else { value = -value; }\n"
        "  if (value < 0) return -value;\n"
        "  return value;\n"
        "}\n";
    LoweredFunction lowered;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, lowered.Lower(source, "choose", &error))
        << error.message;
    const ql_c_lower_result_view_v1 result = ResultView(lowered.get());
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, result.support);
    ASSERT_NE(nullptr, result.ir_artifact);
    EXPECT_EQ(0u, result.diagnostic_count);

    IrHandle opened;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_open(nullptr, result.ir_artifact, opened.output(), &error))
        << error.message;
    ql_ir_view_v1 ir{};
    ir.struct_size = sizeof(ir);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_get_view(opened.get(), &ir, &error))
        << error.message;
    EXPECT_STREQ("choose", ir.function_name);
    EXPECT_GE(ir.block_count, 7u);
    EXPECT_GE(CountOpcode(opened.get(), ir.instruction_count,
                          QL_IR_OPCODE_PHI),
              1u);
    EXPECT_GE(CountOpcode(opened.get(), ir.instruction_count,
                          QL_IR_OPCODE_UB_GUARD),
              3u);
}

TEST(CLower, MakesIntegerPromotionsAndSignednessOperationsExplicit) {
    constexpr char source[] =
        "unsigned long widen(unsigned x, _Bool take) {\n"
        "  unsigned long value = x;\n"
        "  if (take) value = value + 1u;\n"
        "  return value;\n"
        "}\n";
    LoweredFunction lowered;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, lowered.Lower(source, "widen", &error))
        << error.message;
    const ql_c_lower_result_view_v1 result = ResultView(lowered.get());
    if (result.support != QL_C_LOWER_SUPPORTED) {
        const ql_c_lower_diagnostic_view_v1 diagnostic =
            FirstDiagnostic(lowered.get());
        FAIL() << diagnostic.construct_kind << ": " << diagnostic.message;
    }

    IrHandle opened;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_open(nullptr, result.ir_artifact, opened.output(), &error))
        << error.message;
    ql_ir_view_v1 ir{};
    ir.struct_size = sizeof(ir);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_get_view(opened.get(), &ir, &error));
    EXPECT_GE(CountOpcode(opened.get(), ir.instruction_count,
                          QL_IR_OPCODE_ZEXT),
              1u);
    EXPECT_GE(CountOpcode(opened.get(), ir.instruction_count,
                          QL_IR_OPCODE_PHI),
              1u);
}

TEST(CLower, EmitsOneExplicitDefinednessGuardForNestedRiskyExpression) {
    constexpr char source[] =
        "int risky(int a, int b) { return ((a + b) / b) << b; }";
    LoweredFunction lowered;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, lowered.Lower(source, "risky", &error))
        << error.message;
    const ql_c_lower_result_view_v1 result = ResultView(lowered.get());
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, result.support);

    IrHandle opened;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_open(nullptr, result.ir_artifact, opened.output(), &error))
        << error.message;
    ql_ir_view_v1 ir{};
    ir.struct_size = sizeof(ir);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_get_view(opened.get(), &ir, &error));
    EXPECT_EQ(1u, CountOpcode(opened.get(), ir.instruction_count,
                             QL_IR_OPCODE_UB_GUARD));
    for (std::size_t index = 0u; index < ir.instruction_count; ++index) {
        ql_ir_instruction_view_v1 instruction{};
        instruction.struct_size = sizeof(instruction);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_instruction_at(opened.get(), index, &instruction,
                                       &error));
        if (instruction.opcode == QL_IR_OPCODE_UB_GUARD) {
            EXPECT_EQ(QL_IR_EFFECT_UNDEFINED_BEHAVIOR,
                      instruction.effects);
            EXPECT_EQ(1u, instruction.operand_count);
            EXPECT_EQ(0u, instruction.result_count);
        }
    }
}

TEST(CLower, PreservesShortCircuitDefinedness) {
    constexpr char source[] =
        "int safe_divisor(int x) { return x != 0 && 10 / x; }";
    LoweredFunction lowered;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, lowered.Lower(source, "safe_divisor", &error))
        << error.message;
    const ql_c_lower_result_view_v1 result = ResultView(lowered.get());
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, result.support);
    IrHandle opened;
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_open(nullptr, result.ir_artifact, opened.output(), &error))
        << error.message;
}

TEST(CLower, CoversRemainingIntegerOperatorsAtThirtyTwoAndSixtyFourBits) {
    constexpr const char *sources[] = {
        "long f(long a, long b) { long x = a * b; x = x - b; "
        "x = x % b; x = x >> b; return (x & b) | (x ^ ~a); }",
        "unsigned f(unsigned a, unsigned b) { "
        "return (a << b) + (a / b); }",
        "unsigned long f(void) { return 0xffffffffffffffffUL; }",
        "unsigned f(void) { return 0b101u | 077u; }",
    };

    for (const char *source : sources) {
        SCOPED_TRACE(source);
        LoweredFunction lowered;
        ql_error error{};
        ASSERT_EQ(QL_STATUS_OK, lowered.Lower(source, "f", &error))
            << error.message;
        const ql_c_lower_result_view_v1 result = ResultView(lowered.get());
        ASSERT_EQ(QL_C_LOWER_SUPPORTED, result.support);
        IrHandle opened;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_open(nullptr, result.ir_artifact, opened.output(),
                             &error))
            << error.message;
    }
}

struct UnsupportedCase {
    const char *source;
    const char *name;
    ql_c_lower_diagnostic_code code;
    const char *construct;
};

TEST(CLower, RejectsUnmodeledSemanticSurfacesAsUnknown) {
    const UnsupportedCase cases[] = {
        {"int load(const int *p) { return *p; }", "load",
         QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER,
         "function_definition"},
        {"int invoke(int x) { return helper(x); }", "invoke",
         QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL, "call_expression"},
        {"int spin(int x) { while (x) x = x - 1; return x; }", "spin",
         QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_LOOP, "while_statement"},
        {"int observe(int x) { volatile int y = x; return y; }", "observe",
         QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_VOLATILE_OR_ATOMIC,
         "type_qualifier"},
        {"_Noreturn int declared_no_return(int x) { return x; }",
         "declared_no_return",
         QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CONTROL_FLOW,
         "type_qualifier"},
    };

    for (const UnsupportedCase &test_case : cases) {
        SCOPED_TRACE(test_case.name);
        LoweredFunction lowered;
        ql_error error{};
        ASSERT_EQ(QL_STATUS_OK,
                  lowered.Lower(test_case.source, test_case.name, &error))
            << error.message;
        const ql_c_lower_result_view_v1 result = ResultView(lowered.get());
        ASSERT_EQ(QL_C_LOWER_UNKNOWN, result.support);
        ASSERT_EQ(nullptr, result.ir_artifact);
        ASSERT_EQ(1u, result.diagnostic_count);
        const ql_c_lower_diagnostic_view_v1 diagnostic =
            FirstDiagnostic(lowered.get());
        EXPECT_EQ(test_case.code, diagnostic.code);
        EXPECT_STREQ(test_case.construct, diagnostic.construct_kind);
        EXPECT_LT(diagnostic.range.start_byte, diagnostic.range.end_byte);
    }
}

TEST(CLower, RejectsUninitializedReadAndReachableMissingReturn) {
    const UnsupportedCase cases[] = {
        {"int uninit(int x) { int y; if (x) y = 1; return y; }", "uninit",
         QL_C_LOWER_DIAGNOSTIC_UNINITIALIZED_READ, "identifier"},
        {"int partial(int x) { if (x) return 1; }", "partial",
         QL_C_LOWER_DIAGNOSTIC_MISSING_RETURN, "compound_statement"},
    };

    for (const UnsupportedCase &test_case : cases) {
        SCOPED_TRACE(test_case.name);
        LoweredFunction lowered;
        ql_error error{};
        ASSERT_EQ(QL_STATUS_OK,
                  lowered.Lower(test_case.source, test_case.name, &error))
            << error.message;
        const ql_c_lower_result_view_v1 result = ResultView(lowered.get());
        ASSERT_EQ(QL_C_LOWER_UNKNOWN, result.support);
        ASSERT_EQ(nullptr, result.ir_artifact);
        const ql_c_lower_diagnostic_view_v1 diagnostic =
            FirstDiagnostic(lowered.get());
        EXPECT_EQ(test_case.code, diagnostic.code);
    }
}

TEST(CLower, EnforcesVersionedResultViews) {
    constexpr char source[] = "int one(void) { return 1; }";
    LoweredFunction lowered;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, lowered.Lower(source, "one", &error));

    ql_c_lower_result_view_v1 view{};
    view.struct_size = 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_c_lower_result_get_view(lowered.get(), &view, &error));
}

}  // namespace
