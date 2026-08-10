/* The miter over external calls.

   An external call is uninterpreted: nothing here says what the callee
   computes. What the encoding does say is that a callee is a function, so two
   calls handed the same history, the same memory, and the same arguments give
   back the same history, the same memory, and the same value. That single
   statement is what makes a pair of matching calls cancel, and refusing to
   state anything more is what keeps a pair of differing calls undecided
   rather than wrongly decided.

   Every case here drives the method to a verdict. A test that only checked
   that a query was built would not distinguish congruence from its absence.
*/

#include "quodlibet/proof_smt.h"

#include <cstddef>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "quodlibet/product.h"
#include "w2_fixtures.h"

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

/* Return value, termination, and traps, without observing the call order. */
std::uint64_t ValueObservations() {
    return QL_OBSERVE_RETURN_VALUE | QL_OBSERVE_TERMINATION |
           QL_OBSERVE_TRAPS;
}

/* The same, plus the order the external calls happened in. */
std::uint64_t OrderedCallObservations() {
    return ValueObservations() | QL_OBSERVE_EXTERNAL_CALLS;
}

ql_smt_product_outcome_view_v1 Decide(const char *left, const char *left_name,
                                      const char *right,
                                      const char *right_name,
                                      std::uint64_t observations,
                                      w2::Pair *pair, OutcomeRun *run) {
    ql_error error{};
    ql_smt_product_outcome_view_v1 empty{};
    if (pair->Build(left, left_name, right, right_name,
                    w2::ContractObserving(observations), &error) !=
        QL_STATUS_OK) {
        ADD_FAILURE() << "build: " << error.message;
        return empty;
    }
    if (run->Run(*pair, kTrusted, &error) != QL_STATUS_OK) {
        ADD_FAILURE() << "run: " << error.message;
        return empty;
    }
    const ql_smt_product_outcome_view_v1 view = run->view();
    if (view.violation_answer == 0) {
        /* No answer at all is a refusal, not a verdict, and a test that let
           one through would pass for the wrong reason. */
        ADD_FAILURE() << "the method reached no answer: " << view.diagnostic;
    }
    return view;
}

TEST(SmtProductCalls, MatchingCallsCancelAndThePairIsProved) {
    /* Same callee, same argument, both sides. Nothing says what CALLEE_f
       returns, so the only way these agree is congruence. */
    constexpr char left[] =
        "int CALLEE_f(int);\n"
        "int f(int a){ return CALLEE_f(a) + 1; }";
    constexpr char right[] =
        "int CALLEE_f(int);\n"
        "int g(int b){ return 1 + CALLEE_f(b); }";
    w2::Pair pair;
    OutcomeRun run;

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    const ql_smt_product_outcome_view_v1 view =
        Decide(left, "f", right, "g", ValueObservations(), &pair, &run);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
}

TEST(SmtProductCalls, TwoCallsInARowCancelPairwise) {
    /* The second call is handed the history and memory the first produced, so
       cancelling it needs the first to have cancelled already. */
    constexpr char left[] =
        "int CALLEE_f(int); int CALLEE_g(int);\n"
        "int f(int a){ int x = CALLEE_f(a); return CALLEE_g(x); }";
    constexpr char right[] =
        "int CALLEE_f(int); int CALLEE_g(int);\n"
        "int g(int b){ return CALLEE_g(CALLEE_f(b)); }";
    w2::Pair pair;
    OutcomeRun run;

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    const ql_smt_product_outcome_view_v1 view =
        Decide(left, "f", right, "g", ValueObservations(), &pair, &run);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
}

TEST(SmtProductCalls, MatchingPointerResultsNameTheSameDynamicObject) {
    constexpr char left[] =
        "struct D { int value; }; struct D *CALLEE_get(void);"
        " int f(void){ return CALLEE_get()->value; }";
    constexpr char right[] =
        "struct D { int value; }; struct D *CALLEE_get(void);"
        " int g(void){ struct D *p = CALLEE_get(); return p[0].value; }";
    w2::Pair pair;
    OutcomeRun run;

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    const ql_smt_product_outcome_view_v1 view =
        Decide(left, "f", right, "g", OrderedCallObservations(), &pair, &run);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict) << view.diagnostic;
}

