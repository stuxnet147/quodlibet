#include "quodlibet/proof_aigsat.h"
#include "quodlibet/proof_diff.h"

#include <cstddef>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "quodlibet/pipeline.h"
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

class DiffRun {
public:
  DiffRun() = default;
  DiffRun(const DiffRun &) = delete;
  DiffRun &operator=(const DiffRun &) = delete;

  ~DiffRun() {
    ql_artifact_release(outcome_);
    if (instance_ != nullptr) {
      ql_diff_method()->destroy(instance_);
    }
  }

  ql_status Run(const w2::Pair &pair, const char *options_json,
                ql_error *error) {
    ql_run_context_v1 context{};
    ql_artifact *input = pair.artifact();
    ql_status status = ql_diff_method()->create(ql_default_host(), options_json,
                                                &instance_, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    status = ql_diff_method()->validate(instance_, &input, 1u, error);
    if (status != QL_STATUS_OK) {
      return status;
    }
    context.struct_size = sizeof(context);
    context.abi_version = QL_ABI_VERSION;
    context.host = ql_default_host();
    return ql_diff_method()->run(instance_, &context, &input, 1u, &outcome_,
                                 error);
  }

  ql_artifact *outcome() const { return outcome_; }

  ql_diff_outcome_view_v1 view() const {
    ql_diff_outcome_view_v1 result{};
    ql_error error{};
    result.struct_size = sizeof(result);
    EXPECT_EQ(QL_STATUS_OK, ql_diff_outcome_read(outcome_, &result, &error))
        << error.message;
    return result;
  }

private:
  void *instance_ = nullptr;
  ql_artifact *outcome_ = nullptr;
};

constexpr char kAdd[] = "int add(int x, int y){ return x + y; }";
constexpr char kSum[] = "int sum(int a, int b){ return b + a; }";
/* The mismatch sits on the zero boundary, which the generator tries in its
   very first tuple. */
constexpr char kIdentity[] = "int f(int x){ return x; }";
constexpr char kZeroQuirk[] = "int g(int x){ if (x == 0) return 1; return x; }";

/* --- Registration and capability ------------------------------------------ */

TEST(ConcreteDifferential, RegistersAsANamedRefutationMethod) {
  RegistryHandle registry;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            ql_registry_create(nullptr, registry.output(), &error));
  ASSERT_EQ(QL_STATUS_OK, ql_register_diff_method(registry.get(), &error))
      << error.message;

  const ql_method_v1 *method =
      ql_registry_find(registry.get(), QL_DIFF_METHOD_NAME);
  ASSERT_NE(nullptr, method);
  EXPECT_STREQ(QL_ARTIFACT_KIND_OUTCOME, method->output_kind);
  /* It refutes, so it must never be scheduled as a proof producer. */
  EXPECT_EQ(0u, method->flags & QL_METHOD_PROOF_PRODUCER);
  EXPECT_NE(0u, method->flags & QL_METHOD_COUNTEREXAMPLE_PRODUCER);
  EXPECT_NE(0u, method->flags & QL_METHOD_DETERMINISTIC);
  ASSERT_NE(nullptr,
            ql_registry_find_proof_method(registry.get(), QL_DIFF_METHOD_NAME));
}

TEST(ConcreteDifferential, IsRegisteredBesideTheSmtMethodAsABuiltin) {
  RegistryHandle registry;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            ql_registry_create(nullptr, registry.output(), &error));
  ASSERT_EQ(QL_STATUS_OK, ql_register_builtin_methods(registry.get(), &error))
      << error.message;
  EXPECT_NE(nullptr, ql_registry_find(registry.get(), QL_DIFF_METHOD_NAME));
  EXPECT_NE(nullptr,
            ql_registry_find(registry.get(), QL_SMT_PRODUCT_METHOD_NAME));
  /* The AIG/SAT prover joined them; the count is pinned so a method that
     appears without anyone noticing fails here. */
  EXPECT_NE(nullptr, ql_registry_find(registry.get(), QL_AIG_SAT_METHOD_NAME));
  EXPECT_EQ(3u, ql_registry_proof_method_count(registry.get()));
}

