#include "quodlibet/proof_smt.h"

#include <cstddef>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "quodlibet/pipeline.h"
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

class OutcomeRun {
public:
  OutcomeRun() = default;
  OutcomeRun(const OutcomeRun &) = delete;
  OutcomeRun &operator=(const OutcomeRun &) = delete;

  ~OutcomeRun() {
    ql_artifact_release(outcome_);
    if (instance_ != nullptr) {
      ql_smt_product_method()->destroy(instance_);
    }
  }

  /* Drives the method directly so a test states only the two C sources,
     the contract, and the options. */
  ql_status Run(const w2::Pair &pair, const char *options_json,
                ql_error *error) {
    ql_run_context_v1 context{};
    ql_artifact *input = pair.artifact();
    ql_status status = ql_smt_product_method()->create(
        ql_default_host(), options_json, &instance_, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    status = ql_smt_product_method()->validate(instance_, &input, 1u, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    context.struct_size = sizeof(context);
    context.abi_version = QL_ABI_VERSION;
    context.host = ql_default_host();
    return ql_smt_product_method()->run(instance_, &context, &input, 1u,
                                        &outcome_, error);
  }

  ql_artifact *outcome() const { return outcome_; }

  ql_smt_product_outcome_view_v1 view() const {
    ql_smt_product_outcome_view_v1 result{};
    ql_error error{};
    result.struct_size = sizeof(result);
    EXPECT_EQ(QL_STATUS_OK,
              ql_smt_product_outcome_read(outcome_, &result, &error))
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

constexpr char kAdd[] = "int add(int x, int y){ return x + y; }";
constexpr char kSum[] = "int sum(int a, int b){ return b + a; }";
constexpr char kTrusted[] = "{\"unsat_promotion\":\"trusted-backend\"}";

TEST(SmtProductMethod, RegistersAsANamedProofMethod) {
  RegistryHandle registry;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            ql_registry_create(nullptr, registry.output(), &error));
  ASSERT_EQ(QL_STATUS_OK,
            ql_register_smt_product_method(registry.get(), &error))
      << error.message;

  const ql_method_v1 *method =
      ql_registry_find(registry.get(), QL_SMT_PRODUCT_METHOD_NAME);
  ASSERT_NE(nullptr, method);
  EXPECT_STREQ(QL_ARTIFACT_KIND_OUTCOME, method->output_kind);
  ASSERT_NE(nullptr, ql_registry_find_proof_method(registry.get(),
                                                   QL_SMT_PRODUCT_METHOD_NAME));
  EXPECT_EQ(1u, ql_registry_proof_method_count(registry.get()));
}

TEST(SmtProductMethod, AdvertisesProofOnlyWhenTheTrustPolicyIsSelected) {
  RegistryHandle registry;
  ql_proof_method_capability_v1 capability{};
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            ql_registry_create(nullptr, registry.output(), &error));
  ASSERT_EQ(QL_STATUS_OK,
            ql_register_smt_product_method(registry.get(), &error));

  ASSERT_EQ(QL_STATUS_OK, ql_registry_query_proof_capability(
                              registry.get(), QL_SMT_PRODUCT_METHOD_NAME,
                              nullptr, &capability, &error))
      << error.message;
  EXPECT_EQ(QL_PROOF_METHOD_FAMILY_SMT, capability.family);
  EXPECT_EQ(0u, capability.result_kinds & QL_PROOF_RESULT_PROOF);
  EXPECT_EQ(0u, capability.soundness_classes & QL_PROOF_SOUNDNESS_PROOF);
  EXPECT_NE(0u, capability.result_kinds & QL_PROOF_RESULT_COUNTEREXAMPLE);
  /* A loop-free product is complete, so it never reports a bound. */
  EXPECT_EQ(0u, capability.result_kinds & QL_PROOF_RESULT_BOUNDED);

  ASSERT_EQ(QL_STATUS_OK, ql_registry_query_proof_capability(
                              registry.get(), QL_SMT_PRODUCT_METHOD_NAME,
                              kTrusted, &capability, &error))
      << error.message;
  EXPECT_NE(0u, capability.result_kinds & QL_PROOF_RESULT_PROOF);
  EXPECT_NE(0u, capability.soundness_classes & QL_PROOF_SOUNDNESS_PROOF);

  EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
            ql_registry_query_proof_capability(
                registry.get(), QL_SMT_PRODUCT_METHOD_NAME,
                "{\"unsat_promotion\":\"always\"}", &capability, &error));
  EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
            ql_registry_query_proof_capability(
                registry.get(), QL_SMT_PRODUCT_METHOD_NAME,
                "{\"promote\":true}", &capability, &error));
}