TEST(SmtProductCalls, MatchingOutLocalResultsCancelAsOneCallTuple) {
    /* The return, the value written through the pointer, and the predicate
       saying whether that write happened are separate scalar CALL results.
       All of them must participate in congruence for the two reads to agree
       on both value and defined domain. */
    constexpr char left[] =
        "int CALLEE_out(int *, int);"
        " int f(int a){ int x; int r = CALLEE_out(&x, a); return x + r; }";
    constexpr char right[] =
        "int CALLEE_out(int *, int);"
        " int g(int b){ int x; int r = CALLEE_out(&x, b); return r + x; }";
    w2::Pair pair;
    OutcomeRun run;

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    const ql_smt_product_outcome_view_v1 view =
        Decide(left, "f", right, "g", OrderedCallObservations(), &pair, &run);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict) << view.diagnostic;
}

TEST(SmtProductCalls, MatchingRecordReturnImagesCancelAsOneCallTuple) {
    /* A record result is split into packed 64-bit CALL results and then
       materialised in each side's own temporary object. Congruence has to
       relate every chunk for differently ordered member reads to agree. */
    constexpr char left[] =
        "struct R { unsigned long long wide; int middle; unsigned char tag; };"
        " struct R CALLEE_make(int);"
        " int f(int a){ struct R r = CALLEE_make(a);"
        " return (int)(r.wide & 255u) + r.middle + r.tag; }";
    constexpr char right[] =
        "struct R { unsigned long long wide; int middle; unsigned char tag; };"
        " struct R CALLEE_make(int);"
        " int g(int b){ struct R r = CALLEE_make(b);"
        " return r.tag + r.middle + (int)(r.wide & 255u); }";
    w2::Pair pair;
    OutcomeRun run;

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    const ql_smt_product_outcome_view_v1 view =
        Decide(left, "f", right, "g", OrderedCallObservations(), &pair, &run);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict) << view.diagnostic;
}

TEST(SmtProductCalls, VariadicArgumentsUseDefaultPromotions) {
    constexpr char left[] =
        "int CALLEE_v(int, ...);"
        " int f(short a){ return CALLEE_v(7, a); }";
    constexpr char right[] =
        "int CALLEE_v(int, ...);"
        " int g(short b){ int promoted = b; return CALLEE_v(7, promoted); }";
    w2::Pair pair;
    OutcomeRun run;

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    const ql_smt_product_outcome_view_v1 view =
        Decide(left, "f", right, "g", OrderedCallObservations(), &pair, &run);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict) << view.diagnostic;
}

TEST(SmtProductCalls, MatchingRecordArgumentsCancelAsPackedValues) {
    constexpr char left[] =
        "struct R { int x; short y; }; int CALLEE_r(struct R);"
        " int f(int a){ struct R value = {a, (short)(a + 1)};"
        " return CALLEE_r(value); }";
    constexpr char right[] =
        "struct R { int x; short y; }; int CALLEE_r(struct R);"
        " int g(int b){ struct R value = {b, (short)(b + 1)};"
        " return CALLEE_r(value); }";
    w2::Pair pair;
    OutcomeRun run;

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    const ql_smt_product_outcome_view_v1 view =
        Decide(left, "f", right, "g", OrderedCallObservations(), &pair, &run);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict) << view.diagnostic;
}

