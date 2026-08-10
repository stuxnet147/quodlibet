/* Coverage of the widened C type surface: void functions, typedef names, and
   cast expressions.

   The scalar type model in src/c_types.c is a private header, so it is
   exercised here through the lowering's observable behaviour rather than
   directly. That is also the contract that matters: what the lowering accepts
   and what it refuses. */

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

class Lowered {
public:
    ~Lowered() {
        ql_ir_release(ir_);
        ql_c_lower_result_destroy(result_);
        ql_c_frontend_unit_destroy(unit_);
    }

    ql_status Lower(const char *source, const char *name) {
        ql_c_function_view function{};
        ql_c_lower_result_view_v1 view{};
        ql_error error{};
        const std::size_t size = std::strlen(source);
        ql_status status =
            ql_c_frontend_analyze(nullptr, source, size, &unit_, &error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        function.struct_size = sizeof(function);
        status = ql_c_frontend_select_function(unit_, name, std::strlen(name),
                                               &function, &error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_c_lower_selected_function(nullptr, source, size, unit_,
                                              &function, &result_, &error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        view.struct_size = sizeof(view);
        status = ql_c_lower_result_get_view(result_, &view, &error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        support_ = view.support;
        if (view.support != QL_C_LOWER_SUPPORTED) {
            return QL_STATUS_OK;
        }
        status = ql_ir_open(nullptr, view.ir_artifact, &ir_, &error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        ql_ir_verify_report_v1 report{};
        report.struct_size = sizeof(report);
        EXPECT_EQ(QL_STATUS_OK, ql_ir_verify(nullptr, ir_, &report, &error))
            << ql_ir_verify_code_string(report.code) << ": "
            << report.message;
        return QL_STATUS_OK;
    }

    ql_c_lower_support support() const { return support_; }
    ql_ir *ir() const { return ir_; }

    ql_c_lower_diagnostic_code FirstDiagnostic() const {
        ql_c_lower_diagnostic_view_v1 diagnostic{};
        ql_error error{};
        diagnostic.struct_size = sizeof(diagnostic);
        EXPECT_EQ(QL_STATUS_OK,
                  ql_c_lower_result_diagnostic_at(result_, 0u, &diagnostic,
                                                  &error))
            << error.message;
        return diagnostic.code;
    }

private:
    ql_c_frontend_unit *unit_ = nullptr;
    ql_c_lower_result *result_ = nullptr;
    ql_ir *ir_ = nullptr;
    ql_c_lower_support support_ = QL_C_LOWER_UNKNOWN;
};

std::vector<uint8_t> Encode(uint64_t value, uint32_t width) {
    std::vector<uint8_t> bytes((width + 7u) / 8u, 0u);
    for (std::size_t index = 0u; index < bytes.size() && index < 8u;
         ++index) {
        bytes[index] = static_cast<uint8_t>((value >> (index * 8u)) & 0xffu);
    }
    return bytes;
}

/* Runs the module on the given arguments, in parameter-table order. */
ql_ir_interp_result_v1 RunModule(ql_ir *ir,
                                 const std::vector<uint64_t> &arguments) {
    ql_ir_view_v1 view{};
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ql_ir_interp_input_v1> inputs;
    ql_ir_interp_result_v1 result{};
    ql_error error{};
    std::size_t next = 0u;

    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK, ql_ir_get_view(ir, &view, &error));
    for (std::size_t index = 0u; index < view.value_count; ++index) {
        ql_ir_value_view_v1 value{};
        ql_ir_type_view_v1 type{};
        value.struct_size = sizeof(value);
        EXPECT_EQ(QL_STATUS_OK, ql_ir_value_at(ir, index, &value, &error));
        if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
            continue;
        }
        type.struct_size = sizeof(type);
        EXPECT_EQ(QL_STATUS_OK, ql_ir_type_at(ir, value.type, &type, &error));
        EXPECT_LT(next, arguments.size());
        storage.push_back(Encode(arguments[next++],
                                 type.kind == QL_IR_TYPE_BOOL
                                     ? 1u
                                     : type.bit_width));
        inputs.push_back(ql_ir_interp_input_v1{});
        ql_ir_interp_input_init(&inputs.back());
        inputs.back().value = value.id;
    }
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        inputs[index].data = storage[index].data();
        inputs[index].size = storage[index].size();
    }
    result.struct_size = sizeof(result);
    EXPECT_EQ(QL_STATUS_OK,
              ql_ir_interp_run(nullptr, ir,
                               inputs.empty() ? nullptr : inputs.data(),
                               inputs.size(), nullptr, &result, &error))
        << error.message;
    return result;
}

int64_t ReturnedSigned(const ql_ir_interp_result_v1 &result, uint32_t width) {
    uint64_t raw = 0u;
    for (std::size_t index = 0u; index < result.value_size && index < 8u;
         ++index) {
        raw |= static_cast<uint64_t>(result.value[index]) << (index * 8u);
    }
    if (width < 64u && (raw >> (width - 1u)) != 0u) {
        raw |= ~((UINT64_C(1) << width) - UINT64_C(1));
    }
    return static_cast<int64_t>(raw);
}

void ExpectUnknown(const char *source, const char *name,
                   ql_c_lower_diagnostic_code expected) {
    Lowered lowered;
    SCOPED_TRACE(source);
    ASSERT_EQ(QL_STATUS_OK, lowered.Lower(source, name));
    ASSERT_EQ(QL_C_LOWER_UNKNOWN, lowered.support());
    EXPECT_EQ(expected, lowered.FirstDiagnostic());
}

TEST(CLowerTypes, LowersVoidFunctionsWithAnImplicitReturn) {
    Lowered lowered;
    ql_ir_view_v1 view{};
    ql_ir_type_view_v1 type{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower("void nothing(int a) { int b; b = a + 1; }",
                            "nothing"));
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_get_view(lowered.ir(), &view, &error));
    type.struct_size = sizeof(type);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_type_at(lowered.ir(), view.return_type, &type, &error));
    EXPECT_EQ(QL_IR_TYPE_VOID, type.kind);

