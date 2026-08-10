/* Globals and static storage duration, against real compiled execution.

   A global is an object in the flat memory model exactly as a pointer
   parameter's region is, so reads are loads and writes are stores. That is
   what makes the order of a write to a global and a call fall out rather than
   be arranged: both thread the memory state, so neither can float past the
   other. These tests check that order concretely, by comparing the arguments
   the callee saw against the arguments the compiled reference passed it.

   They also compare the final image of each global against the compiled
   reference's global after the same run, which is the externally reachable
   memory a contract observes. */

#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#define QL_GLOBAL_FUNCTION(name, ...)                                        \
    extern "C" {                                                             \
    __VA_ARGS__                                                              \
    }                                                                        \
    static const char name##_source[] = #__VA_ARGS__

/* The storage the lowered bodies below touch. The reference reads and writes
   these; the interpreter is handed an object with the same initial bytes. */
extern "C" {
int GLOBAL_a = 0;
static int GLOBAL_double_impl(int a) { return a * 2; }
int GLOBAL_double(int a) { return GLOBAL_double_impl(a); }
}

QL_GLOBAL_FUNCTION(reading, extern int GLOBAL_a;
    int global_read(int x) { return GLOBAL_a + x; });
QL_GLOBAL_FUNCTION(block_extern,
    int global_block_extern(int x) {
        extern int GLOBAL_a;
        return GLOBAL_a + x;
    });
QL_GLOBAL_FUNCTION(writing, extern int GLOBAL_a;
    int global_write(int x) {
        GLOBAL_a = x + 1;
        return GLOBAL_a;
    });
QL_GLOBAL_FUNCTION(branching, extern int GLOBAL_a;
    int global_branch(int x) {
        if (x) {
            GLOBAL_a = x;
        }
        return GLOBAL_a;
    });
QL_GLOBAL_FUNCTION(addressing, extern int GLOBAL_a;
    int global_address(int x) {
        int *p = &GLOBAL_a;
        *p = x;
        return GLOBAL_a;
    });
/* A write, a call that reads what was written, another write, another call.
   Getting either order wrong changes the arguments the callee sees. */
QL_GLOBAL_FUNCTION(ordering, extern int GLOBAL_a; int GLOBAL_double(int);
    int global_order(int x) {
        GLOBAL_a = x;
        GLOBAL_a = GLOBAL_double(GLOBAL_a);
        return GLOBAL_double(GLOBAL_a);
    });
/* A stated initial value must reach the body, whatever bytes the caller
   supplied for the object. */
QL_GLOBAL_FUNCTION(initialized, int GLOBAL_init = 7;
    int global_init(int x) { return GLOBAL_init + x; });

/* Two declarations of one name. Written out rather than compiled, because a
   redeclaration is legal C but a redefinition is not legal C++. */
static const char duplicate_source[] =
    "extern int GLOBAL_a;\n"
    "extern int GLOBAL_a;\n"
    "int global_dup(int x) { GLOBAL_a = x; return GLOBAL_a + 1; }\n";

/* A parameter of the same name hides the global for the whole body. */
static const char shadow_source[] =
    "extern int GLOBAL_a;\n"
    "int global_shadow(int GLOBAL_a) { return GLOBAL_a + 1; }\n";

/* A function is interpreted at an arbitrary invocation, so a mutable static
   begins with the persistent image supplied by the caller. Its source
   initializer ran before that invocation and must not be replayed here. */
static const char persistent_static_source[] =
    "int persistent_static(int rounds) {\n"
    "  while (rounds-- > 0) {\n"
    "    static int value = 1;\n"
    "    ++value;\n"
    "    if (rounds == 0) return value;\n"
    "  }\n"
    "  return 0;\n"
    "}\n";

