/* Struct and union lowering, checked against real compiled execution.

   Layout is the whole risk here: an offset this lowering computes differently
   from the target ABI would read the wrong bytes and still look plausible.
   So the reference functions below are compiled by the same compiler that
   builds this binary and run on real structs, and the model runs on a byte
   image of the same struct. If a single offset disagreed, the values would.
*/

#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#define QL_REC_FUNCTION(name, ...)                                           \
    extern "C" {                                                             \
    __VA_ARGS__                                                              \
    }                                                                        \
    static const char name##_source[] = #__VA_ARGS__

/* One text produces the struct the reference uses and the struct the lowering
   reads, so a layout difference cannot hide behind two declarations. */
QL_REC_FUNCTION(mixed,
    struct REC_A { char tag; int value; long long wide; short small; };
    unsigned int rec_sum(struct REC_A *p) {
        return (unsigned int) p->tag + (unsigned int) p->value +
               (unsigned int) p->wide + (unsigned int) p->small;
    });
QL_REC_FUNCTION(nested,
    struct REC_INNER { int a; int b; };
    struct REC_OUTER { char pad; struct REC_INNER inner; int tail; };
    unsigned int rec_nested(struct REC_OUTER *p) {
        return (unsigned int) p->inner.a * 100u +
               (unsigned int) p->inner.b * 10u + (unsigned int) p->tail;
    });
QL_REC_FUNCTION(through,
    struct REC_LINK { int head; struct REC_LINK *next; };
    int rec_through(struct REC_LINK *p) { return p->next->head; });
QL_REC_FUNCTION(writeback,
    struct REC_PAIR { int first; int second; };
    int rec_write(struct REC_PAIR *p, int v) {
        p->second = v;
        return p->first + p->second;
    });
QL_REC_FUNCTION(deref_dot,
    struct REC_ONE { int only; };
    int rec_dot(struct REC_ONE *p) { return (*p).only; });
QL_REC_FUNCTION(overlap,
    union REC_U { int as_int; unsigned int as_unsigned; };
    int rec_union(union REC_U *p) { return p->as_unsigned > 0; });
QL_REC_FUNCTION(labels,
    enum REC_E { REC_ZERO, REC_ONE, REC_TEN = 10, REC_ELEVEN };
    int rec_enum(int a) { return a + REC_TEN + REC_ELEVEN + REC_ONE; });
QL_REC_FUNCTION(sizeof_members,
    struct REC_SIZE { char tag; int values[3]; };
    int rec_size(struct REC_SIZE *p) {
        return (int)(sizeof(p->tag) + sizeof(p->values) +
                     sizeof((*p).values[0]));
    });