    const ql_ir_interp_result_v1 result = RunModule(lowered.ir(), {7u});
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_RETURN, result.outcome);
    EXPECT_EQ(0u, result.has_value);
}

TEST(CLowerTypes, LowersAnExplicitBareReturnOnEveryPath) {
    Lowered lowered;
    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower("void early(int a) {\n"
                            "  if (a < 0) { return; }\n"
                            "  if (a > 10) { return; }\n"
                            "}",
                            "early"));
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
    for (uint64_t argument : {UINT64_C(0), UINT64_C(20),
                              static_cast<uint64_t>(-5)}) {
        const ql_ir_interp_result_v1 result = RunModule(lowered.ir(), {argument});
        EXPECT_EQ(QL_IR_INTERP_OUTCOME_RETURN, result.outcome);
        EXPECT_EQ(0u, result.has_value);
    }
}

TEST(CLowerTypes, RefusesAValueReturnedFromAVoidFunction) {
    ExpectUnknown("void wrong(int a) { return a; }", "wrong",
                  QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR);
}

TEST(CLowerTypes, RefusesVoidWhereAnObjectTypeIsRequired) {
    ExpectUnknown("int bad(void a) { return 0; }", "bad",
                  QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR);
}

TEST(CLowerTypes, ResolvesTypedefNamesThroughTheirDeclaredChain) {
    Lowered lowered;
    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower("typedef int TYP_0;\n"
                            "typedef TYP_0 TYP_1;\n"
                            "typedef TYP_1 TYP_2;\n"
                            "TYP_2 chained(TYP_1 a, TYP_0 b) {\n"
                            "  TYP_2 c;\n"
                            "  c = a + b;\n"
                            "  return c;\n"
                            "}",
                            "chained"));
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
    const ql_ir_interp_result_v1 result = RunModule(lowered.ir(), {4u, 5u});
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, result.outcome);
    EXPECT_EQ(9, ReturnedSigned(result, 32u));
}

TEST(CLowerTypes, KeepsTheSignednessAndWidthATypedefStandsFor) {
    Lowered lowered;
    /* If the typedef resolved to signed int, the shift would report undefined
       behaviour instead of wrapping. */
    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower("typedef unsigned int TYP_0;\n"
                            "TYP_0 shift(TYP_0 a) { return a << 31; }",
                            "shift"));
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
    const ql_ir_interp_result_v1 result = RunModule(lowered.ir(), {3u});
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, result.outcome)
        << ql_ir_interp_ub_reason_string(result.ub_reason);
    EXPECT_EQ(static_cast<int64_t>(static_cast<int32_t>(3u << 31)),
              ReturnedSigned(result, 32u));
}