TEST(ConcreteDifferential, NeverAdvertisesProofOrBoundedSoundness) {
  RegistryHandle registry;
  ql_proof_method_capability_v1 capability{};
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            ql_registry_create(nullptr, registry.output(), &error));
  ASSERT_EQ(QL_STATUS_OK, ql_register_diff_method(registry.get(), &error));

  ASSERT_EQ(QL_STATUS_OK, ql_registry_query_proof_capability(
                              registry.get(), QL_DIFF_METHOD_NAME, nullptr,
                              &capability, &error))
      << error.message;
  EXPECT_EQ(QL_PROOF_METHOD_FAMILY_CONCRETE_DIFFERENTIAL, capability.family);
  EXPECT_EQ(QL_PROOF_SOUNDNESS_COUNTEREXAMPLE, capability.soundness_classes);
  EXPECT_NE(0u, capability.result_kinds & QL_PROOF_RESULT_COUNTEREXAMPLE);
  EXPECT_EQ(0u, capability.result_kinds & QL_PROOF_RESULT_PROOF);
  /* Sampling exhausts no bound, so BOUNDED_CLEAN is not on offer either.
     No option can turn either of these on. */
  EXPECT_EQ(0u, capability.result_kinds & QL_PROOF_RESULT_BOUNDED);

  ASSERT_EQ(QL_STATUS_OK, ql_registry_query_proof_capability(
                              registry.get(), QL_DIFF_METHOD_NAME,
                              "{\"seed\":1,\"tests\":8}", &capability, &error));
  EXPECT_EQ(0u, capability.result_kinds &
                    (QL_PROOF_RESULT_PROOF | QL_PROOF_RESULT_BOUNDED));
}

TEST(ConcreteDifferential, RejectsUnknownAndMalformedOptions) {
  RegistryHandle registry;
  ql_proof_method_capability_v1 capability{};
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            ql_registry_create(nullptr, registry.output(), &error));
  ASSERT_EQ(QL_STATUS_OK, ql_register_diff_method(registry.get(), &error));

  EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
            ql_registry_query_proof_capability(
                registry.get(), QL_DIFF_METHOD_NAME, "{\"rounds\":4}",
                &capability, &error));
  EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
            ql_registry_query_proof_capability(
                registry.get(), QL_DIFF_METHOD_NAME, "{\"tests\":\"many\"}",
                &capability, &error));
  EXPECT_EQ(QL_STATUS_PARSE_ERROR, ql_registry_query_proof_capability(
                                       registry.get(), QL_DIFF_METHOD_NAME,
                                       "[1]", &capability, &error));

  std::string too_many = "{\"tests\":";
  too_many += std::to_string(QL_DIFF_MAX_TESTS + 1u);
  too_many += "}";
  EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
            ql_registry_query_proof_capability(
                registry.get(), QL_DIFF_METHOD_NAME, too_many.c_str(),
                &capability, &error));
}

TEST(ConcreteDifferential, RefusesASchemaV1ProblemBeforeExecuting) {
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
  ASSERT_EQ(QL_STATUS_OK, ql_diff_method()->create(ql_default_host(), nullptr,
                                                   &instance, &error));
  EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
            ql_diff_method()->validate(instance, &artifact, 1u, &error));
  EXPECT_NE(nullptr, std::strstr(error.message, "schema v2"));
  ql_diff_method()->destroy(instance);
  ql_artifact_release(artifact);
}

/* --- Refutation ----------------------------------------------------------- */

/* No GTEST_SKIP guard anywhere in this file: the whole point of this method is
   that it needs no solver, so it must produce the same answers in a build with
   Bitwuzla disabled. */
