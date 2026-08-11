/* A function that calls itself.

   The name a definition declares is in scope inside its own body, so a direct
   recursive call is a call to a declared function and lowers like one. What it
   is not is an external call: the symbol is the function under comparison, and
   two sides that both name it denote their own definitions rather than one
   shared callee. The lowering says only what the C says; the miter is where
   that difference has to be answered, and `test_proof_smt_calls.cpp` fixes the
   refusal.

   The interpreter cannot recurse into the IR, so these runs hand it the same
   compiled C function the reference calls. That is honest here for the same
   reason it is honest for any other callee specification: the function is
   pure, writes no memory, and its result depends only on its arguments. */

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

extern "C" {
int REC_sum(int n) {
    if (n <= 0) {
        return 0;
    }
    return n + REC_sum(n - 1);
}
int REC_collatz(int n, int steps) {
    if (n <= 1) {
        return steps;
    }
    if ((n & 1) == 0) {
        return REC_collatz(n / 2, steps + 1);
    }
    return REC_collatz(n * 3 + 1, steps + 1);
}
}

struct Lowered {
    ql_c_frontend_unit *unit = nullptr;
    ql_c_lower_result *result = nullptr;
    ql_ir *ir = nullptr;

    Lowered() = default;
    Lowered(const Lowered &) = delete;
    Lowered &operator=(const Lowered &) = delete;
    ~Lowered() {
        ql_ir_release(ir);
        ql_c_lower_result_destroy(result);
        ql_c_frontend_unit_destroy(unit);
    }
};

ql_c_lower_support Lower(Lowered *lowered, const char *source,
                         const char *name, ql_ir **ir) {
    const std::size_t size = std::strlen(source);
    ql_c_function_view function{};
    ql_c_lower_result_view_v1 view{};
    ql_error error{};

    EXPECT_EQ(QL_STATUS_OK, ql_c_frontend_analyze(nullptr, source, size,
                                                  &lowered->unit, &error))
        << error.message;
    function.struct_size = sizeof(function);
    EXPECT_EQ(QL_STATUS_OK,
              ql_c_frontend_select_function(lowered->unit, name,
                                            std::strlen(name), &function,
                                            &error))
        << error.message;
    EXPECT_EQ(QL_STATUS_OK,
              ql_c_lower_selected_function(nullptr, source, size,
                                           lowered->unit, &function,
                                           &lowered->result, &error))
        << error.message;
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK,
              ql_c_lower_result_get_view(lowered->result, &view, &error));
    if (ir != nullptr && view.ir_artifact != nullptr) {
        EXPECT_EQ(QL_STATUS_OK, ql_ir_open(nullptr, view.ir_artifact,
                                           &lowered->ir, &error))
            << error.message;
        *ir = lowered->ir;
    }
    return view.support;
}

std::vector<std::uint8_t> Encode(std::uint64_t value, std::uint32_t width) {
    const std::size_t size = (width + 7u) / 8u;
    std::vector<std::uint8_t> bytes(size, 0u);
    for (std::size_t index = 0u; index < size; ++index) {
        bytes[index] = static_cast<std::uint8_t>((value >> (index * 8u)) &
                                                 0xffu);
    }
    return bytes;
}

std::int32_t Read(const ql_ir_interp_argument_v1 &argument) {
    std::uint32_t raw = 0u;
    const std::uint8_t *bytes =
        static_cast<const std::uint8_t *>(argument.data);
    for (std::size_t index = 0u; index < argument.size && index < 4u;
         ++index) {
        raw |= static_cast<std::uint32_t>(bytes[index]) << (index * 8u);
    }
    return static_cast<std::int32_t>(raw);
}

/* The specification is the compiled function itself. The interpreter is not
   recursing; it is asking what the callee named here returns, and the answer
   is the one the reference would get. */
int QL_CALL InvokeSelf(void *user_data, const char *symbol,
                       const ql_ir_interp_argument_v1 *arguments,
                       std::size_t argument_count, void *result,
                       std::size_t result_size) {
    std::size_t *depth = static_cast<std::size_t *>(user_data);
    std::int32_t returned = 0;

    if (result_size != 4u) {
        ADD_FAILURE() << "an int result should be 4 bytes, not " << result_size;
        return 0;
    }
    if (std::strcmp(symbol, "REC_sum") == 0 && argument_count == 1u) {
        returned = REC_sum(Read(arguments[0]));
    } else if (std::strcmp(symbol, "REC_collatz") == 0 &&
               argument_count == 2u) {
        returned = REC_collatz(Read(arguments[0]), Read(arguments[1]));
    } else {
        ADD_FAILURE() << "unexpected callee " << symbol << " with "
                      << argument_count << " arguments";
        return 0;
    }
    ++*depth;
    std::memcpy(result, &returned, sizeof(returned));
    return 1;
}

