/* Storage the function makes for itself.

   A local normally lives in an SSA value, which has no address. Taking one
   means giving the local an object, and these tests check that the object is
   the right size, that reads and writes go through memory once it exists, and
   that a branch merges the storage rather than a value the variable no longer
   has. As elsewhere the reference is the same function compiled by the
   compiler that builds this binary and actually run. */

#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#define QL_LOCAL_FUNCTION(name, ...)                                         \
    extern "C" {                                                             \
    __VA_ARGS__                                                              \
    }                                                                        \
    static const char name##_source[] = #__VA_ARGS__

QL_LOCAL_FUNCTION(echo, int loc_echo(int x) { return *&x; });
/* `[index] = value` fills the elements it names and leaves the rest zero, and
   an omitted bound is the largest index plus one. The reference is stated
   rather than compiled here: an array designator is a C99 form that C++ warns
   about, and the expected value is short enough to write down. */
static const char sparse_source[] =
    "int loc_sparse(int x) {\n"
    "  int table[] = {[4] = 40, [0] = 1, [2] = 20};\n"
    "  table[1] += x;\n"
    "  return table[0] + table[1] + table[2] + table[3] + table[4];\n"
    "}\n";

/* One object with a shape. The first subscript advances a row and the
   second an element, which is the only thing a second bound changes. */
QL_LOCAL_FUNCTION(grid, int loc_grid(int x, int y) {
    int cells[3][4];
    int row;
    int column;
    for (row = 0; row < 3; row++) {
        for (column = 0; column < 4; column++) {
            cells[row][column] = row * 10 + column + x;
        }
    }
    return cells[2][3] + cells[0][0] + cells[1][2] + y;
});
/* Rows, not elements: each brace pair fills one row of the same object. */
QL_LOCAL_FUNCTION(grid_init, int loc_grid_init(int x) {
    int cells[2][3] = {{1, 2, 3}, {4, 5, 6}};
    cells[1][0] += x;
    return cells[0][0] + cells[0][2] + cells[1][0] + cells[1][2];
});
/* Three reads and two writes, in that order. A lowering that kept the object
   in an SSA value would fold the reads into one and the compiled reference
   would stop agreeing about how many accesses happened. */
QL_LOCAL_FUNCTION(volatile_cell, int loc_volatile(int x) {
    volatile int cell = x;
    int total = cell;
    cell = cell + 1;
    return total + cell;
});
QL_LOCAL_FUNCTION(volatile_pointee, int loc_volatile_pointee(int x) {
    volatile int cell = x;
    volatile int *p = &cell;
    *p = *p + 1;
    return *p;
});
QL_LOCAL_FUNCTION(bump, int loc_bump(int x) {
    int v = x + 1;
    int *p = &v;
    *p = *p * 2;
    return v;
});
QL_LOCAL_FUNCTION(branchy, int loc_branch(int a, int b) {
    int v = a;
    int *p = &v;
    if (b) {
        *p = b;
    } else {
        *p = a - 1;
    }
    return v;
});
QL_LOCAL_FUNCTION(two_slots, int loc_two(int a, int b) {
    int x = a;
    int y = b;
    int *p = &x;
    int *q = &y;
    *p = *q + 1;
    return x - y;
});
QL_LOCAL_FUNCTION(narrow_slot, int loc_narrow(int a) {
    short v = a;
    short *p = &v;
    *p = *p + 1;
    return v;
});
QL_LOCAL_FUNCTION(late_init, int loc_late(int a) {
    int v;
    v = a + 3;
    int *p = &v;
    return *p;
});
QL_LOCAL_FUNCTION(branch_init, int loc_branch_init(int a, int b) {
    int v;
    if (b) {
        v = a + 1;
    } else {
        v = a - 1;
    }
    int *p = &v;
    return *p;
});
QL_LOCAL_FUNCTION(braced_scalar, int loc_braced_scalar(int a, int b) {
    int value = {a};
    int *pointer = {(int *)0};
    return value + (pointer == 0) + (b - b);
});
static const char vla_source[] =
    "int loc_vla(int n, int seed) {\n"
    "  int values[n];\n"
    "  for (int i = 0; i < n; ++i) values[i] = seed + i * 3;\n"
    "  return values[n - 1] + (int)sizeof(values);\n"
    "}\n";
