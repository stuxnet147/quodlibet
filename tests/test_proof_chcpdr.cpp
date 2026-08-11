/* prove.chc-pdr: invariant synthesis where the fixed-vocabulary induction
   gives up.

   The centerpiece is a pair the structural fast path cannot close: one loop
   counts up from zero while the other counts down from the input, so the
   relating invariant i + j = n names an input and is outside the
   equality/offset/affine candidate vocabulary. PDR closes it from the
   entry-anchored sum template, and the tests also pin the discipline: a
   reachable bad state never becomes a counterexample, and promotion demands
   the explicit trusted-backend policy. */

#include "quodlibet/proof_chcpdr.h"

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "quodlibet/solver.h"
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

class PdrRun {
public:
    PdrRun() = default;
    PdrRun(const PdrRun &) = delete;
    PdrRun &operator=(const PdrRun &) = delete;

    ~PdrRun() {
        ql_artifact_release(outcome_);
        if (instance_ != nullptr) {
            ql_chcpdr_method()->destroy(instance_);
        }
        ql_solver_session_destroy(session_);
    }

    ql_status Run(const w2::Pair &pair, const char *options_json,
                  ql_error *error) {
        ql_run_context_v1 context{};
        ql_artifact *input = pair.artifact();
        ql_status status = ql_chcpdr_method()->create(
            ql_default_host(), options_json, &instance_, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_chcpdr_method()->validate(instance_, &input, 1u, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        /* PDR asks many small questions; one session keeps the per-query
           cost at the solve rather than at the backend installation. */
        if (session_ == nullptr) {
            ql_error session_error{};
            (void)ql_solver_session_create(nullptr,
                                           ql_bitwuzla_solver_descriptor(),
                                           nullptr, &session_,
                                           &session_error);
        }
        context.struct_size = sizeof(context);
        context.abi_version = QL_ABI_VERSION;
        context.host = ql_default_host();
        context.solver_session = session_;
        return ql_chcpdr_method()->run(instance_, &context, &input, 1u,
                                       &outcome_, error);
    }

    ql_chcpdr_outcome_view_v1 view() const {
        ql_chcpdr_outcome_view_v1 result{};
        ql_error error{};
        result.struct_size = sizeof(result);
        EXPECT_EQ(QL_STATUS_OK,
                  ql_chcpdr_outcome_read(outcome_, &result, &error))
            << error.message;
        return result;
    }

private:
    void *instance_ = nullptr;
    ql_artifact *outcome_ = nullptr;
    ql_solver_session *session_ = nullptr;
};

bool BackendAvailable() {
    return ql_bitwuzla_solver_descriptor()->capability.availability !=
           QL_SOLVER_UNAVAILABLE;
}

constexpr char kTrusted[] = "{\"unsat_promotion\":\"trusted-backend\"}";

/* The differentiator: the invariant i + j = n is entry-anchored and outside
   the fast path's constant-coefficient vocabulary. */
constexpr char kCountUp[] =
    "unsigned up(unsigned n) {"
    "  unsigned i = 0u;"
    "  unsigned s = 0u;"
    "  while (i != n) { s = s + 2u; i = i + 1u; }"
    "  return s;"
    "}";
constexpr char kCountDown[] =
    "unsigned down(unsigned n) {"
    "  unsigned j = n;"
    "  unsigned t = 0u;"
    "  while (j != 0u) { t = t + 2u; j = j - 1u; }"
    "  return t;"
    "}";

/* Same-shape loops the candidate vocabulary covers; PDR should agree with
   the fast path from its seeded equalities. */
constexpr char kStrideOne[] =
    "unsigned f(unsigned n) {"
    "  unsigned i = 0u;"
    "  unsigned s = 0u;"
    "  while (i != n) { s = s + 1u; i = i + 1u; }"
    "  return s;"
    "}";
constexpr char kStrideOneToo[] =
    "unsigned g(unsigned m) {"
    "  unsigned k = 0u;"
    "  unsigned u = 0u;"
    "  while (k != m) { u = u + 1u; k = k + 1u; }"
    "  return u;"
    "}";
constexpr char kStrideTwo[] =
    "unsigned h(unsigned m) {"
    "  unsigned k = 0u;"
    "  unsigned u = 0u;"
    "  while (k != m) { u = u + 2u; k = k + 1u; }"
    "  return u;"
    "}";

TEST(ChcPdr, RegistersAsANamedProofMethod) {
    RegistryHandle registry;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_registry_create(nullptr, registry.output(), &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_register_chcpdr_method(registry.get(), &error))
        << error.message;

    const ql_method_v1 *method =
        ql_registry_find(registry.get(), QL_CHCPDR_METHOD_NAME);
    ASSERT_NE(nullptr, method);
    EXPECT_STREQ(QL_ARTIFACT_KIND_OUTCOME, method->output_kind);
    EXPECT_NE(0u, method->flags & QL_METHOD_PROOF_PRODUCER);
    EXPECT_EQ(0u, method->flags & QL_METHOD_COUNTEREXAMPLE_PRODUCER);
    ASSERT_NE(nullptr, ql_registry_find_proof_method(registry.get(),
                                                     QL_CHCPDR_METHOD_NAME));
}

TEST(ChcPdr, AdvertisesProofSoundnessOnlyUnderTheTrustPolicy) {
    ql_proof_method_capability_v1 capability{};
    ql_error error{};

    ql_proof_method_capability_init(&capability);
    ASSERT_EQ(QL_STATUS_OK,
              ql_proof_method_query_capability(ql_chcpdr_proof_method(),
                                               nullptr, &capability, &error))
        << error.message;
    EXPECT_EQ(0u, capability.soundness_classes & QL_PROOF_SOUNDNESS_PROOF);
    EXPECT_EQ(0u, capability.result_kinds & QL_PROOF_RESULT_COUNTEREXAMPLE);

    ql_proof_method_capability_init(&capability);
    ASSERT_EQ(QL_STATUS_OK,
              ql_proof_method_query_capability(ql_chcpdr_proof_method(),
                                               kTrusted, &capability, &error))
        << error.message;
    EXPECT_NE(0u, capability.soundness_classes & QL_PROOF_SOUNDNESS_PROOF);
}

TEST(ChcPdr, RejectsAnUnknownOptionBeforeRunning) {
    void *instance = nullptr;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_chcpdr_method()->create(ql_default_host(),
                                         "{\"frames\":4}", &instance,
                                         &error));
    EXPECT_EQ(nullptr, instance);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_chcpdr_method()->create(ql_default_host(),
                                         "{\"unsat_promotion\":\"maybe\"}",
                                         &instance, &error));
    EXPECT_EQ(nullptr, instance);
}