TEST(SmtProductMethod, RefusesASchemaV1ProblemBeforeExecuting) {
  ql_problem_definition_v1 definition{};
  ql_artifact *artifact = nullptr;
  void *instance = nullptr;
  ql_error error{};

  ql_problem_definition_init(&definition);
  definition.left_source = kAdd;
  definition.left_source_size = sizeof(kAdd) - 1u;
  definition.left_function_name = "add";
  definition.left_function_name_size = 3u;
  definition.right_source = kSum;
  definition.right_source_size = sizeof(kSum) - 1u;
  definition.right_function_name = "sum";
  definition.right_function_name_size = 3u;
  ASSERT_EQ(QL_STATUS_OK, ql_problem_artifact_create(nullptr, &definition,
                                                     &artifact, &error));
  ASSERT_EQ(QL_STATUS_OK, ql_smt_product_method()->create(
                              ql_default_host(), nullptr, &instance, &error));
  EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
            ql_smt_product_method()->validate(instance, &artifact, 1u, &error));
  EXPECT_NE(nullptr, std::strstr(error.message, "schema v2"));
  ql_smt_product_method()->destroy(instance);
  ql_artifact_release(artifact);
}

/* --- The three end-to-end results ----------------------------------------- */

TEST(SmtProductMethod, ProvesEquivalenceOnlyUnderTheRecordedTrustPolicy) {
  w2::Pair pair;
  OutcomeRun without;
  OutcomeRun with;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kAdd, "add", kSum, "sum", w2::DefaultContract(), &error))
      << error.message;

  ASSERT_EQ(QL_STATUS_OK, without.Run(pair, nullptr, &error)) << error.message;
  const ql_smt_product_outcome_view_v1 unproved = without.view();
  EXPECT_EQ(QL_VERDICT_UNKNOWN, unproved.verdict);
  EXPECT_EQ(QL_EVIDENCE_UNKNOWN, unproved.evidence_class);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, unproved.violation_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, unproved.domain_answer);
  EXPECT_EQ(QL_UNSAT_PROMOTION_NONE, unproved.unsat_promotion);
  EXPECT_NE(nullptr, std::strstr(unproved.diagnostic, "evidence only"));

  ASSERT_EQ(QL_STATUS_OK, with.Run(pair, kTrusted, &error)) << error.message;
  const ql_smt_product_outcome_view_v1 proved = with.view();
  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, proved.verdict);
  EXPECT_EQ(QL_EVIDENCE_PROOF, proved.evidence_class);
  EXPECT_EQ(QL_UNSAT_PROMOTION_TRUSTED_BACKEND, proved.unsat_promotion);
  /* Bitwuzla exposes no proof object, and the envelope must say so rather
     than let a reader assume a certificate was checked. */
  EXPECT_EQ(0u, proved.checked_proof);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, proved.violation_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, proved.domain_answer);

  ql_digest zero{};
  EXPECT_EQ(0u, ql_digest_equal(&proved.solver_binary_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&proved.solver_query_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&proved.problem_digest, &zero));
  EXPECT_EQ(0u, ql_digest_equal(&proved.cache_key, &zero));
  /* The two runs answer the same question with different trust policies,
     so their cache identities must differ. */
  EXPECT_EQ(0u, ql_digest_equal(&proved.cache_key, &unproved.cache_key));
}

