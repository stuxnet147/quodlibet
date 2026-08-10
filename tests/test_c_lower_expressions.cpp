/* The expression forms the first slice did not carry: `?:`, the comma
   operator, assignment used for its value, `++`/`--`, and compound
   assignment.

   Every case is compared against the same C function compiled by the compiler
   that builds this test. `QL_EXPR_FUNCTION` writes the definition and the
   source string the lowering reads from one text, so the two cannot drift.
   Only types whose width the target ABI and both hosts agree on appear here:
   `long` is deliberately absent, exactly as in the other differential tests.

   Two things are checked, not one. On defined inputs the interpreter has to
   return the reference's value exactly. On undefined inputs it has to report
   `UNDEFINED_BEHAVIOR` rather than a value, and the reference is never called
   there because running undefined behaviour would make the comparison
   meaningless. */

#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#define QL_EXPR_FUNCTION(name, ...)                                          \
    extern "C" {                                                             \
    __VA_ARGS__                                                              \
    }                                                                        \
    static const char name##_source[] = #__VA_ARGS__

/* `b == 0` must not make this undefined: the division is only reached when
   the condition chose it. This is the conditional's whole definedness rule. */
QL_EXPR_FUNCTION(guarded_divide, int expr_guarded(int a, int b) {
    return b != 0 ? a / b : a;
});
QL_EXPR_FUNCTION(select_value, int expr_select(int a, int b) {
    return a > b ? a - b : b - a;
});
QL_EXPR_FUNCTION(nested_conditional, int expr_nested(int a, int b) {
    return a > 0 ? (b > 0 ? 1 : 2) : (b > 0 ? 3 : 4);
});
QL_EXPR_FUNCTION(local_enum,
    enum EXPR_LOCAL_ENUM { EXPR_NEGATIVE = -3, EXPR_POSITIVE = 5 };
    int expr_local_enum(int a, int b) {
        enum EXPR_LOCAL_ENUM selected =
            a > b ? EXPR_POSITIVE : EXPR_NEGATIVE;
        return selected + (a - a) + (b - b);
    });
QL_EXPR_FUNCTION(comma_value, int expr_comma(int a, int b) {
    int c = 0;
    return (c = a + 1, c + b);
});
QL_EXPR_FUNCTION(assignment_value, int expr_assign(int a, int b) {
    int c;
    int d;
    d = (c = a + b);
    return c + d;
});
QL_EXPR_FUNCTION(postfix, int expr_postfix(int a, int b) {
    int c = a;
    int d = c++;
    return d * 4 + c + b;
});
QL_EXPR_FUNCTION(prefix, int expr_prefix(int a, int b) {
    int c = a;
    int d = ++c;
    return d * 4 + c + b;
});
QL_EXPR_FUNCTION(decrement, int expr_decrement(int a, int b) {
    int c = a;
    int d = c--;
    int e = --c;
    return d * 4 + e + b;
});
QL_EXPR_FUNCTION(compound_arithmetic, int expr_compound(int a, int b) {
    int c = a;
    c += b;
    c -= 3;
    c *= 2;
    return c;
});
QL_EXPR_FUNCTION(compound_bitwise, int expr_bits(unsigned a, unsigned b) {
    unsigned c = a;
    c &= b;
    c |= 5u;
    c ^= 0xffu;
    return (int) c;
});
/* The shift keeps its own definedness rule, which is the point of routing a
   compound assignment through the ordinary binary operator. */
QL_EXPR_FUNCTION(compound_shift, int expr_shift(unsigned a, int b) {
    unsigned c = a;
    c <<= b;
    c >>= 1;
    return (int) c;
});
/* `c /= b` is undefined at `b == 0`, and at INT_MIN / -1. */
QL_EXPR_FUNCTION(compound_divide, int expr_divide(int a, int b) {
    int c = a;
    c /= b;
    return c;
});
/* The narrower type converts back after the wider operation, so the result
   is the one C states rather than the one the arithmetic produced. */
