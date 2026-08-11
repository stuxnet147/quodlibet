/* Bounded unrolling turns a cyclic lowered function into an acyclic one that
   agrees with the cyclic interpreter on every input inside the bound and
   rejects, through ASSUME, every input beyond it. The reader re-validates the
   rebuilt graph, so these tests exercise the same acceptance gate the proof
   method relies on. */

#include "unroll.h"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "quodlibet/c_frontend.h"
#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"

namespace {

class LoweredFunction {
public:
    LoweredFunction() = default;
    LoweredFunction(const LoweredFunction &) = delete;
    LoweredFunction &operator=(const LoweredFunction &) = delete;

    ~LoweredFunction() {
        ql_ir_release(ir_);
        ql_c_lower_result_destroy(result_);
        ql_c_frontend_unit_destroy(unit_);
    }

    void Build(const char *source, const char *name) {
        ql_error error{};
        ql_c_function_view function{};
        ql_c_lower_result_view_v1 view{};

        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_frontend_analyze(nullptr, source, std::strlen(source),
                                        &unit_, &error))
            << error.message;
        function.struct_size = sizeof(function);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_frontend_select_function(unit_, name, std::strlen(name),
                                                &function, &error))
            << error.message;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_lower_selected_function(nullptr, source,
                                               std::strlen(source), unit_,
                                               &function, &result_, &error))
            << error.message;
        view.struct_size = sizeof(view);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_c_lower_result_get_view(result_, &view, &error))
            << error.message;
        ASSERT_EQ(QL_C_LOWER_SUPPORTED, view.support);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_open(nullptr, view.ir_artifact, &ir_, &error))
            << error.message;
    }

    const ql_ir *ir() const { return ir_; }

private:
    ql_c_frontend_unit *unit_ = nullptr;
    ql_c_lower_result *result_ = nullptr;
    ql_ir *ir_ = nullptr;
};

class UnrolledFunction {
public:
    UnrolledFunction() = default;
    UnrolledFunction(const UnrolledFunction &) = delete;
    UnrolledFunction &operator=(const UnrolledFunction &) = delete;

    ~UnrolledFunction() {
        ql_ir_release(ir_);
        ql_artifact_release(artifact_);
    }

    void Build(const ql_ir *original, uint32_t bound) {
        ql_error error{};
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_unroll_bounded(nullptr, original, bound, &stats_,
                                       &artifact_, &error))
            << error.message;
        ASSERT_EQ(QL_STATUS_OK, ql_ir_open(nullptr, artifact_, &ir_, &error))
            << error.message;
    }

    const ql_ir *ir() const { return ir_; }
    const ql_unroll_stats_v1 &stats() const { return stats_; }

private:
    ql_artifact *artifact_ = nullptr;
    ql_ir *ir_ = nullptr;
    ql_unroll_stats_v1 stats_{};
};

std::vector<ql_ir_value_id> ParameterIds(const ql_ir *ir) {
    std::vector<ql_ir_value_id> parameters;
    ql_ir_view_v1 view{};
    ql_error error{};

    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK, ql_ir_get_view(ir, &view, &error));
    for (size_t index = 0; index < view.value_count; ++index) {
        ql_ir_value_view_v1 value{};
        value.struct_size = sizeof(value);
        EXPECT_EQ(QL_STATUS_OK, ql_ir_value_at(ir, index, &value, &error));
        if (value.definition_kind == QL_IR_VALUE_PARAMETER) {
            parameters.push_back(value.id);
        }
    }
    return parameters;
}

ql_ir_interp_result_v1 RunOnInt32(const ql_ir *ir, int32_t argument) {
    ql_ir_interp_result_v1 result{};
    ql_ir_interp_options_v1 options{};
    ql_ir_interp_input_v1 input{};
    ql_error error{};
    uint8_t bytes[4];

    std::memcpy(bytes, &argument, sizeof(bytes));
    const std::vector<ql_ir_value_id> parameters = ParameterIds(ir);
    EXPECT_EQ(1u, parameters.size());
    ql_ir_interp_input_init(&input);
    input.value = parameters[0];
    input.data = bytes;
    input.size = sizeof(bytes);
    ql_ir_interp_options_init(&options);
    result.struct_size = sizeof(result);
    EXPECT_EQ(QL_STATUS_OK, ql_ir_interp_run(nullptr, ir, &input, 1u, &options,
                                             &result, &error))
        << error.message;
    return result;
}

constexpr char kSumLoop[] =
    "int sum(int n) {"
    "  int total = 0;"
    "  for (int i = 0; i < n; i++) { total += i; }"
    "  return total;"
    "}";

constexpr char kNestedLoop[] =
    "int grid(int n) {"
    "  int total = 0;"
    "  for (int i = 0; i < n; i++) {"
    "    for (int j = 0; j < i; j++) { total += j + 1; }"
    "  }"
    "  return total;"
    "}";

constexpr char kLoopFree[] =
    "int pick(int x) { if (x > 3) return x + 1; return x - 1; }";