static const char vla_once_source[] =
    "int loc_vla_once(int n) {\n"
    "  int values[n++];\n"
    "  values[0] = n;\n"
    "  return values[0] + (int)sizeof(values);\n"
    "}\n";

namespace {

const uint64_t kBase = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;
const uint64_t kStride = 4096u;

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
            ADD_FAILURE() << "the lowering did not accept " << name;
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

/* Gives every declared object a widely spaced region and binds the base and
   size parameters to it. `sizes` is in object order, which is the order the
   base and size parameters appear in, so a slot sized differently from what
   the lowering assumed would trip the pinning assumption rather than pass
   quietly. */
Outcome Execute(ql_ir *ir, const std::vector<uint64_t> &scalars,
                const std::vector<uint64_t> &sizes,
                std::vector<std::vector<uint8_t>> *images) {
    ql_ir_view_v1 view{};
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ql_ir_interp_input_v1> inputs;
    std::vector<ql_ir_interp_object_v1> objects;
    ql_ir_interp_options_v1 options{};
    ql_error error{};
    Outcome run;
    std::size_t next_scalar = 0u;
    std::size_t next_base = 0u;
    std::size_t next_size = 0u;

    images->clear();
    for (std::size_t index = 0u; index < sizes.size(); ++index) {
        images->push_back(std::vector<uint8_t>(
            static_cast<std::size_t>(sizes[index]), 0u));
    }
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK, ql_ir_get_view(ir, &view, &error));
    for (std::size_t index = 0u; index < view.value_count; ++index) {
        ql_ir_value_view_v1 value{};
        ql_ir_type_view_v1 type{};
        const char *name;
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
        } else if (std::strstr(name, ".__base") != nullptr) {
            EXPECT_LT(next_base, sizes.size());
            storage.push_back(Encode(kBase + kStride * next_base,
                                     type.bit_width));
            ++next_base;
        } else if (std::strstr(name, ".__size") != nullptr) {
            EXPECT_LT(next_size, sizes.size());
            storage.push_back(Encode(sizes[next_size], type.bit_width));
            ++next_size;
        } else {
            EXPECT_LT(next_scalar, scalars.size());
            storage.push_back(Encode(scalars[next_scalar++],
                                     type.kind == QL_IR_TYPE_BOOL
                                         ? 1u
                                         : type.bit_width));
        }
    }
    EXPECT_EQ(sizes.size(), next_base);
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        inputs[index].data = storage[index].empty() ? nullptr
                                                    : storage[index].data();
        inputs[index].size = storage[index].size();
    }
    for (std::size_t index = 0u; index < sizes.size(); ++index) {
        ql_ir_interp_object_v1 object{};
        ql_ir_interp_object_init(&object);
        object.base = kBase + kStride * index;
        object.size = sizes[index];
        object.final_image = (*images)[index].data();
        objects.push_back(object);
    }
    ql_ir_interp_options_init(&options);
    options.objects = objects.empty() ? nullptr : objects.data();
    options.object_count = objects.size();
    run.result.struct_size = sizeof(run.result);
    run.status = ql_ir_interp_run(nullptr, ir,
                                  inputs.empty() ? nullptr : inputs.data(),
                                  inputs.size(), &options, &run.result,
                                  &error);
    return run;
}

/* Counts the instructions of one opcode, which is how a test says that an
   access happened exactly as often as the C performs it. */
std::size_t CountOpcode(ql_ir *ir, ql_ir_opcode opcode) {
    ql_ir_view_v1 view{};
    ql_error error{};
    std::size_t count = 0u;
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK, ql_ir_get_view(ir, &view, &error));
    for (std::size_t index = 0u; index < view.instruction_count; ++index) {
        ql_ir_instruction_view_v1 instruction{};
        instruction.struct_size = sizeof(instruction);
        EXPECT_EQ(QL_STATUS_OK,
                  ql_ir_instruction_at(ir, index, &instruction, &error));
        if (instruction.opcode == opcode) {
            ++count;
        }
    }
    return count;
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

