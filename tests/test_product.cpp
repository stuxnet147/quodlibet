#include "quodlibet/product.h"

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

std::string ArtifactText(const ql_artifact *artifact) {
    ql_artifact_view view{};
    ql_error error{};
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK, ql_artifact_get_view(artifact, &view, &error));
    return std::string(static_cast<const char *>(view.data), view.size);
}

/* Runs one terminal query against the pinned Bitwuzla backend. Returns
   QL_SOLVER_CHECK_INVALID when the backend is unavailable so a caller can
   skip instead of reporting a pass it never obtained. */
ql_solver_check_kind Solve(const ql_product_query *query,
                           const ql_artifact *terminal,
                           std::string *model_out) {
    const ql_solver_descriptor_v1 *descriptor =
        ql_bitwuzla_solver_descriptor();
    ql_product_query_view_v1 view{};
    ql_solver *solver = nullptr;
    ql_solver_check_request_v1 request{};
    ql_solver_check_result_v1 result{};
    ql_error error{};
    ql_solver_check_kind kind = QL_SOLVER_CHECK_INVALID;

    if (descriptor->capability.availability == QL_SOLVER_UNAVAILABLE) {
        return QL_SOLVER_CHECK_INVALID;
    }
    if (ql_solver_create(nullptr, descriptor, nullptr, &solver, &error) !=
        QL_STATUS_OK) {
        return QL_SOLVER_CHECK_INVALID;
    }
    view.struct_size = sizeof(view);
    EXPECT_EQ(QL_STATUS_OK,
              ql_product_query_get_view(query, &view, &error));
    EXPECT_EQ(QL_STATUS_OK,
              ql_solver_add_smt2(solver,
                                 ql_product_query_prefix_artifact(query),
                                 &error))
        << error.message;
    EXPECT_EQ(QL_STATUS_OK, ql_solver_add_smt2(solver, terminal, &error))
        << error.message;
    ql_solver_check_request_init(&request, view.logic);
    request.maximum_bv_width = view.maximum_bv_width;
    if (model_out != nullptr) {
        request.artifact_requests = QL_SOLVER_REQUEST_MODEL;
    }
    ql_solver_check_result_init(&result);
    if (ql_solver_check(solver, &request, &result, &error) == QL_STATUS_OK) {
        kind = result.kind;
        if (model_out != nullptr && result.model_artifact != nullptr) {
            *model_out = ArtifactText(result.model_artifact);
        }
    } else {
        ADD_FAILURE() << error.message;
    }
    ql_solver_check_result_clear(&result);
    ql_solver_destroy(solver);
    return kind;
}

ql_solver_check_kind SolveViolation(const ql_product_query *query) {
    return Solve(query, ql_product_query_violation_artifact(query), nullptr);
}

ql_solver_check_kind SolveDomain(const ql_product_query *query) {
    return Solve(query, ql_product_query_domain_artifact(query), nullptr);
}

/* Builds a miter or fails the calling test with the reason. */
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

constexpr char kAdd[] = "int add(int x, int y){ return x + y; }";
constexpr char kSum[] = "int sum(int a, int b){ return b + a; }";

