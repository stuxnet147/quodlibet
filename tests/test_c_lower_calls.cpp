/* Calls to declared callees, against real compiled execution.

   An external callee is uninterpreted in the IR, so the interpreter cannot
   invent what it returns and the test has to say. The specification below is
   the same C function the reference calls, which is what makes the comparison
   mean anything: both sides get the identical callee, and only the lowering
   differs. The specification also records the order it was called in, so the
   observable sequence is compared and not just the returned value. */

#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#define QL_CALL_FUNCTION(name, ...)                                          \
    extern "C" {                                                             \
    __VA_ARGS__                                                              \
    }                                                                        \
    static const char name##_source[] = #__VA_ARGS__

/* The callees the lowered bodies below call. The reference runs these; the
   interpreter is handed a specification that runs the same ones. */
extern "C" {
int CALLEE_double(int a) { return a * 2; }
int CALLEE_sum(int a, int b) { return a + b; }
int CALLEE_variadic(int tag, ...) {
    va_list arguments;
    va_start(arguments, tag);
    const int first = va_arg(arguments, int);
    const int second = va_arg(arguments, int);
    va_end(arguments);
    return tag + first + second;
}
/* An address whose low 32 bits are zero. Nothing dereferences it; it exists
   so that a caller which kept only 32 bits of the result would see null where
   the compiled reference sees an address. */
char *CALLEE_high(void) {
    return reinterpret_cast<char *>(static_cast<uintptr_t>(UINT64_C(1) << 32));
}
static int CALLEE_storage = 73;
int *CALLEE_cell(void) { return &CALLEE_storage; }
void CALLEE_sink(int value) { CALLEE_storage = value; }
}

QL_CALL_FUNCTION(single, int CALLEE_double(int);
    int call_single(int a) { return CALLEE_double(a) + 1; });
QL_CALL_FUNCTION(twoargs, int CALLEE_sum(int, int);
    int call_two(int a, int b) { return CALLEE_sum(a, b); });
QL_CALL_FUNCTION(sequence, int CALLEE_double(int); int CALLEE_sum(int, int);
    int call_sequence(int a, int b) {
        int x = CALLEE_double(a);
        int y = CALLEE_double(b);
        return CALLEE_sum(x, y);
    });
QL_CALL_FUNCTION(branching, int CALLEE_double(int);
    int call_branch(int a, int b) {
        int v;
        if (b) {
            v = CALLEE_double(a);
        } else {
            v = a;
        }
        return v;
    });
QL_CALL_FUNCTION(discarded, int CALLEE_double(int);
    int call_discard(int a) {
        CALLEE_double(a);
        return a;
    });
QL_CALL_FUNCTION(widening, int CALLEE_double(int);
    int call_widen(short a) { return CALLEE_double(a); });
QL_CALL_FUNCTION(block_scope_prototype,
    int call_block_scope_prototype(int a) {
        extern int CALLEE_double(int);
        return CALLEE_double(a) + 3;
    });
QL_CALL_FUNCTION(block_scope_plain_prototype,
    int call_block_scope_plain_prototype(int a) {
        int CALLEE_double(int);
        return CALLEE_double(a) - 4;
    });
QL_CALL_FUNCTION(variadic, int CALLEE_variadic(int, ...);
    int call_variadic(short a, unsigned char b) {
        return CALLEE_variadic(3, a, b);
    });
/* The stars between the return type and the callee's name belong to the
   return type. Reading them is what lets this declaration be found at all. */
QL_CALL_FUNCTION(pointerresult, char *CALLEE_high(void);
    int call_ptr_result(void) { return CALLEE_high() == 0; });
QL_CALL_FUNCTION(pointerfollow, int *CALLEE_cell(void);
    int call_ptr_follow(void) { return *CALLEE_cell(); });
QL_CALL_FUNCTION(voidreturn, void CALLEE_sink(int);
    void call_void_return(int value) { return CALLEE_sink(value); });
QL_CALL_FUNCTION(indirect, struct CALL_VTABLE {
        int (*callback)(int);
        char *(*pointer_callback)(void);
    };
    int call_indirect(struct CALL_VTABLE *table, int value) {
        return table->callback(value) + 1;
    }
    int call_indirect_pointer(struct CALL_VTABLE *table) {
        return table->pointer_callback() == 0;
    });
