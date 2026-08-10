/* Pointer lowering, end to end and against real compiled execution.

   The reference functions here are compiled by the same compiler that builds
   this binary and run on real arrays, exactly as in test_ir_differential.cpp.
   The IR sees a symbolic object instead, at QL_IR_INTERP_FIRST_OBJECT_ADDRESS,
   which is sound to compare because none of these functions returns an
   address: only the loaded and stored data crosses the comparison.

   These tests also check the object-parameter layout the lowering commits to,
   because W2 builds the miter's memory half against it. The GNU-C-only void
   pointer arithmetic case cannot be compiled verbatim in this C++ test
   translation unit, so its reference uses the equivalent char-pointer byte
   arithmetic while the lowered source retains the GNU C spelling. */

#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#define QL_PTR_FUNCTION(name, ...)                                           \
    extern "C" {                                                             \
    __VA_ARGS__                                                              \
    }                                                                        \
    static const char name##_source[] = #__VA_ARGS__

QL_PTR_FUNCTION(head, int ptr_head(int *p) { return *p; });
QL_PTR_FUNCTION(index, int ptr_index(int *p, int i) { return p[i]; });
QL_PTR_FUNCTION(offset, int ptr_offset(int *p, int i) { return *(p + i); });
QL_PTR_FUNCTION(back, int ptr_back(int *p, int i) { return *(p + 3 - i); });
QL_PTR_FUNCTION(write, int ptr_write(int *p, int v) {
    p[1] = v;
    return p[1] + p[0];
});
QL_PTR_FUNCTION(swap_first, int ptr_swap(int *p) {
    int t = p[0];
    p[0] = p[1];
    p[1] = t;
    return p[0];
});
/* A compound assignment and an increment through a pointer read, combine, and
   write the same element, so the final image says whether the address was
   computed once and used for both. */
QL_PTR_FUNCTION(bump, int ptr_bump(int *p, int i) {
    p[i] += 7;
    return p[i] + p[0];
});
QL_PTR_FUNCTION(step, int ptr_step(int *p, int i) {
    int old = p[i]++;
    --p[0];
    return old * 3 + p[i];
});
QL_PTR_FUNCTION(nullness, int ptr_null(int *p) { return p == 0; });
QL_PTR_FUNCTION(conditional_null_right,
    int ptr_conditional_null_right(int *p, int choose) {
        return (choose ? p : 0) == 0;
    });
QL_PTR_FUNCTION(conditional_null_left,
    int ptr_conditional_null_left(int *p, int choose) {
        return (choose ? 0 : p) == 0;
    });
QL_PTR_FUNCTION(conditional_null_dereference,
    int ptr_conditional_null_dereference(int *p, int choose) {
        return *(choose ? p : 0);
    });
QL_PTR_FUNCTION(narrow_load, int ptr_short(short *p, int i) {
    return p[i];
});
QL_PTR_FUNCTION(local_decl, int ptr_local(int *p, int i) {
    int *cursor = p + i;
    return *cursor;
});
QL_PTR_FUNCTION(distance, int ptr_distance(int *p, int i) {
    int *far = p + i;
    return (int) (far - p);
});
QL_PTR_FUNCTION(named_pointer, typedef int *QL_PTR_INTP;
    int ptr_named(QL_PTR_INTP p, int i) { return p[i]; });
QL_PTR_FUNCTION(address_of_element, int ptr_address(int *p, int i) {
    int *slot = &p[i];
    return *slot + 1;
});
/* None of these designators is evaluated. In particular the far-out index
   must not emit an access guard or read memory. */
QL_PTR_FUNCTION(sizeof_designators, int ptr_sizeof(int *p, int i) {
    return (int)(sizeof(*p) + sizeof(p[i]) + sizeof(p[1000000]) +
                 sizeof(&p[i]) + sizeof(p));
});

QL_PTR_FUNCTION(cast_through_void, int ptr_cast(int *p, int i) {
    void *raw = (void *)p;
    int *back = (int *)raw;
    return back[i] + 1;
});
QL_PTR_FUNCTION(cast_to_bytes, int ptr_bytes(int *p, int i) {
    char *bytes = (char *)p;
    return bytes[i * 4] + 1;
});
QL_PTR_FUNCTION(cast_from_bits, int ptr_from_bits(unsigned long long address) {
    return ((int *)address)[0];
});
static const char void_arithmetic_source[] =
    "long ptr_void_distance(void *p, int i) {\n"
    "    void *q = p + i;\n"
    "    return q - p;\n"
    "}\n";