void ExpectUnknown(const char *source,
                   ql_c_lower_diagnostic_code expected_code) {
    ql_c_frontend_unit *unit = nullptr;
    ql_c_lower_result *result = nullptr;
    ql_c_function_view function{};
    ql_c_lower_result_view_v1 view{};
    ql_c_lower_diagnostic_view_v1 diagnostic{};
    ql_error error{};
    const std::size_t size = std::strlen(source);

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, source, size, &unit, &error));
    function.struct_size = sizeof(function);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_select_function(unit, "f", 1u, &function,
                                            &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_lower_selected_function(nullptr, source, size, unit,
                                           &function, &result, &error))
        << error.message;
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_lower_result_get_view(result, &view, &error));
    EXPECT_EQ(QL_C_LOWER_UNKNOWN, view.support);
    diagnostic.struct_size = sizeof(diagnostic);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_lower_result_diagnostic_at(result, 0u, &diagnostic,
                                              &error));
    EXPECT_EQ(expected_code, diagnostic.code);
    ql_c_lower_result_destroy(result);
    ql_c_frontend_unit_destroy(unit);
}

TEST(CLowerLocals, MatchesCompiledExecutionWithStorageForLocals) {
    struct Case {
        const char *name;
        const char *source;
        const char *function;
        std::vector<uint64_t> sizes;
        int arguments;
        int32_t (*reference)(int32_t, int32_t);
    };
    const Case cases[] = {
        /* The parameter itself gets storage, sized as an int. */
        {"echo", echo_source, "loc_echo", {4u}, 1,
         [](int32_t a, int32_t) { return loc_echo(a); }},
        {"bump", bump_source, "loc_bump", {4u}, 1,
         [](int32_t a, int32_t) { return loc_bump(a); }},
        {"branch", branchy_source, "loc_branch", {4u}, 2,
         [](int32_t a, int32_t b) { return loc_branch(a, b); }},
        {"two", two_slots_source, "loc_two", {4u, 4u}, 2,
         [](int32_t a, int32_t b) { return loc_two(a, b); }},
        /* A short slot is two bytes, not a machine word. */
        {"narrow", narrow_slot_source, "loc_narrow", {2u}, 1,
         [](int32_t a, int32_t) { return loc_narrow(a); }},
        {"late", late_init_source, "loc_late", {4u}, 1,
         [](int32_t a, int32_t) { return loc_late(a); }},
        {"branch-init", branch_init_source, "loc_branch_init", {4u}, 2,
         [](int32_t a, int32_t b) { return loc_branch_init(a, b); }},
        {"braced-scalar", braced_scalar_source, "loc_braced_scalar", {}, 2,
         [](int32_t a, int32_t b) { return loc_braced_scalar(a, b); }},
    };

    uint64_t state = UINT64_C(0x27f4b8c1590ae362);
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.name);
        ASSERT_TRUE(lowered.Open(item.source, item.function));
        for (std::size_t round = 0u; round < 128u; ++round) {
            std::vector<std::vector<uint8_t>> images;
            /* Small values keep every case inside the defined range, so the
               comparison measures storage rather than signed overflow. */
            const int32_t a =
                static_cast<int32_t>(NextRandom(&state) % 1000u) - 500;
            const int32_t b =
                static_cast<int32_t>(NextRandom(&state) % 1000u) - 500;
            std::vector<uint64_t> scalars;
            scalars.push_back(Widen(a));
            if (item.arguments > 1) {
                scalars.push_back(Widen(b));
            }
            const Outcome run =
                Execute(lowered.ir(), scalars, item.sizes, &images);
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);
            EXPECT_EQ(item.reference(a, b), Returned(run.result));
        }
    }
}