TEST(SmtProductMethod, CacheIdentityBindsExactSolverOptions) {
  w2::Pair pair;
  OutcomeRun defaults;
  OutcomeRun explicit_options;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kAdd, "add", kSum, "sum", w2::DefaultContract(),
                       &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, defaults.Run(pair, kTrusted, &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK,
            explicit_options.Run(
                pair,
                "{\"unsat_promotion\":\"trusted-backend\","
                "\"solver_options\":\"{}\"}",
                &error))
      << error.message;

  const ql_smt_product_outcome_view_v1 left = defaults.view();
  const ql_smt_product_outcome_view_v1 right = explicit_options.view();
  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, left.verdict);
  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, right.verdict);
  EXPECT_EQ(0u, ql_digest_equal(&left.cache_key, &right.cache_key));
}

TEST(SmtProductMethod, EmitsAReplayedCounterexample) {
  w2::Pair pair;
  OutcomeRun run;
  ql_artifact *counterexample = nullptr;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build("int f(int x){ return x; }", "f",
                       "int g(int x){ if (x == 7) return 7; return 0; }", "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

  const ql_smt_product_outcome_view_v1 view = run.view();
  EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, view.verdict);
  EXPECT_EQ(QL_EVIDENCE_COUNTEREXAMPLE, view.evidence_class);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.violation_answer);
  EXPECT_EQ(1u, view.replay_confirmed);
  EXPECT_NE(nullptr, std::strstr(view.diagnostic, "concrete replay"));

  ASSERT_EQ(QL_STATUS_OK, ql_smt_product_outcome_counterexample(
                              nullptr, run.outcome(), &counterexample, &error))
      << error.message;
  ASSERT_NE(nullptr, counterexample);
  ql_artifact_view artifact_view{};
  artifact_view.struct_size = sizeof(artifact_view);
  ASSERT_EQ(QL_STATUS_OK,
            ql_artifact_get_view(counterexample, &artifact_view, &error));
  EXPECT_EQ(
      1u, ql_digest_equal(&artifact_view.digest, &view.counterexample_digest));
  EXPECT_NE(std::string::npos,
            std::string(static_cast<const char *>(artifact_view.data),
                        artifact_view.size)
                .find("\"replayed\":true"));
  ql_artifact_release(counterexample);
}

TEST(SmtProductMethod, ReportsUnknownWhenTheLoweringCannotStateTheFunction) {
  w2::CFunction supported;
  ql_error error{};

  /* The restricted-C slice refuses an atomic access, and the method must
     inherit that as UNKNOWN rather than encode a narrower question. */
  ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&supported, kAdd, "add"));
  w2::CFunction unmodeled;
  ASSERT_EQ(QL_STATUS_OK,
            unmodeled.Build("int add(int x, int y){ _Atomic int z = x; "
                            "return z + y; }",
                            "add", &error));
  EXPECT_EQ(QL_C_LOWER_UNKNOWN, unmodeled.support());
}

TEST(SmtProductMethod, AVacuousDomainNeverBecomesAProof) {
  constexpr char precondition[] =
      "{\"schema_version\":1,\"expression\":{\"op\":\"and\",\"args\":["
      "{\"op\":\"slt\",\"left\":{\"op\":\"arg\",\"index\":0},"
      "\"right\":{\"op\":\"int\",\"signed\":true,\"width\":32,"
      "\"value\":\"0\"}},"
      "{\"op\":\"sgt\",\"left\":{\"op\":\"arg\",\"index\":0},"
      "\"right\":{\"op\":\"int\",\"signed\":true,\"width\":32,"
      "\"value\":\"0\"}}]}}";
  w2::Pair pair;
  OutcomeRun run;
  ql_semantic_contract_v1 contract = w2::DefaultContract();
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  contract.precondition_json = precondition;
  contract.precondition_json_size = sizeof(precondition) - 1u;
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build("int f(int x){ return x; }", "f",
                       "int g(int x){ return 0; }", "g", contract, &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

  const ql_smt_product_outcome_view_v1 view = run.view();
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
  EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.domain_answer);
  EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
  EXPECT_NE(nullptr, std::strstr(view.diagnostic, "vacuous"));
}