static const char callback_argument_source[] =
    "int CALLEE_accept(int (*)(int), int);\n"
    "int pass_callback(int (*callback)(int), int value) {\n"
    "  return CALLEE_accept(callback, value) + (callback != 0);\n"
    "}\n"
    "int select_callback(int (*left)(int), int (*right)(int), int choose) {\n"
    "  if (choose) left = right; else left = 0;\n"
    "  return left != 0;\n"
    "}\n";

namespace {

/* Everything the interpreter's callee hook saw, in the order it saw it. */
struct CallLog {
    std::vector<std::string> symbols;
    std::vector<std::vector<int32_t>> arguments;
};

int32_t Read(const ql_ir_interp_argument_v1 &argument) {
    uint32_t raw = 0u;
    const uint8_t *bytes = static_cast<const uint8_t *>(argument.data);
    for (std::size_t index = 0u; index < argument.size && index < 4u;
         ++index) {
        raw |= static_cast<uint32_t>(bytes[index]) << (index * 8u);
    }
    return static_cast<int32_t>(raw);
}

uint64_t Read64(const ql_ir_interp_argument_v1 &argument) {
    uint64_t raw = 0u;
    const uint8_t *bytes = static_cast<const uint8_t *>(argument.data);
    for (std::size_t index = 0u; index < argument.size && index < 8u;
         ++index) {
        raw |= static_cast<uint64_t>(bytes[index]) << (index * 8u);
    }
    return raw;
}

int QL_CALL Invoke(void *user_data, const char *symbol,
                   const ql_ir_interp_argument_v1 *arguments,
                   std::size_t argument_count, void *result,
                   std::size_t result_size) {
    CallLog *log = static_cast<CallLog *>(user_data);
    std::vector<int32_t> seen;
    int32_t value = 0;

    for (std::size_t index = 0u; index < argument_count; ++index) {
        seen.push_back(Read(arguments[index]));
    }
    if (std::strcmp(symbol, "CALLEE_high") == 0 && argument_count == 0u) {
        /* The same address the reference returns, in the width the callee's
           declaration gives its result. */
        const uint64_t address = UINT64_C(1) << 32;
        if (result_size != 8u) {
            ADD_FAILURE() << "a pointer result should be 8 bytes, not "
                          << result_size;
            return 0;
        }
        log->symbols.push_back(symbol);
        log->arguments.push_back(seen);
        for (std::size_t index = 0u; index < 8u; ++index) {
            static_cast<uint8_t *>(result)[index] =
                static_cast<uint8_t>((address >> (index * 8u)) & 0xffu);
        }
        return 1;
    }
    if (std::strcmp(symbol, "CALLEE_cell") == 0 && argument_count == 0u) {
        const uint64_t address =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CALLEE_cell()));
        if (result_size != 8u) {
            ADD_FAILURE() << "a pointer result should be 8 bytes, not "
                          << result_size;
            return 0;
        }
        log->symbols.push_back(symbol);
        log->arguments.push_back(seen);
        for (std::size_t index = 0u; index < 8u; ++index) {
            static_cast<uint8_t *>(result)[index] =
                static_cast<uint8_t>((address >> (index * 8u)) & 0xffu);
        }
        return 1;
    }
    if (std::strcmp(symbol, "CALLEE_double") == 0 && argument_count == 1u) {
        value = CALLEE_double(seen[0]);
    } else if (std::strcmp(symbol, "CALLEE_sum") == 0 &&
               argument_count == 2u) {
        value = CALLEE_sum(seen[0], seen[1]);
    } else if (std::strcmp(symbol, "CALLEE_variadic") == 0 &&
               argument_count == 3u) {
        EXPECT_EQ(4u, arguments[1].size);
        EXPECT_EQ(4u, arguments[2].size);
        value = seen[0] + seen[1] + seen[2];
    } else if (std::strcmp(symbol, "CALLEE_sink") == 0 &&
               argument_count == 1u && result_size == 0u) {
        CALLEE_sink(seen[0]);
    } else if (std::strcmp(symbol, "__ql_indirect_call_v1") == 0) {
        const uint64_t target = Read64(arguments[0]);
        if (target == static_cast<uint64_t>(
                          reinterpret_cast<uintptr_t>(&CALLEE_double)) &&
            argument_count == 2u) {
            value = CALLEE_double(seen[1]);
        } else if (target == static_cast<uint64_t>(
                                 reinterpret_cast<uintptr_t>(&CALLEE_high)) &&
                   argument_count == 1u && result_size == 8u) {
            const uint64_t address = static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(CALLEE_high()));
            log->symbols.push_back(symbol);
            log->arguments.push_back(seen);
            for (std::size_t index = 0u; index < 8u; ++index) {
                static_cast<uint8_t *>(result)[index] = static_cast<uint8_t>(
                    (address >> (index * 8u)) & 0xffu);
            }
            return 1;
        } else {
            return 0;
        }
    } else {
        /* Refusing is what an unspecified callee has to mean. */
        return 0;
    }
    log->symbols.push_back(symbol);
    log->arguments.push_back(seen);
    if (result_size >= 4u) {
        uint8_t *bytes = static_cast<uint8_t *>(result);
        const uint32_t raw = static_cast<uint32_t>(value);
        for (std::size_t index = 0u; index < 4u; ++index) {
            bytes[index] = static_cast<uint8_t>((raw >> (index * 8u)) & 0xffu);
        }
    }
    return 1;
}

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

