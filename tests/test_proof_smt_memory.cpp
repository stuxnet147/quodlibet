#include "quodlibet/proof_smt.h"

#include <cstddef>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "quodlibet/product.h"
#include "w2_fixtures.h"

/* The miter over the flat memory model. Every case here states two C functions
   that take pointers, so the query has to carry the object table, the model's
   standing assumptions, the access guards, and the final-memory observation.
   A case that only checked that a query was built would say nothing: each test
   drives the method end to end and asserts the verdict. */

namespace {

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

    ql_status Run(const w2::Pair &pair, const char *options_json,
                  ql_error *error) {
        ql_run_context_v1 context{};
        ql_artifact *input = pair.artifact();
        ql_status status = ql_smt_product_method()->create(
            ql_default_host(), options_json, &instance_, error);
        if (status != QL_STATUS_OK) {
            return status;
        }
        context.struct_size = sizeof(context);
        context.abi_version = QL_ABI_VERSION;
        context.host = ql_default_host();
        return ql_smt_product_method()->run(instance_, &context, &input, 1u,
                                            &outcome_, error);
    }

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

constexpr char kTrusted[] = "{\"unsat_promotion\":\"trusted-backend\"}";

/* Return value, memory, termination, and traps together: a pointer function
   that writes has to agree on what it left behind as well as what it gave
   back. */
std::uint64_t MemoryObservations() {
    return QL_OBSERVE_RETURN_VALUE | QL_OBSERVE_MEMORY |
           QL_OBSERVE_TERMINATION | QL_OBSERVE_TRAPS;
}

TEST(SmtProductMemory, BuildsAnArrayQueryWithSharedObjects) {
    w2::Pair pair;
    ql_product_query *query = nullptr;
    ql_product_query_view_v1 view{};
    ql_product_object_v1 object{};
    ql_artifact_view prefix{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int f(int *p){ return *p; }", "f",
                         "int g(int *q){ return q[0]; }", "g",
                         w2::ContractObserving(MemoryObservations()), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_product_query_build(nullptr, pair.problem(), pair.left_ir(),
                                     pair.right_ir(), &query, &error))
        << error.message;
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_product_query_get_view(query, &view, &error));
    EXPECT_EQ(QL_SOLVER_LOGIC_QF_ABV, view.logic);
    EXPECT_EQ(MemoryObservations(), view.covered_observations);
    EXPECT_LE(64u, view.maximum_bv_width);

    ASSERT_EQ(1u, ql_product_query_object_count(query));
    ASSERT_EQ(QL_STATUS_OK,
              ql_product_query_object_at(query, 0u, &object, &error));
    EXPECT_STREQ("obj0_base", object.base_symbol);
    EXPECT_STREQ("obj0_size", object.size_symbol);
    /* One C argument, then the memory value, then this object's base. */
    EXPECT_EQ(2u, object.left_base_parameter);
    EXPECT_EQ(2u, object.right_base_parameter);
    EXPECT_STREQ("mem0", ql_product_query_memory_symbol(query));

    prefix.struct_size = sizeof(prefix);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(ql_product_query_prefix_artifact(query),
                                   &prefix, &error));
    const std::string text(static_cast<const char *>(prefix.data),
                           prefix.size);
    EXPECT_NE(std::string::npos, text.find("(set-logic QF_ABV)"));
    EXPECT_NE(std::string::npos,
              text.find("(declare-const mem0 (Array (_ BitVec 64) "
                        "(_ BitVec 8)))"));
    EXPECT_NE(std::string::npos, text.find("(select "));
    /* The model's standing constraints reach the query through the IR's own
       ASSUME instructions rather than being restated by the encoder. */
    EXPECT_NE(std::string::npos, text.find("quodlibet_assumptions"));
    ql_product_query_destroy(query);
}

TEST(SmtProductMemory, ProvesTwoSpellingsOfTheSameWrite) {
    w2::Pair pair;
    OutcomeRun run;
    ql_error error{};

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int f(int *p){ *p = *p + 1; return *p; }", "f",
                         "int g(int *q){ q[0] = q[0] + 1; return q[0]; }", "g",
                         w2::ContractObserving(MemoryObservations()), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

    const ql_smt_product_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
    EXPECT_EQ(QL_EVIDENCE_PROOF, view.evidence_class);
    /* Bitwuzla exposes no certificate, and the envelope keeps saying so. */
    EXPECT_EQ(0u, view.checked_proof);
}

TEST(SmtProductMemory, ProvesAcrossAStructMemberSpelling) {
    constexpr char left[] =
        "struct S { int a; int b; };"
        " int f(struct S *p){ p->b = p->a; return p->b; }";
    constexpr char right[] =
        "struct S { int a; int b; };"
        " int g(struct S *q){ int v = q->a; q->b = v; return v; }";
    w2::Pair pair;
    OutcomeRun run;
    ql_error error{};

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(left, "f", right, "g",
                         w2::ContractObserving(MemoryObservations()), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, run.view().verdict);
}

TEST(SmtProductMemory, AWriteOfADifferentValueIsAReplayedCounterexample) {
    w2::Pair pair;
    OutcomeRun run;
    ql_error error{};

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    /* The two write the same bytes of the same object and return the same
       value, so the only thing that separates them is what those bytes hold
       afterwards. The counterexample exists because memory is observed and
       for no other reason, which the companion case below fixes. */
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int f(int *p){ p[0] = 1; return 0; }", "f",
                         "int g(int *q){ q[0] = 2; return 0; }", "g",
                         w2::ContractObserving(MemoryObservations()), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

    const ql_smt_product_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.violation_answer);
    EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, view.verdict);
    EXPECT_EQ(QL_EVIDENCE_COUNTEREXAMPLE, view.evidence_class);
    /* A model that no concrete run reproduces is never a counterexample. */
    EXPECT_EQ(1u, view.replay_confirmed);
}

TEST(SmtProductMemory, IgnoringMemoryLeavesTheStoredValueDifferenceUnseen) {
    w2::Pair pair;
    OutcomeRun run;
    ql_error error{};

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    /* The identical pair with memory off. The verdict follows the contract's
       observation set, so this fixes that the memory axis, and not some other
       difference, decided the case above. */
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int f(int *p){ p[0] = 1; return 0; }", "f",
                         "int g(int *q){ q[0] = 2; return 0; }", "g",
                         w2::ContractObserving(QL_OBSERVE_RETURN_VALUE),
                         &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, run.view().verdict)
        << run.view().diagnostic;
}

TEST(SmtProductMemory, ADifferentReturnedByteIsACounterexample) {
    w2::Pair pair;
    OutcomeRun run;
    ql_error error{};

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int f(int *p){ return p[0]; }", "f",
                         "int g(int *q){ return q[1]; }", "g",
                         w2::ContractObserving(MemoryObservations()), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;
    EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, run.view().verdict)
        << run.view().diagnostic;
    EXPECT_EQ(1u, run.view().replay_confirmed);
}

TEST(SmtProductMemory, RefusesAMemoryObservationFinerThanTheFinalState) {
    w2::Pair pair;
    OutcomeRun run;
    ql_semantic_contract_v1 contract =
        w2::ContractObserving(MemoryObservations());
    ql_error error{};

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    contract.memory_observation = QL_MEMORY_ORDERED_WRITES;
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int f(int *p){ p[0] = 1; return 0; }", "f",
                         "int g(int *q){ q[0] = 1; return 0; }", "g",
                         contract, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;

    const ql_smt_product_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_NOT_QUERIED, view.violation_answer);
    EXPECT_NE(nullptr, std::strstr(view.diagnostic, "final reachable state"));
}

}  // namespace