TEST(SmtProductMethod, RunsThroughAPipelineSelectedByName) {
  constexpr char pipeline_json[] =
      "{\"schema_version\":1,\"nodes\":[{\"name\":\"smt\","
      "\"method\":\"prove.smt-product\","
      "\"options\":{\"unsat_promotion\":\"trusted-backend\"}}]}";
  RegistryHandle registry;
  w2::Pair pair;
  ql_scheduler *scheduler = nullptr;
  ql_pipeline *pipeline = nullptr;
  ql_pipeline_result *result = nullptr;
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  ASSERT_EQ(QL_STATUS_OK, ql_scheduler_create(nullptr, 1u, &scheduler, &error));
  ASSERT_EQ(QL_STATUS_OK,
            ql_registry_create(nullptr, registry.output(), &error));
  ASSERT_EQ(QL_STATUS_OK,
            ql_register_smt_product_method(registry.get(), &error));
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kAdd, "add", kSum, "sum", w2::DefaultContract(), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, ql_pipeline_from_json(
                              registry.get(), nullptr, pipeline_json,
                              sizeof(pipeline_json) - 1u, &pipeline, &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, ql_pipeline_run(pipeline, scheduler, pair.artifact(),
                                          nullptr, &result, &error))
      << error.message;
  ASSERT_EQ(1u, ql_pipeline_result_count(result));

  ql_smt_product_outcome_view_v1 view{};
  view.struct_size = sizeof(view);
  ASSERT_EQ(QL_STATUS_OK,
            ql_smt_product_outcome_read(ql_pipeline_result_artifact(result, 0u),
                                        &view, &error))
      << error.message;
  EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);

  ql_pipeline_result_destroy(result);
  ql_pipeline_destroy(pipeline);
  ql_scheduler_destroy(scheduler);
}

TEST(SmtProductMethod, DischargesRefinementDirectionsSeparately) {
  constexpr char left[] = "int f(int x, int y){ return x / y; }";
  constexpr char right[] =
      "int g(int a, int b){ if (b == 0) return 0; return a / b; }";
  w2::Pair right_refines;
  w2::Pair left_refines;
  OutcomeRun right_run;
  OutcomeRun left_run;
  ql_semantic_contract_v1 contract =
      w2::ContractObserving(QL_OBSERVE_RETURN_VALUE);
  ql_error error{};

  if (!BackendAvailable()) {
    GTEST_SKIP() << "Bitwuzla support is disabled";
  }
  contract.ub_policy = QL_UB_LANGUAGE_REFINEMENT;
  contract.relation = QL_RELATION_RIGHT_REFINES_LEFT;
  ASSERT_EQ(QL_STATUS_OK,
            right_refines.Build(left, "f", right, "g", contract, &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, right_run.Run(right_refines, kTrusted, &error))
      << error.message;
  EXPECT_EQ(QL_VERDICT_PROVED_RIGHT_REFINES_LEFT, right_run.view().verdict);

  contract.relation = QL_RELATION_LEFT_REFINES_RIGHT;
  ASSERT_EQ(QL_STATUS_OK,
            left_refines.Build(left, "f", right, "g", contract, &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, left_run.Run(left_refines, kTrusted, &error))
      << error.message;
  EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, left_run.view().verdict);
  EXPECT_EQ(1u, left_run.view().replay_confirmed);
}

} // namespace