struct InterpRun {
    ql_status status = QL_STATUS_OK;
    ql_ir_interp_result_v1 result{};
};

InterpRun Interpret(const ql_ir *ir, const std::vector<std::int32_t> &scalars,
              std::size_t *calls) {
    ql_ir_view_v1 view{};
    ql_ir_interp_callees_v1 callees{};
    ql_ir_interp_options_v1 options{};
    std::vector<ql_ir_interp_input_v1> inputs;
    std::vector<std::vector<std::uint8_t>> storage;
    std::size_t next = 0u;
    ql_error error{};
    InterpRun run;

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
        inputs.push_back(ql_ir_interp_input_v1{});
        ql_ir_interp_input_init(&inputs.back());
        inputs.back().value = value.id;
        if (type.kind == QL_IR_TYPE_MEMORY ||
            type.kind == QL_IR_TYPE_EVENT_TRACE) {
            storage.push_back(std::vector<std::uint8_t>());
            continue;
        }
        EXPECT_LT(next, scalars.size());
        storage.push_back(Encode(
            static_cast<std::uint32_t>(scalars[next++]), type.bit_width));
    }
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        inputs[index].data =
            storage[index].empty() ? nullptr : storage[index].data();
        inputs[index].size = storage[index].size();
    }
    callees.struct_size = sizeof(callees);
    callees.invoke = &InvokeSelf;
    callees.user_data = calls;
    ql_ir_interp_options_init(&options);
    options.callees = &callees;
    run.result.struct_size = sizeof(run.result);
    run.status = ql_ir_interp_run(nullptr, ir,
                                  inputs.empty() ? nullptr : inputs.data(),
                                  inputs.size(), &options, &run.result,
                                  &error);
    return run;
}

std::int32_t Returned(const ql_ir_interp_result_v1 &result) {
    std::uint32_t raw = 0u;
    for (std::size_t index = 0u; index < result.value_size && index < 4u;
         ++index) {
        raw |= static_cast<std::uint32_t>(result.value[index]) << (index * 8u);
    }
    return static_cast<std::int32_t>(raw);
}

TEST(CLowerRecursion, MatchesCompiledExecutionForADirectRecursiveCall) {
    static const char source[] =
        "int REC_sum(int n) {\n"
        "  if (n <= 0) return 0;\n"
        "  return n + REC_sum(n - 1);\n"
        "}\n";
    Lowered lowered;
    ql_ir *ir = nullptr;
    ql_ir_verify_report_v1 report{};
    ql_error error{};

    ASSERT_EQ(QL_C_LOWER_SUPPORTED, Lower(&lowered, source, "REC_sum", &ir));
    ASSERT_NE(nullptr, ir);
    ql_ir_verify_report_init(&report);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_verify(nullptr, ir, &report, &error))
        << report.message;

    for (const std::int32_t input : {-3, 0, 1, 5, 40}) {
        std::size_t calls = 0u;
        SCOPED_TRACE(input);
        const InterpRun run = Interpret(ir, {input}, &calls);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
        EXPECT_EQ(REC_sum(input), Returned(run.result));
        /* The body makes the call on exactly the path the C takes. */
        EXPECT_EQ(input <= 0 ? 0u : 1u, calls);
    }
}

TEST(CLowerRecursion, CarriesTwoArgumentsThroughATailRecursiveCall) {
    static const char source[] =
        "int REC_collatz(int n, int steps) {\n"
        "  if (n <= 1) return steps;\n"
        "  if ((n & 1) == 0) return REC_collatz(n / 2, steps + 1);\n"
        "  return REC_collatz(n * 3 + 1, steps + 1);\n"
        "}\n";
    Lowered lowered;
    ql_ir *ir = nullptr;
    ql_ir_verify_report_v1 report{};
    ql_error error{};

    ASSERT_EQ(QL_C_LOWER_SUPPORTED,
              Lower(&lowered, source, "REC_collatz", &ir));
    ASSERT_NE(nullptr, ir);
    ql_ir_verify_report_init(&report);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_verify(nullptr, ir, &report, &error))
        << report.message;

    for (const std::int32_t input : {1, 2, 6, 27}) {
        std::size_t calls = 0u;
        SCOPED_TRACE(input);
        const InterpRun run = Interpret(ir, {input, 0}, &calls);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
        EXPECT_EQ(REC_collatz(input, 0), Returned(run.result));
        EXPECT_EQ(input <= 1 ? 0u : 1u, calls);
    }
}