TEST(SmtProductCalls, ADifferentCalleeDoesNotCancel) {
    /* Two different callees are two different functions, and nothing says
       they agree anywhere. The miter must not decide this. */
    constexpr char left[] =
        "int CALLEE_f(int);\n"
        "int f(int a){ return CALLEE_f(a); }";
    constexpr char right[] =
        "int CALLEE_h(int);\n"
        "int g(int b){ return CALLEE_h(b); }";
    w2::Pair pair;
    OutcomeRun run;

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    const ql_smt_product_outcome_view_v1 view =
        Decide(left, "f", right, "g", ValueObservations(), &pair, &run);
    EXPECT_NE(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
}

TEST(SmtProductCalls, ADifferentArgumentDoesNotCancel) {
    /* Same callee, different argument. Congruence has nothing to say, so the
       two results are free to differ and the pair stays undecided. */
    constexpr char left[] =
        "int CALLEE_f(int);\n"
        "int f(int a){ return CALLEE_f(a); }";
    constexpr char right[] =
        "int CALLEE_f(int);\n"
        "int g(int b){ return CALLEE_f(b + 1); }";
    w2::Pair pair;
    OutcomeRun run;

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    const ql_smt_product_outcome_view_v1 view =
        Decide(left, "f", right, "g", ValueObservations(), &pair, &run);
    EXPECT_NE(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
}

TEST(SmtProductCalls, ObservingTheCallOrderIsWhatSeparatesThesePairs) {
    /* Both sides call the same two callees with the same arguments and return
       the same value; only the order differs. With the call order observed
       the two histories are different and the pair is not proved. With it
       ignored the returned value is all there is to compare, and the pair is
       proved. The two runs differ in the contract alone. */
    constexpr char left[] =
        "int CALLEE_f(int); int CALLEE_g(int);\n"
        "int f(int a){ CALLEE_f(a); CALLEE_g(a); return a; }";
    constexpr char right[] =
        "int CALLEE_f(int); int CALLEE_g(int);\n"
        "int g(int b){ CALLEE_g(b); CALLEE_f(b); return b; }";

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    {
        w2::Pair pair;
        OutcomeRun run;
        const ql_smt_product_outcome_view_v1 view =
            Decide(left, "f", right, "g", ValueObservations(), &pair, &run);
        EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
        EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict)
            << "with the call order unobserved the values are all that differ";
    }
    {
        w2::Pair pair;
        OutcomeRun run;
        const ql_smt_product_outcome_view_v1 view =
            Decide(left, "f", right, "g", OrderedCallObservations(), &pair,
                   &run);
        EXPECT_NE(QL_VERDICT_PROVED_EQUIVALENT, view.verdict)
            << "with the call order observed the two histories differ";
    }
}

TEST(SmtProductCalls, ObservingTheCallOrderStillProvesTheSameOrder) {
    /* The order axis is not simply refusing everything: two spellings of the
       same call sequence still agree with the order observed. */
    constexpr char left[] =
        "int CALLEE_f(int); int CALLEE_g(int);\n"
        "int f(int a){ CALLEE_f(a); CALLEE_g(a); return a; }";
    constexpr char right[] =
        "int CALLEE_f(int); int CALLEE_g(int);\n"
        "int g(int b){ int t = b; CALLEE_f(t); CALLEE_g(t); return t; }";
    w2::Pair pair;
    OutcomeRun run;

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    const ql_smt_product_outcome_view_v1 view =
        Decide(left, "f", right, "g", OrderedCallObservations(), &pair, &run);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
}

TEST(SmtProductCalls, TheQueryDeclaresCallResultsRatherThanDefiningThem) {
    constexpr char left[] =
        "int CALLEE_f(int);\n"
        "int f(int a){ return CALLEE_f(a); }";
    w2::Pair pair;
    ql_product_query *query = nullptr;
    ql_artifact_view prefix{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(left, "f", left, "f",
                         w2::ContractObserving(OrderedCallObservations()),
                         &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_product_query_build(nullptr, pair.problem(), pair.left_ir(),
                                     pair.right_ir(), &query, &error))
        << error.message;
    prefix.struct_size = sizeof(prefix);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(ql_product_query_prefix_artifact(query),
                                   &prefix, &error));
    const std::string text(static_cast<const char *>(prefix.data),
                           prefix.size);
    /* One history both sides start from, and the results left free. */
    EXPECT_NE(std::string::npos, text.find("(declare-const trace0 "));
    EXPECT_NE(std::string::npos, text.find("l_final_trace"));
    EXPECT_NE(std::string::npos, text.find("r_final_trace"));
    /* Congruence reaches the query as an assumption about the environment,
       not as a claim about either program. */
    EXPECT_NE(std::string::npos, text.find("quodlibet_assumptions"));
    ql_product_query_destroy(query);
}

}  // namespace