namespace {

constexpr uint64_t kBase = UINT64_C(0x20000);
constexpr uint64_t kStride = UINT64_C(0x1000);

struct CallLog {
    std::vector<std::string> symbols;
    std::vector<int32_t> arguments;
};

int32_t ReadArgument(const ql_ir_interp_argument_v1 &argument) {
    uint32_t raw = 0u;
    const uint8_t *bytes = static_cast<const uint8_t *>(argument.data);
    for (std::size_t index = 0u; index < argument.size && index < 4u;
         ++index) {
        raw |= static_cast<uint32_t>(bytes[index]) << (index * 8u);
    }
    return static_cast<int32_t>(raw);
}

int QL_CALL Invoke(void *user_data, const char *symbol,
                   const ql_ir_interp_argument_v1 *arguments,
                   std::size_t argument_count, void *result,
                   std::size_t result_size) {
    CallLog *log = static_cast<CallLog *>(user_data);
    int32_t value;

    if (std::strcmp(symbol, "GLOBAL_double") != 0 || argument_count != 1u) {
        return 0;
    }
    value = GLOBAL_double_impl(ReadArgument(arguments[0]));
    log->symbols.push_back(symbol);
    log->arguments.push_back(ReadArgument(arguments[0]));
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
                ADD_FAILURE() << "the lowering did not accept " << name << ": "
                              << diagnostic.message;
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
        /* The verifier runs on every lowering these tests produce, so a guard
           the globals path forgot to place is caught here and not only where
           it happens to matter. */
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
    /* Final image of each object, indexed by the name its base parameter
       carries. */
    std::map<std::string, std::vector<uint8_t>> images;
};

/* `initial` gives the bytes each named global starts from, which is how the
   run is put in the same starting state as the compiled reference. */
Outcome Execute(ql_ir *ir, const std::vector<uint64_t> &scalars,
                const std::map<std::string, int32_t> &initial, CallLog *log) {
    ql_ir_view_v1 view{};
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ql_ir_interp_input_v1> inputs;
    std::vector<ql_ir_interp_object_v1> objects;
    std::vector<std::string> object_names;
    std::vector<std::vector<uint8_t>> finals;
    std::vector<std::vector<uint8_t>> initials;
    ql_ir_interp_callees_v1 callees{};
    ql_ir_interp_options_v1 options{};
    ql_error error{};
    Outcome run;
    std::size_t next_scalar = 0u;
    std::size_t next_size = 0u;

    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK, ql_ir_get_view(ir, &view, &error));
    for (std::size_t index = 0u; index < view.value_count; ++index) {
        ql_ir_value_view_v1 value{};
        ql_ir_type_view_v1 type{};
        std::string name;
        value.struct_size = sizeof(value);
        EXPECT_EQ(QL_STATUS_OK, ql_ir_value_at(ir, index, &value, &error));
        if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
            continue;
        }
        type.struct_size = sizeof(type);
        EXPECT_EQ(QL_STATUS_OK, ql_ir_type_at(ir, value.type, &type, &error));
        name = value.name != nullptr ? value.name : "";
        inputs.push_back(ql_ir_interp_input_v1{});
        ql_ir_interp_input_init(&inputs.back());
        inputs.back().value = value.id;
        if (type.kind == QL_IR_TYPE_MEMORY ||
            type.kind == QL_IR_TYPE_EVENT_TRACE) {
            storage.push_back(std::vector<uint8_t>());
        } else if (name.find(".__base") != std::string::npos) {
            const std::size_t at = name.find('@');
            object_names.push_back(name.substr(0u, at));
            storage.push_back(
                Encode(kBase + kStride * object_names.size(), type.bit_width));
        } else if (name.find(".__size") != std::string::npos) {
            /* Everything these tests declare is a four-byte int. */
            storage.push_back(Encode(4u, type.bit_width));
            ++next_size;
        } else {
            EXPECT_LT(next_scalar, scalars.size());
            storage.push_back(Encode(scalars[next_scalar++],
                                     type.kind == QL_IR_TYPE_BOOL
                                         ? 1u
                                         : type.bit_width));
        }
    }
    EXPECT_EQ(object_names.size(), next_size);
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        inputs[index].data = storage[index].empty() ? nullptr
                                                    : storage[index].data();
        inputs[index].size = storage[index].size();
    }
    finals.resize(object_names.size());
    initials.resize(object_names.size());
    for (std::size_t index = 0u; index < object_names.size(); ++index) {
        ql_ir_interp_object_v1 object{};
        const std::map<std::string, int32_t>::const_iterator found =
            initial.find(object_names[index]);
        finals[index].assign(4u, 0u);
        initials[index] = Encode(found != initial.end()
                                     ? static_cast<uint64_t>(
                                           static_cast<uint32_t>(found->second))
                                     : 0u,
                                 32u);
        ql_ir_interp_object_init(&object);
        object.base = kBase + kStride * (index + 1u);
        object.size = 4u;
        object.initial = initials[index].data();
        object.final_image = finals[index].data();
        objects.push_back(object);
    }
    callees.struct_size = sizeof(callees);
    callees.invoke = &Invoke;
    callees.user_data = log;
    ql_ir_interp_options_init(&options);
    options.objects = objects.empty() ? nullptr : objects.data();
    options.object_count = objects.size();
    options.callees = &callees;
    run.result.struct_size = sizeof(run.result);
    run.status = ql_ir_interp_run(nullptr, ir,
                                  inputs.empty() ? nullptr : inputs.data(),
                                  inputs.size(), &options, &run.result,
                                  &error);
    for (std::size_t index = 0u; index < object_names.size(); ++index) {
        run.images[object_names[index]] = finals[index];
    }
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

