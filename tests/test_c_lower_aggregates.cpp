/* Arrays and record locals, against real compiled execution.

   Both are storage rather than values. An array decays to a pointer to its
   first element wherever it is named, a record is reached only through its
   members, and neither is ever loaded or stored whole. So both become
   objects in the flat memory model, on the same footing as a caller's
   pointer region: the size is pinned, every access carries a guard, and the
   final image is what a contract observes.

   Two things are compared here. The returned value is compared against the
   compiled function, which is the only thing that can say whether the member
   offsets and the element widths are right; a layout that is merely
   self-consistent would satisfy a test written against the lowering's own
   arithmetic and still disagree with a real compiler. And the object size
   each test states is the compiler's own `sizeof`, which the module's pinning
   assumption has to agree with or the run reports the assumption violated. */

#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#define QL_AGG_FUNCTION(name, ...)                                           \
    extern "C" {                                                             \
    __VA_ARGS__                                                              \
    }                                                                        \
    static const char name##_source[] = #__VA_ARGS__

QL_AGG_FUNCTION(write, int agg_write(int a, int b) {
    int buf[4];
    buf[0] = a;
    buf[1] = b;
    buf[2] = buf[0] + buf[1];
    buf[3] = buf[2] - a;
    return buf[3];
});
QL_AGG_FUNCTION(bytes, int agg_bytes(int a) {
    char small[4];
    small[0] = (char)a;
    small[1] = (char)(a + 1);
    small[2] = (char)(a + 2);
    small[3] = (char)(a + 3);
    return small[0] + small[3];
});
QL_AGG_FUNCTION(record, struct AGG_PAIR { int first; int second; };
    int agg_record(int a, int b) {
        struct AGG_PAIR p;
        p.first = a;
        p.second = b;
        return p.first * 2 + p.second;
    });
QL_AGG_FUNCTION(nested, struct AGG_INNER { int x; int y; };
    struct AGG_OUTER { char tag; struct AGG_INNER inner; };
    int agg_nested(int a, int b) {
        struct AGG_OUTER o;
        o.tag = (char)a;
        o.inner.x = a;
        o.inner.y = b;
        return o.inner.x + o.inner.y + o.tag;
    });
QL_AGG_FUNCTION(member, struct AGG_HOLDER { int head; int slots[4]; };
    int agg_member(int a) {
        struct AGG_HOLDER h;
        h.head = a;
        h.slots[0] = a + 1;
        h.slots[3] = a + 2;
        return h.head + h.slots[0] + h.slots[3];
    });
QL_AGG_FUNCTION(address, int agg_address(int a) {
    int buf[3];
    int *p = buf;
    p[0] = a;
    p[1] = a + 1;
    buf[2] = p[0] + p[1];
    return buf[2];
});
QL_AGG_FUNCTION(param, int agg_param(int a[], int n) {
    return a[0] + a[n];
});

namespace {

constexpr uint64_t kBase = UINT64_C(0x30000);
constexpr uint64_t kStride = UINT64_C(0x1000);

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
                ADD_FAILURE() << name << ": " << diagnostic.message;
            } else {
                ADD_FAILURE() << name << ": lowering did not accept it";
            }
            return false;
        }
        if (ql_ir_open(nullptr, view.ir_artifact, &ir_, &error) !=
            QL_STATUS_OK) {
            ADD_FAILURE() << "open: " << error.message;
            return false;
        }
        /* Every lowering these tests produce goes through the verifier, so a
           guard the aggregate paths forgot to place is caught here and not
           only where it happens to matter. */
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
    std::size_t object_count = 0u;
};

/* `sizes` gives one size per object, in the order the module declares them:
   pointer parameters, then locals that live in storage, then globals. For an
   object the lowering pinned, the size stated here has to be the size the
   module pinned or the ASSUME rejects the run, which is what makes stating
   the compiler's `sizeof` a check rather than a restatement. */