TEST(ProductMiter, EmitsOneSharedInputPerCorrespondingArgument) {
    w2::Pair pair;
    QueryHandle query;
    ql_product_query_view_v1 view{};
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(BuildQuery(&pair, &query, kAdd, "add", kSum,
                                       "sum", w2::DefaultContract()));
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_product_query_get_view(query.get(), &view, &error));
    EXPECT_EQ(QL_PRODUCT_SCHEMA_VERSION, view.schema_version);
    EXPECT_EQ(QL_RELATION_EQUIVALENCE, view.relation);
    EXPECT_EQ(QL_UB_MUST_MATCH, view.ub_policy);
    EXPECT_EQ(QL_OBSERVE_ALL, view.covered_observations);
    EXPECT_EQ(QL_SOLVER_LOGIC_QF_BV, view.logic);
    EXPECT_EQ(2u, view.input_count);
    EXPECT_EQ(QL_IR_TYPE_BIT_VECTOR, view.return_type_kind);
    EXPECT_EQ(32u, view.return_bit_width);

    ql_product_input_v1 input{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_product_query_input_at(query.get(), 1u, &input, &error));
    EXPECT_EQ(1u, input.left_parameter);
    EXPECT_EQ(1u, input.right_parameter);
    EXPECT_EQ(QL_SOURCE_TYPE_SIGNED_INTEGER, input.kind);
    EXPECT_EQ(32u, input.bit_width);
    EXPECT_STREQ("in1", input.symbol);
    EXPECT_EQ(QL_STATUS_NOT_FOUND,
              ql_product_query_input_at(query.get(), 2u, &input, &error));

    const std::string prefix =
        ArtifactText(ql_product_query_prefix_artifact(query.get()));
    EXPECT_NE(std::string::npos, prefix.find("(set-logic QF_BV)"));
    EXPECT_NE(std::string::npos,
              prefix.find("(declare-const in0 (_ BitVec 32))"));
    EXPECT_NE(std::string::npos,
              prefix.find("(define-fun quodlibet_violation () Bool"));
    /* The terminal queries stay separate so one shared prefix serves both. */
    EXPECT_EQ(std::string::npos, prefix.find("(assert"));
    EXPECT_EQ("(assert quodlibet_violation)\n",
              ArtifactText(ql_product_query_violation_artifact(query.get())));
    EXPECT_EQ("(assert quodlibet_domain)\n",
              ArtifactText(ql_product_query_domain_artifact(query.get())));
}

TEST(ProductMiter, CommutedAdditionHasNoViolationAndANonEmptyDomain) {
    w2::Pair pair;
    QueryHandle query;

    ASSERT_NO_FATAL_FAILURE(BuildQuery(&pair, &query, kAdd, "add", kSum,
                                       "sum", w2::DefaultContract()));
    const ql_solver_check_kind violation = SolveViolation(query.get());
    if (violation == QL_SOLVER_CHECK_INVALID) {
        GTEST_SKIP() << "Bitwuzla backend is unavailable";
    }
    EXPECT_EQ(QL_SOLVER_CHECK_UNSAT, violation);
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, SolveDomain(query.get()));
}

TEST(ProductMiter, DifferentReturnValuesProduceASatisfiableViolation) {
    w2::Pair pair;
    QueryHandle query;
    std::string model;

    ASSERT_NO_FATAL_FAILURE(
        BuildQuery(&pair, &query, kAdd, "add",
                   "int sum(int a, int b){ return b - a; }", "sum",
                   w2::DefaultContract()));
    const ql_solver_check_kind kind =
        Solve(query.get(), ql_product_query_violation_artifact(query.get()),
              &model);
    if (kind == QL_SOLVER_CHECK_INVALID) {
        GTEST_SKIP() << "Bitwuzla backend is unavailable";
    }
    ASSERT_EQ(QL_SOLVER_CHECK_SAT, kind);
    EXPECT_NE(std::string::npos, model.find("in0"));
    EXPECT_NE(std::string::npos, model.find("in1"));
}

/* --- Observation axes, pinned one at a time ------------------------------- */

TEST(ProductMiter, ReturnValueAxisAloneDecidesAValueDifference) {
    w2::Pair without;
    w2::Pair with;
    QueryHandle without_query;
    QueryHandle with_query;
    /* Neither side can overflow, so the two defined domains are identical and
       only the return-value axis can separate them. */
    constexpr char left[] = "int f(int x){ return x; }";
    constexpr char right[] = "int g(int x){ return 0; }";

    ASSERT_NO_FATAL_FAILURE(
        BuildQuery(&without, &without_query, left, "f", right, "g",
                   w2::ContractObserving(QL_OBSERVE_TERMINATION)));
    ASSERT_NO_FATAL_FAILURE(
        BuildQuery(&with, &with_query, left, "f", right, "g",
                   w2::ContractObserving(QL_OBSERVE_TERMINATION |
                                         QL_OBSERVE_RETURN_VALUE)));

    const ql_solver_check_kind off = SolveViolation(without_query.get());
    if (off == QL_SOLVER_CHECK_INVALID) {
        GTEST_SKIP() << "Bitwuzla backend is unavailable";
    }
    EXPECT_EQ(QL_SOLVER_CHECK_UNSAT, off);
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, SolveViolation(with_query.get()));
}