QL_EXPR_FUNCTION(compound_narrow, int expr_narrow(short a, int b) {
    short c = a;
    c += (short) b;
    return c;
});
QL_EXPR_FUNCTION(sizeof_types,
    typedef unsigned short TYP_SIZE_WORD;
    struct expr_size_pair { char first; int second; };
    int expr_sizeof_types(int a, int b) {
        return (int)(sizeof(TYP_SIZE_WORD) + sizeof(int *) +
                     sizeof(struct expr_size_pair)) + (a - a) + (b - b);
    });
/* Tree-sitter includes a literal's leading sign in number_literal. The
   lowering must still select the type of the magnitude before applying the
   unary minus, including the unsigned-wrap case. */
QL_EXPR_FUNCTION(signed_literals, int expr_signed_literals(int a, int b) {
    return -1 + (+7 - 7) + (a - a) + (b - b);
});
QL_EXPR_FUNCTION(unsigned_negative_literal,
    unsigned expr_unsigned_negative_literal(unsigned a, unsigned b) {
        return -1u + (a - a) + (b - b);
    });
QL_EXPR_FUNCTION(character_escapes, int expr_character_escapes(int a, int b) {
    return a + b + '\xc0' + '\101' + '\n';
});
QL_EXPR_FUNCTION(void_assignment, int expr_void_assignment(int a, int b) {
    (void)(a = b);
    return a;
});
QL_EXPR_FUNCTION(void_divide, int expr_void_divide(int a, int b) {
    (void)(a / b);
    return 0;
});

namespace {

class Lowered {
public:
    ~Lowered() {
        ql_ir_release(ir_);
        ql_c_lower_result_destroy(result_);
        ql_c_frontend_unit_destroy(unit_);
    }

    bool Open(const char *source, const char *name) {
        ql_c_function_view function{};
        ql_c_lower_result_view_v1 view{};
        ql_ir_verify_report_v1 report{};
        ql_error error{};
        const std::size_t size = std::strlen(source);

        if (ql_c_frontend_analyze(nullptr, source, size, &unit_, &error) !=
            QL_STATUS_OK) {
            ADD_FAILURE() << "analyze: " << error.message;
            return false;
        }
        function.struct_size = sizeof(function);
        if (ql_c_frontend_select_function(unit_, name, std::strlen(name),
                                          &function, &error) !=
            QL_STATUS_OK) {
            ADD_FAILURE() << "select: " << error.message;
            return false;
        }
        if (ql_c_lower_selected_function(nullptr, source, size, unit_,
                                         &function, &result_, &error) !=
            QL_STATUS_OK) {
            ADD_FAILURE() << "lower: " << error.message;
            return false;
        }
        view.struct_size = sizeof(view);
        if (ql_c_lower_result_get_view(result_, &view, &error) !=
                QL_STATUS_OK ||
            view.support != QL_C_LOWER_SUPPORTED) {
            ql_c_lower_diagnostic_view_v1 diagnostic{};
            diagnostic.struct_size = sizeof(diagnostic);
            if (view.diagnostic_count != 0u &&
                ql_c_lower_result_diagnostic_at(result_, 0u, &diagnostic,
                                                &error) == QL_STATUS_OK) {
                ADD_FAILURE() << "the lowering did not accept " << name
                              << ": " << diagnostic.message;
            } else {
                ADD_FAILURE() << "the lowering did not accept " << name;
            }
            return false;
        }
        if (ql_ir_open(nullptr, view.ir_artifact, &ir_, &error) !=
            QL_STATUS_OK) {
            ADD_FAILURE() << "open: " << error.message;
            return false;
        }
        /* Every lowering this file produces goes through the verifier, so a
           gain in what is accepted cannot quietly cost a gain in what is
           justified. */
        report.struct_size = sizeof(report);
        if (ql_ir_verify(nullptr, ir_, &report, &error) != QL_STATUS_OK) {
            ADD_FAILURE() << "verify: "
                          << ql_ir_verify_code_string(report.code) << ": "
                          << report.message;
            return false;
        }
        return true;
    }