TEST(ConcreteDifferential, FindsAMismatchOnABoundaryInputAndReplayConfirmsIt) {
  w2::Pair pair;
  DiffRun run;
  ql_artifact *counterexample = nullptr;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kIdentity, "f", kZeroQuirk, "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, nullptr, &error)) << error.message;

  const ql_diff_outcome_view_v1 view = run.view();
  EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, view.verdict);
  EXPECT_EQ(QL_EVIDENCE_COUNTEREXAMPLE, view.evidence_class);
  /* A witness produced by concretely running both sides is replayed by
     construction; there is no unreplayed stage to confirm. */
  EXPECT_EQ(1u, view.replay_confirmed);
  EXPECT_EQ(0u, view.checked_proof);
  /* The zero pattern is the first tuple the generator emits. */
  EXPECT_EQ(1u, view.tests_executed);
  EXPECT_EQ(1u, view.tests_conclusive);

  ASSERT_EQ(QL_STATUS_OK, ql_diff_outcome_counterexample(
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

TEST(ConcreteDifferential, FindsAMismatchThatOnlyRandomInputsReach) {
  /* Every boundary tuple assigns both arguments the same pattern, and
     x + y == x - y holds whenever y is zero or the two happen to agree
     modulo 2^32, so this pair is reached by the random phase. */
  w2::Pair pair;
  DiffRun run;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            pair.Build("int f(int x, int y){ return x + y; }", "f",
                       "int g(int a, int b){ return a - b; }", "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, nullptr, &error)) << error.message;

  const ql_diff_outcome_view_v1 view = run.view();
  EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, view.verdict);
  EXPECT_EQ(1u, view.replay_confirmed);
}

TEST(ConcreteDifferential, FindingNothingIsUnknownAndNeverBoundedClean) {
  w2::Pair pair;
  DiffRun run;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kAdd, "add", kSum, "sum", w2::DefaultContract(), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, "{\"tests\":64}", &error))
      << error.message;

  const ql_diff_outcome_view_v1 view = run.view();
  EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
  EXPECT_EQ(QL_EVIDENCE_UNKNOWN, view.evidence_class);
  /* This is the line the whole method hangs on. Sixty-four passing tests
     over a 2^64 input space is not a bound anyone exhausted. */
  EXPECT_NE(QL_VERDICT_BOUNDED_CLEAN, view.verdict);
  EXPECT_NE(QL_EVIDENCE_BOUNDED, view.evidence_class);
  EXPECT_EQ(0u, view.replay_confirmed);
  EXPECT_EQ(64u, view.tests_executed);
  EXPECT_EQ(64u, view.tests_conclusive);
  EXPECT_NE(nullptr, std::strstr(view.diagnostic, "exhausts no bound"));
}

TEST(ConcreteDifferential, TheSameSeedReproducesTheSameSearch) {
  w2::Pair first_pair;
  w2::Pair second_pair;
  w2::Pair third_pair;
  DiffRun first;
  DiffRun second;
  DiffRun third;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK, first_pair.Build(kAdd, "add", kSum, "sum",
                                           w2::DefaultContract(), &error));
  ASSERT_EQ(QL_STATUS_OK, second_pair.Build(kAdd, "add", kSum, "sum",
                                            w2::DefaultContract(), &error));
  ASSERT_EQ(QL_STATUS_OK, third_pair.Build(kAdd, "add", kSum, "sum",
                                           w2::DefaultContract(), &error));
  ASSERT_EQ(QL_STATUS_OK,
            first.Run(first_pair, "{\"seed\":11,\"tests\":32}", &error));
  ASSERT_EQ(QL_STATUS_OK,
            second.Run(second_pair, "{\"seed\":11,\"tests\":32}", &error));
  ASSERT_EQ(QL_STATUS_OK,
            third.Run(third_pair, "{\"seed\":12,\"tests\":32}", &error));

  const ql_diff_outcome_view_v1 a = first.view();
  const ql_diff_outcome_view_v1 b = second.view();
  const ql_diff_outcome_view_v1 c = third.view();
  EXPECT_EQ(11u, a.seed);
  EXPECT_EQ(1u, ql_digest_equal(&a.cache_key, &b.cache_key));
  /* A different sample is a different search and must not reuse the first
     one's cached answer. */
  EXPECT_EQ(0u, ql_digest_equal(&a.cache_key, &c.cache_key));
}

