/* prove.egraph: equality saturation as a checked proof.

   The verdict under test is issued by the independent replay checker, not by
   the engine, so these tests pin three things: a rewrite-distance pair
   proves with checked_proof=true and no solver anywhere, a pair the
   catalogue cannot connect stays UNKNOWN rather than becoming a
   counterexample, and everything outside the pure term fragment is refused
   with a diagnostic instead of being narrowed. */

#include "quodlibet/proof_egraph.h"

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "quodlibet/egraph.h"
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

class EgraphProofRun {
public:
    EgraphProofRun() = default;
    EgraphProofRun(const EgraphProofRun &) = delete;
    EgraphProofRun &operator=(const EgraphProofRun &) = delete;

    ~EgraphProofRun() {
        ql_artifact_release(outcome_);
        if (instance_ != nullptr) {
            ql_egraph_proof_method_entry()->destroy(instance_);
        }
    }

    ql_status Run(const w2::Pair &pair, const char *options_json,
                  ql_error *error) {
        ql_run_context_v1 context{};
        ql_artifact *input = pair.artifact();
        ql_status status = ql_egraph_proof_method_entry()->create(
            ql_default_host(), options_json, &instance_, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        status = ql_egraph_proof_method_entry()->validate(instance_, &input,
                                                          1u, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        context.struct_size = sizeof(context);
        context.abi_version = QL_ABI_VERSION;
        context.host = ql_default_host();
        return ql_egraph_proof_method_entry()->run(instance_, &context,
                                                   &input, 1u, &outcome_,
                                                   error);
    }

    ql_egraph_proof_outcome_view_v1 view() const {
        ql_egraph_proof_outcome_view_v1 result{};
        ql_error error{};
        result.struct_size = sizeof(result);
        EXPECT_EQ(QL_STATUS_OK,
                  ql_egraph_proof_outcome_read(outcome_, &result, &error))
            << error.message;
        return result;
    }

private:
    void *instance_ = nullptr;
    ql_artifact *outcome_ = nullptr;
};

TEST(EgraphProof, RegistersAsANamedProofMethod) {
    RegistryHandle registry;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_registry_create(nullptr, registry.output(), &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_register_egraph_proof_method(registry.get(), &error))
        << error.message;

    const ql_method_v1 *method =
        ql_registry_find(registry.get(), QL_EGRAPH_PROOF_METHOD_NAME);
    ASSERT_NE(nullptr, method);
    EXPECT_STREQ(QL_ARTIFACT_KIND_OUTCOME, method->output_kind);
    EXPECT_NE(0u, method->flags & QL_METHOD_PROOF_PRODUCER);
    EXPECT_EQ(0u, method->flags & QL_METHOD_COUNTEREXAMPLE_PRODUCER);
    ASSERT_NE(nullptr,
              ql_registry_find_proof_method(registry.get(),
                                            QL_EGRAPH_PROOF_METHOD_NAME));
}

TEST(EgraphProof, AdvertisesProofSoundnessAndNoCounterexamples) {
    ql_proof_method_capability_v1 capability{};
    ql_error error{};

    ql_proof_method_capability_init(&capability);
    ASSERT_EQ(QL_STATUS_OK,
              ql_proof_method_query_capability(ql_egraph_proof_method(),
                                               nullptr, &capability, &error))
        << error.message;
    EXPECT_NE(0u, capability.soundness_classes & QL_PROOF_SOUNDNESS_PROOF);
    EXPECT_EQ(0u,
              capability.result_kinds & QL_PROOF_RESULT_COUNTEREXAMPLE);
    EXPECT_EQ(0u, capability.result_kinds & QL_PROOF_RESULT_BOUNDED);
}

TEST(EgraphProof, ProvesACommutedSumWithACheckedProofAndNoSolver) {
    w2::Pair pair;
    EgraphProofRun run;
    ql_error error{};

    /* Unsigned, because signed +
       carries an overflow UB guard in this profile and guards are outside
       the pure term fragment. */
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(
                  "unsigned f(unsigned a, unsigned b){ return a + b; }", "f",
                  "unsigned g(unsigned x, unsigned y){ return y + x; }", "g",
                  w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, nullptr, &error)) << error.message;

    const ql_egraph_proof_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict)
        << view.diagnostic;
    EXPECT_EQ(QL_EVIDENCE_PROOF, view.evidence_class);
    EXPECT_EQ(1u, view.checked_proof);
    EXPECT_EQ(0u, view.assumed_count);
    EXPECT_EQ(0u, view.rejected_count);
    EXPECT_EQ(QL_EGRAPH_RULE_CATALOGUE_VERSION,
              view.rule_catalogue_version);
}

TEST(EgraphProof, ProvesSelfAnnihilationAgainstTheConstant) {
    w2::Pair pair;
    EgraphProofRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int f(int a){ return a ^ a; }", "f",
                         "int g(int a){ return 0; }", "g",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, nullptr, &error)) << error.message;

    const ql_egraph_proof_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict)
        << view.diagnostic;
    EXPECT_EQ(1u, view.checked_proof);
}

TEST(EgraphProof, ProvesAnIdenticalSpellingThroughSharedTerms) {
    w2::Pair pair;
    EgraphProofRun run;
    ql_error error{};

    /* The local is folded by IDENTITY, so both sides build literally the
       same term and no rewrite is even needed. */
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(
                  "unsigned f(unsigned a){ unsigned t = a + 1u; return t; }",
                  "f", "unsigned g(unsigned a){ return a + 1u; }", "g",
                  w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, nullptr, &error)) << error.message;

    const ql_egraph_proof_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict)
        << view.diagnostic;
    EXPECT_EQ(1u, view.checked_proof);
}

TEST(EgraphProof, APairTheCatalogueCannotConnectStaysUnknown) {
    w2::Pair pair;
    EgraphProofRun run;
    ql_error error{};

    /* These are genuinely different functions. Saturation completing without
       a merge must stay UNKNOWN: the e-graph cannot refute anything. */
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int f(int a){ return a + 1; }", "f",
                         "int g(int a){ return a + 2; }", "g",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, nullptr, &error)) << error.message;

    const ql_egraph_proof_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(QL_EVIDENCE_UNKNOWN, view.evidence_class);
    EXPECT_EQ(0u, view.checked_proof);
}

TEST(EgraphProof, RefusesControlFlowWithADiagnosticInsteadOfNarrowing) {
    w2::Pair pair;
    EgraphProofRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int f(int a){ if (a > 0) return 1; return 0; }",
                         "f", "int g(int a){ return a > 0 ? 1 : 0; }", "g",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, nullptr, &error)) << error.message;

    const ql_egraph_proof_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_NE(nullptr, std::strstr(view.diagnostic, "fragment"))
        << view.diagnostic;
}

TEST(EgraphProof, RefusesADivisionBecauseItsGuardIsOutsideTheFragment) {
    w2::Pair pair;
    EgraphProofRun run;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int f(int a, int b){ return a / b; }", "f",
                         "int g(int a, int b){ return a / b; }", "g",
                         w2::DefaultContract(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, nullptr, &error)) << error.message;

    const ql_egraph_proof_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(0u, view.checked_proof);
}

TEST(EgraphProof, RejectsAnUnknownOptionBeforeRunning) {
    void *instance = nullptr;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_proof_method_entry()->create(
                  ql_default_host(), "{\"nodes\":10}", &instance, &error));
    EXPECT_EQ(nullptr, instance);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_egraph_proof_method_entry()->create(
                  ql_default_host(), "{\"iteration_limit\":0}", &instance,
                  &error));
    EXPECT_EQ(nullptr, instance);
}

}  // namespace
