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

#include <algorithm>
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
int CALLEE_write_out(int *output, int value) {
    *output = value * 3;
    return value - 5;
}
double CALLEE_scale(float value) { return (double)value * 2.5; }
double CALLEE_variadic_float(int tag, ...) {
    va_list arguments;
    va_start(arguments, tag);
    const double value = va_arg(arguments, double);
    va_end(arguments);
    return (double)tag + value;
}
struct CALL_RECORD {
    int left;
    short right;
    unsigned char tag;
};
int CALLEE_record(struct CALL_RECORD value) {
    return value.left + value.right * 3 + value.tag * 11;
}
struct CALL_RETURN_RECORD {
    unsigned long long wide;
    int middle;
    unsigned char tag;
};
struct CALL_RETURN_RECORD CALLEE_make_record(int left, int right) {
    struct CALL_RETURN_RECORD value{};
    value.wide = (static_cast<unsigned long long>(
                      static_cast<unsigned int>(left))
                  << 32u) |
                 static_cast<unsigned int>(right);
    value.middle = left - right;
    value.tag = 9u;
    return value;
}
int call_record_reference(int left, int right) {
    struct CALL_RECORD value = {left, static_cast<short>(right), 7u};
    return CALLEE_record(value) + 5;
}
int call_record_return_reference(int left, int right) {
    const struct CALL_RETURN_RECORD value = CALLEE_make_record(left, right);
    return static_cast<int>(value.wide & 0xffu) + value.middle * 3 +
           value.tag * 11;
}
int call_record_return_member_reference(int left, int right) {
    return CALLEE_make_record(left, right).middle;
}
struct SELECTED_RECORD {
    int value;
    unsigned char tag;
};
struct SELECTED_RECORD selected_record_roundtrip(struct SELECTED_RECORD input) {
    input.value += input.tag;
    input.tag = static_cast<unsigned char>(input.tag + 3u);
    return input;
}
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
QL_CALL_FUNCTION(outlocal, int CALLEE_write_out(int *, int);
    int call_out_local(int value) {
        int output;
        int status = CALLEE_write_out(&output, value);
        return output + status;
    });
QL_CALL_FUNCTION(float_result, double CALLEE_scale(float);
    double call_float(float value) { return CALLEE_scale(value) + 0.25; });
QL_CALL_FUNCTION(variadic_float, double CALLEE_variadic_float(int, ...);
    double call_variadic_float(float value) {
        return CALLEE_variadic_float(2, value);
    });
static const char record_argument_source[] =
    "struct CALL_RECORD { int left; short right; unsigned char tag; };\n"
    "int CALLEE_record(struct CALL_RECORD);\n"
    "int call_record(int left, int right) {\n"
    "  struct CALL_RECORD value = {left, (short)right, 7};\n"
    "  return CALLEE_record(value) + 5;\n"
    "}\n";
static const char record_return_source[] =
    "struct CALL_RETURN_RECORD { unsigned long long wide; int middle; "
    "unsigned char tag; };\n"
    "struct CALL_RETURN_RECORD CALLEE_make_record(int, int);\n"
    "int call_record_return(int left, int right) {\n"
    "  struct CALL_RETURN_RECORD value = CALLEE_make_record(left, right);\n"
    "  return (int)(value.wide & 255u) + value.middle * 3 + "
    "value.tag * 11;\n"
    "}\n"
    "int call_record_return_member(int left, int right) {\n"
    "  return CALLEE_make_record(left, right).middle;\n"
    "}\n";
static const char selected_record_source[] =
    "struct SELECTED_RECORD { int value; unsigned char tag; };\n"
    "struct SELECTED_RECORD selected_record_roundtrip(\n"
    "    struct SELECTED_RECORD input) {\n"
    "  input.value += input.tag;\n"
    "  input.tag = (unsigned char)(input.tag + 3u);\n"
    "  return input;\n"
    "}\n";
QL_CALL_FUNCTION(indirect, struct CALL_VTABLE {
        int (*callback)(int);
        char *(*pointer_callback)(void);
    };
    int call_indirect(struct CALL_VTABLE *table, int value) {
        return table->callback(value) + 1;
    }
    int call_indirect_pointer(struct CALL_VTABLE *table) {
        return table->pointer_callback() == 0;
    }
    int call_indirect_parenthesized(struct CALL_VTABLE *table, int value) {
        return (*table->callback)(value) - 1;
    });
