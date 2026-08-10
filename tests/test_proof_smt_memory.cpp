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

/* A pointer loaded from memory can name storage beyond the source signature.
   The lowering appends one auxiliary object for the access, and the product
   must bind the matching late parameters on both sides to one shared region. */
TEST(SmtProductMemory, ProvesThroughAPointerLoadedFromMemory) {
    constexpr char left[] =
        "struct D { int value; }; struct H { struct D *next; };"
        " int f(struct H *p){ return p->next->value; }";
    constexpr char right[] =
        "struct D { int value; }; struct H { struct D *next; };"
        " int g(struct H *q){ struct D *r = q->next; return r[0].value; }";
    w2::Pair pair;
    OutcomeRun run;
    ql_product_query *query = nullptr;
    ql_error error{};

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(left, "f", right, "g",
                         w2::ContractObserving(MemoryObservations()), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_product_query_build(nullptr, pair.problem(), pair.left_ir(),
                                     pair.right_ir(), &query, &error))
        << error.message;
    EXPECT_EQ(2u, ql_product_query_object_count(query));
    ql_product_query_destroy(query);
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;
    const ql_smt_product_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict) << view.diagnostic;
}

/* A string literal is an object whose bytes the program states. The lowering
   states them as one MEMORY_IMAGE rather than a store per byte, so the miter
   has to turn that into the array theory's initial contents for the range:
   one equality per byte against `select` on the initial memory. If it did
   not, the object's bytes would be unconstrained and the solver could pick
   any of them. */
TEST(SmtProductMemory, AMemoryImageBecomesInitialArrayContents) {
    constexpr char left[] =
        "const char TXT_L[] = \"hi\";\n"
        "int f(int i){ return TXT_L[i]; }";
    constexpr char right[] =
        "const char TXT_R[] = \"hi\";\n"
        "int g(int j){ return TXT_R[j]; }";
    w2::Pair pair;
    ql_product_query *query = nullptr;
    ql_artifact_view prefix{};
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              pair.Build(left, "f", right, "g",
                         w2::ContractObserving(MemoryObservations()), &error))
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

    /* 'h', 'i', and the terminator, each pinned against a select on the
       initial memory rather than against a stored-into version. */
    EXPECT_NE(std::string::npos, text.find("(_ bv104 8)"));
    EXPECT_NE(std::string::npos, text.find("(_ bv105 8)"));
    EXPECT_NE(std::string::npos, text.find("(_ bv0 8)"));
    EXPECT_NE(std::string::npos, text.find("(select mem0 "));
    /* The image reaches the query as an assumption, so it is inside the
       assumption conjunction and not asserted unconditionally. */
    EXPECT_NE(std::string::npos, text.find("quodlibet_assumptions"));
    /* Nothing writes those bytes: with the image there is no store chain to
       build a new memory version from. */
    EXPECT_EQ(std::string::npos, text.find("(store mem0 "));
    ql_product_query_destroy(query);
}

/* Same pair, driven to a verdict: the two literals are the same bytes, so the
   values read out of them agree. This is what says the encoding is usable and
   not merely present. */
TEST(SmtProductMemory, ProvesTwoLiteralsWithTheSameBytesAgree) {
    constexpr char left[] =
        "const char TXT_A[] = \"hi\";\n"
        "int f(int i){ return TXT_A[i]; }";
    constexpr char right[] =
        "const char TXT_B[] = \"hi\";\n"
        "int g(int j){ return TXT_B[j]; }";
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
    const ql_smt_product_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
}

/* Two sides whose static data differs cannot both hold in this model: the
   miter gives corresponding objects one base in one shared array, so the two
   images contradict each other and the domain is empty.

   That is incomplete, not unsound, and the pipeline is what makes the
   difference: an empty domain is never promoted, so the pair comes back
   without a verdict instead of coming back proved. Making it decidable means
   giving the objects a lowering invents an identity per side, which is a
   change to what the miter says storage is, not to what an image says. */
TEST(SmtProductMemory, DifferentStaticBytesEmptyTheDomainRatherThanProving) {
    constexpr char left[] =
        "const char TXT_C[] = \"hi\";\n"
        "int f(int i){ return TXT_C[i]; }";
    constexpr char right[] =
        "const char TXT_D[] = \"ho\";\n"
        "int g(int j){ return TXT_D[j]; }";
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
    const ql_smt_product_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.domain_answer);
    /* An empty domain proves nothing, and the envelope says so rather than
       reading the vacuous UNSAT as agreement. */
    EXPECT_NE(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
}

/* A global is storage both sides share, so a pair over one reaches a verdict
   the same way a pointer argument does. Until the object table admitted
   objects a lowering makes for itself, this pair could not even be built. */
TEST(SmtProductMemory, ProvesAPairOverAGlobal) {
    w2::Pair pair;
    OutcomeRun run;
    ql_error error{};

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int GA; int f(int i){ GA = i + 1; return GA; }", "f",
                         "int GB; int g(int j){ GB = 1 + j; return GB; }", "g",
                         w2::ContractObserving(MemoryObservations()), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;
    const ql_smt_product_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
}

/* And a real difference over that global is found, so the pair above is not
   passing because the miter cannot see the storage at all. */
TEST(SmtProductMemory, AGlobalLeftHoldingADifferentValueIsACounterexample) {
    w2::Pair pair;
    OutcomeRun run;
    ql_error error{};

    if (!BackendAvailable()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    ASSERT_EQ(QL_STATUS_OK,
              pair.Build("int GC; int f(int i){ GC = i + 1; return i; }", "f",
                         "int GD; int g(int j){ GD = j + 2; return j; }", "g",
                         w2::ContractObserving(MemoryObservations()), &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK, run.Run(pair, kTrusted, &error)) << error.message;
    const ql_smt_product_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.violation_answer);
    EXPECT_NE(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
}

/* A local array is storage the lowering invents. Two spellings of the same
   writes agree, which needs the object table, the pinned size, and the access
   guards all to line up. */
TEST(SmtProductMemory, ProvesAPairOverALocalArray) {
    constexpr char left[] =
        "int f(int a){ int buf[4]; buf[0] = a; buf[1] = a + 1; "
        "return buf[0] + buf[1]; }";
    constexpr char right[] =
        "int g(int b){ int arr[4]; arr[1] = b + 1; arr[0] = b; "
        "return arr[1] + arr[0]; }";
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
    const ql_smt_product_outcome_view_v1 view = run.view();
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_UNSAT, view.violation_answer);
    EXPECT_EQ(QL_SMT_PRODUCT_ANSWER_SAT, view.domain_answer);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
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