TEST(ChcPdr, ClosesTheCountUpCountDownPairTheFastPathCannot) {
    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    w2::Pair pair;
    PdrRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(kCountUp, "up", kCountDown, "down",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

    const ql_chcpdr_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict)
        << view.diagnostic;
    EXPECT_EQ(QL_EVIDENCE_PROOF, view.evidence_class);
    EXPECT_EQ(1u, view.invariant_verified);
    /* The promotion rests on the recorded trusted backend, not a checked
       certificate, and the outcome must say so. */
    EXPECT_EQ(0u, view.checked_proof);
    EXPECT_GE(view.invariant_lemma_count, 1u);
}

TEST(ChcPdr, TheSameInvariantIsNotPromotedWithoutThePolicy) {
    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    w2::Pair pair;
    PdrRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(kCountUp, "up", kCountDown, "down",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, nullptr, &error)) << error.message;

    const ql_chcpdr_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    /* The work was done and recorded; only the promotion was withheld. */
    EXPECT_EQ(1u, view.invariant_verified);
    EXPECT_NE(nullptr, std::strstr(view.diagnostic, "trusted-backend"))
        << view.diagnostic;
}

TEST(ChcPdr, AgreesWithTheFastPathOnAPlainEqualityPair) {
    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    w2::Pair pair;
    PdrRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(kStrideOne, "f", kStrideOneToo, "g",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

    const ql_chcpdr_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict)
        << view.diagnostic;
    EXPECT_EQ(1u, view.invariant_verified);
}

TEST(ChcPdr, AReachableBadStateIsNeverACounterexample) {
    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    w2::Pair pair;
    PdrRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(kStrideOne, "f", kStrideTwo, "h",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

    const ql_chcpdr_outcome_view_v1 view = run.view();
    /* Whether the search surfaces the concrete trace or exhausts a budget
       blocking unreachable cubes first, the discipline is the same: an
       inequivalent pair never produces a verdict stronger than UNKNOWN, and
       no invariant can verify. */
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict) << view.diagnostic;
    EXPECT_EQ(0u, view.invariant_verified);
}

TEST(ChcPdr, RefusesANestedLoopWithTheSerializerDiagnostic) {
    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    w2::Pair pair;
    PdrRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("unsigned f(unsigned n) {"
                         "  unsigned s = 0u;"
                         "  for (unsigned i = 0u; i != n; i++) {"
                         "    for (unsigned j = 0u; j != n; j++) { s = s + 1u; }"
                         "  }"
                         "  return s;"
                         "}",
                         "f",
                         "unsigned g(unsigned n) { return n * n; }", "g",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

    const ql_chcpdr_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(0u, view.invariant_verified);
}

}  // namespace