TEST(ProductMiter, UndefinedBehaviourDomainsSeparateTheTwoRefinementDirections) {
    /* The left function divides without guarding the zero divisor, so it is
       undefined exactly where the right one returns zero. */
    constexpr char left[] = "int f(int x, int y){ return x / y; }";
    constexpr char right[] =
        "int g(int a, int b){ if (b == 0) return 0; return a / b; }";
    w2::Pair must_match;
    w2::Pair left_refines;
    w2::Pair right_refines;
    QueryHandle must_match_query;
    QueryHandle left_refines_query;
    QueryHandle right_refines_query;

    ql_semantic_contract_v1 contract =
        w2::ContractObserving(QL_OBSERVE_RETURN_VALUE);
    ASSERT_NO_FATAL_FAILURE(BuildQuery(&must_match, &must_match_query, left,
                                       "f", right, "g", contract));

    contract.ub_policy = QL_UB_LANGUAGE_REFINEMENT;
    contract.relation = QL_RELATION_RIGHT_REFINES_LEFT;
    ASSERT_NO_FATAL_FAILURE(BuildQuery(&right_refines, &right_refines_query,
                                       left, "f", right, "g", contract));

    contract.relation = QL_RELATION_LEFT_REFINES_RIGHT;
    ASSERT_NO_FATAL_FAILURE(BuildQuery(&left_refines, &left_refines_query,
                                       left, "f", right, "g", contract));

    const ql_solver_check_kind strict = SolveViolation(must_match_query.get());
    if (strict == QL_SOLVER_CHECK_INVALID) {
        GTEST_SKIP() << "Bitwuzla backend is unavailable";
    }
    /* Equal defined domains are required, and they differ at y == 0. */
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, strict);
    /* Beh(right) subseteq Beh(left): where the left side is defined the two
       agree, and the left side's UB at y == 0 permits anything. */
    EXPECT_EQ(QL_SOLVER_CHECK_UNSAT,
              SolveViolation(right_refines_query.get()));
    /* Beh(left) subseteq Beh(right) fails: the right side is defined at
       y == 0 where the left side is not. */
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, SolveViolation(left_refines_query.get()));
}

TEST(ProductMiter, ComparingOnlyWhereBothAreDefinedAcceptsTheDivisionPair) {
    constexpr char left[] = "int f(int x, int y){ return x / y; }";
    constexpr char right[] =
        "int g(int a, int b){ if (b == 0) return 0; return a / b; }";
    w2::Pair pair;
    QueryHandle query;
    ql_semantic_contract_v1 contract =
        w2::ContractObserving(QL_OBSERVE_RETURN_VALUE);

    contract.ub_policy = QL_UB_COMPARE_WHERE_BOTH_DEFINED;
    ASSERT_NO_FATAL_FAILURE(
        BuildQuery(&pair, &query, left, "f", right, "g", contract));
    const ql_solver_check_kind kind = SolveViolation(query.get());
    if (kind == QL_SOLVER_CHECK_INVALID) {
        GTEST_SKIP() << "Bitwuzla backend is unavailable";
    }
    EXPECT_EQ(QL_SOLVER_CHECK_UNSAT, kind);
    /* The intersection is inhabited, so the UNSAT above is not vacuous. */
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, SolveDomain(query.get()));
}