TEST(ConcreteDifferential, CountsInputsThePreconditionRejects) {
  /* The precondition admits only strictly positive x, so the zero and
     negative boundary patterns are rejected rather than counted as passing
     comparisons. */
  constexpr char precondition[] =
      "{\"schema_version\":1,\"expression\":"
      "{\"op\":\"sgt\",\"left\":{\"op\":\"arg\",\"index\":0},"
      "\"right\":{\"op\":\"int\",\"signed\":true,\"width\":32,"
      "\"value\":\"0\"}}}";
  w2::Pair pair;
  DiffRun run;
  ql_semantic_contract_v1 contract =
      w2::ContractObserving(QL_OBSERVE_RETURN_VALUE);
  ql_error error{};

  contract.precondition_json = precondition;
  contract.precondition_json_size = sizeof(precondition) - 1u;
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kIdentity, "f", "int g(int x){ return x; }", "g",
                       contract, &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, run.Run(pair, "{\"tests\":32}", &error))
      << error.message;

  const ql_diff_outcome_view_v1 view = run.view();
  EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
  EXPECT_EQ(32u, view.tests_executed);
  EXPECT_GT(view.tests_precondition_rejected, 0u);
  EXPECT_GT(view.tests_conclusive, 0u);
  EXPECT_EQ(view.tests_executed,
            view.tests_conclusive + view.tests_precondition_rejected);
}

TEST(ConcreteDifferential, ReportsUnknownWhenTheLoweringCannotStateTheSide) {
  w2::Pair pair;
  DiffRun run;
  ql_error error{};

  /* Volatile access leaves the restricted-C slice. The method inherits
     that as UNKNOWN instead of comparing a narrower question. */
  ASSERT_EQ(QL_STATUS_TYPE_MISMATCH,
            pair.Build(kIdentity, "f",
                       "int g(int x){ _Atomic int y = x; return y; }", "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error));
  (void)run;
}

TEST(ConcreteDifferential, RunsThroughAPipelineSelectedByName) {
  constexpr char pipeline_json[] =
      "{\"schema_version\":1,\"nodes\":[{\"name\":\"differential\","
      "\"method\":\"refute.concrete-differential\","
      "\"options\":{\"tests\":16}}]}";
  RegistryHandle registry;
  w2::Pair pair;
  ql_scheduler *scheduler = nullptr;
  ql_pipeline *pipeline = nullptr;
  ql_pipeline_result *result = nullptr;
  ql_error error{};

  ASSERT_EQ(QL_STATUS_OK, ql_scheduler_create(nullptr, 1u, &scheduler, &error));
  ASSERT_EQ(QL_STATUS_OK,
            ql_registry_create(nullptr, registry.output(), &error));
  ASSERT_EQ(QL_STATUS_OK, ql_register_diff_method(registry.get(), &error));
  ASSERT_EQ(QL_STATUS_OK,
            pair.Build(kIdentity, "f", kZeroQuirk, "g",
                       w2::ContractObserving(QL_OBSERVE_RETURN_VALUE), &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, ql_pipeline_from_json(
                              registry.get(), nullptr, pipeline_json,
                              sizeof(pipeline_json) - 1u, &pipeline, &error))
      << error.message;
  ASSERT_EQ(QL_STATUS_OK, ql_pipeline_run(pipeline, scheduler, pair.artifact(),
                                          nullptr, &result, &error))
      << error.message;
  ASSERT_EQ(1u, ql_pipeline_result_count(result));

  ql_diff_outcome_view_v1 view{};
  view.struct_size = sizeof(view);
  ASSERT_EQ(QL_STATUS_OK,
            ql_diff_outcome_read(ql_pipeline_result_artifact(result, 0u), &view,
                                 &error))
      << error.message;
  EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, view.verdict);

  ql_pipeline_result_destroy(result);
  ql_pipeline_destroy(pipeline);
  ql_scheduler_destroy(scheduler);
}

TEST(ConcreteDifferential, RefusesToReadAnOutcomeFromAnotherMethod) {
  ql_artifact *foreign = nullptr;
  ql_diff_outcome_view_v1 view{};
  ql_error error{};
  constexpr char json[] =
      "{\"kind\":\"quodlibet.outcome\",\"schema_version\":1,"
      "\"method\":\"prove.smt-product\",\"search\":{},\"trust\":{}}";

  ASSERT_EQ(QL_STATUS_OK,
            ql_artifact_create(nullptr, QL_ARTIFACT_KIND_OUTCOME, 1u, json,
                               sizeof(json) - 1u, &foreign, &error));
  view.struct_size = sizeof(view);
  EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
            ql_diff_outcome_read(foreign, &view, &error));
  ql_artifact_release(foreign);
}

} // namespace