Outcome Execute(ql_ir *ir, const std::vector<uint64_t> &scalars,
                CallLog *log,
                const ql_ir_interp_object_v1 *dynamic_object = nullptr) {
    ql_ir_view_v1 view{};
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ql_ir_interp_input_v1> inputs;
    ql_ir_interp_callees_v1 callees{};
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
        const std::string name = value.name != nullptr ? value.name : "";
        if (name.find(".__base") != std::string::npos) {
            EXPECT_NE(nullptr, dynamic_object);
            storage.push_back(Encode(dynamic_object != nullptr
                                         ? dynamic_object->base
                                         : 0u,
                                     type.bit_width));
            continue;
        }
        if (name.find(".__size") != std::string::npos) {
            EXPECT_NE(nullptr, dynamic_object);
            storage.push_back(Encode(dynamic_object != nullptr
                                         ? dynamic_object->size
                                         : 0u,
                                     type.bit_width));
            continue;
        }
        EXPECT_LT(next, scalars.size());
        storage.push_back(Encode(scalars[next++],
                                 type.kind == QL_IR_TYPE_BOOL
                                     ? 1u
                                     : type.bit_width));
    }
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        inputs[index].data = storage[index].empty() ? nullptr
                                                    : storage[index].data();
        inputs[index].size = storage[index].size();
    }
    callees.struct_size = sizeof(callees);
    callees.invoke = &Invoke;
    callees.user_data = log;
    ql_ir_interp_options_init(&options);
    options.callees = &callees;
    if (dynamic_object != nullptr) {
        options.objects = dynamic_object;
        options.object_count = 1u;
    }
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