/* A volatile object is storage, every access to it is an event, and the two
   sides of a comparison observe the same trace. The counts below are what say
   the accesses were neither folded together nor invented. */
TEST(CLowerLocals, MakesEveryVolatileAccessAnObservableEvent) {
    Lowered lowered;
    std::vector<std::vector<uint8_t>> images;
    ASSERT_TRUE(lowered.Open(volatile_cell_source, "loc_volatile"));
    /* One store for the initialiser, one for the assignment; three reads. */
    EXPECT_EQ(2u, CountOpcode(lowered.ir(), QL_IR_OPCODE_STORE));
    EXPECT_EQ(3u, CountOpcode(lowered.ir(), QL_IR_OPCODE_LOAD));
    EXPECT_EQ(5u, CountOpcode(lowered.ir(), QL_IR_OPCODE_TRACE_APPEND));

    for (const int32_t input : {-7, 0, 1, 1000}) {
        SCOPED_TRACE(input);
        const Outcome run =
            Execute(lowered.ir(), {Widen(input)}, {4u}, &images);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
        EXPECT_EQ(loc_volatile(input), Returned(run.result));
    }
}

/* `volatile int *p` qualifies the object, not the pointer. The accesses
   through p are the same events the object's own name would produce. */
TEST(CLowerLocals, CarriesTheQualifierThroughAPointerToTheObject) {
    Lowered lowered;
    std::vector<std::vector<uint8_t>> images;
    ASSERT_TRUE(lowered.Open(volatile_pointee_source, "loc_volatile_pointee"));
    EXPECT_EQ(2u, CountOpcode(lowered.ir(), QL_IR_OPCODE_STORE));
    EXPECT_EQ(2u, CountOpcode(lowered.ir(), QL_IR_OPCODE_LOAD));
    EXPECT_EQ(4u, CountOpcode(lowered.ir(), QL_IR_OPCODE_TRACE_APPEND));

    for (const int32_t input : {-7, 0, 1, 1000}) {
        SCOPED_TRACE(input);
        const Outcome run =
            Execute(lowered.ir(), {Widen(input)}, {4u}, &images);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
        EXPECT_EQ(loc_volatile_pointee(input), Returned(run.result));
    }
}

TEST(CLowerLocals, AddressesATwoDimensionalArrayByRowThenElement) {
    Lowered lowered;
    std::vector<std::vector<uint8_t>> images;
    ASSERT_TRUE(lowered.Open(grid_source, "loc_grid"));
    for (const int32_t x : {-5, 0, 7}) {
        for (const int32_t y : {-1, 3}) {
            SCOPED_TRACE(x * 100 + y);
            const Outcome run = Execute(
                lowered.ir(), {Widen(x), Widen(y)}, {3u * 4u * 4u}, &images);
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);
            EXPECT_EQ(loc_grid(x, y), Returned(run.result));
        }
    }
}

TEST(CLowerLocals, FillsATwoDimensionalArrayRowByRow) {
    Lowered lowered;
    std::vector<std::vector<uint8_t>> images;
    ASSERT_TRUE(lowered.Open(grid_init_source, "loc_grid_init"));
    for (const int32_t x : {-9, 0, 4}) {
        SCOPED_TRACE(x);
        const Outcome run =
            Execute(lowered.ir(), {Widen(x)}, {2u * 3u * 4u}, &images);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(loc_grid_init(x), Returned(run.result));
    }
}

TEST(CLowerLocals, FillsTheElementsAnIndexDesignatorNames) {
    Lowered lowered;
    std::vector<std::vector<uint8_t>> images;
    ASSERT_TRUE(lowered.Open(sparse_source, "loc_sparse"));
    for (const int32_t x : {-3, 0, 11}) {
        SCOPED_TRACE(x);
        const Outcome run =
            Execute(lowered.ir(), {Widen(x)}, {5u * 4u}, &images);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        /* 1 + x + 20 + 0 + 40, with the three unnamed elements zero. */
        EXPECT_EQ(61 + x, Returned(run.result));
    }
}