    ql_ir *ir() const { return ir_; }

private:
    ql_c_frontend_unit *unit_ = nullptr;
    ql_c_lower_result *result_ = nullptr;
    ql_ir *ir_ = nullptr;
};

std::vector<uint8_t> Encode(uint64_t value, uint32_t width) {
    std::vector<uint8_t> bytes((width + 7u) / 8u, 0u);
    for (std::size_t index = 0u; index < bytes.size() && index < 8u;
         ++index) {
        bytes[index] = static_cast<uint8_t>((value >> (index * 8u)) & 0xffu);
    }
    return bytes;
}
struct Outcome {
    ql_ir_interp_result_v1 result{};
    ql_status status = QL_STATUS_INTERNAL_ERROR;
};

Outcome Execute(ql_ir *ir, const std::vector<uint64_t> &arguments) {
    ql_ir_view_v1 view{};
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ql_ir_interp_input_v1> inputs;
    ql_ir_interp_options_v1 options{};
    ql_error error{};
    Outcome run;
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
        inputs.push_back(ql_ir_interp_input_v1{});
        ql_ir_interp_input_init(&inputs.back());
        inputs.back().value = value.id;
        if (type.kind == QL_IR_TYPE_MEMORY ||
            type.kind == QL_IR_TYPE_EVENT_TRACE) {
            storage.push_back(std::vector<uint8_t>());
            continue;
        }
        EXPECT_LT(next, arguments.size());
        storage.push_back(Encode(arguments[next++],
                                 type.kind == QL_IR_TYPE_BOOL
                                     ? 1u
                                     : type.bit_width));
    }
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        inputs[index].data = storage[index].empty() ? nullptr
                                                    : storage[index].data();
        inputs[index].size = storage[index].size();
    }
    ql_ir_interp_options_init(&options);
    run.result.struct_size = sizeof(run.result);
    run.status = ql_ir_interp_run(nullptr, ir,
                                  inputs.empty() ? nullptr : inputs.data(),
                                  inputs.size(), &options, &run.result,
                                  &error);
    return run;
}

int32_t Returned(const ql_ir_interp_result_v1 &result) {
    uint32_t raw = 0u;
    for (std::size_t index = 0u; index < result.value_size && index < 4u;
         ++index) {
        raw |= static_cast<uint32_t>(result.value[index]) << (index * 8u);
    }
    return static_cast<int32_t>(raw);
}

uint64_t Widen(int32_t value) {
    return static_cast<uint64_t>(static_cast<uint32_t>(value));
}