namespace {

const uint64_t kBase = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;
const std::size_t kElements = 4u;

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

/* Binds by parameter name, which is also how this test pins the layout the
   lowering commits to: the C arguments first, then __memory, then one
   base/size pair per pointer argument. */
Outcome Execute(ql_ir *ir, uint64_t pointer,
                const std::vector<uint64_t> &scalars, uint64_t object_base,
                uint64_t object_size, const uint8_t *initial,
                uint8_t *final_image) {
    ql_ir_view_v1 view{};
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ql_ir_interp_input_v1> inputs;
    ql_ir_interp_object_v1 object{};
    ql_ir_interp_options_v1 options{};
    ql_error error{};
    Outcome run;
    std::size_t next_scalar = 0u;

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
        if (type.kind == QL_IR_TYPE_MEMORY) {
            EXPECT_STREQ("__memory", name);
            storage.push_back(std::vector<uint8_t>());
        } else if (type.kind == QL_IR_TYPE_POINTER) {
            storage.push_back(Encode(pointer, type.bit_width));
        } else if (std::strstr(name, ".__base") != nullptr) {
            storage.push_back(Encode(object_base, type.bit_width));
        } else if (std::strstr(name, ".__size") != nullptr) {
            storage.push_back(Encode(object_size, type.bit_width));
        } else {
            EXPECT_LT(next_scalar, scalars.size());
            storage.push_back(Encode(scalars[next_scalar++],
                                     type.kind == QL_IR_TYPE_BOOL
                                         ? 1u
                                         : type.bit_width));
        }
    }
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        inputs[index].data = storage[index].empty() ? nullptr
                                                    : storage[index].data();
        inputs[index].size = storage[index].size();
    }
    ql_ir_interp_object_init(&object);
    object.base = object_base;
    object.size = object_size;
    object.initial = initial;
    object.final_image = final_image;
    ql_ir_interp_options_init(&options);
    options.objects = &object;
    options.object_count = 1u;
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

