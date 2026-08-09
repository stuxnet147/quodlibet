#include "quodlibet/replay.h"

#include <cstddef>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "w2_fixtures.h"

namespace {

class QueryHandle {
public:
    QueryHandle() = default;
    QueryHandle(const QueryHandle &) = delete;
    QueryHandle &operator=(const QueryHandle &) = delete;
    ~QueryHandle() { ql_product_query_destroy(query_); }

    ql_product_query **output() { return &query_; }
    ql_product_query *get() const { return query_; }

private:
    ql_product_query *query_ = nullptr;
};

class WitnessHandle {
public:
    WitnessHandle() = default;
    WitnessHandle(const WitnessHandle &) = delete;
    WitnessHandle &operator=(const WitnessHandle &) = delete;
    ~WitnessHandle() { ql_replay_witness_destroy(witness_); }

    ql_replay_witness **output() { return &witness_; }
    ql_replay_witness *get() const { return witness_; }

private:
    ql_replay_witness *witness_ = nullptr;
};

class ArtifactHandle {
public:
    ArtifactHandle() = default;
    ArtifactHandle(const ArtifactHandle &) = delete;
    ArtifactHandle &operator=(const ArtifactHandle &) = delete;
    ~ArtifactHandle() { ql_artifact_release(artifact_); }

    ql_artifact **output() { return &artifact_; }
    ql_artifact *get() const { return artifact_; }
    void adopt(ql_artifact *artifact) {
        ql_artifact_release(artifact_);
        artifact_ = artifact;
    }

private:
    ql_artifact *artifact_ = nullptr;
};

std::string ArtifactText(const ql_artifact *artifact) {
    ql_artifact_view view{};
    ql_error error{};
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK, ql_artifact_get_view(artifact, &view, &error));
    return std::string(static_cast<const char *>(view.data), view.size);
}

/* Runs the violation query with a model request. Returns false when the
   pinned backend is unavailable so a caller skips rather than passes. */
bool SolveForModel(const ql_product_query *query, ArtifactHandle *model) {
    const ql_solver_descriptor_v1 *descriptor =
        ql_bitwuzla_solver_descriptor();
    ql_product_query_view_v1 view{};
    ql_solver *solver = nullptr;
    ql_solver_check_request_v1 request{};
    ql_solver_check_result_v1 result{};
    ql_error error{};
    bool solved = false;

    if (descriptor->capability.availability == QL_SOLVER_UNAVAILABLE ||
        ql_solver_create(nullptr, descriptor, nullptr, &solver, &error) !=
            QL_STATUS_OK) {
        return false;
    }
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK, ql_product_query_get_view(query, &view, &error));
    EXPECT_EQ(QL_STATUS_OK,
              ql_solver_add_smt2(solver,
                                 ql_product_query_prefix_artifact(query),
                                 &error))
        << error.message;
    EXPECT_EQ(QL_STATUS_OK,
              ql_solver_add_smt2(solver,
                                 ql_product_query_violation_artifact(query),
                                 &error));
    ql_solver_check_request_init(&request, view.logic);
    request.maximum_bv_width = view.maximum_bv_width;
    request.artifact_requests = QL_SOLVER_REQUEST_MODEL;
    ql_solver_check_result_init(&result);
    if (ql_solver_check(solver, &request, &result, &error) == QL_STATUS_OK &&
        result.kind == QL_SOLVER_CHECK_SAT &&
        result.model_artifact != nullptr) {
        ql_artifact_retain(result.model_artifact);
        model->adopt(result.model_artifact);
        solved = true;
    }
    ql_solver_check_result_clear(&result);
    ql_solver_destroy(solver);
    return solved;
}

void BuildQuery(w2::Pair *pair, QueryHandle *query, const char *left_source,
                const char *left_name, const char *right_source,
                const char *right_name,
                const ql_semantic_contract_v1 &contract) {
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK,
              pair->Build(left_source, left_name, right_source, right_name,
                          contract, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_product_query_build(nullptr, pair->problem(),
                                     pair->left_ir(), pair->right_ir(),
                                     query->output(), &error))
        << error.message;
}

ql_artifact *MakeModel(const std::string &text) {
    ql_artifact *artifact = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_SOLVER_MODEL, 1u,
                                 text.data(), text.size(), &artifact,
                                 &error));
    return artifact;
}

constexpr char kIdentity[] = "int f(int x){ return x; }";
constexpr char kSevenOnly[] =
    "int g(int x){ if (x == 7) return 7; return 0; }";