uint64_t NextRandom(uint64_t *state) {
    uint64_t value;
    *state += UINT64_C(0x9e3779b97f4a7c15);
    value = *state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

uint64_t Widen(int32_t value) {
    return static_cast<uint64_t>(static_cast<uint32_t>(value));
}

TEST(CLowerCalls, MatchesCompiledExecutionIncludingTheCallSequence) {
    struct Case {
        const char *name;
        const char *source;
        const char *function;
        int arguments;
        int32_t (*reference)(int32_t, int32_t);
        std::vector<std::string> expected_symbols;
    };
    const Case cases[] = {
        {"single", single_source, "call_single", 1,
         [](int32_t a, int32_t) { return call_single(a); },
         {"CALLEE_double"}},
        {"two", twoargs_source, "call_two", 2,
         [](int32_t a, int32_t b) { return call_two(a, b); }, {"CALLEE_sum"}},
        /* Three calls whose order is part of what is observed. */
        {"sequence", sequence_source, "call_sequence", 2,
         [](int32_t a, int32_t b) { return call_sequence(a, b); },
         {"CALLEE_double", "CALLEE_double", "CALLEE_sum"}},
        /* A call the source discards still happened. */
        {"discard", discarded_source, "call_discard", 1,
         [](int32_t a, int32_t) { return call_discard(a); },
         {"CALLEE_double"}},
        /* The argument promotes to the declared parameter type. */
        {"widen", widening_source, "call_widen", 1,
         [](int32_t a, int32_t) {
             return call_widen(static_cast<short>(a));
         },
         {"CALLEE_double"}},
        {"block-scope-prototype", block_scope_prototype_source,
         "call_block_scope_prototype", 1,
         [](int32_t a, int32_t) { return call_block_scope_prototype(a); },
         {"CALLEE_double"}},
        {"block-scope-plain-prototype", block_scope_plain_prototype_source,
         "call_block_scope_plain_prototype", 1,
         [](int32_t a, int32_t) {
             return call_block_scope_plain_prototype(a);
         },
         {"CALLEE_double"}},
        {"variadic", variadic_source, "call_variadic", 2,
         [](int32_t a, int32_t b) {
             return call_variadic(static_cast<short>(a),
                                  static_cast<unsigned char>(b));
         },
         {"CALLEE_variadic"}},
    };

    uint64_t state = UINT64_C(0x3d81f0b46e295ca7);
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.name);
        ASSERT_TRUE(lowered.Open(item.source, item.function));
        for (std::size_t round = 0u; round < 64u; ++round) {
            CallLog log;
            /* Small values keep every case inside the defined range, so the
               comparison measures calls rather than signed overflow. */
            const int32_t a =
                static_cast<int32_t>(NextRandom(&state) % 200u) - 100;
            const int32_t b =
                static_cast<int32_t>(NextRandom(&state) % 200u) - 100;
            std::vector<uint64_t> scalars;
            scalars.push_back(Widen(a));
            if (item.arguments > 1) {
                scalars.push_back(Widen(b));
            }
            const Outcome run = Execute(lowered.ir(), scalars, &log);
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);
            EXPECT_EQ(item.reference(a, b), Returned(run.result));
            /* The call sequence is observable, so it is compared too. */
            EXPECT_EQ(item.expected_symbols, log.symbols);
            EXPECT_EQ(item.expected_symbols.size(), run.result.events);
        }
    }
}