namespace {

const uint64_t kBase = QL_IR_INTERP_FIRST_OBJECT_ADDRESS;

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

/* An object per pointer parameter, laid out end to end above the first page
   so the model's disjointness assumption holds. */
struct Region {
    uint64_t base;
    uint64_t size;
    const uint8_t *initial;
    uint8_t *final_image;
};

struct Outcome {
    ql_ir_interp_result_v1 result{};
    ql_status status = QL_STATUS_INTERNAL_ERROR;
};

Outcome Execute(ql_ir *ir, const std::vector<uint64_t> &pointers,
                const std::vector<uint64_t> &scalars,
                const std::vector<Region> &regions) {
    ql_ir_view_v1 view{};
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ql_ir_interp_input_v1> inputs;
    std::vector<ql_ir_interp_object_v1> objects;
    ql_ir_interp_options_v1 options{};
    ql_error error{};
    Outcome run;
    std::size_t next_pointer = 0u;
    std::size_t next_scalar = 0u;
    std::size_t next_base = 0u;
    std::size_t next_size = 0u;

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
            storage.push_back(std::vector<uint8_t>());
        } else if (type.kind == QL_IR_TYPE_POINTER) {
            EXPECT_LT(next_pointer, pointers.size());
            storage.push_back(Encode(pointers[next_pointer++],
                                     type.bit_width));
        } else if (std::strstr(name, ".__base") != nullptr) {
            EXPECT_LT(next_base, regions.size());
            storage.push_back(Encode(regions[next_base++].base,
                                     type.bit_width));
        } else if (std::strstr(name, ".__size") != nullptr) {
            EXPECT_LT(next_size, regions.size());
            storage.push_back(Encode(regions[next_size++].size,
                                     type.bit_width));
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
    for (const Region &region : regions) {
        ql_ir_interp_object_v1 object{};
        ql_ir_interp_object_init(&object);
        object.base = region.base;
        object.size = region.size;
        object.initial = region.initial;
        object.final_image = region.final_image;
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

template <typename Record, typename Reference>
void CompareOverRandomRecords(const char *source, const char *function,
                              Reference reference, uint64_t seed) {
    Lowered lowered;
    uint64_t state = seed;
    ASSERT_TRUE(lowered.Open(source, function));
    for (std::size_t round = 0u; round < 128u; ++round) {
        Record record;
        Record image;
        uint8_t *bytes = reinterpret_cast<uint8_t *>(&record);
        for (std::size_t index = 0u; index < sizeof(Record); ++index) {
            bytes[index] = static_cast<uint8_t>(NextRandom(&state));
        }
        std::memcpy(&image, &record, sizeof(Record));

        const Region region = {kBase, sizeof(Record),
                               reinterpret_cast<const uint8_t *>(&image),
                               nullptr};
        const Outcome run = Execute(lowered.ir(), {kBase}, {}, {region});
        ASSERT_EQ(QL_STATUS_OK, run.status);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        EXPECT_EQ(static_cast<int32_t>(reference(&record)),
                  Returned(run.result));
    }
}

TEST(CLowerRecords, MatchesTheTargetLayoutForAMixedStruct) {
    /* char, int, long long, short: every padding decision the ABI makes shows
       up in the answer. */
    CompareOverRandomRecords<struct REC_A>(
        mixed_source, "rec_sum",
        [](struct REC_A *p) { return rec_sum(p); },
        UINT64_C(0x51c4a7b30e2d6f89));
}

TEST(CLowerRecords, ReachesThroughNestedRecords) {
    CompareOverRandomRecords<struct REC_OUTER>(
        nested_source, "rec_nested",
        [](struct REC_OUTER *p) { return rec_nested(p); },
        UINT64_C(0x2f8b17d940a6c3e5));
}

TEST(CLowerRecords, ReadsAMemberThroughADereferenceAndDot) {
    CompareOverRandomRecords<struct REC_ONE>(
        deref_dot_source, "rec_dot",
        [](struct REC_ONE *p) { return rec_dot(p); },
        UINT64_C(0x7d3e50a1c9f28b46));
}

TEST(CLowerRecords, ReadsUnionMembersOverTheSameBytes) {
    CompareOverRandomRecords<union REC_U>(
        overlap_source, "rec_union",
        [](union REC_U *p) { return rec_union(p); },
        UINT64_C(0x1a6f9c25d374e8b0));
}

TEST(CLowerRecords, QueriesMemberTypesWithoutReadingTheObject) {
    CompareOverRandomRecords<struct REC_SIZE>(
        sizeof_members_source, "rec_size",
        [](struct REC_SIZE *p) { return rec_size(p); },
        UINT64_C(0x8b4f21d6a3509ce7));
}

TEST(CLowerRecords, StoresIntoAMemberAndLeavesItVisible) {
    Lowered lowered;
    uint64_t state = UINT64_C(0x6c2d94f1a70b35e8);
    ASSERT_TRUE(lowered.Open(writeback_source, "rec_write"));
    for (std::size_t round = 0u; round < 128u; ++round) {
        struct REC_PAIR record;
        struct REC_PAIR image;
        struct REC_PAIR final_image;
        const int32_t value =
            static_cast<int32_t>(NextRandom(&state) & 0xffffu);
        record.first = static_cast<int>(NextRandom(&state) & 0xffffu);
        record.second = static_cast<int>(NextRandom(&state) & 0xffffu);
        std::memcpy(&image, &record, sizeof(record));
        std::memset(&final_image, 0, sizeof(final_image));

        const Region region = {kBase, sizeof(record),
                               reinterpret_cast<const uint8_t *>(&image),
                               reinterpret_cast<uint8_t *>(&final_image)};
        const Outcome run = Execute(lowered.ir(), {kBase},
                                    {static_cast<uint64_t>(
                                        static_cast<uint32_t>(value))},
                                    {region});
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome)
            << ql_ir_interp_ub_reason_string(run.result.ub_reason);
        const int32_t expected = rec_write(&record, value);
        EXPECT_EQ(expected, Returned(run.result));
        /* Memory is observable, so the struct the model leaves behind has to
           match the one the reference left behind. */
        EXPECT_EQ(record.first, final_image.first);
        EXPECT_EQ(record.second, final_image.second);
    }
}

/* A pointer read out of memory has no object in the table, so no guard could
   ever justify dereferencing it. The lowering says so up front instead of
   emitting IR that is undefined on every input, which would look lowered and
   prove nothing. */
TEST(CLowerRecords, RefusesToFollowAPointerReadOutOfMemory) {
    ql_c_frontend_unit *unit = nullptr;
    ql_c_lower_result *result = nullptr;
    ql_c_function_view function{};
    ql_c_lower_result_view_v1 view{};
    ql_c_lower_diagnostic_view_v1 diagnostic{};
    ql_error error{};
    const std::size_t size = std::strlen(through_source);

    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_analyze(nullptr, through_source, size, &unit,
                                    &error));
    function.struct_size = sizeof(function);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_frontend_select_function(unit, "rec_through", 11u,
                                            &function, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_lower_selected_function(nullptr, through_source, size,
                                           unit, &function, &result, &error))
        << error.message;
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_lower_result_get_view(result, &view, &error));
    EXPECT_EQ(QL_C_LOWER_UNKNOWN, view.support);
    diagnostic.struct_size = sizeof(diagnostic);
    ASSERT_EQ(QL_STATUS_OK,
              ql_c_lower_result_diagnostic_at(result, 0u, &diagnostic,
                                              &error));
    EXPECT_EQ(QL_C_LOWER_DIAGNOSTIC_UNSUPPORTED_POINTER, diagnostic.code);
    ql_c_lower_result_destroy(result);
    ql_c_frontend_unit_destroy(unit);
}