TEST(CLowerTypes, ResolvesATypedefThatNamesAPointer) {
    Lowered lowered;
    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower("typedef int *TYP_0;\n"
                            "int deref(TYP_0 a) { return *a; }",
                            "deref"));
    EXPECT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
}

TEST(CLowerTypes, NamesTheRealObstacleBehindATypedef) {
    /* The obstacle a typedef hides has to be reported as what it is, or the
       coverage tables blame the wrong thing. */
    ExpectUnknown("typedef int TYP_0[4];\n"
                  "int indexed(TYP_0 a) { return 0; }",
                  "indexed", QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER);
    ExpectUnknown("struct TYP_0 { int f; };\n"
                  "typedef struct TYP_0 TYP_1;\n"
                  "int aggregate(TYP_1 a) { return 0; }",
                  "aggregate", QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE);
}

TEST(CLowerTypes, RefusesATypeNameTheUnitNeverDeclared) {
    /* Guessing that an undeclared name means int would be a guess about
       semantics, and a wrong one changes the answer about the function. */
    ExpectUnknown("int unknown_name(TYP_UNKNOWN a) { return 0; }",
                  "unknown_name", QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE);
}

/* The preamble names are the ones resolved without the unit declaring them,
   and that is not an exception to the rule above. Every AnghaBench source
   opens with the same fixed block, `typedef long scalar_t__;` among it;
   record extraction keeps only the type and callee context and drops that
   block, so the definitions are known even though the extracted unit no
   longer carries them. */
TEST(CLowerTypes, ResolvesTheTypedefTheCorpusPreambleDeclares) {
    Lowered lowered;
    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower("scalar_t__ widened(scalar_t__ a) "
                            "{ return a + 1; }",
                            "widened"));
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
    /* It is a signed 64-bit integer, so this addition wraps nowhere near
       where a 32-bit one would. */
    EXPECT_EQ(INT64_C(2147483648),
              ReturnedSigned(RunModule(lowered.ir(),
                                       {UINT64_C(2147483647)}), 64u));
}

TEST(CLowerTypes, ResolvesTheRestOfTheCorpusPreamble) {
    /* The same preamble also spells `typedef unsigned long size_t;` and
       `typedef long intptr_t; typedef unsigned long uintptr_t;`. Width and
       signedness are both observable here: an unsigned 64-bit `size_t`
       shifted left by 32 keeps its bits, and a signed 64-bit `intptr_t`
       carries its sign where a 32-bit one would have dropped it. */
    struct Case {
        const char *source;
        const char *name;
        std::vector<uint64_t> arguments;
        int64_t expected;
    };
    const Case cases[] = {
        {"size_t high(size_t a) { return a << 32; }", "high",
         {UINT64_C(3)}, INT64_C(12884901888)},
        {"intptr_t negate(intptr_t a) { return -a; }", "negate",
         {UINT64_C(4294967296)}, INT64_C(-4294967296)},
        {"uintptr_t shift(uintptr_t a) { return a >> 32; }", "shift",
         {UINT64_C(12884901888)}, INT64_C(3)},
    };
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.source);
        ASSERT_EQ(QL_STATUS_OK, lowered.Lower(item.source, item.name));
        ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
        EXPECT_EQ(item.expected,
                  ReturnedSigned(RunModule(lowered.ir(), item.arguments),
                                 64u));
    }
}

TEST(CLowerTypes, LetsTheUnitOverrideAPreambleTypedef) {
    /* The fallback is for the preamble extraction removed, not an override:
       a unit that declares `size_t` itself means what it says. */
    Lowered lowered;
    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower("typedef unsigned short size_t;\n"
                            "int narrow_size(size_t a) { return a + 1; }",
                            "narrow_size"));
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
    /* An unsigned short promotes to int, so this is 65536 rather than the
       zero a 16-bit wrap would give. */
    EXPECT_EQ(65536, ReturnedSigned(RunModule(lowered.ir(),
                                              {UINT64_C(65535)}), 32u));
}

TEST(CLowerTypes, LetsTheUnitOverrideTheCorpusTypedef) {
    /* A unit that declares the name itself means what it says. The table is
       a fallback for the preamble extraction removed, not an override. */
    Lowered lowered;
    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower("typedef short scalar_t__;\n"
                            "int narrow(scalar_t__ a) { return a + 1; }",
                            "narrow"));
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
    /* A short parameter promotes to int, so this is 32768 rather than the
       negative a 16-bit wrap would give. */
    EXPECT_EQ(32768, ReturnedSigned(RunModule(lowered.ir(),
                                              {UINT64_C(32767)}), 32u));
}