int32_t ImageValue(const std::vector<uint8_t> &image) {
    uint32_t raw = 0u;
    for (std::size_t index = 0u; index < image.size() && index < 4u; ++index) {
        raw |= static_cast<uint32_t>(image[index]) << (index * 8u);
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

TEST(CLowerGlobals, MatchesCompiledExecutionIncludingTheFinalGlobalState) {
    struct Case {
        const char *label;
        const char *source;
        const char *function;
        int32_t (*reference)(int32_t);
    };
    const Case cases[] = {
        {"read", reading_source, "global_read",
         [](int32_t x) { return global_read(x); }},
        {"block-extern", block_extern_source, "global_block_extern",
         [](int32_t x) { return global_block_extern(x); }},
        {"write", writing_source, "global_write",
         [](int32_t x) { return global_write(x); }},
        {"branch", branching_source, "global_branch",
         [](int32_t x) { return global_branch(x); }},
        {"address", addressing_source, "global_address",
         [](int32_t x) { return global_address(x); }},
    };

    uint64_t state = UINT64_C(0x5c1f2a7d9b34e601);
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.label);
        ASSERT_TRUE(lowered.Open(item.source, item.function));
        for (std::size_t round = 0u; round < 64u; ++round) {
            CallLog log;
            const int32_t start =
                static_cast<int32_t>(NextRandom(&state) % 200u) - 100;
            const int32_t x =
                static_cast<int32_t>(NextRandom(&state) % 200u) - 100;
            int32_t expected_return;
            int32_t expected_global;

            GLOBAL_a = start;
            expected_return = item.reference(x);
            expected_global = GLOBAL_a;

            const Outcome run =
                Execute(lowered.ir(), {Widen(x)}, {{"GLOBAL_a", start}}, &log);
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);
            EXPECT_EQ(expected_return, Returned(run.result));
            ASSERT_EQ(1u, run.images.count("GLOBAL_a"));
            EXPECT_EQ(expected_global,
                      ImageValue(run.images.find("GLOBAL_a")->second));
        }
    }
}

TEST(CLowerGlobals, KeepsWritesAndCallsInTheOrderTheSourceWroteThem) {
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(ordering_source, "global_order"));
    for (int32_t x : {0, 1, 3, -5, 21}) {
        CallLog log;
        int32_t expected_return;
        int32_t expected_global;
        SCOPED_TRACE(x);

        GLOBAL_a = 999;
        expected_return = global_order(x);
        expected_global = GLOBAL_a;

        const Outcome run =
            Execute(lowered.ir(), {Widen(x)}, {{"GLOBAL_a", 999}}, &log);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(expected_return, Returned(run.result));
        EXPECT_EQ(expected_global,
                  ImageValue(run.images.find("GLOBAL_a")->second));
        /* The second call must see what the first call's result was stored
           as. Reordering the store past either call changes this. */
        EXPECT_EQ(std::vector<int32_t>({x, x * 2}), log.arguments);
        EXPECT_EQ(2u, run.result.events);
    }
}

TEST(CLowerGlobals, TakesTheStatedInitialValueOverWhateverMemoryHeld) {
    /* The object is handed bytes that are not 7, so a body that read the
       supplied image instead of the declared initial value would return
       something else. */
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(initialized_source, "global_init"));
    for (int32_t x : {0, 1, -4, 30}) {
        CallLog log;
        SCOPED_TRACE(x);
        const Outcome run = Execute(lowered.ir(), {Widen(x)},
                                    {{"GLOBAL_init", -12345}}, &log);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(global_init(x), Returned(run.result));
        EXPECT_EQ(7, ImageValue(run.images.find("GLOBAL_init")->second));
    }
}

TEST(CLowerGlobals, DeclaringOneNameTwiceStillNamesOneObject) {
    /* Two objects would be assumed disjoint, so the write through one would
       be invisible to the read through the other and the body would return
       whatever the caller happened to supply. */
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(duplicate_source, "global_dup"));
    for (int32_t x : {0, 1, -9, 44}) {
        CallLog log;
        SCOPED_TRACE(x);
        const Outcome run =
            Execute(lowered.ir(), {Widen(x)}, {{"GLOBAL_a", 555}}, &log);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(1u, run.images.size());
        EXPECT_EQ(x + 1, Returned(run.result));
        EXPECT_EQ(x, ImageValue(run.images.find("GLOBAL_a")->second));
    }
}

TEST(CLowerGlobals, AParameterOfTheSameNameHidesTheGlobal) {
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(shadow_source, "global_shadow"));
    for (int32_t x : {0, 3, -11}) {
        CallLog log;
        SCOPED_TRACE(x);
        const Outcome run =
            Execute(lowered.ir(), {Widen(x)}, {{"GLOBAL_a", 555}}, &log);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        /* The body never reaches the global, so it gets no object at all. */
        EXPECT_TRUE(run.images.empty());
        EXPECT_EQ(x + 1, Returned(run.result));
    }
}

TEST(CLowerGlobals, CarriesMutableStaticStateWithoutReplayingItsInitializer) {
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(persistent_static_source, "persistent_static"));
    for (int32_t start : {-9, 0, 41}) {
        for (int32_t rounds : {1, 2, 5}) {
            CallLog log;
            SCOPED_TRACE(start);
            SCOPED_TRACE(rounds);
            const Outcome run = Execute(lowered.ir(), {Widen(rounds)},
                                        {{"value", start}}, &log);
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);
            EXPECT_EQ(start + rounds, Returned(run.result));
            ASSERT_EQ(1u, run.images.count("value"));
            EXPECT_EQ(start + rounds,
                      ImageValue(run.images.find("value")->second));
        }
    }
}

}  // namespace