TEST(ProductMiter, AnUnsatisfiablePreconditionLeavesAnEmptyDomain) {
    constexpr char precondition[] =
        "{\"schema_version\":1,\"expression\":{\"op\":\"and\",\"args\":["
        "{\"op\":\"slt\",\"left\":{\"op\":\"arg\",\"index\":0},"
        "\"right\":{\"op\":\"int\",\"signed\":true,\"width\":32,"
        "\"value\":\"0\"}},"
        "{\"op\":\"sgt\",\"left\":{\"op\":\"arg\",\"index\":0},"
        "\"right\":{\"op\":\"int\",\"signed\":true,\"width\":32,"
        "\"value\":\"0\"}}]}}";
    w2::Pair pair;
    QueryHandle query;
    ql_semantic_contract_v1 contract = w2::DefaultContract();

    contract.precondition_json = precondition;
    contract.precondition_json_size = sizeof(precondition) - 1u;
    ASSERT_NO_FATAL_FAILURE(
        BuildQuery(&pair, &query, "int f(int x){ return x; }", "f",
                   "int g(int x){ return x + 1; }", "g", contract));

    const ql_solver_check_kind domain = SolveDomain(query.get());
    if (domain == QL_SOLVER_CHECK_INVALID) {
        GTEST_SKIP() << "Bitwuzla backend is unavailable";
    }
    /* No input satisfies the precondition, so the miter is trivially UNSAT
       and says nothing about the two functions. */
    EXPECT_EQ(QL_SOLVER_CHECK_UNSAT, domain);
    EXPECT_EQ(QL_SOLVER_CHECK_UNSAT, SolveViolation(query.get()));
}

TEST(ProductMiter, PreconditionRestrictsTheCounterexampleSearch) {
    constexpr char precondition[] =
        "{\"schema_version\":1,\"expression\":{\"op\":\"eq\","
        "\"left\":{\"op\":\"arg\",\"index\":0},"
        "\"right\":{\"op\":\"int\",\"signed\":true,\"width\":32,"
        "\"value\":\"7\"}}}";
    w2::Pair restricted;
    w2::Pair unrestricted;
    QueryHandle restricted_query;
    QueryHandle unrestricted_query;
    constexpr char left[] = "int f(int x){ return x; }";
    constexpr char right[] = "int g(int x){ if (x == 7) return 7; return 0; }";
    ql_semantic_contract_v1 contract =
        w2::ContractObserving(QL_OBSERVE_RETURN_VALUE);

    ASSERT_NO_FATAL_FAILURE(BuildQuery(&unrestricted, &unrestricted_query,
                                       left, "f", right, "g", contract));
    contract.precondition_json = precondition;
    contract.precondition_json_size = sizeof(precondition) - 1u;
    ASSERT_NO_FATAL_FAILURE(BuildQuery(&restricted, &restricted_query, left,
                                       "f", right, "g", contract));

    const ql_solver_check_kind open_kind =
        SolveViolation(unrestricted_query.get());
    if (open_kind == QL_SOLVER_CHECK_INVALID) {
        GTEST_SKIP() << "Bitwuzla backend is unavailable";
    }
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, open_kind);
    EXPECT_EQ(QL_SOLVER_CHECK_UNSAT, SolveViolation(restricted_query.get()));
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, SolveDomain(restricted_query.get()));
}

/* The restricted-C slice cannot emit a trap or a divergence, so these two
   axes are pinned against hand-built IR. Without them a miter could drop the
   axis and still look correct on every C source the frontend accepts. */

class NullaryFunction {
public:
    NullaryFunction() = default;
    NullaryFunction(const NullaryFunction &) = delete;
    NullaryFunction &operator=(const NullaryFunction &) = delete;

    ~NullaryFunction() {
        ql_ir_release(ir_);
        ql_artifact_release(ir_artifact_);
        ql_artifact_release(signature_);
    }