TEST(CLowerPointers, LoadsThroughAPointerParameter) {
    Lowered lowered;
    const int32_t data[kElements] = {11, 22, 33, 44};
    ASSERT_TRUE(lowered.Open(head_source, "ptr_head"));
    const Outcome run =
        Execute(lowered.ir(), kBase, {}, kBase, sizeof(data),
                reinterpret_cast<const uint8_t *>(data), nullptr);
    ASSERT_EQ(QL_STATUS_OK, run.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
        << ql_ir_interp_ub_reason_string(run.result.ub_reason);
    EXPECT_EQ(11, Returned(run.result));
}

TEST(CLowerPointers, StoresThroughAPointerParameterAndLeavesThemVisible) {
    Lowered lowered;
    const int32_t data[kElements] = {5, 6, 7, 8};
    int32_t final_image[kElements] = {0, 0, 0, 0};
    ASSERT_TRUE(lowered.Open(write_source, "ptr_write"));
    const Outcome run =
        Execute(lowered.ir(), kBase, {99u}, kBase, sizeof(data),
                reinterpret_cast<const uint8_t *>(data),
                reinterpret_cast<uint8_t *>(final_image));
    ASSERT_EQ(QL_STATUS_OK, run.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
        << ql_ir_interp_ub_reason_string(run.result.ub_reason);
    EXPECT_EQ(104, Returned(run.result));
    EXPECT_EQ(99, final_image[1]);
    EXPECT_EQ(5, final_image[0]);
}

TEST(CLowerPointers, TreatsANullDereferenceAsUndefined) {
    Lowered lowered;
    const int32_t data[kElements] = {1, 2, 3, 4};
    ASSERT_TRUE(lowered.Open(head_source, "ptr_head"));
    /* The guard the lowering emits asks whether the address is inside the
       object, and no object may contain address zero. */
    const Outcome run =
        Execute(lowered.ir(), 0u, {}, kBase, sizeof(data),
                reinterpret_cast<const uint8_t *>(data), nullptr);
    ASSERT_EQ(QL_STATUS_OK, run.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR, run.result.outcome);
    EXPECT_EQ(QL_IR_INTERP_UB_GUARD_FAILED, run.result.ub_reason)
        << "the guard the lowering emitted should have rejected this, not "
           "the interpreter's own bounds check";
}

TEST(CLowerPointers, RejectsSubscriptsThatLeaveTheObject) {
    Lowered lowered;
    const int32_t data[kElements] = {1, 2, 3, 4};
    ASSERT_TRUE(lowered.Open(index_source, "ptr_index"));
    for (int32_t i : {0, 1, 2, 3}) {
        SCOPED_TRACE(i);
        const Outcome run =
            Execute(lowered.ir(), kBase, {static_cast<uint64_t>(i)}, kBase,
                    sizeof(data), reinterpret_cast<const uint8_t *>(data),
                    nullptr);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
        EXPECT_EQ(data[i], Returned(run.result));
    }
    for (int32_t i : {-1, 4, 100}) {
        SCOPED_TRACE(i);
        const Outcome run =
            Execute(lowered.ir(), kBase, {static_cast<uint64_t>(i)}, kBase,
                    sizeof(data), reinterpret_cast<const uint8_t *>(data),
                    nullptr);
        EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
                  run.result.outcome);
    }
}

TEST(CLowerPointers, ComparesPointersAgainstNull) {
    Lowered lowered;
    const int32_t data[kElements] = {1, 2, 3, 4};
    ASSERT_TRUE(lowered.Open(nullness_source, "ptr_null"));
    EXPECT_EQ(1, Returned(Execute(lowered.ir(), 0u, {}, kBase, sizeof(data),
                                  reinterpret_cast<const uint8_t *>(data),
                                  nullptr)
                              .result));
    EXPECT_EQ(0, Returned(Execute(lowered.ir(), kBase, {}, kBase,
                                  sizeof(data),
                                  reinterpret_cast<const uint8_t *>(data),
                                  nullptr)
                              .result));
}

TEST(CLowerPointers, SelectsBetweenAPointerAndANullPointerConstant) {
    struct Case {
        const char *source;
        const char *function;
        int32_t (*reference)(int32_t *, int32_t);
    };
    const Case cases[] = {
        {conditional_null_right_source, "ptr_conditional_null_right",
         ptr_conditional_null_right},
        {conditional_null_left_source, "ptr_conditional_null_left",
         ptr_conditional_null_left},
    };
    int32_t data[kElements] = {1, 2, 3, 4};
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.function);
        ASSERT_TRUE(lowered.Open(item.source, item.function));
        for (int32_t choose : {0, 1}) {
            const Outcome run =
                Execute(lowered.ir(), kBase,
                        {static_cast<uint64_t>(choose)}, kBase, sizeof(data),
                        reinterpret_cast<const uint8_t *>(data), nullptr);
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
            EXPECT_EQ(item.reference(data, choose), Returned(run.result));
        }
    }
}

TEST(CLowerPointers, KeepsPointerAuthorityAcrossANullConditional) {
    Lowered lowered;
    int32_t data[kElements] = {17, 2, 3, 4};
    ASSERT_TRUE(lowered.Open(conditional_null_dereference_source,
                             "ptr_conditional_null_dereference"));
    const Outcome selected =
        Execute(lowered.ir(), kBase, {1u}, kBase, sizeof(data),
                reinterpret_cast<const uint8_t *>(data), nullptr);
    ASSERT_EQ(QL_STATUS_OK, selected.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, selected.result.outcome);
    EXPECT_EQ(ptr_conditional_null_dereference(data, 1),
              Returned(selected.result));

    const Outcome null_selected =
        Execute(lowered.ir(), kBase, {0u}, kBase, sizeof(data),
                reinterpret_cast<const uint8_t *>(data), nullptr);
    ASSERT_EQ(QL_STATUS_OK, null_selected.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              null_selected.result.outcome);
    EXPECT_EQ(QL_IR_INTERP_UB_GUARD_FAILED, null_selected.result.ub_reason);
}

/* The comparison against real execution. Every input here is in range, so the
   reference is never called on an access C leaves undefined. */