Outcome Execute(ql_ir *ir, const std::vector<uint64_t> &scalars,
                const std::vector<uint64_t> &sizes,
                const void *initial_image) {
    ql_ir_view_v1 view{};
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ql_ir_interp_input_v1> inputs;
    std::vector<ql_ir_interp_object_v1> objects;
    std::vector<std::vector<uint8_t>> images;
    ql_ir_interp_options_v1 options{};
    ql_error error{};
    Outcome run;
    std::size_t next_scalar = 0u;
    std::size_t next_base = 0u;
    std::size_t next_size = 0u;
    std::size_t next_pointer = 0u;

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
        const std::string name = value.name != nullptr ? value.name : "";
        inputs.push_back(ql_ir_interp_input_v1{});
        ql_ir_interp_input_init(&inputs.back());
        inputs.back().value = value.id;
        if (type.kind == QL_IR_TYPE_MEMORY ||
            type.kind == QL_IR_TYPE_EVENT_TRACE) {
            storage.push_back(std::vector<uint8_t>());
        } else if (name.find(".__base") != std::string::npos) {
            storage.push_back(
                Encode(kBase + kStride * (next_base + 1u), type.bit_width));
            ++next_base;
        } else if (name.find(".__size") != std::string::npos) {
            EXPECT_LT(next_size, sizes.size()) << "unexpected object " << name;
            storage.push_back(
                Encode(next_size < sizes.size() ? sizes[next_size] : 0u,
                       type.bit_width));
            ++next_size;
        } else if (type.kind == QL_IR_TYPE_POINTER) {
            /* A pointer parameter arrives holding the base of the object it
               was given, which is the first object the module declares. */
            storage.push_back(Encode(kBase + kStride * (next_pointer + 1u),
                                     type.bit_width));
            ++next_pointer;
        } else {
            EXPECT_LT(next_scalar, scalars.size());
            storage.push_back(Encode(scalars[next_scalar++],
                                     type.kind == QL_IR_TYPE_BOOL
                                         ? 1u
                                         : type.bit_width));
        }
    }
    EXPECT_EQ(sizes.size(), next_size);
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        inputs[index].data = storage[index].empty() ? nullptr
                                                    : storage[index].data();
        inputs[index].size = storage[index].size();
    }
    images.resize(sizes.size());
    for (std::size_t index = 0u; index < sizes.size(); ++index) {
        ql_ir_interp_object_v1 object{};
        ql_ir_interp_object_init(&object);
        object.base = kBase + kStride * (index + 1u);
        object.size = sizes[index];
        images[index].assign(static_cast<std::size_t>(sizes[index]), 0u);
        if (index == 0u && initial_image != nullptr) {
            object.initial = initial_image;
        }
        object.final_image = images[index].empty() ? nullptr
                                                   : images[index].data();
        objects.push_back(object);
    }
    ql_ir_interp_options_init(&options);
    options.objects = objects.empty() ? nullptr : objects.data();
    options.object_count = objects.size();
    run.object_count = objects.size();
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

TEST(CLowerAggregates, MatchesCompiledExecutionOnArraysAndRecords) {
    struct Case {
        const char *label;
        const char *source;
        const char *function;
        int arguments;
        uint64_t object_size;
        int32_t (*reference)(int32_t, int32_t);
    };
    const Case cases[] = {
        {"write", write_source, "agg_write", 2, sizeof(int[4]),
         [](int32_t a, int32_t b) { return agg_write(a, b); }},
        /* Byte elements: the element width has to reach the address
           arithmetic, or every index but zero lands somewhere else. */
        {"bytes", bytes_source, "agg_bytes", 1, sizeof(char[4]),
         [](int32_t a, int32_t) { return agg_bytes(a); }},
        {"record", record_source, "agg_record", 2, sizeof(struct AGG_PAIR),
         [](int32_t a, int32_t b) { return agg_record(a, b); }},
        /* A record inside a record, where the inner one's alignment decides
           the outer offsets and the trailing padding. */
        {"nested", nested_source, "agg_nested", 2, sizeof(struct AGG_OUTER),
         [](int32_t a, int32_t b) { return agg_nested(a, b); }},
        /* An array member inside a record. */
        {"member", member_source, "agg_member", 1, sizeof(struct AGG_HOLDER),
         [](int32_t a, int32_t) { return agg_member(a); }},
        /* The array's name is a pointer to its first element, and writing
           through that pointer is writing the array. */
        {"address", address_source, "agg_address", 1, sizeof(int[3]),
         [](int32_t a, int32_t) { return agg_address(a); }},
    };

    uint64_t state = UINT64_C(0x71b3e0c95d24af86);
    for (const Case &item : cases) {
        Lowered lowered;
        SCOPED_TRACE(item.label);
        ASSERT_TRUE(lowered.Open(item.source, item.function));
        for (std::size_t round = 0u; round < 64u; ++round) {
            /* Small values keep every case inside the defined range, so the
               comparison measures layout rather than signed overflow. */
            const int32_t a =
                static_cast<int32_t>(NextRandom(&state) % 100u) - 50;
            const int32_t b =
                static_cast<int32_t>(NextRandom(&state) % 100u) - 50;
            std::vector<uint64_t> scalars;
            scalars.push_back(Widen(a));
            if (item.arguments > 1) {
                scalars.push_back(Widen(b));
            }
            const Outcome run =
                Execute(lowered.ir(), scalars, {item.object_size}, nullptr);
            ASSERT_EQ(QL_STATUS_OK, run.status);
            ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
                << ql_ir_interp_ub_reason_string(run.result.ub_reason);
            EXPECT_EQ(item.reference(a, b), Returned(run.result));
        }
    }
}