    /* `int name(void)` whose entry block uses the requested terminator. */
    void Build(const char *name, ql_ir_terminator_kind kind,
               std::uint64_t code) {
        ql_source_signature_definition_v1 signature{};
        ql_ir_builder *builder = nullptr;
        ql_ir_type_definition_v1 type{};
        ql_ir_terminator_definition_v1 terminator{};
        ql_ir_type_id bv32 = QL_IR_INVALID_TYPE_ID;
        ql_ir_block_id entry = QL_IR_INVALID_BLOCK_ID;
        ql_error error{};

        ql_source_signature_definition_init(&signature);
        ql_source_type_init(&signature.return_type,
                            QL_SOURCE_TYPE_SIGNED_INTEGER);
        signature.return_type.bit_width = 32u;
        signature.function_name = name;
        signature.function_name_size = std::strlen(name);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_source_signature_artifact_create(nullptr, &signature,
                                                      &signature_, &error))
            << error.message;

        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_builder_create(nullptr, &builder, &error));
        ql_ir_type_definition_init(&type, QL_IR_TYPE_BIT_VECTOR);
        type.bit_width = 32u;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_builder_add_type(builder, &type, &bv32, &error));
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_builder_set_function(builder, name,
                                             std::strlen(name), bv32,
                                             &error));
        ASSERT_EQ(QL_STATUS_OK, ql_ir_builder_add_block(builder, "entry", 5u,
                                                        &entry, &error));
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_builder_set_entry_block(builder, entry, &error));
        ql_ir_terminator_definition_init(&terminator, kind);
        terminator.code = code;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_builder_set_terminator(builder, entry, &terminator,
                                               &error))
            << error.message;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_builder_finish(builder, &ir_artifact_, &error))
            << error.message;
        ql_ir_builder_destroy(builder);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_ir_open(nullptr, ir_artifact_, &ir_, &error))
            << error.message;
    }

    ql_artifact *signature() const { return signature_; }
    ql_ir *ir() const { return ir_; }

private:
    ql_artifact *signature_ = nullptr;
    ql_artifact *ir_artifact_ = nullptr;
    ql_ir *ir_ = nullptr;
};

class SyntheticPair {
public:
    SyntheticPair() = default;
    SyntheticPair(const SyntheticPair &) = delete;
    SyntheticPair &operator=(const SyntheticPair &) = delete;

    ~SyntheticPair() {
        ql_product_query_destroy(query_);
        ql_problem_release(problem_);
        ql_artifact_release(artifact_);
    }

    void Build(const NullaryFunction &left, const char *left_name,
               const NullaryFunction &right, const char *right_name,
               const ql_semantic_contract_v1 &contract) {
        static constexpr char placeholder[] = "int placeholder(void){return 0;}";
        ql_problem_definition_v2 definition{};
        ql_error error{};

        ql_problem_definition_v2_init(&definition);
        definition.contract = contract;
        definition.left_source = placeholder;
        definition.left_source_size = sizeof(placeholder) - 1u;
        definition.left_function_name = left_name;
        definition.left_function_name_size = std::strlen(left_name);
        definition.right_source = placeholder;
        definition.right_source_size = sizeof(placeholder) - 1u;
        definition.right_function_name = right_name;
        definition.right_function_name_size = std::strlen(right_name);
        definition.left_signature = left.signature();
        definition.right_signature = right.signature();
        ASSERT_EQ(QL_STATUS_OK,
                  ql_problem_artifact_create_v2(nullptr, &definition,
                                                &artifact_, &error))
            << error.message;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_problem_open(nullptr, artifact_, &problem_, &error))
            << error.message;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_product_query_build(nullptr, problem_, left.ir(),
                                         right.ir(), &query_, &error))
            << error.message;
    }

    ql_product_query *query() const { return query_; }

private:
    ql_artifact *artifact_ = nullptr;
    ql_problem *problem_ = nullptr;
    ql_product_query *query_ = nullptr;
};