uint64_t NextRandom(uint64_t *state) {
    uint64_t value;
    *state += UINT64_C(0x9e3779b97f4a7c15);
    value = *state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

struct Case {
    const char *name;
    const char *source;
    const char *function;
    int32_t (*reference)(int32_t, int32_t);
    /* Says which inputs the reference may be called on. Anything else is
       expected to be reported as undefined instead of answered. */
    bool (*defined)(int32_t, int32_t);
};

bool Always(int32_t, int32_t) { return true; }

const Case kCases[] = {
    {"guarded", guarded_divide_source, "expr_guarded",
     [](int32_t a, int32_t b) { return expr_guarded(a, b); },
     [](int32_t a, int32_t b) {
         return b != -1 || a != INT32_MIN;
     }},
    {"select", select_value_source, "expr_select",
     [](int32_t a, int32_t b) { return expr_select(a, b); }, &Always},
    {"nested", nested_conditional_source, "expr_nested",
     [](int32_t a, int32_t b) { return expr_nested(a, b); }, &Always},
    {"local-enum", local_enum_source, "expr_local_enum",
     [](int32_t a, int32_t b) { return expr_local_enum(a, b); }, &Always},
    {"comma", comma_value_source, "expr_comma",
     [](int32_t a, int32_t b) { return expr_comma(a, b); }, &Always},
    {"assign", assignment_value_source, "expr_assign",
     [](int32_t a, int32_t b) { return expr_assign(a, b); }, &Always},
    {"postfix", postfix_source, "expr_postfix",
     [](int32_t a, int32_t b) { return expr_postfix(a, b); }, &Always},
    {"prefix", prefix_source, "expr_prefix",
     [](int32_t a, int32_t b) { return expr_prefix(a, b); }, &Always},
    {"decrement", decrement_source, "expr_decrement",
     [](int32_t a, int32_t b) { return expr_decrement(a, b); }, &Always},
    {"compound", compound_arithmetic_source, "expr_compound",
     [](int32_t a, int32_t b) { return expr_compound(a, b); }, &Always},
    {"bitwise", compound_bitwise_source, "expr_bits",
     [](int32_t a, int32_t b) {
         return expr_bits(static_cast<unsigned>(a),
                          static_cast<unsigned>(b));
     },
     &Always},
    {"shift", compound_shift_source, "expr_shift",
     [](int32_t a, int32_t b) {
         return expr_shift(static_cast<unsigned>(a), b);
     },
     [](int32_t, int32_t b) { return b >= 0 && b < 32; }},
    {"divide", compound_divide_source, "expr_divide",
     [](int32_t a, int32_t b) { return expr_divide(a, b); },
     [](int32_t a, int32_t b) {
         return b != 0 && (b != -1 || a != INT32_MIN);
     }},
    {"narrow", compound_narrow_source, "expr_narrow",
     [](int32_t a, int32_t b) {
         return expr_narrow(static_cast<short>(a), b);
     },
     &Always},
    {"sizeof-types", sizeof_types_source, "expr_sizeof_types",
     [](int32_t a, int32_t b) { return expr_sizeof_types(a, b); },
     &Always},
    {"signed-literals", signed_literals_source, "expr_signed_literals",
     [](int32_t a, int32_t b) { return expr_signed_literals(a, b); },
     &Always},
    {"unsigned-negative-literal", unsigned_negative_literal_source,
     "expr_unsigned_negative_literal",
     [](int32_t a, int32_t b) {
         return static_cast<int32_t>(expr_unsigned_negative_literal(
             static_cast<unsigned>(a), static_cast<unsigned>(b)));
     },
     &Always},
    {"character-escapes", character_escapes_source, "expr_character_escapes",
     [](int32_t a, int32_t b) { return expr_character_escapes(a, b); },
     &Always},
    {"void-assignment", void_assignment_source, "expr_void_assignment",
     [](int32_t a, int32_t b) { return expr_void_assignment(a, b); },
     &Always},
    {"void-divide", void_divide_source, "expr_void_divide",
     [](int32_t a, int32_t b) { return expr_void_divide(a, b); },
     [](int32_t a, int32_t b) {
         return b != 0 && (b != -1 || a != INT32_MIN);
     }},
};

/* Small enough that the arithmetic in every case stays inside the defined
   range, plus the edges the partial operations care about. */
const int32_t kBoundaries[] = {
    0, 1, -1, 2, -2, 3, 7, 8, 31, 32, 33, -3, -7, 100, -100,
    INT32_MIN, INT32_MAX, 255, -255, 16
};

}  // namespace

TEST(CLowerExpressions, MatchesCompiledExecutionOnDefinedInputs) {
    uint64_t state = UINT64_C(0x51f3ad9c7e2b6104);
    for (const Case &item : kCases) {
        Lowered lowered;
        SCOPED_TRACE(item.name);
        ASSERT_TRUE(lowered.Open(item.source, item.function));
        for (std::size_t round = 0u; round < 512u; ++round) {
            /* Values are kept small so that the reference's own arithmetic
               stays defined and the comparison measures the new expression
               forms rather than overflow. */
            const int32_t a =
                static_cast<int32_t>(NextRandom(&state) % 200u) - 100;
            const int32_t b =
                static_cast<int32_t>(NextRandom(&state) % 200u) - 100;
            if (!item.defined(a, b)) {
                continue;
            }
            const Outcome run = Execute(lowered.ir(), {Widen(a), Widen(b)});
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << "a=" << a << " b=" << b << " "
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);
            EXPECT_EQ(item.reference(a, b), Returned(run.result))
                << "a=" << a << " b=" << b;
        }
    }
}

TEST(CLowerExpressions, ReportsUndefinedBehaviourInsteadOfAnswering) {
    /* The undefined side has to be reached, or this test would pass while
       checking nothing. Every case that has undefined inputs says so, and the
       count below is what proves the boundary values found them. */
    std::size_t undefined_seen = 0u;
    std::size_t cases_with_undefined_inputs = 0u;
    for (const Case &item : kCases) {
        Lowered lowered;
        bool reached = false;
        SCOPED_TRACE(item.name);
        ASSERT_TRUE(lowered.Open(item.source, item.function));
        for (int32_t a : kBoundaries) {
            for (int32_t b : kBoundaries) {
                if (item.defined(a, b)) {
                    continue;
                }
                reached = true;
                ++undefined_seen;
                const Outcome run =
                    Execute(lowered.ir(), {Widen(a), Widen(b)});
                ASSERT_EQ(QL_STATUS_OK, run.status);
                EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                          run.result.outcome)
                    << "a=" << a << " b=" << b;
            }
        }
        if (reached) {
            ++cases_with_undefined_inputs;
        }
    }
    EXPECT_GE(cases_with_undefined_inputs, 3u);
    EXPECT_GT(undefined_seen, 0u);
}