TEST(CLowerPointers, MatchesCompiledExecutionOnPointerFunctions) {
    struct Case {
        const char *name;
        const char *source;
        const char *function;
        int arguments;
        int32_t (*reference)(int32_t *, int32_t);
    };
    const Case cases[] = {
        {"head", head_source, "ptr_head", 0,
         [](int32_t *p, int32_t) { return ptr_head(p); }},
        {"index", index_source, "ptr_index", 1,
         [](int32_t *p, int32_t i) { return ptr_index(p, i); }},
        {"offset", offset_source, "ptr_offset", 1,
         [](int32_t *p, int32_t i) { return ptr_offset(p, i); }},
        {"back", back_source, "ptr_back", 1,
         [](int32_t *p, int32_t i) { return ptr_back(p, i); }},
        {"write", write_source, "ptr_write", 1,
         [](int32_t *p, int32_t v) { return ptr_write(p, v); }},
        {"swap", swap_first_source, "ptr_swap", 0,
         [](int32_t *p, int32_t) { return ptr_swap(p); }},
        {"bump", bump_source, "ptr_bump", 1,
         [](int32_t *p, int32_t i) { return ptr_bump(p, i); }},
        {"step", step_source, "ptr_step", 1,
         [](int32_t *p, int32_t i) { return ptr_step(p, i); }},
    };

    uint64_t state = UINT64_C(0x39a1c4f70bd52e18);
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.name);
        ASSERT_TRUE(lowered.Open(item.source, item.function));
        for (std::size_t round = 0u; round < 256u; ++round) {
            int32_t reference_data[kElements];
            int32_t model_initial[kElements];
            int32_t model_final[kElements];
            const int32_t argument =
                item.arguments == 0
                    ? 0
                    : static_cast<int32_t>(NextRandom(&state) % 4u);
            for (std::size_t index = 0u; index < kElements; ++index) {
                reference_data[index] =
                    static_cast<int32_t>(NextRandom(&state) & 0xffffu);
                model_initial[index] = reference_data[index];
            }
            std::memset(model_final, 0, sizeof(model_final));

            const Outcome run =
                Execute(lowered.ir(), kBase,
                        {static_cast<uint64_t>(
                            static_cast<uint32_t>(argument))},
                        kBase, sizeof(model_initial),
                        reinterpret_cast<const uint8_t *>(model_initial),
                        reinterpret_cast<uint8_t *>(model_final));
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);

            const int32_t expected = item.reference(reference_data, argument);
            EXPECT_EQ(expected, Returned(run.result));
            /* Memory is observable, so the final image has to match too. */
            for (std::size_t index = 0u; index < kElements; ++index) {
                EXPECT_EQ(reference_data[index], model_final[index])
                    << "element " << index;
            }
        }
    }
}

TEST(CLowerPointers, CarriesTheElementWidthIntoSubscriptArithmetic) {
    Lowered lowered;
    const int16_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_TRUE(lowered.Open(narrow_load_source, "ptr_short"));
    for (int32_t i = 0; i < 8; ++i) {
        SCOPED_TRACE(i);
        const Outcome run =
            Execute(lowered.ir(), kBase, {static_cast<uint64_t>(i)}, kBase,
                    sizeof(data), reinterpret_cast<const uint8_t *>(data),
                    nullptr);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
        EXPECT_EQ(ptr_short(const_cast<int16_t *>(data), i),
                  Returned(run.result));
    }
}

/* The pointer surface this unit added: a pointer local, the difference of two
   pointers, a typedef that names a pointer, and taking the address of
   something that already has one. */
TEST(CLowerPointers, CarriesTheWiderPointerSurface) {
    struct Case {
        const char *name;
        const char *source;
        const char *function;
        int32_t (*reference)(int32_t *, int32_t);
    };
    const Case cases[] = {
        {"local", local_decl_source, "ptr_local",
         [](int32_t *p, int32_t i) { return ptr_local(p, i); }},
        {"distance", distance_source, "ptr_distance",
         [](int32_t *p, int32_t i) { return ptr_distance(p, i); }},
        {"typedef", named_pointer_source, "ptr_named",
         [](int32_t *p, int32_t i) { return ptr_named(p, i); }},
        {"address", address_of_element_source, "ptr_address",
         [](int32_t *p, int32_t i) { return ptr_address(p, i); }},
        {"sizeof", sizeof_designators_source, "ptr_sizeof",
         [](int32_t *p, int32_t i) { return ptr_sizeof(p, i); }},
        {"void-arithmetic", void_arithmetic_source, "ptr_void_distance",
         [](int32_t *p, int32_t i) {
             char *bytes = reinterpret_cast<char *>(p);
             return static_cast<int32_t>((bytes + i) - bytes);
         }},
    };
    uint64_t state = UINT64_C(0x4b7e2c9013fa65d8);
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.name);
        ASSERT_TRUE(lowered.Open(item.source, item.function));
        for (std::size_t round = 0u; round < 64u; ++round) {
            int32_t data[kElements];
            const int32_t index =
                static_cast<int32_t>(NextRandom(&state) % kElements);
            for (std::size_t at = 0u; at < kElements; ++at) {
                data[at] = static_cast<int32_t>(NextRandom(&state) & 0xffffu);
            }
            const Outcome run =
                Execute(lowered.ir(), kBase,
                        {static_cast<uint64_t>(
                            static_cast<uint32_t>(index))},
                        kBase, sizeof(data),
                        reinterpret_cast<const uint8_t *>(data), nullptr);
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);
            EXPECT_EQ(item.reference(data, index), Returned(run.result));
        }
    }
}

