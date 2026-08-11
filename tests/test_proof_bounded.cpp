/* search.bounded-symbolic: bounded symbolic search over unrolled loops.

   The discipline under test is the one METHODS.md assigns to a bounded
   search: a SAT model becomes COUNTEREXAMPLE only after a concrete replay
   against the ORIGINAL cyclic functions reproduces the violation, an UNSAT
   is BOUNDED_CLEAN with its bound recorded and is never a proof, and the
   same pair flips from BOUNDED_CLEAN to COUNTEREXAMPLE when the bound grows
   past the iteration where the two functions part ways. */

#include "quodlibet/proof_bounded.h"

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "quodlibet/proof_smt.h"
#include "w2_fixtures.h"

namespace {

class RegistryHandle {
public:
    RegistryHandle() = default;
    RegistryHandle(const RegistryHandle &) = delete;
    RegistryHandle &operator=(const RegistryHandle &) = delete;
    ~RegistryHandle() { ql_registry_destroy(registry_); }

    ql_registry **output() { return &registry_; }
    ql_registry *get() const { return registry_; }

private:
    ql_registry *registry_ = nullptr;
};

class BoundedRun {
public:
    BoundedRun() = default;
    BoundedRun(const BoundedRun &) = delete;
    BoundedRun &operator=(const BoundedRun &) = delete;

    ~BoundedRun() {
        ql_artifact_release(outcome_);
        if (instance_ != nullptr) {
            ql_bounded_method()->destroy(instance_);
        }
    }

    ql_status Run(const w2::Pair &pair, const char *options_json,
                  ql_error *error) {
        ql_run_context_v1 context{};
        ql_artifact *input = pair.artifact();
        ql_status status = ql_bounded_method()->create(
            ql_default_host(), options_json, &instance_, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_bounded_method()->validate(instance_, &input, 1u, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        context.struct_size = sizeof(context);
        context.abi_version = QL_ABI_VERSION;
        context.host = ql_default_host();
        return ql_bounded_method()->run(instance_, &context, &input, 1u,
                                        &outcome_, error);
    }

    ql_artifact *outcome() const { return outcome_; }

    ql_bounded_outcome_view_v1 view() const {
        ql_bounded_outcome_view_v1 result{};
        ql_error error{};
        result.struct_size = sizeof(result);
        EXPECT_EQ(QL_STATUS_OK,
                  ql_bounded_outcome_read(outcome_, &result, &error))
            << error.message;
        return result;
    }

private:
    void *instance_ = nullptr;
    ql_artifact *outcome_ = nullptr;
};

bool BackendAvailable() {
    return ql_bitwuzla_solver_descriptor()->capability.availability !=
           QL_SOLVER_UNAVAILABLE;
}

/* Returns 7 exactly when the loop runs at least ten iterations, so the
   difference from the constant function sits beyond a small bound and inside
   a large one. */
constexpr char kLateDivergence[] =
    "int f(int n) {"
    "  for (int i = 0; i < n; i++) { if (i == 9) return 7; }"
    "  return 0;"
    "}";
constexpr char kConstantZero[] = "int g(int n) { return 0; }";

constexpr char kSumLoop[] =
    "int sum(int n) {"
    "  int total = 0;"
    "  for (int i = 0; i < n; i++) { total += i; }"
    "  return total;"
    "}";
constexpr char kSumLoopWhile[] =
    "int acc(int n) {"
    "  int total = 0;"
    "  int i = 0;"
    "  while (i < n) { total += i; i++; }"
    "  return total;"
    "}";
constexpr char kSumLoopOffByOne[] =
    "int off(int n) {"
    "  int total = 0;"
    "  for (int i = 0; i < n; i++) { total += i; }"
    "  if (n == 3) return total + 1;"
    "  return total;"
    "}";

TEST(BoundedSymbolic, RegistersAsANamedBoundedMethod) {
    RegistryHandle registry;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_registry_create(nullptr, registry.output(), &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_register_bounded_method(registry.get(), &error))
        << error.message;

    const ql_method_v1 *method =
        ql_registry_find(registry.get(), QL_BOUNDED_METHOD_NAME);
    ASSERT_NE(nullptr, method);
    EXPECT_STREQ(QL_ARTIFACT_KIND_OUTCOME, method->output_kind);
    /* A bounded search is never a proof producer. */
    EXPECT_EQ(0u, method->flags & QL_METHOD_PROOF_PRODUCER);
    EXPECT_NE(0u, method->flags & QL_METHOD_COUNTEREXAMPLE_PRODUCER);
    ASSERT_NE(nullptr, ql_registry_find_proof_method(registry.get(),
                                                     QL_BOUNDED_METHOD_NAME));
}

TEST(BoundedSymbolic, NeverAdvertisesProofSoundnessUnderAnyOption) {
    ql_proof_method_capability_v1 capability{};
    ql_error error{};

    ql_proof_method_capability_init(&capability);
    ASSERT_EQ(QL_STATUS_OK,
              ql_proof_method_query_capability(
                  ql_bounded_proof_method(),
                  "{\"unroll_bound\":64,\"timeout_ms\":1000}", &capability,
                  &error))
        << error.message;
    EXPECT_EQ(0u, capability.soundness_classes & QL_PROOF_SOUNDNESS_PROOF);
    EXPECT_EQ(0u, capability.result_kinds & QL_PROOF_RESULT_PROOF);
    EXPECT_NE(0u, capability.result_kinds & QL_PROOF_RESULT_BOUNDED);
    EXPECT_NE(0u,
              capability.result_kinds & QL_PROOF_RESULT_COUNTEREXAMPLE);
}

TEST(BoundedSymbolic, RejectsAnUnknownOptionBeforeRunning) {
    void *instance = nullptr;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_bounded_method()->create(ql_default_host(),
                                          "{\"unroll\":4}", &instance,
                                          &error));
    EXPECT_EQ(nullptr, instance);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_bounded_method()->create(ql_default_host(),
                                          "{\"unroll_bound\":0}", &instance,
                                          &error));
    EXPECT_EQ(nullptr, instance);
}

TEST(BoundedSymbolic, FindsALoopCounterexampleAndReplaysIt) {
    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    w2::Pair pair;
    BoundedRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(kSumLoop, "sum", kSumLoopOffByOne, "off",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, "{\"unroll_bound\":8}", &error))
        << error.message;