TEST(CLowerLocals, SizesTheSlotFromTheDeclaredTypeNotAWord) {
    /* A two-byte slot must reject a four-byte access. Rounding every slot up
       to a machine word would make this read look legal. */
    Lowered lowered;
    std::vector<std::vector<uint8_t>> images;
    ASSERT_TRUE(lowered.Open(narrow_slot_source, "loc_narrow"));
    const Outcome wrong =
        Execute(lowered.ir(), {Widen(3)}, {4u}, &images);
    ASSERT_EQ(QL_STATUS_OK, wrong.status);
    /* The lowering pinned the size to two, so a table claiming four
       contradicts it and the run says the inputs were outside the assumed
       domain rather than inventing an answer. */
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_ASSUMPTION_VIOLATED, wrong.result.outcome);
}

TEST(CLowerLocals, BindsASingleDimensionVlaAtItsDeclaration) {
    Lowered lowered;
    std::vector<std::vector<uint8_t>> images;
    ASSERT_TRUE(lowered.Open(vla_source, "loc_vla"));
    for (int32_t n : {1, 2, 7, 12}) {
        const int32_t seed = 11 - n;
        const Outcome run =
            Execute(lowered.ir(), {Widen(n), Widen(seed)},
                    {static_cast<uint64_t>(n) * sizeof(int32_t)}, &images);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(seed + (n - 1) * 3 + n * 4, Returned(run.result));
    }

    for (int32_t invalid : {0, -1}) {
        const Outcome run =
            Execute(lowered.ir(), {Widen(invalid), Widen(3)}, {4u}, &images);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                  run.result.outcome);
        EXPECT_EQ(QL_IR_INTERP_UB_GUARD_FAILED, run.result.ub_reason);
    }
}