TEST(Replay, DecodesASolverModelAndConfirmsTheViolation) {
    w2::Pair pair;
    QueryHandle query;
    ArtifactHandle model;
    ArtifactHandle counterexample;
    WitnessHandle witness;
    ql_replay_result_v1 result{};
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(
        BuildQuery(&pair, &query, kIdentity, "f", kSevenOnly, "g",
                   w2::ContractObserving(QL_OBSERVE_RETURN_VALUE)));
    if (!SolveForModel(query.get(), &model)) {
        GTEST_SKIP() << "Bitwuzla backend is unavailable";
    }

    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_decode_model(nullptr, query.get(), model.get(),
                                     witness.output(), &error))
        << error.message;
    ASSERT_EQ(1u, ql_replay_witness_count(witness.get()));

    ql_replay_value_v1 value{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_witness_value_at(witness.get(), 0u, &value, &error));
    EXPECT_EQ(QL_SOURCE_TYPE_SIGNED_INTEGER, value.kind);
    EXPECT_EQ(32u, value.bit_width);
    EXPECT_EQ(4u, value.size);

    result.struct_size = sizeof(result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_execute(nullptr, pair.problem(), query.get(),
                                pair.left_ir(), pair.right_ir(),
                                witness.get(), &result, &error))
        << error.message;
    EXPECT_EQ(1u, result.conclusive);
    EXPECT_EQ(1u, result.precondition_evaluated);
    EXPECT_EQ(1u, result.precondition_holds);
    EXPECT_EQ(1u, result.violated);
    EXPECT_EQ(1u, result.left.defined);
    EXPECT_EQ(1u, result.right.defined);
    EXPECT_EQ(1u, result.left.returns);
    EXPECT_EQ(1u, result.right.returns);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_RETURN, result.left.interp_outcome);

    ql_artifact *artifact = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_counterexample_artifact_create(
                  nullptr, query.get(), witness.get(), &result, &artifact,
                  &error))
        << error.message;
    counterexample.adopt(artifact);
    const std::string text = ArtifactText(counterexample.get());
    EXPECT_NE(std::string::npos, text.find("quodlibet.counterexample"));
    EXPECT_NE(std::string::npos, text.find("\"replayed\":true"));
    EXPECT_NE(std::string::npos, text.find("\"return_value\""));
}

TEST(Replay, AModelThatDoesNotReproduceIsNotACounterexample) {
    /* The two functions differ only at x == 7. A model naming any other
       input satisfies nothing, and the replay must say so. */
    w2::Pair pair;
    QueryHandle query;
    ArtifactHandle model;
    WitnessHandle witness;
    ql_replay_result_v1 result{};
    ql_artifact *artifact = nullptr;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(
        BuildQuery(&pair, &query, kIdentity, "f", kSevenOnly, "g",
                   w2::ContractObserving(QL_OBSERVE_RETURN_VALUE)));
    model.adopt(MakeModel(
        "(\n  (define-fun in0 () (_ BitVec 32) "
        "#b00000000000000000000000000000000)\n)\n"));
    ASSERT_NE(nullptr, model.get());

    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_decode_model(nullptr, query.get(), model.get(),
                                     witness.output(), &error))
        << error.message;
    result.struct_size = sizeof(result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_execute(nullptr, pair.problem(), query.get(),
                                pair.left_ir(), pair.right_ir(),
                                witness.get(), &result, &error))
        << error.message;
    EXPECT_EQ(1u, result.conclusive);
    EXPECT_EQ(0u, result.violated);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_replay_counterexample_artifact_create(
                  nullptr, query.get(), witness.get(), &result, &artifact,
                  &error));
    EXPECT_EQ(nullptr, artifact);
}