    const ql_bounded_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, view.verdict) << view.diagnostic;
    EXPECT_EQ(QL_EVIDENCE_COUNTEREXAMPLE, view.evidence_class);
    EXPECT_EQ(1u, view.replay_confirmed);
    EXPECT_EQ(0u, view.checked_proof);

    ql_artifact *counterexample = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_bounded_outcome_counterexample(nullptr, run.outcome(),
                                                &counterexample, &error));
    ASSERT_NE(nullptr, counterexample);
    ql_artifact_release(counterexample);
}

TEST(BoundedSymbolic, ReportsBoundedCleanInsideTheBoundAndSaysSo) {
    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    w2::Pair pair;
    BoundedRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(kLateDivergence, "f", kConstantZero, "g",
                         w2::DefaultContract(), &error))
        << error.message;
    /* The divergence needs ten iterations; four are searched. */
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, "{\"unroll_bound\":4}", &error))
        << error.message;

    const ql_bounded_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_BOUNDED_CLEAN, view.verdict) << view.diagnostic;
    EXPECT_EQ(QL_EVIDENCE_BOUNDED, view.evidence_class);
    EXPECT_EQ(4u, view.unroll_bound);
    EXPECT_EQ(1u, view.bound_cut_used);
    EXPECT_EQ(0u, view.checked_proof);
    EXPECT_EQ(0u, view.replay_confirmed);
}

TEST(BoundedSymbolic, TheSamePairFlipsWhenTheBoundCrossesTheDivergence) {
    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    w2::Pair pair;
    BoundedRun wide;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(kLateDivergence, "f", kConstantZero, "g",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, wide.Run(pair, "{\"unroll_bound\":16}", &error))
        << error.message;

    const ql_bounded_outcome_view_v1 view = wide.view();
    EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, view.verdict) << view.diagnostic;
    EXPECT_EQ(1u, view.replay_confirmed);
}

TEST(BoundedSymbolic, TwoLoopSpellingsOfOneSumAreCleanWithinTheBound) {
    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    w2::Pair pair;
    BoundedRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(kSumLoop, "sum", kSumLoopWhile, "acc",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, "{\"unroll_bound\":6}", &error))
        << error.message;

    const ql_bounded_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_BOUNDED_CLEAN, view.verdict) << view.diagnostic;
    EXPECT_EQ(QL_EVIDENCE_BOUNDED, view.evidence_class);
}

TEST(BoundedSymbolic, ALoopFreePairIsCleanWithoutTouchingTheBound) {
    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    w2::Pair pair;
    BoundedRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int a(int x){ return x + x; }", "a",
                         "int b(int x){ return 2 * x; }", "b",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, "{\"unroll_bound\":4}", &error))
        << error.message;

    const ql_bounded_outcome_view_v1 view = run.view();
    /* Equivalent and loop-free: the search is exhaustive in effect, but the
       method still refuses to say more than its name allows. */
    EXPECT_EQ(QL_VERDICT_BOUNDED_CLEAN, view.verdict) << view.diagnostic;
    EXPECT_EQ(0u, view.bound_cut_used);
    EXPECT_EQ(0u, view.checked_proof);
}

}  // namespace