TEST(CLowerAggregates, AnArrayParameterIsAPointerIntoTheCallersObject) {
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(param_source, "agg_param"));
    for (int32_t n : {0, 1, 3}) {
        int32_t data[4] = {11, -7, 40, 3};
        SCOPED_TRACE(n);
        const Outcome run =
            Execute(lowered.ir(), {Widen(n)}, {sizeof(data)},
                    reinterpret_cast<const void *>(data));
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(agg_param(data, n), Returned(run.result));
    }
}

TEST(CLowerAggregates, AnIndexPastTheArrayIsUndefined) {
    /* The bound the lowering pinned is what makes this undefined rather than
       a read of whatever sits after the object. If the size were wrong, this
       would return a value instead. */
    static const char source[] =
        "int over(int i) { int buf[4]; buf[0] = 1; return buf[i]; }\n";
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(source, "over"));
    const Outcome inside = Execute(lowered.ir(), {Widen(0)}, {sizeof(int[4])},
                                   nullptr);
    ASSERT_EQ(QL_STATUS_OK, inside.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_RETURN, inside.result.outcome)
        << ql_ir_interp_ub_reason_string(inside.result.ub_reason);

    const Outcome outside = Execute(lowered.ir(), {Widen(4)}, {sizeof(int[4])},
                                    nullptr);
    ASSERT_EQ(QL_STATUS_OK, outside.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR, outside.result.outcome);
    EXPECT_EQ(QL_IR_INTERP_UB_GUARD_FAILED, outside.result.ub_reason);
}

TEST(CLowerAggregates, AStringLiteralIsAnObjectHoldingItsOwnBytes) {
    /* `const char S[] = "hi";` is an array whose bound the initialiser
       states and whose bytes the program states, so the lowering pins the
       size at three and states the bytes as one memory image the entry block
       assumes.

       The image is an assumption, so the run has to supply those bytes. That
       makes the test's own spelling of the literal a check on the lowering's:
       supply different bytes and the assumption rejects the run rather than
       quietly returning something else. */
    static const char source[] =
        "const char AGG_TEXT[] = \"hi\";\n"
        "int pick(int i) { return AGG_TEXT[i]; }\n";
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(source, "pick"));
    const char expected[] = {'h', 'i', '\0'};
    for (int32_t i = 0; i < 3; ++i) {
        SCOPED_TRACE(i);
        const Outcome run = Execute(lowered.ir(), {Widen(i)}, {3u}, expected);
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(static_cast<int32_t>(expected[i]), Returned(run.result));
    }

    /* Bytes that are not the ones the program states are refused, not
       silently read. */
    const char wrong[] = {'h', 'o', '\0'};
    const Outcome bad = Execute(lowered.ir(), {Widen(0)}, {3u}, wrong);
    ASSERT_EQ(QL_STATUS_OK, bad.status);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_ASSUMPTION_VIOLATED, bad.result.outcome);
}

}  // namespace