TEST(CLowerCalls, MatchesCompiledExecutionForACalleeThatReturnsAPointer) {
    /* The declaration is `char *CALLEE_high(void);`, so the callee is named
       inside the pointer declarator rather than under the declaration itself.
       Reading the stars off that chain is what makes the callee findable and
       what gives its result the pointer's width: a 32-bit result would have
       truncated this address to null and answered 1. */
    Lowered lowered;
    CallLog log;
    ASSERT_TRUE(lowered.Open(pointerresult_source, "call_ptr_result"));
    const Outcome run = Execute(lowered.ir(), {}, &log);
    ASSERT_EQ(QL_STATUS_OK, run.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
        << ql_ir_interp_ub_reason_string(run.result.ub_reason);
    EXPECT_EQ(call_ptr_result(), Returned(run.result));
    EXPECT_EQ(std::vector<std::string>{"CALLEE_high"}, log.symbols);
    EXPECT_EQ(1u, run.result.events);
}

TEST(CLowerCalls, FollowsAPointerTheCalleeReturned) {
    /* The source signature cannot name the returned object. The dynamic
       descriptor binds the interpreter to the same real storage the compiled
       callee returns, so this compares both the call and the following load. */
    Lowered lowered;
    CallLog log;
    ql_ir_interp_object_v1 object{};
    ql_ir_interp_object_init(&object);
    object.base =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(CALLEE_cell()));
    object.size = sizeof(CALLEE_storage);
    object.initial = &CALLEE_storage;
    ASSERT_TRUE(lowered.Open(pointerfollow_source, "call_ptr_follow"));
    const Outcome run = Execute(lowered.ir(), {}, &log, &object);
    ASSERT_EQ(QL_STATUS_OK, run.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
        << ql_ir_interp_ub_reason_string(run.result.ub_reason);
    EXPECT_EQ(call_ptr_follow(), Returned(run.result));
    EXPECT_EQ(std::vector<std::string>{"CALLEE_cell"}, log.symbols);
    EXPECT_EQ(1u, run.result.events);
}

TEST(CLowerCalls, AReturnOfAVoidExpressionStillRunsTheCall) {
    Lowered lowered;
    CallLog log;
    CALLEE_storage = 0;
    ASSERT_TRUE(lowered.Open(voidreturn_source, "call_void_return"));
    const Outcome run = Execute(lowered.ir(), {Widen(91)}, &log);
    ASSERT_EQ(QL_STATUS_OK, run.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
        << ql_ir_interp_ub_reason_string(run.result.ub_reason);
    EXPECT_EQ(0u, run.result.has_value);
    EXPECT_EQ(91, CALLEE_storage);
    EXPECT_EQ(std::vector<std::string>{"CALLEE_sink"}, log.symbols);
    EXPECT_EQ(1u, run.result.events);
}

TEST(CLowerCalls, AnIndirectCallCarriesItsTargetAndMatchesCompiledC) {
    Lowered lowered;
    struct CALL_VTABLE native = {CALLEE_double, CALLEE_high};
    struct CALL_VTABLE image = native;
    ql_ir_interp_object_v1 object{};
    ql_ir_interp_object_init(&object);
    object.base = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;
    object.size = sizeof(image);
    object.initial = &image;
    ASSERT_TRUE(lowered.Open(indirect_source, "call_indirect"));
    for (int32_t value : {-91, 0, 37, 1000}) {
        CallLog log;
        const Outcome run = Execute(
            lowered.ir(), {object.base, Widen(value)}, &log, &object);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(call_indirect(&native, value), Returned(run.result));
        EXPECT_EQ(std::vector<std::string>{"__ql_indirect_call_v1"},
                  log.symbols);
        EXPECT_EQ(1u, run.result.events);
    }

    image.callback = nullptr;
    CallLog null_log;
    const Outcome null_run = Execute(
        lowered.ir(), {object.base, Widen(7)}, &null_log, &object);
    ASSERT_EQ(QL_STATUS_OK, null_run.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              null_run.result.outcome);
    EXPECT_TRUE(null_log.symbols.empty());
    EXPECT_EQ(0u, null_run.result.events);

    Lowered pointer_lowered;
    CallLog pointer_log;
    image = native;
    ASSERT_TRUE(pointer_lowered.Open(indirect_source,
                                     "call_indirect_pointer"));
    const Outcome pointer_run = Execute(
        pointer_lowered.ir(), {object.base}, &pointer_log, &object);
    ASSERT_EQ(QL_STATUS_OK, pointer_run.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, pointer_run.result.outcome)
        << ql_ir_interp_ub_reason_string(pointer_run.result.ub_reason);
    EXPECT_EQ(call_indirect_pointer(&native), Returned(pointer_run.result));
    EXPECT_EQ(std::vector<std::string>{"__ql_indirect_call_v1"},
              pointer_log.symbols);
}

TEST(CLowerCalls, CarriesAFunctionPointerAsAnOpaqueExternalArgument) {
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(callback_argument_source, "pass_callback"));

    Lowered assigned;
    ASSERT_TRUE(assigned.Open(callback_argument_source, "select_callback"));
}

TEST(CLowerCalls, TakesOnlyTheCallsTheBranchActuallyRan) {
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(branching_source, "call_branch"));
    for (int32_t b : {0, 1, 7}) {
        CallLog log;
        SCOPED_TRACE(b);
        const Outcome run = Execute(lowered.ir(), {Widen(5), Widen(b)}, &log);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
        EXPECT_EQ(call_branch(5, b), Returned(run.result));
        EXPECT_EQ(b != 0 ? 1u : 0u, run.result.events);
    }
}