QL_CALL_FUNCTION(parameter_indirect,
    int call_parameter_indirect(int (*callback)(int), int value) {
        return callback(value) + 1;
    }
    int call_parameter_indirect_parenthesized(int (*callback)(int), int value) {
        return (*callback)(value) + 1;
    });
QL_CALL_FUNCTION(typedef_parameter_indirect,
    typedef int (*CALL_INT_TYPEDEF)(int);
    int call_typedef_parameter(CALL_INT_TYPEDEF callback, int value) {
        return callback(value) - 2;
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
static const char callback_typedef_argument_source[] =
    "typedef int (*CALL_INT_TYPEDEF)(int);\n"
    "int CALLEE_accept(CALL_INT_TYPEDEF, int);\n"
    "int pass_typedef_callback(CALL_INT_TYPEDEF callback, int value) {\n"
    "  return CALLEE_accept(callback, value);\n"
    "}\n";

namespace {

/* Everything the interpreter's callee hook saw, in the order it saw it. */
struct CallLog {
    std::vector<std::string> symbols;
    std::vector<std::vector<int32_t>> arguments;
    bool out_local_was_written = true;
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
    if (std::strcmp(symbol, "CALLEE_write_out") == 0 &&
        argument_count == 2u) {
        if (result_size != 9u) {
            ADD_FAILURE() << "return, out value, and definedness need 9 bytes, "
                          << "not " << result_size;
            return 0;
        }
        int32_t supplied = 0;
        const int32_t returned = CALLEE_write_out(&supplied, seen[1]);
        const uint32_t values[2] = {static_cast<uint32_t>(returned),
                                    static_cast<uint32_t>(supplied)};
        uint8_t *bytes = static_cast<uint8_t *>(result);
        for (std::size_t value_index = 0u; value_index < 2u; ++value_index) {
            for (std::size_t byte = 0u; byte < 4u; ++byte) {
                bytes[value_index * 4u + byte] = static_cast<uint8_t>(
                    (values[value_index] >> (byte * 8u)) & 0xffu);
            }
        }
        bytes[8] = log->out_local_was_written ? 1u : 0u;
        log->symbols.push_back(symbol);
        log->arguments.push_back(seen);
        return 1;
    }
    if (std::strcmp(symbol, "CALLEE_scale") == 0 && argument_count == 1u) {
        if (arguments[0].size != sizeof(float) ||
            result_size != sizeof(double)) {
            ADD_FAILURE() << "float call widths are " << arguments[0].size
                          << " and " << result_size;
            return 0;
        }
        float input = 0.0f;
        std::memcpy(&input, arguments[0].data, sizeof(input));
        const double returned = CALLEE_scale(input);
        std::memcpy(result, &returned, sizeof(returned));
        log->symbols.push_back(symbol);
        log->arguments.push_back(seen);
        return 1;
    }
    if (std::strcmp(symbol, "CALLEE_variadic_float") == 0 &&
        argument_count == 2u) {
        if (arguments[0].size != sizeof(int32_t) ||
            arguments[1].size != sizeof(double) ||
            result_size != sizeof(double)) {
            ADD_FAILURE() << "variadic float was not promoted to double";
            return 0;
        }
        double promoted = 0.0;
        std::memcpy(&promoted, arguments[1].data, sizeof(promoted));
        const double returned = (double)seen[0] + promoted;
        std::memcpy(result, &returned, sizeof(returned));
        log->symbols.push_back(symbol);
        log->arguments.push_back(seen);
        return 1;
    }
    if (std::strcmp(symbol, "CALLEE_record") == 0 && argument_count == 1u) {
        if (arguments[0].size != sizeof(uint64_t) ||
            result_size != sizeof(int32_t)) {
            ADD_FAILURE() << "record call image has the wrong packed width";
            return 0;
        }
        CALL_RECORD record{};
        std::memcpy(&record, arguments[0].data, sizeof(record));
        value = CALLEE_record(record);
    } else if (std::strcmp(symbol, "CALLEE_make_record") == 0 &&
               argument_count == 2u) {
        if (result_size != sizeof(CALL_RETURN_RECORD)) {
            ADD_FAILURE() << "record return image has the wrong packed width";
            return 0;
        }
        const CALL_RETURN_RECORD record =
            CALLEE_make_record(seen[0], seen[1]);
        std::memcpy(result, &record, sizeof(record));
        log->symbols.push_back(symbol);
        log->arguments.push_back(seen);
        return 1;
    } else if (std::strcmp(symbol, "CALLEE_double") == 0 &&
               argument_count == 1u) {
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
                const ql_ir_interp_object_v1 *dynamic_object = nullptr,
                std::size_t dynamic_object_count = 0u) {
    ql_ir_view_v1 view{};
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ql_ir_interp_input_v1> inputs;
    ql_ir_interp_callees_v1 callees{};
    ql_ir_interp_options_v1 options{};
    ql_error error{};
    Outcome run;
    std::size_t next = 0u;
    std::size_t next_object = 0u;
    const std::size_t supplied_object_count =
        dynamic_object_count != 0u ? dynamic_object_count
                                   : (dynamic_object != nullptr ? 1u : 0u);

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
            const std::size_t which =
                supplied_object_count == 0u
                    ? 0u
                    : std::min(next_object, supplied_object_count - 1u);
            storage.push_back(Encode(dynamic_object != nullptr
                                         ? dynamic_object[which].base
                                         : 0u,
                                     type.bit_width));
            continue;
        }
        if (name.find(".__size") != std::string::npos) {
            EXPECT_NE(nullptr, dynamic_object);
            const std::size_t which =
                supplied_object_count == 0u
                    ? 0u
                    : std::min(next_object, supplied_object_count - 1u);
            storage.push_back(Encode(dynamic_object != nullptr
                                         ? dynamic_object[which].size
                                         : 0u,
                                     type.bit_width));
            ++next_object;
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
        options.object_count = supplied_object_count;
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

uint64_t FloatBits(float value) {
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double ReturnedDouble(const ql_ir_interp_result_v1 &result) {
    double value = 0.0;
    EXPECT_EQ(sizeof(value), result.value_size);
    if (result.value_size == sizeof(value)) {
        std::memcpy(&value, result.value, sizeof(value));
    }
    return value;
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

TEST(CLowerCalls, CarriesFloatingArgumentsResultsAndVariadicPromotion) {
    for (float input : {-3.25f, 0.0f, 7.5f}) {
        Lowered direct;
        CallLog direct_log;
        ASSERT_TRUE(direct.Open(float_result_source, "call_float"));
        const Outcome direct_run =
            Execute(direct.ir(), {FloatBits(input)}, &direct_log);
        ASSERT_EQ(QL_STATUS_OK, direct_run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, direct_run.result.outcome);
        EXPECT_EQ(call_float(input), ReturnedDouble(direct_run.result));
        EXPECT_EQ(std::vector<std::string>{"CALLEE_scale"},
                  direct_log.symbols);

        Lowered variadic;
        CallLog variadic_log;
        ASSERT_TRUE(variadic.Open(variadic_float_source,
                                  "call_variadic_float"));
        const Outcome variadic_run =
            Execute(variadic.ir(), {FloatBits(input)}, &variadic_log);
        ASSERT_EQ(QL_STATUS_OK, variadic_run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, variadic_run.result.outcome);
        EXPECT_EQ(call_variadic_float(input),
                  ReturnedDouble(variadic_run.result));
        EXPECT_EQ(std::vector<std::string>{"CALLEE_variadic_float"},
                  variadic_log.symbols);
    }
}

TEST(CLowerCalls, PassesARecordByValueAsItsPackedObjectImage) {
    Lowered lowered;
    uint8_t initial[sizeof(CALL_RECORD)]{};
    ql_ir_interp_object_v1 object{};
    ql_ir_interp_object_init(&object);
    object.base = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;
    object.size = sizeof(initial);
    object.initial = initial;
    ASSERT_TRUE(lowered.Open(record_argument_source, "call_record"));
    for (int32_t left : {-91, 0, 37}) {
        for (int32_t right : {-17, 0, 29}) {
            CallLog log;
            const Outcome run = Execute(
                lowered.ir(), {Widen(left), Widen(right)}, &log, &object);
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);
            EXPECT_EQ(call_record_reference(left, right), Returned(run.result));
            EXPECT_EQ(std::vector<std::string>{"CALLEE_record"}, log.symbols);
        }
    }
}

TEST(CLowerCalls, MaterializesARecordReturnedByValue) {
    for (const char *function : {"call_record_return",
                                 "call_record_return_member"}) {
        Lowered lowered;
        uint8_t first_initial[sizeof(CALL_RETURN_RECORD)]{};
        uint8_t second_initial[sizeof(CALL_RETURN_RECORD)]{};
        ql_ir_interp_object_v1 objects[2]{};
        const std::size_t object_count =
            std::strcmp(function, "call_record_return") == 0 ? 2u : 1u;
        for (std::size_t index = 0u; index < object_count; ++index) {
            ql_ir_interp_object_init(&objects[index]);
            objects[index].base = QL_IR_INTERP_FIRST_OBJECT_ADDRESS +
                                  index * sizeof(CALL_RETURN_RECORD);
            objects[index].size = sizeof(CALL_RETURN_RECORD);
            objects[index].initial =
                index == 0u ? first_initial : second_initial;
        }
        ASSERT_TRUE(lowered.Open(record_return_source, function));
        for (int32_t left : {-91, 0, 37}) {
            for (int32_t right : {-17, 0, 29}) {
                CallLog log;
                const Outcome run = Execute(
                    lowered.ir(), {Widen(left), Widen(right)}, &log, objects,
                    object_count);
                ASSERT_EQ(QL_STATUS_OK, run.status);
                ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                    << ql_ir_interp_ub_reason_string(run.result.ub_reason);
                const int expected =
                    std::strcmp(function, "call_record_return") == 0
                        ? call_record_return_reference(left, right)
                        : call_record_return_member_reference(left, right);
                EXPECT_EQ(expected, Returned(run.result));
                EXPECT_EQ(std::vector<std::string>{"CALLEE_make_record"},
                          log.symbols);
            }
        }
    }
}

TEST(CLowerCalls, CarriesASelectedFunctionsRecordBoundaryAsAnObjectImage) {
    static_assert(sizeof(SELECTED_RECORD) == sizeof(uint64_t));
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(selected_record_source,
                             "selected_record_roundtrip"));
    for (const int32_t value : {-91, 0, 37}) {
        for (const uint8_t tag : {uint8_t{0}, uint8_t{9}, uint8_t{251}}) {
            SELECTED_RECORD input{};
            input.value = value;
            input.tag = tag;
            uint64_t image = 0u;
            std::memcpy(&image, &input, sizeof(input));

            uint8_t initial[sizeof(SELECTED_RECORD)]{};
            ql_ir_interp_object_v1 object{};
            ql_ir_interp_object_init(&object);
            object.base = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;
            object.size = sizeof(SELECTED_RECORD);
            object.initial = initial;
            CallLog log;
            const Outcome run =
                Execute(lowered.ir(), {image}, &log, &object, 1u);
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);
            ASSERT_EQ(sizeof(SELECTED_RECORD), run.result.value_size);

            const SELECTED_RECORD expected =
                selected_record_roundtrip(input);
            SELECTED_RECORD actual{};
            std::memcpy(&actual, run.result.value, sizeof(actual));
            EXPECT_EQ(expected.value, actual.value);
            EXPECT_EQ(expected.tag, actual.tag);
            EXPECT_TRUE(log.symbols.empty());
        }
    }
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

TEST(CLowerCalls, CarriesOutLocalValueAndDefinednessFromTheSameCall) {
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(outlocal_source, "call_out_local"));

    for (int32_t value : {-91, 0, 37, 1000}) {
        int32_t initial = 0x12345678;
        int32_t final_image = 0;
        ql_ir_interp_object_v1 object{};
        ql_ir_interp_object_init(&object);
        object.base = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;
        object.size = sizeof(initial);
        object.initial = &initial;
        object.final_image = &final_image;
        CallLog log;
        const Outcome run =
            Execute(lowered.ir(), {Widen(value)}, &log, &object);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(call_out_local(value), Returned(run.result));
        EXPECT_EQ(value * 3, final_image);
        EXPECT_EQ(std::vector<std::string>{"CALLEE_write_out"}, log.symbols);
    }

    int32_t initial = 73;
    ql_ir_interp_object_v1 object{};
    ql_ir_interp_object_init(&object);
    object.base = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;
    object.size = sizeof(initial);
    object.initial = &initial;
    CallLog no_write;
    no_write.out_local_was_written = false;
    const Outcome undefined =
        Execute(lowered.ir(), {Widen(11)}, &no_write, &object);
    ASSERT_EQ(QL_STATUS_OK, undefined.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              undefined.result.outcome);
    EXPECT_EQ(QL_IR_INTERP_UB_GUARD_FAILED, undefined.result.ub_reason);
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

    Lowered parenthesized;
    ASSERT_TRUE(parenthesized.Open(indirect_source,
                                   "call_indirect_parenthesized"));
    for (int32_t value : {-17, 0, 41}) {
        CallLog log;
        const Outcome run = Execute(
            parenthesized.ir(), {object.base, Widen(value)}, &log, &object);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
        EXPECT_EQ(call_indirect_parenthesized(&native, value),
                  Returned(run.result));
        EXPECT_EQ(std::vector<std::string>{"__ql_indirect_call_v1"},
                  log.symbols);
    }
}

TEST(CLowerCalls, CarriesAFunctionPointerAsAnOpaqueExternalArgument) {
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(callback_argument_source, "pass_callback"));

    Lowered assigned;
    ASSERT_TRUE(assigned.Open(callback_argument_source, "select_callback"));

    Lowered typed;
    ASSERT_TRUE(typed.Open(callback_typedef_argument_source,
                           "pass_typedef_callback"));
}

TEST(CLowerCalls, CallsAFunctionPointerParameterWithItsDeclaredSignature) {
    Lowered lowered;
    uint8_t dummy = 0u;
    ql_ir_interp_object_v1 object{};
    ql_ir_interp_object_init(&object);
    object.base = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;
    object.size = 1u;
    object.initial = &dummy;

    ASSERT_TRUE(lowered.Open(parameter_indirect_source,
                             "call_parameter_indirect"));
    for (int32_t value : {-91, 0, 37, 1000}) {
        CallLog log;
        const Outcome run = Execute(
            lowered.ir(),
            {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
                 &CALLEE_double)),
             Widen(value)},
            &log, &object);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(call_parameter_indirect(CALLEE_double, value),
                  Returned(run.result));
        EXPECT_EQ(std::vector<std::string>{"__ql_indirect_call_v1"},
                  log.symbols);
    }

    CallLog null_log;
    const Outcome null_run =
        Execute(lowered.ir(), {0u, Widen(7)}, &null_log, &object);
    ASSERT_EQ(QL_STATUS_OK, null_run.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              null_run.result.outcome);
    EXPECT_TRUE(null_log.symbols.empty());
}

TEST(CLowerCalls, CallsAParenthesizedFunctionPointerParameter) {
    Lowered lowered;
    uint8_t dummy = 0u;
    ql_ir_interp_object_v1 object{};
    ql_ir_interp_object_init(&object);
    object.base = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;
    object.size = 1u;
    object.initial = &dummy;

    ASSERT_TRUE(lowered.Open(parameter_indirect_source,
                             "call_parameter_indirect_parenthesized"));
    for (int32_t value : {-91, 0, 37, 1000}) {
        CallLog log;
        const Outcome run = Execute(
            lowered.ir(),
            {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
                 &CALLEE_double)),
             Widen(value)},
            &log, &object);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(call_parameter_indirect_parenthesized(CALLEE_double, value),
                  Returned(run.result));
        EXPECT_EQ(std::vector<std::string>{"__ql_indirect_call_v1"},
                  log.symbols);
    }

    CallLog null_log;
    const Outcome null_run =
        Execute(lowered.ir(), {0u, Widen(7)}, &null_log, &object);
    ASSERT_EQ(QL_STATUS_OK, null_run.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              null_run.result.outcome);
    EXPECT_TRUE(null_log.symbols.empty());
}