/* The IR has to name the callee, because the symbol is the only thing that
   later says which calls correspond. A recursive call names the function
   itself, which is exactly how the miter recognizes it. */
TEST(CLowerRecursion, TheRecursiveCallNamesTheFunctionItself) {
    static const char source[] =
        "int REC_sum(int n) {\n"
        "  if (n <= 0) return 0;\n"
        "  return n + REC_sum(n - 1);\n"
        "}\n";
    Lowered lowered;
    ql_ir *ir = nullptr;
    ql_ir_view_v1 view{};
    ql_error error{};
    std::size_t call_sites = 0u;

    ASSERT_EQ(QL_C_LOWER_SUPPORTED, Lower(&lowered, source, "REC_sum", &ir));
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_get_view(ir, &view, &error));
    ASSERT_NE(nullptr, view.function_name);
    EXPECT_STREQ("REC_sum", view.function_name);
    for (std::size_t index = 0u; index < view.instruction_count; ++index) {
        ql_ir_instruction_view_v1 instruction{};
        instruction.struct_size = sizeof(instruction);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_instruction_at(ir, index, &instruction, &error));
        if (instruction.opcode != QL_IR_OPCODE_CALL) {
            continue;
        }
        ++call_sites;
        ASSERT_NE(nullptr, instruction.symbol);
        EXPECT_EQ(std::string("REC_sum"),
                  std::string(instruction.symbol, instruction.symbol_size));
    }
    EXPECT_EQ(1u, call_sites);
}

/* A declared corpus function used as a value becomes an opaque token both
   sides share. The selected function must not, because a shared token would
   say the two definitions under comparison are one function. It is a call
   target and nothing else. */
TEST(CLowerRecursion, TheSelectedFunctionIsNotAnExternalToken) {
    struct Case {
        const char *source;
        const char *name;
    };
    const Case cases[] = {
        {"int FUN_0(int a) { return FUN_0 != 0; }", "FUN_0"},
        {"int FUN_3(int (*callback)(int), int a);\n"
         "int FUN_0(int a) { return FUN_3(FUN_0, a); }",
         "FUN_0"},
    };
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.source);
        EXPECT_EQ(QL_C_LOWER_UNKNOWN,
                  Lower(&lowered, item.source, item.name, nullptr));
    }
}

/* `?:`, `&&`, and `||` select between operands this lowering has already
   evaluated. For a pure operand that is only a definedness question, and the
   guards answer it. For an operand with an effect it is not: the call, store,
   or increment would run on the path C skips, and the IR would not record that
   it was conditional. Refusing is the only honest answer until the operands
   get real control flow.

   Recursion is where this bites hardest, because `return n <= 0 ? 0 : f(n-1)`
   is the ordinary shape, and evaluating the false arm at the base case is
   unbounded recursion rather than a wrong value. */
TEST(CLowerRecursion, RefusesAnEffectOnAPathTheConditionSkips) {
    struct Case {
        const char *source;
        const char *name;
    };
    const Case cases[] = {
        {"int REC_sum(int n) { return n <= 0 ? 0 : n + REC_sum(n - 1); }",
         "REC_sum"},
        {"int CALLEE_f(int);\n"
         "int f(int a) { return a ? CALLEE_f(a) : 0; }",
         "f"},
        {"int CALLEE_f(int);\n"
         "int f(int a) { return a && CALLEE_f(a); }",
         "f"},
        {"int CALLEE_f(int);\n"
         "int f(int a) { return a || CALLEE_f(a); }",
         "f"},
        {"int f(int *p) { return p != 0 && (*p = 1); }", "f"},
        {"int f(int a, int b) { return a > 0 && b++ > 0; }", "f"},
    };
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.source);
        EXPECT_EQ(QL_C_LOWER_UNKNOWN,
                  Lower(&lowered, item.source, item.name, nullptr));
    }
}

/* The left operand of a short-circuit operator is always evaluated, and so is
   the condition of a conditional expression. Neither is on a skipped path, so
   neither is refused. */
TEST(CLowerRecursion, KeepsAnEffectTheConditionAlwaysEvaluates) {
    struct Case {
        const char *source;
        const char *name;
    };
    const Case cases[] = {
        {"int CALLEE_f(int);\n"
         "int f(int a) { return CALLEE_f(a) && a; }",
         "f"},
        {"int CALLEE_f(int);\n"
         "int f(int a) { return CALLEE_f(a) ? 1 : 0; }",
         "f"},
    };
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.source);
        EXPECT_EQ(QL_C_LOWER_SUPPORTED,
                  Lower(&lowered, item.source, item.name, nullptr));
    }
}

}  // namespace