TEST(CLowerLocals, EvaluatesAVlaBoundOnceAndKeepsItsSize) {
    Lowered lowered;
    std::vector<std::vector<uint8_t>> images;
    ASSERT_TRUE(lowered.Open(vla_once_source, "loc_vla_once"));
    const Outcome run = Execute(lowered.ir(), {Widen(3)}, {12u}, &images);
    ASSERT_EQ(QL_STATUS_OK, run.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
    EXPECT_EQ(16, Returned(run.result));
}

TEST(CLowerLocals, KeepsVariablyModifiedTypesOutsideTheSlice) {
    /* A variably modified array type, as opposed to a plain variable-length
       array object, is still outside this slice. */
    ExpectUnknown("int f(int n) { int a[n][2]; return a[0][0]; }",
                  QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE);
    ExpectUnknown("int f(int n) { int (*p)[n]; return (int)sizeof(*p); }",
                  QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_TYPE);
}

/* A VLA declared inside a loop is sized again on every iteration. One region
   serves the whole function and only has to be large enough; what the
   accesses are checked against is this iteration's byte count. An index that
   the current bound admits but an earlier, larger one would have allowed is
   therefore still out of bounds. */
TEST(CLowerLocals, ChecksALoopVlaAgainstTheCurrentIterationsBound) {
    Lowered lowered;
    std::vector<std::vector<uint8_t>> images;
    static const char source[] =
        "int f(int n) {\n"
        "  int total = 0;\n"
        "  while (n > 0) {\n"
        "    int a[n];\n"
        "    a[n - 1] = n;\n"
        "    total += a[n - 1];\n"
        "    n--;\n"
        "  }\n"
        "  return total;\n"
        "}\n";
    ASSERT_TRUE(lowered.Open(source, "f"));
    for (const int32_t n : {0, 1, 3, 5}) {
        SCOPED_TRACE(n);
        /* The region is sized for the first iteration, which is the largest,
           and every later iteration fits inside it. */
        const Outcome run = Execute(
            lowered.ir(), {Widen(n)},
            {static_cast<uint64_t>(n > 0 ? n : 1) * 4u}, &images);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        int32_t expected = 0;
        for (int32_t k = n; k > 0; --k) {
            expected += k;
        }
        EXPECT_EQ(expected, Returned(run.result));
    }
}

TEST(CLowerLocals, RefusesStorageWithNothingPutInIt) {
    const char *source = "int f(int a) { int v; int *p = &v; return *p; }";
    /* The address may escape to code that initializes the object. Modeling
       that requires a callee memory-write contract, not an eager UB guard. */
    ExpectUnknown(source, QL_C_LOWER_DIAGNOSTIC_UNINITIALIZED_READ);
}

TEST(CLowerLocals, RefusesStorageInitializedOnOnlyOneBranch) {
    const char *source =
        "int f(int a) { int v; if (a) v = 1; return *&v; }";
    ExpectUnknown(source, QL_C_LOWER_DIAGNOSTIC_UNINITIALIZED_READ);
}

TEST(CLowerLocals, RefusesInvalidScalarBraceInitializers) {
    ExpectUnknown("int f(void) { int value = {1, 2}; return value; }",
                  QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_EXPRESSION);
}

TEST(CLowerLocals, IndeterminateSsaAndCompoundReadsAreUndefined) {
    const char *conditional_source =
        "int f(int take) { int value; if (take) value = 7; return value; }";
    Lowered conditional;
    std::vector<std::vector<uint8_t>> images;
    ASSERT_TRUE(conditional.Open(conditional_source, "f"));
    const Outcome defined = Execute(conditional.ir(), {Widen(1)}, {}, &images);
    ASSERT_EQ(QL_STATUS_OK, defined.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, defined.result.outcome);
    EXPECT_EQ(7, Returned(defined.result));
    const Outcome undefined =
        Execute(conditional.ir(), {Widen(0)}, {}, &images);
    ASSERT_EQ(QL_STATUS_OK, undefined.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              undefined.result.outcome);

    const char *compound_source =
        "int compound(void) { int value; return value += 1; }";
    Lowered compound;
    ASSERT_TRUE(compound.Open(compound_source, "compound"));
    const Outcome compound_run = Execute(compound.ir(), {}, {}, &images);
    ASSERT_EQ(QL_STATUS_OK, compound_run.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              compound_run.result.outcome);
}

TEST(CLowerLocals, ValueFunctionFallthroughIsUndefinedExceptForMain) {
    const char *partial_source =
        "int partial(int take) { if (take) return 9; }";
    Lowered partial;
    std::vector<std::vector<uint8_t>> images;
    ASSERT_TRUE(partial.Open(partial_source, "partial"));
    const Outcome returned = Execute(partial.ir(), {Widen(1)}, {}, &images);
    ASSERT_EQ(QL_STATUS_OK, returned.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, returned.result.outcome);
    EXPECT_EQ(9, Returned(returned.result));
    const Outcome fell_through =
        Execute(partial.ir(), {Widen(0)}, {}, &images);
    ASSERT_EQ(QL_STATUS_OK, fell_through.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              fell_through.result.outcome);

    Lowered main_function;
    ASSERT_TRUE(main_function.Open("int main(void) { }", "main"));
    const Outcome main_result = Execute(main_function.ir(), {}, {}, &images);
    ASSERT_EQ(QL_STATUS_OK, main_result.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, main_result.result.outcome);
    EXPECT_EQ(0, Returned(main_result.result));
}

/* Two declarations of one name in disjoint scopes are two objects, so the
   inner one shadowing the outer is not a collision. Each declaration made its
   own slot, and the address taken of the outer name still reaches the outer
   object. */
TEST(CLowerLocals, GivesAShadowedNameItsOwnStorage) {
    Lowered lowered;
    std::vector<std::vector<uint8_t>> images;
    static const char source[] =
        "int f(int a) { int v; { long long v = a + 1; a += (int)v; } "
        "v = a; return *&v; }";
    ASSERT_TRUE(lowered.Open(source, "f"));
    for (const int32_t a : {-4, 0, 9}) {
        SCOPED_TRACE(a);
        const Outcome run =
            Execute(lowered.ir(), {Widen(a)}, {4u, 8u}, &images);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(a + (a + 1), Returned(run.result));
    }
}

}  // namespace