TEST(CLowerCalls, StopsWhenNobodySaysWhatTheCalleeDoes) {
    /* Guessing a result would be guessing the answer, so a run with no
       specification for the callee reports that it does not know rather than
       inventing one. */
    Lowered lowered;
    ql_ir_interp_options_v1 options{};
    ql_ir_interp_result_v1 result{};
    std::vector<ql_ir_interp_input_v1> inputs;
    std::vector<std::vector<uint8_t>> storage;
    ql_ir_view_v1 view{};
    ql_error error{};
    ASSERT_TRUE(lowered.Open(single_source, "call_single"));

    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_get_view(lowered.ir(), &view, &error));
    for (std::size_t index = 0u; index < view.value_count; ++index) {
        ql_ir_value_view_v1 value{};
        ql_ir_type_view_v1 type{};
        value.struct_size = sizeof(value);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_value_at(lowered.ir(), index, &value, &error));
        if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
            continue;
        }
        type.struct_size = sizeof(type);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_type_at(lowered.ir(), value.type, &type, &error));
        inputs.push_back(ql_ir_interp_input_v1{});
        ql_ir_interp_input_init(&inputs.back());
        inputs.back().value = value.id;
        storage.push_back(type.kind == QL_IR_TYPE_MEMORY ||
                                  type.kind == QL_IR_TYPE_EVENT_TRACE
                              ? std::vector<uint8_t>()
                              : Encode(3u, type.bit_width));
    }
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        inputs[index].data = storage[index].empty() ? nullptr
                                                    : storage[index].data();
        inputs[index].size = storage[index].size();
    }
    ql_ir_interp_options_init(&options);
    result.struct_size = sizeof(result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_interp_run(nullptr, lowered.ir(), inputs.data(),
                               inputs.size(), &options, &result, &error));
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNSUPPORTED, result.outcome);
}

TEST(CLowerCalls, RefusesCallsItCannotCheckAgainstADeclaration) {
    struct Case {
        const char *source;
        const char *name;
    };
    const Case cases[] = {
        /* No declaration, so nothing says what the arguments or result are. */
        {"int f(int a) { return missing(a); }", "f"},
        /* The wrong number of arguments is a mistake, not a semantics. */
        {"int CALLEE_sum(int, int);\nint f(int a) { return CALLEE_sum(a); }",
         "f"},
        /* A real extern object mixed with a prototype still needs external
           object memory semantics. */
        {"int f(int a) { extern int CALLEE_double(int), external_value; "
         "return CALLEE_double(a) + external_value; }",
         "f"},
        /* Function pointers are opaque values in this slice. They may be
           transferred and null-tested, but not used as data addresses. */
        {"int f(int (*callback)(int)) { return callback + 1 != 0; }", "f"},
        /* Direct callback invocation is the next slice, not an undeclared
           external call accidentally accepted under the callback's name. */
        {"int f(int (*callback)(int), int value) "
         "{ return callback(value); }",
         "f"},
    };
    for (const Case &item : cases) {
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
        /* A semantic limit is UNKNOWN, never a status failure. */
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_lower_selected_function(nullptr, item.source, size,
                                               unit, &function, &result,
                                               &error))
            << error.message;
        view.struct_size = sizeof(view);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_lower_result_get_view(result, &view, &error));
        EXPECT_EQ(QL_C_LOWER_UNKNOWN, view.support);
        ql_c_lower_result_destroy(result);
        ql_c_frontend_unit_destroy(unit);
    }
}

/* An argument may itself be a call. The outer call has to be handed the
   history and the memory that stand after the inner one ran, or the IR says
   the outer call never saw the inner call's effects. Nothing about the
   returned value shows this, which is why it is checked on the IR directly. */
TEST(CLowerCalls, ANestedCallThreadsItsStateIntoTheOuterCall) {
    static const char source[] =
        "int CALLEE_double(int); int CALLEE_sum(int, int);\n"
        "int nested(int a) { return CALLEE_sum(CALLEE_double(a), a); }\n";
    Lowered lowered;
    ql_ir_view_v1 view{};
    ql_error error{};
    std::vector<ql_ir_instruction_view_v1> calls;

    ASSERT_TRUE(lowered.Open(source, "nested"));
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK, ql_ir_get_view(lowered.ir(), &view, &error));
    for (std::size_t index = 0u; index < view.instruction_count; ++index) {
        ql_ir_instruction_view_v1 instruction{};
        instruction.struct_size = sizeof(instruction);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_instruction_at(lowered.ir(), index, &instruction,
                                       &error));
        if (instruction.opcode == QL_IR_OPCODE_CALL) {
            calls.push_back(instruction);
        }
    }
    ASSERT_EQ(2u, calls.size());
    /* The inner call runs first and produces a trace and a memory; the outer
       call consumes exactly those two. */
    ASSERT_LE(2u, calls[0].result_count);
    ASSERT_LE(2u, calls[1].operand_count);
    EXPECT_EQ(calls[0].results[0], calls[1].operands[0]);
    EXPECT_EQ(calls[0].results[1], calls[1].operands[1]);
}

}  // namespace