TEST(CLowerRecords, LowersEnumeratorsAsTheConstantsTheyName) {
    Lowered lowered;
    ASSERT_TRUE(lowered.Open(labels_source, "rec_enum"));
    for (int32_t a : {0, 1, -5, 1000}) {
        SCOPED_TRACE(a);
        const Outcome run =
            Execute(lowered.ir(), {},
                    {static_cast<uint64_t>(static_cast<uint32_t>(a))}, {});
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, run.result.outcome);
        EXPECT_EQ(rec_enum(a), Returned(run.result));
    }
}

TEST(CLowerRecords, RefusesWholeRecordValues) {
    struct Case {
        const char *source;
        const char *name;
    };
    const Case cases[] = {
        {"struct S { int a; };\nint by_value(struct S s) { return s.a; }",
         "by_value"},
        /* A record local is storage now, but the record still has no value:
           copying one whole is a memory operation this slice does not do. */
        {"struct S { int a; };\nint whole(int a) "
         "{ struct S s; struct S t; t = s; return t.a + a; }",
         "whole"},
        /* A record that contains itself has no layout, and Tree-sitter
           accepts the declaration, so this has to be refused. */
        {"struct S { struct S inner; };\nint cyclic(struct S *p) "
         "{ return 0; }",
         "cyclic"},
    };
    for (const Case &item : cases) {
        ql_c_frontend_unit *unit = nullptr;
        ql_c_lower_result *result = nullptr;
        ql_c_function_view function{};
        ql_c_lower_result_view_v1 view{};
        ql_error error{};
        const std::size_t size = std::strlen(item.source);
        SCOPED_TRACE(item.name);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_frontend_analyze(nullptr, item.source, size, &unit,
                                        &error));
        function.struct_size = sizeof(function);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_frontend_select_function(unit, item.name,
                                                std::strlen(item.name),
                                                &function, &error));
        /* A semantic limit is an UNKNOWN result, never a status failure. */
        EXPECT_EQ(QL_STATUS_OK,
                  ql_c_lower_selected_function(nullptr, item.source, size,
                                               unit, &function, &result,
                                               &error))
            << error.message;
        view.struct_size = sizeof(view);
        if (result != nullptr) {
            EXPECT_EQ(QL_STATUS_OK,
                      ql_c_lower_result_get_view(result, &view, &error));
            EXPECT_EQ(QL_C_LOWER_UNKNOWN, view.support);
        }
        ql_c_lower_result_destroy(result);
        ql_c_frontend_unit_destroy(unit);
    }
}

}  // namespace