TEST(ProductMiter, TrapAxisAloneDecidesADifferentTrapCode) {
    NullaryFunction left;
    NullaryFunction right;
    SyntheticPair without;
    SyntheticPair with;

    ASSERT_NO_FATAL_FAILURE(
        left.Build("trap_one", QL_IR_TERMINATOR_TRAP, 1u));
    ASSERT_NO_FATAL_FAILURE(
        right.Build("trap_two", QL_IR_TERMINATOR_TRAP, 2u));
    ASSERT_NO_FATAL_FAILURE(without.Build(
        left, "trap_one", right, "trap_two",
        w2::ContractObserving(QL_OBSERVE_RETURN_VALUE |
                              QL_OBSERVE_TERMINATION)));
    ASSERT_NO_FATAL_FAILURE(with.Build(
        left, "trap_one", right, "trap_two",
        w2::ContractObserving(QL_OBSERVE_RETURN_VALUE |
                              QL_OBSERVE_TERMINATION | QL_OBSERVE_TRAPS)));

    const ql_solver_check_kind off = SolveViolation(without.query());
    if (off == QL_SOLVER_CHECK_INVALID) {
        GTEST_SKIP() << "Bitwuzla backend is unavailable";
    }
    EXPECT_EQ(QL_SOLVER_CHECK_UNSAT, off);
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, SolveViolation(with.query()));
}

TEST(ProductMiter, TerminationAxisAloneDecidesADivergingSide) {
    NullaryFunction left;
    NullaryFunction right;
    SyntheticPair without;
    SyntheticPair with;

    ASSERT_NO_FATAL_FAILURE(
        left.Build("halts", QL_IR_TERMINATOR_TRAP, 1u));
    ASSERT_NO_FATAL_FAILURE(
        right.Build("spins", QL_IR_TERMINATOR_DIVERGE, 0u));
    ASSERT_NO_FATAL_FAILURE(without.Build(
        left, "halts", right, "spins",
        w2::ContractObserving(QL_OBSERVE_RETURN_VALUE)));
    ASSERT_NO_FATAL_FAILURE(with.Build(
        left, "halts", right, "spins",
        w2::ContractObserving(QL_OBSERVE_RETURN_VALUE |
                              QL_OBSERVE_TERMINATION)));

    const ql_solver_check_kind off = SolveViolation(without.query());
    if (off == QL_SOLVER_CHECK_INVALID) {
        GTEST_SKIP() << "Bitwuzla backend is unavailable";
    }
    EXPECT_EQ(QL_SOLVER_CHECK_UNSAT, off);
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, SolveViolation(with.query()));
}

TEST(ProductMiter, RefusesASchemaV1ProblemInsteadOfGuessingTheCorrespondence) {
    w2::CFunction left;
    w2::CFunction right;
    ql_problem_definition_v1 definition{};
    ql_artifact *artifact = nullptr;
    ql_problem *problem = nullptr;
    ql_ir *left_ir = nullptr;
    ql_ir *right_ir = nullptr;
    ql_product_query *query = nullptr;
    ql_error error{};

    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&left, kAdd, "add"));
    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(&right, kSum, "sum"));
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
    ASSERT_EQ(QL_STATUS_OK,
              ql_problem_open(nullptr, artifact, &problem, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_open(nullptr, left.ir_artifact(), &left_ir, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_open(nullptr, right.ir_artifact(), &right_ir, &error));

    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_product_query_build(nullptr, problem, left_ir, right_ir,
                                     &query, &error));
    EXPECT_EQ(nullptr, query);

    ql_ir_release(left_ir);
    ql_ir_release(right_ir);
    ql_problem_release(problem);
    ql_artifact_release(artifact);
}

TEST(ProductMiter, RefusesAnIrThatDoesNotMatchItsProblemSignature) {
    w2::Pair pair;
    w2::CFunction other;
    ql_ir *other_ir = nullptr;
    ql_product_query *query = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, pair.Build(kAdd, "add", kSum, "sum",
                                       w2::DefaultContract(), &error))
        << error.message;
    ASSERT_NO_FATAL_FAILURE(w2::BuildOrFail(
        &other, "long add(long x, long y){ return x + y; }", "add"));
    ASSERT_EQ(QL_STATUS_OK,
              ql_ir_open(nullptr, other.ir_artifact(), &other_ir, &error));

    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_product_query_build(nullptr, pair.problem(), other_ir,
                                     pair.right_ir(), &query, &error));
    EXPECT_EQ(nullptr, query);
    ql_ir_release(other_ir);
}

}  // namespace