TEST(CLowerExpressions, AConditionalDoesNotDemandTheArmItDidNotTake) {
    /* The heart of it: `b != 0 ? a / b : a` at `b == 0` is a value, not
       undefined behaviour. An eager definedness rule would report UB here and
       invent an obstacle C does not have. */
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(guarded_divide_source, "expr_guarded"));
    for (int32_t a : {0, 1, -1, 42, -42, INT32_MAX, INT32_MIN}) {
        const Outcome run = Execute(lowered.ir(), {Widen(a), Widen(0)});
        SCOPED_TRACE(a);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(expr_guarded(a, 0), Returned(run.result));
    }
}

TEST(CLowerExpressions, RefusesWhatItCannotState) {
    struct Refused {
        const char *source;
        const char *name;
        int expected;
    };
    const Refused cases[] = {
        /* A const local is not writable, whatever the operator. */
        {"int f(int a) { const int c = a; c += 1; return c; }", "f",
         -1},
        /* An arm of a conditional that has no value this slice can carry. */
        {"struct S { int x; };\n"
         "int f(int a, struct S *p, struct S *q) { return (a ? *p : *q).x; }",
         "f", -1},
        /* Only an integer constant expression equal to zero is a null pointer
           constant. A non-zero integer arm does not acquire pointer type. */
        {"int f(int a, int *p) { return (a ? p : 1) != 0; }", "f",
         QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER},
        /* `sizeof(expression)` must not lower its operand. A future static
           type query may accept this without ever evaluating `1 / a`. */
        {"unsigned long f(int a) { return sizeof(1 / a); }", "f",
         QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION},
        /* The first slice accepts complete scalar, pointer and record type
           descriptors, but not array declarators. */
        {"unsigned long f(void) { return sizeof(int[4]); }", "f",
         QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE},
        /* The target byte cannot represent these escape values. A compiler
           may diagnose them before translation, while the lowering receives
           source text and must conservatively keep it UNKNOWN. */
        {"int f(void) { return '\\x100'; }", "f",
         QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION},
        {"int f(void) { return '\\400'; }", "f",
         QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION},
    };
    for (const Refused &item : cases) {
        ql_c_frontend_unit *unit = nullptr;
        ql_c_lower_result *result = nullptr;
        ql_c_function_view function{};
        ql_c_lower_result_view_v1 view{};
        ql_error error{};
        const std::size_t size = std::strlen(item.source);
        SCOPED_TRACE(item.source);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_frontend_analyze(nullptr, item.source, size, &unit,
                                        &error));
        function.struct_size = sizeof(function);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_frontend_select_function(unit, item.name,
                                                std::strlen(item.name),
                                                &function, &error));
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_lower_selected_function(nullptr, item.source, size,
                                               unit, &function, &result,
                                               &error));
        view.struct_size = sizeof(view);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_lower_result_get_view(result, &view, &error));
        /* A limit is always UNKNOWN, never a status failure. */
        EXPECT_EQ(QL_C_LOWER_UNKNOWN, view.support);
        if (item.expected >= 0) {
            ql_c_lower_diagnostic_view_v1 diagnostic{};
            diagnostic.struct_size = sizeof(diagnostic);
            ASSERT_EQ(1u, view.diagnostic_count);
            ASSERT_EQ(QL_STATUS_OK,
                      ql_c_lower_result_diagnostic_at(result, 0u,
                                                      &diagnostic, &error));
            EXPECT_EQ(static_cast<ql_c_lower_diagnostic_code>(item.expected),
                      diagnostic.code);
        }
        ql_c_lower_result_destroy(result);
        ql_c_frontend_unit_destroy(unit);
    }
}