/* A pointer loaded through a pointer-to-pointer gets an auxiliary object in
   the flat table. The record differential test executes the corresponding
   linked-object case; this one retains the original status regression. */
TEST(CLowerPointers, LowersADoubleIndirectionWithoutAStatusFailure) {
    static const char source[] =
        "struct LINK_CELL { int value; };\n"
        "typedef struct LINK_CELL *CELL_PTR;\n"
        "int deref_twice(CELL_PTR *p) {\n"
        "    CELL_PTR q;\n"
        "    q = *p;\n"
        "    return q->value;\n"
        "}\n";
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(source, "deref_twice"));
}

/* A finite object table is still a resource boundary, but 32 entries rejected
   ordinary acyclic corpus bodies. Keep enough independent pointer-load sites
   here to catch both that old boundary and an accidental reduction to 64. */
TEST(CLowerPointers, CarriesMoreThanSixtyFourDynamicObjects) {
    std::string source = "int deref_many(int **p) { int sum = 0;\n";
    for (std::size_t index = 0u; index < 65u; ++index) {
        source += "sum += *p[" + std::to_string(index) + "];\n";
    }
    source += "return sum; }\n";

    Lowered lowered;
    ASSERT_TRUE(lowered.Open(source.c_str(), "deref_many"));
}

TEST(CLowerPointers, IntegerAddressBitsCanNameADynamicObject) {
    Lowered lowered;
    const int32_t data = 91;
    ASSERT_TRUE(lowered.Open(cast_from_bits_source, "ptr_from_bits"));
    const Outcome run =
        Execute(lowered.ir(), 0u, {kBase}, kBase, sizeof(data),
                reinterpret_cast<const uint8_t *>(&data), nullptr);
    ASSERT_EQ(QL_STATUS_OK, run.status);
    ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
        << ql_ir_interp_ub_reason_string(run.result.ub_reason);
    EXPECT_EQ(ptr_from_bits(static_cast<unsigned long long>(
                  reinterpret_cast<uintptr_t>(&data))),
              Returned(run.result));
}

/* A cast to a pointer is a reinterpretation, not a computation: under this
   profile a pointer is its address, so the bytes reached through the cast
   type are the bytes at the same address. Compiled execution is what says
   that, since the element width of the cast type is what moves a subscript.

   The object the address came from travels with it, which is why the
   dereference after the cast is still allowed: nothing about the cast makes
   the storage less known than it was. */
TEST(CLowerPointers, PointerCastsReinterpretWithoutMovingTheAddress) {
    struct Case {
        const char *name;
        const char *source;
        const char *function;
        int32_t (*reference)(int32_t *, int32_t);
    };
    const Case cases[] = {
        {"through-void", cast_through_void_source, "ptr_cast",
         [](int32_t *p, int32_t i) { return ptr_cast(p, i); }},
        /* char elements: the cast type is what makes the subscript step one
           byte instead of four. */
        {"to-bytes", cast_to_bytes_source, "ptr_bytes",
         [](int32_t *p, int32_t i) { return ptr_bytes(p, i); }},
    };
    uint64_t state = UINT64_C(0x2f9d4c1b7a63e850);
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.name);
        ASSERT_TRUE(lowered.Open(item.source, item.function));
        for (std::size_t round = 0u; round < 64u; ++round) {
            int32_t data[kElements];
            const int32_t index =
                static_cast<int32_t>(NextRandom(&state) % kElements);
            for (std::size_t at = 0u; at < kElements; ++at) {
                data[at] = static_cast<int32_t>(NextRandom(&state) & 0x3fu);
            }
            const Outcome run =
                Execute(lowered.ir(), kBase,
                        {static_cast<uint64_t>(
                            static_cast<uint32_t>(index))},
                        kBase, sizeof(data),
                        reinterpret_cast<const uint8_t *>(data), nullptr);
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);
            EXPECT_EQ(item.reference(data, index), Returned(run.result));
        }
    }
}

}  // namespace