TEST(Replay, ReplayingTheDistinguishingInputConfirmsTheViolation) {
    w2::Pair pair;
    QueryHandle query;
    ArtifactHandle model;
    WitnessHandle witness;
    ql_replay_result_v1 result{};
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(
        BuildQuery(&pair, &query, kIdentity, "f", kSevenOnly, "g",
                   w2::ContractObserving(QL_OBSERVE_RETURN_VALUE)));
    /* Seven is the one input on which the two agree, so it must NOT be a
       counterexample; eight is one on which they differ. */
    model.adopt(MakeModel(
        "((define-fun in0 () (_ BitVec 32) (_ bv7 32)))"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_decode_model(nullptr, query.get(), model.get(),
                                     witness.output(), &error))
        << error.message;
    result.struct_size = sizeof(result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_execute(nullptr, pair.problem(), query.get(),
                                pair.left_ir(), pair.right_ir(),
                                witness.get(), &result, &error));
    EXPECT_EQ(0u, result.violated);

    WitnessHandle other;
    ArtifactHandle other_model;
    other_model.adopt(MakeModel("((define-fun in0 () (_ BitVec 32) #x00000008))"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_decode_model(nullptr, query.get(), other_model.get(),
                                     other.output(), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_execute(nullptr, pair.problem(), query.get(),
                                pair.left_ir(), pair.right_ir(), other.get(),
                                &result, &error));
    EXPECT_EQ(1u, result.violated);
}

TEST(Replay, RejectsAModelThatOmitsOrMisstatesAnInput) {
    w2::Pair pair;
    QueryHandle query;
    ArtifactHandle empty_model;
    ArtifactHandle short_model;
    ql_replay_witness *witness = nullptr;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(
        BuildQuery(&pair, &query, "int f(int x, int y){ return x + y; }", "f",
                   "int g(int a, int b){ return b + a; }", "g",
                   w2::ContractObserving(QL_OBSERVE_RETURN_VALUE)));

    empty_model.adopt(MakeModel("()\n"));
    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_replay_decode_model(nullptr, query.get(), empty_model.get(),
                                     &witness, &error));
    EXPECT_EQ(nullptr, witness);

    /* A 32-bit input assigned an 8-bit literal is a decoder refusal, not a
       zero-extended guess. */
    short_model.adopt(MakeModel(
        "((define-fun in0 () (_ BitVec 32) #b00000001)"
        " (define-fun in1 () (_ BitVec 32) #b00000000000000000000000000000000))"));
    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_replay_decode_model(nullptr, query.get(), short_model.get(),
                                     &witness, &error));
    EXPECT_EQ(nullptr, witness);
}

TEST(Replay, AWitnessOutsideThePreconditionIsInconclusive) {
    constexpr char precondition[] =
        "{\"schema_version\":1,\"expression\":{\"op\":\"sgt\","
        "\"left\":{\"op\":\"arg\",\"index\":0},"
        "\"right\":{\"op\":\"int\",\"signed\":true,\"width\":32,"
        "\"value\":\"100\"}}}";
    w2::Pair pair;
    QueryHandle query;
    ArtifactHandle model;
    WitnessHandle witness;
    ql_replay_result_v1 result{};
    ql_semantic_contract_v1 contract =
        w2::ContractObserving(QL_OBSERVE_RETURN_VALUE);
    ql_error error{};

    contract.precondition_json = precondition;
    contract.precondition_json_size = sizeof(precondition) - 1u;
    ASSERT_NO_FATAL_FAILURE(BuildQuery(&pair, &query, kIdentity, "f",
                                       "int g(int x){ return 0; }", "g",
                                       contract));
    /* Zero breaks the two functions apart, but the problem never admitted
       it, so the replay must refuse to conclude. */
    model.adopt(MakeModel("((define-fun in0 () (_ BitVec 32) (_ bv0 32)))"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_decode_model(nullptr, query.get(), model.get(),
                                     witness.output(), &error))
        << error.message;
    result.struct_size = sizeof(result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_execute(nullptr, pair.problem(), query.get(),
                                pair.left_ir(), pair.right_ir(),
                                witness.get(), &result, &error));
    EXPECT_EQ(1u, result.precondition_evaluated);
    EXPECT_EQ(0u, result.precondition_holds);
    EXPECT_EQ(0u, result.conclusive);
    EXPECT_EQ(0u, result.violated);
}

TEST(Replay, UndefinedBehaviourOnOneSideIsReportedRatherThanCompared) {
    w2::Pair pair;
    QueryHandle query;
    ArtifactHandle model;
    WitnessHandle witness;
    ql_replay_result_v1 result{};
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(BuildQuery(
        &pair, &query, "int f(int x, int y){ return x / y; }", "f",
        "int g(int a, int b){ if (b == 0) return 0; return a / b; }", "g",
        w2::ContractObserving(QL_OBSERVE_RETURN_VALUE)));
    model.adopt(MakeModel(
        "((define-fun in0 () (_ BitVec 32) (_ bv4 32))"
        " (define-fun in1 () (_ BitVec 32) (_ bv0 32)))"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_decode_model(nullptr, query.get(), model.get(),
                                     witness.output(), &error))
        << error.message;
    result.struct_size = sizeof(result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_replay_execute(nullptr, pair.problem(), query.get(),
                                pair.left_ir(), pair.right_ir(),
                                witness.get(), &result, &error))
        << error.message;
    EXPECT_EQ(1u, result.conclusive);
    EXPECT_EQ(0u, result.left.defined);
    EXPECT_EQ(1u, result.right.defined);
    EXPECT_EQ(QL_IR_INTERP_OUTCOME_UNDEFINED_BEHAVIOR,
              result.left.interp_outcome);
    /* Under the default UB_MUST_MATCH the differing defined domains are the
       violation, and no return value is compared. */
    EXPECT_EQ(1u, result.violated);
}

}  // namespace