TEST(Unroll, ASumLoopUnrollsToAnAcyclicGraphTheReaderAccepts) {
    LoweredFunction lowered;
    UnrolledFunction unrolled;
    ql_ir_view_v1 original_view{};
    ql_ir_view_v1 unrolled_view{};
    ql_error error{};

    lowered.Build(kSumLoop, "sum");
    original_view.struct_size = sizeof(original_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_get_view(lowered.ir(), &original_view, &error));
    ASSERT_EQ(QL_IR_CFG_CYCLIC, original_view.cfg_kind);

    unrolled.Build(lowered.ir(), 8u);
    unrolled_view.struct_size = sizeof(unrolled_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_get_view(unrolled.ir(), &unrolled_view, &error));
    EXPECT_EQ(QL_IR_CFG_ACYCLIC, unrolled_view.cfg_kind);
    EXPECT_EQ(original_view.return_type, unrolled_view.return_type);
    EXPECT_EQ(std::string(original_view.function_name,
                          original_view.function_name_size),
              std::string(unrolled_view.function_name,
                          unrolled_view.function_name_size));
    EXPECT_EQ(1u, unrolled.stats().bound_cut_used);
    EXPECT_GE(unrolled.stats().retreating_edges, 1u);
}

TEST(Unroll, AgreesWithTheCyclicInterpreterInsideTheBound) {
    LoweredFunction lowered;
    UnrolledFunction unrolled;

    lowered.Build(kSumLoop, "sum");
    unrolled.Build(lowered.ir(), 8u);
    for (int32_t n = -2; n <= 6; ++n) {
        const ql_ir_interp_result_v1 original = RunOnInt32(lowered.ir(), n);
        const ql_ir_interp_result_v1 rebuilt = RunOnInt32(unrolled.ir(), n);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, original.outcome) << n;
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, rebuilt.outcome) << n;
        ASSERT_EQ(original.value_size, rebuilt.value_size) << n;
        EXPECT_EQ(0, std::memcmp(original.value, rebuilt.value,
                                 original.value_size))
            << n;
    }
}

TEST(Unroll, RejectsAnInputBeyondTheBoundThroughTheAssume) {
    LoweredFunction lowered;
    UnrolledFunction unrolled;

    lowered.Build(kSumLoop, "sum");
    unrolled.Build(lowered.ir(), 4u);
    const ql_ir_interp_result_v1 original = RunOnInt32(lowered.ir(), 20);
    const ql_ir_interp_result_v1 rebuilt = RunOnInt32(unrolled.ir(), 20);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_RETURN, original.outcome);
    /* The cut is an assumption rejection: the input leaves the modeled
       domain, and nothing is claimed about it. */
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_ASSUMPTION_VIOLATED, rebuilt.outcome);
}

TEST(Unroll, CarriesANestedLoopFaithfully) {
    LoweredFunction lowered;
    UnrolledFunction unrolled;

    lowered.Build(kNestedLoop, "grid");
    /* Every retreating traversal spends one copy, inner and outer alike, so
       n = 3 costs the outer loop 3 and the inner bodies 0 + 1 + 2. */
    unrolled.Build(lowered.ir(), 12u);
    for (int32_t n = 0; n <= 3; ++n) {
        const ql_ir_interp_result_v1 original = RunOnInt32(lowered.ir(), n);
        const ql_ir_interp_result_v1 rebuilt = RunOnInt32(unrolled.ir(), n);
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, original.outcome) << n;
        ASSERT_EQ(QL_IR_INTERP_OUTCOME_RETURN, rebuilt.outcome) << n;
        ASSERT_EQ(original.value_size, rebuilt.value_size) << n;
        EXPECT_EQ(0, std::memcmp(original.value, rebuilt.value,
                                 original.value_size))
            << n;
    }
}

TEST(Unroll, RebuildsALoopFreeFunctionWithoutACut) {
    LoweredFunction lowered;
    UnrolledFunction unrolled;

    lowered.Build(kLoopFree, "pick");
    unrolled.Build(lowered.ir(), 4u);
    EXPECT_EQ(0u, unrolled.stats().bound_cut_used);
    EXPECT_EQ(0u, unrolled.stats().retreating_edges);
    for (int32_t x = 0; x <= 6; ++x) {
        const ql_ir_interp_result_v1 original = RunOnInt32(lowered.ir(), x);
        const ql_ir_interp_result_v1 rebuilt = RunOnInt32(unrolled.ir(), x);
        ASSERT_EQ(original.outcome, rebuilt.outcome) << x;
        ASSERT_EQ(original.value_size, rebuilt.value_size) << x;
        EXPECT_EQ(0, std::memcmp(original.value, rebuilt.value,
                                 original.value_size))
            << x;
    }
}

TEST(Unroll, RefusesAZeroBound) {
    LoweredFunction lowered;
    ql_artifact *artifact = nullptr;
    ql_error error{};

    lowered.Build(kSumLoop, "sum");
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_ir_unroll_bounded(nullptr, lowered.ir(), 0u, nullptr,
                                   &artifact, &error));
    EXPECT_EQ(nullptr, artifact);
}

}  // namespace