TEST(CLowerTypes, StopsOnATypedefChainThatDoesNotTerminate) {
    /* Tree-sitter accepts `typedef A B; typedef B A;` as a translation unit,
       so resolution must bound itself rather than loop. */
    ExpectUnknown("typedef TYP_1 TYP_0;\n"
                  "typedef TYP_0 TYP_1;\n"
                  "int cyclic(TYP_0 a) { return 0; }",
                  "cyclic", QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE);
}

TEST(CLowerTypes, LowersCastsBetweenScalarTypes) {
    Lowered lowered;
    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower(
                  "int truncating(int a) { return (short)(a + 1); }",
                  "truncating"));
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
    EXPECT_EQ(static_cast<int64_t>(static_cast<int16_t>(0x8000)),
              ReturnedSigned(RunModule(lowered.ir(), {0x7fffu}), 32u));
    EXPECT_EQ(6, ReturnedSigned(RunModule(lowered.ir(), {5u}), 32u));
}

TEST(CLowerTypes, CastChangesSignednessBeforeTheOperationThatFollows) {
    Lowered lowered;
    /* Casting to unsigned makes the comparison unsigned, so a negative
       operand compares greater, not less. */
    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower("int compare(int a) {\n"
                            "  return (unsigned int)a > 5u;\n"
                            "}",
                            "compare"));
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
    EXPECT_EQ(1, ReturnedSigned(RunModule(lowered.ir(),
                                    {static_cast<uint64_t>(-1)}), 32u));
    EXPECT_EQ(0, ReturnedSigned(RunModule(lowered.ir(), {3u}), 32u));
}

TEST(CLowerTypes, CastsThroughATypedefName) {
    Lowered lowered;
    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower("typedef unsigned char TYP_0;\n"
                            "int narrow(int a) { return (TYP_0)a; }",
                            "narrow"));
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
    EXPECT_EQ(0xffu, ReturnedSigned(RunModule(lowered.ir(), {0x1ffu}), 32u));
}

/* `(T)(e)` is a cast when T names a type and a call when it names something
   callable. The grammar cannot tell, and Tree-sitter resolves it toward a
   call, so the lowering has to undo that using the typedef names the unit
   declared. Reading these as calls would file casts under unsupported_call
   and misreport what the corpus contains. */
TEST(CLowerTypes, ReadsAParenthesisedTypedefNameAsACastNotACall) {
    Lowered lowered;
    ASSERT_EQ(QL_STATUS_OK,
              lowered.Lower("typedef short TYP_0;\n"
                            "int folded(int a, int b) {\n"
                            "  return (TYP_0)(a + b);\n"
                            "}",
                            "folded"));
    ASSERT_EQ(QL_C_LOWER_SUPPORTED, lowered.support());
    EXPECT_EQ(static_cast<int64_t>(static_cast<int16_t>(0x8000)),
              ReturnedSigned(RunModule(lowered.ir(), {0x7fffu, 1u}), 32u));
}

TEST(CLowerTypes, LetsAVisibleObjectShadowTheTypedefName) {
    /* With a local named TYP_0 in scope, `(TYP_0)(a)` is a call through that
       object, which this slice does not model. Treating it as a cast would
       silently change the program's meaning. */
    ExpectUnknown("typedef short TYP_0;\n"
                  "int shadowed(int a) {\n"
                  "  int TYP_0;\n"
                  "  TYP_0 = a;\n"
                  "  return (TYP_0)(a);\n"
                  "}",
                  "shadowed", QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_CALL);
}

TEST(CLowerTypes, RefusesCastsItCannotRepresent) {
    /* A cast to a pointer is a reinterpretation this profile does carry: a
       pointer is its address. More indirection stays outside the slice, and
       a void result cannot be used where the return needs an object value. */
    ExpectUnknown("int to_array(int a) { return (int (*)[4])a != 0; }",
                  "to_array", QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER);
    ExpectUnknown("int to_deep(int a) { return (int ***)a != 0; }", "to_deep",
                  QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER);
    ExpectUnknown("int to_void(int a) { return (void)a; }", "to_void",
                  QL_C_LOWER_DIAGNOSTIC_TYPE_ERROR);
}

}  // namespace