TEST(CLowerCalls, CallsAFunctionPointerTypedefParameter) {
    Lowered lowered;
    uint8_t dummy = 0u;
    ql_ir_interp_object_v1 object{};
    ql_ir_interp_object_init(&object);
    object.base = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;
    object.size = 1u;
    object.initial = &dummy;

    ASSERT_TRUE(lowered.Open(typedef_parameter_indirect_source,
                             "call_typedef_parameter"));
    for (int32_t value : {-91, 0, 37, 1000}) {
        CallLog log;
        const Outcome run = Execute(
            lowered.ir(),
            {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
                 &CALLEE_double)),
             Widen(value)},
            &log, &object);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(call_typedef_parameter(CALLEE_double, value),
                  Returned(run.result));
        EXPECT_EQ(std::vector<std::string>{"__ql_indirect_call_v1"},
                  log.symbols);
    }

    CallLog null_log;
    const Outcome null_run =
        Execute(lowered.ir(), {0u, Widen(7)}, &null_log, &object);
    ASSERT_EQ(QL_STATUS_OK, null_run.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              null_run.result.outcome);
    EXPECT_TRUE(null_log.symbols.empty());
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
        /* A direct record return has a preallocated temporary. An indirect
           target's return type is known, but no static call site identifies
           storage during the object prepass, so it remains UNKNOWN rather
           than becoming an internal status failure. */
        {"struct R { int value; };"
         " int f(struct R (*callback)(void)) {"
         " struct R value = callback(); return value.value; }",
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
