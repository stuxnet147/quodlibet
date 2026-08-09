#include "quodlibet/combine.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct ResultDeleter {
    void operator()(ql_combine_result *result) const {
        ql_combine_result_release(result);
    }
};

using ResultPtr = std::unique_ptr<ql_combine_result, ResultDeleter>;

ql_digest digest_of(const char *text) {
    ql_digest digest{};
    ql_digest_data(text, std::strlen(text), &digest);
    return digest;
}

const ql_digest &problem_digest() {
    static const ql_digest value = digest_of("problem");
    return value;
}

const ql_digest &contract_digest() {
    static const ql_digest value = digest_of("contract");
    return value;
}

ql_combine_request_v1 make_request(
    ql_relation relation = QL_RELATION_EQUIVALENCE) {
    ql_combine_request_v1 request{};
    ql_combine_request_init(&request);
    request.problem_digest = problem_digest();
    request.contract_digest = contract_digest();
    request.ir_semantics_version = 2u;
    request.relation = relation;
    return request;
}

ql_combine_input_v1 make_input(const char *name, ql_verdict verdict,
                               std::uint32_t declaration_order,
                               ql_relation relation = QL_RELATION_EQUIVALENCE) {
    ql_combine_input_v1 input{};
    ql_combine_input_init(&input);
    input.method_name = name;
    input.method_version = "1.0";
    input.problem_digest = problem_digest();
    input.contract_digest = contract_digest();
    input.ir_semantics_version = 2u;
    input.relation = relation;
    input.declaration_order = declaration_order;
    input.completion_order = declaration_order;
    input.outcome.verdict = verdict;
    input.outcome.evidence_digest = digest_of(name);
    return input;
}

/* A proof whose certificate an independent checker validated. */
ql_combine_input_v1 checked_proof(const char *name, ql_verdict verdict,
                                  std::uint32_t order,
                                  ql_relation relation =
                                      QL_RELATION_EQUIVALENCE) {
    ql_combine_input_v1 input = make_input(name, verdict, order, relation);
    input.evidence.proof_checked = 1u;
    input.evidence.checker_identity = "ql.checker";
    return input;
}

/* A counterexample whose witness was replayed against the IR. */
ql_combine_input_v1 replayed_counterexample(const char *name,
                                            std::uint32_t order) {
    ql_combine_input_v1 input =
        make_input(name, QL_VERDICT_COUNTEREXAMPLE, order);
    input.evidence.witness_replayed = 1u;
    return input;
}

ResultPtr combine(const ql_combine_request_v1 &request,
                  const std::vector<ql_combine_input_v1> &inputs) {
    ql_combine_result *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error))
        << error.message;
    return ResultPtr(raw);
}

ql_combine_result_view_v1 view_of(const ql_combine_result *result) {
    ql_combine_result_view_v1 view{};
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_combine_result_get_view(result, &view, &error))
        << error.message;
    return view;
}

/* Rule 1. */
TEST(Combine, RefusesResultsThatAnswerADifferentQuestion) {
    const ql_combine_request_v1 request = make_request();
    ql_combine_result *raw = nullptr;
    ql_error error{};

    std::vector<ql_combine_input_v1> inputs = {
        checked_proof("a", QL_VERDICT_PROVED_EQUIVALENT, 0u)};
    inputs[0].problem_digest = digest_of("another problem");
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));

    inputs[0] = checked_proof("a", QL_VERDICT_PROVED_EQUIVALENT, 0u);
    inputs[0].contract_digest = digest_of("another contract");
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));

    inputs[0] = checked_proof("a", QL_VERDICT_PROVED_EQUIVALENT, 0u);
    inputs[0].ir_semantics_version = 99u;
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));

    inputs[0] = checked_proof("a", QL_VERDICT_PROVED_EQUIVALENT, 0u);
    inputs[0].relation = QL_RELATION_LEFT_REFINES_RIGHT;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));

    inputs[0] = checked_proof("a", QL_VERDICT_PROVED_EQUIVALENT, 0u);
    inputs[0].ub_policy = QL_UB_LANGUAGE_REFINEMENT;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));

    inputs[0] = checked_proof("a", QL_VERDICT_PROVED_EQUIVALENT, 0u);
    inputs[0].observations |= QL_OBSERVE_MEMORY;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));

    inputs[0] = checked_proof("a", QL_VERDICT_PROVED_EQUIVALENT, 0u);
    inputs[0].memory_observation = QL_MEMORY_ORDERED_WRITES;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));
    EXPECT_EQ(nullptr, raw);
}

/* Rule 2. */
TEST(Combine, AnUncheckedProofIsWithdrawnNotAccepted) {
    const ql_combine_request_v1 request = make_request();
    ql_combine_input_v1 raw_unsat =
        make_input("smt", QL_VERDICT_PROVED_EQUIVALENT, 0u);
    raw_unsat.evidence.backend_identity = "bitwuzla 0.9.1";
    const ResultPtr result = combine(request, {raw_unsat});
    const ql_combine_result_view_v1 view = view_of(result.get());

    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(0u, view.accepted_count);
    EXPECT_EQ(1u, view.withdrawn_count);
    ASSERT_EQ(1u, view.finding_count);
    EXPECT_EQ(QL_COMBINE_DISPOSITION_WITHDRAWN, view.findings[0].disposition);
    EXPECT_EQ(QL_COMBINE_CODE_UNTRUSTED_BACKEND, view.findings[0].code);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.findings[0].reported_verdict);
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.findings[0].effective_verdict);
}

TEST(Combine, AStatedTrustedBackendIsTheOnlyOtherWayAProofCounts) {
    ql_combine_request_v1 request = make_request();
    const char *trusted[] = {"bitwuzla 0.9.1"};
    request.trusted_backends = trusted;
    request.trusted_backend_count = 1u;

    ql_combine_input_v1 input =
        make_input("smt", QL_VERDICT_PROVED_EQUIVALENT, 0u);
    input.evidence.backend_identity = "bitwuzla 0.9.1";
    ResultPtr result = combine(request, {input});
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view_of(result.get()).verdict);

    input.evidence.backend_identity = "bitwuzla 0.8.0";
    result = combine(request, {input});
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view_of(result.get()).verdict);

    input.evidence.backend_identity = nullptr;
    result = combine(request, {input});
    EXPECT_EQ(QL_COMBINE_CODE_UNCHECKED_PROOF,
              view_of(result.get()).findings[0].code);
}

TEST(Combine, AnUnreplayedModelIsNotACounterexample) {
    const ql_combine_request_v1 request = make_request();
    const ql_combine_input_v1 candidate =
        make_input("smt", QL_VERDICT_COUNTEREXAMPLE, 0u);
    const ResultPtr result = combine(request, {candidate});
    const ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(QL_COMBINE_CODE_UNREPLAYED_WITNESS, view.findings[0].code);
}

TEST(Combine, AnExhaustedBudgetWithdrawsWhateverTheMethodReported) {
    const ql_combine_request_v1 request = make_request();
    ql_combine_input_v1 input =
        checked_proof("smt", QL_VERDICT_PROVED_EQUIVALENT, 0u);
    input.evidence.budget_exhausted = 1u;
    const ResultPtr result = combine(request, {input});
    const ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(QL_COMBINE_CODE_BUDGET_EXHAUSTED, view.findings[0].code);
}

/* Rule 2, the negative half: agreement carries no weight. */
TEST(Combine, ThreeAgreeingUncheckedProofsStillDecideNothing) {
    const ql_combine_request_v1 request = make_request();
    std::vector<ql_combine_input_v1> inputs;
    for (int index = 0; index < 3; ++index) {
        ql_combine_input_v1 input = make_input(
            index == 0 ? "a" : (index == 1 ? "b" : "c"),
            QL_VERDICT_PROVED_EQUIVALENT,
            static_cast<std::uint32_t>(index));
        input.evidence.backend_identity = "some solver";
        inputs.push_back(input);
    }
    const ResultPtr result = combine(request, inputs);
    const ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(3u, view.withdrawn_count);
}

TEST(Combine, OneCheckedProofOutweighsAnyNumberOfUncheckedDisagreements) {
    const ql_combine_request_v1 request = make_request();
    std::vector<ql_combine_input_v1> inputs = {
        checked_proof("checked", QL_VERDICT_PROVED_EQUIVALENT, 3u)};
    for (int index = 0; index < 4; ++index) {
        /* Unreplayed models. A vote would drown the one checked proof. */
        inputs.push_back(make_input(index == 0   ? "m0"
                                    : index == 1 ? "m1"
                                    : index == 2 ? "m2"
                                                 : "m3",
                                    QL_VERDICT_COUNTEREXAMPLE,
                                    static_cast<std::uint32_t>(index)));
    }
    const ResultPtr result = combine(request, inputs);
    const ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
    EXPECT_EQ(0u, view.inconsistent);
    EXPECT_EQ(0u, view.deciding_input);
    EXPECT_EQ(4u, view.withdrawn_count);
}

/* Rule 3. */
TEST(Combine, OneCheckedProofSettlesExactlyTheRelationItEstablishes) {
    const ql_combine_request_v1 equivalence = make_request();
    ResultPtr result = combine(
        equivalence,
        {checked_proof("a", QL_VERDICT_PROVED_LEFT_REFINES_RIGHT, 0u)});
    ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(QL_COMBINE_DISPOSITION_ACCEPTED, view.findings[0].disposition);
    EXPECT_EQ(QL_COMBINE_CODE_WEAKER_RELATION, view.findings[0].code);

    const ql_combine_request_v1 refinement =
        make_request(QL_RELATION_LEFT_REFINES_RIGHT);
    result = combine(refinement,
                     {checked_proof("a", QL_VERDICT_PROVED_LEFT_REFINES_RIGHT,
                                    0u, QL_RELATION_LEFT_REFINES_RIGHT)});
    EXPECT_EQ(QL_VERDICT_PROVED_LEFT_REFINES_RIGHT,
              view_of(result.get()).verdict);

    /* An equivalence proof implies both directions. */
    result = combine(refinement,
                     {checked_proof("a", QL_VERDICT_PROVED_EQUIVALENT, 0u,
                                    QL_RELATION_LEFT_REFINES_RIGHT)});
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view_of(result.get()).verdict);
}

TEST(Combine, OppositeRefinementsComposeOnlyWhenTheCallerAsksForThatRule) {
    ql_combine_request_v1 request = make_request();
    const std::vector<ql_combine_input_v1> inputs = {
        checked_proof("left", QL_VERDICT_PROVED_LEFT_REFINES_RIGHT, 0u),
        checked_proof("right", QL_VERDICT_PROVED_RIGHT_REFINES_LEFT, 1u)};

    ResultPtr result = combine(request, inputs);
    ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(0u, view.refinement_composition_applied);

    request.allow_refinement_composition = 1u;
    result = combine(request, inputs);
    view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
    EXPECT_EQ(1u, view.refinement_composition_applied);
    EXPECT_EQ(0u, view.deciding_input);
    EXPECT_EQ(1u, view.supporting_input);
}

TEST(Combine, CompositionNeedsBothDirectionsAndBothMustBeValid) {
    ql_combine_request_v1 request = make_request();
    request.allow_refinement_composition = 1u;

    /* Only one direction. */
    ResultPtr result = combine(
        request,
        {checked_proof("left", QL_VERDICT_PROVED_LEFT_REFINES_RIGHT, 0u)});
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view_of(result.get()).verdict);

    /* The second direction is a raw solver result, so there is nothing to
       compose with. */
    ql_combine_input_v1 unchecked =
        make_input("right", QL_VERDICT_PROVED_RIGHT_REFINES_LEFT, 1u);
    unchecked.evidence.backend_identity = "some solver";
    result = combine(
        request,
        {checked_proof("left", QL_VERDICT_PROVED_LEFT_REFINES_RIGHT, 0u),
         unchecked});
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view_of(result.get()).verdict);
}

/* Rule 4. */
TEST(Combine, ACheckedProofAgainstAReplayedCounterexampleIsInconsistent) {
    const ql_combine_request_v1 request = make_request();
    const ResultPtr result = combine(
        request, {checked_proof("prover", QL_VERDICT_PROVED_EQUIVALENT, 0u),
                  replayed_counterexample("refuter", 1u)});
    const ql_combine_result_view_v1 view = view_of(result.get());

    EXPECT_EQ(1u, view.inconsistent);
    /* Not resolved by priority, ordering, or counting. */
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(0u, view.deciding_input);
    EXPECT_EQ(1u, view.conflicting_input);
    EXPECT_EQ(2u, view.accepted_count);
}

TEST(Combine, TheConflictSurvivesReorderingTheInputs) {
    const ql_combine_request_v1 request = make_request();
    const ResultPtr result = combine(
        request, {replayed_counterexample("refuter", 0u),
                  checked_proof("prover", QL_VERDICT_PROVED_EQUIVALENT, 1u)});
    const ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(1u, view.inconsistent);
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(1u, view.deciding_input);
    EXPECT_EQ(0u, view.conflicting_input);
}

TEST(Combine, AComposedEquivalenceAlsoConflictsWithACounterexample) {
    ql_combine_request_v1 request = make_request();
    request.allow_refinement_composition = 1u;
    const ResultPtr result = combine(
        request,
        {checked_proof("left", QL_VERDICT_PROVED_LEFT_REFINES_RIGHT, 0u),
         checked_proof("right", QL_VERDICT_PROVED_RIGHT_REFINES_LEFT, 1u),
         replayed_counterexample("refuter", 2u)});
    const ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(1u, view.inconsistent);
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(2u, view.conflicting_input);
}

TEST(Combine, AWeakerProofDoesNotConflictWithACounterexample) {
    /* One direction proved and equivalence refuted is an ordinary outcome,
       not a defect: the two claims are about different relations. */
    const ql_combine_request_v1 request = make_request();
    const ResultPtr result = combine(
        request,
        {checked_proof("left", QL_VERDICT_PROVED_LEFT_REFINES_RIGHT, 0u),
         replayed_counterexample("refuter", 1u)});
    const ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(0u, view.inconsistent);
    EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, view.verdict);
    EXPECT_EQ(1u, view.deciding_input);
}

/* Rule 5. */
TEST(Combine, ManyUnknownsRemainUnknown) {
    const ql_combine_request_v1 request = make_request();
    const ResultPtr result =
        combine(request, {make_input("a", QL_VERDICT_UNKNOWN, 0u),
                          make_input("b", QL_VERDICT_UNKNOWN, 1u),
                          make_input("c", QL_VERDICT_UNKNOWN, 2u)});
    const ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(0u, view.withdrawn_count);
    for (std::size_t index = 0u; index < view.finding_count; ++index) {
        EXPECT_EQ(QL_COMBINE_DISPOSITION_NEUTRAL,
                  view.findings[index].disposition);
    }
}

/* Rule 6. */
TEST(Combine, BoundedResultsStayBoundedAndKeepTheirWholeVector) {
    const ql_combine_request_v1 request = make_request();
    const ql_combine_bound_v1 first[] = {{"unrolling", 8u}, {"inputs", 64u}};
    const ql_combine_bound_v1 second[] = {{"path-length", 20u}};

    ql_combine_input_v1 a = make_input("bmc", QL_VERDICT_BOUNDED_CLEAN, 0u);
    a.bounds = first;
    a.bound_count = 2u;
    ql_combine_input_v1 b = make_input("fuzz", QL_VERDICT_BOUNDED_CLEAN, 1u);
    b.bounds = second;
    b.bound_count = 1u;

    const ResultPtr result = combine(request, {a, b});
    const ql_combine_result_view_v1 view = view_of(result.get());
    /* Any number of bounded results remains bounded. */
    EXPECT_EQ(QL_VERDICT_BOUNDED_CLEAN, view.verdict);
    EXPECT_EQ(2u, view.bounded_count);
    /* The dimensions are not collapsed into one scalar. */
    ASSERT_EQ(3u, view.bound_count);
    EXPECT_STREQ("unrolling", view.bounds[0].dimension);
    EXPECT_EQ(8u, view.bounds[0].value);
    EXPECT_STREQ("inputs", view.bounds[1].dimension);
    EXPECT_STREQ("path-length", view.bounds[2].dimension);
}

TEST(Combine, BoundedNeverOutranksACheckedProofAndIsNeverPromoted) {
    const ql_combine_request_v1 request = make_request();
    const ql_combine_bound_v1 bounds[] = {{"unrolling", 8u}};
    ql_combine_input_v1 bounded =
        make_input("bmc", QL_VERDICT_BOUNDED_CLEAN, 0u);
    bounded.bounds = bounds;
    bounded.bound_count = 1u;
    /* Bounded evidence marked as if it had been checked and replayed still
       cannot become a proof; the verdict itself carries the boundary. */
    bounded.evidence.proof_checked = 1u;
    bounded.evidence.witness_replayed = 1u;

    ResultPtr result = combine(request, {bounded});
    EXPECT_EQ(QL_VERDICT_BOUNDED_CLEAN, view_of(result.get()).verdict);

    result = combine(request,
                     {bounded, checked_proof("prover",
                                             QL_VERDICT_PROVED_EQUIVALENT,
                                             1u)});
    const ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
    EXPECT_EQ(0u, view.inconsistent);
}

/* Rule 7. */
TEST(Combine, ADownstreamResultNeedsItsNormalizationToHold) {
    const ql_combine_request_v1 request = make_request();
    ql_combine_input_v1 normalizer =
        make_input("normalize.egraph", QL_VERDICT_PROVED_EQUIVALENT, 0u);
    normalizer.role = QL_COMBINE_ROLE_NORMALIZER;
    ql_combine_input_v1 downstream =
        checked_proof("prove.smt", QL_VERDICT_PROVED_EQUIVALENT, 1u);
    downstream.depends_on = 0u;

    /* The normalization's own proof was never checked, so nothing downstream
       transfers back to the original problem. */
    ResultPtr result = combine(request, {normalizer, downstream});
    ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(QL_COMBINE_CODE_BROKEN_NORMALIZATION, view.findings[1].code);

    normalizer.evidence.proof_checked = 1u;
    result = combine(request, {normalizer, downstream});
    view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdict);
    EXPECT_EQ(1u, view.deciding_input);
}

TEST(Combine, ANormalizationProofNeverDecidesTheRequestedRelationItself) {
    const ql_combine_request_v1 request = make_request();
    ql_combine_input_v1 normalizer =
        checked_proof("normalize.egraph", QL_VERDICT_PROVED_EQUIVALENT, 0u);
    normalizer.role = QL_COMBINE_ROLE_NORMALIZER;
    const ResultPtr result = combine(request, {normalizer});
    const ql_combine_result_view_v1 view = view_of(result.get());
    EXPECT_EQ(QL_VERDICT_UNKNOWN, view.verdict);
    EXPECT_EQ(1u, view.accepted_count);
    EXPECT_EQ(QL_COMBINE_NO_INPUT, view.deciding_input);
}

TEST(Combine, RejectsADependencyThatIsNotANormalizer) {
    const ql_combine_request_v1 request = make_request();
    std::vector<ql_combine_input_v1> inputs = {
        checked_proof("a", QL_VERDICT_PROVED_EQUIVALENT, 0u),
        checked_proof("b", QL_VERDICT_PROVED_EQUIVALENT, 1u)};
    inputs[1].depends_on = 0u;
    ql_combine_result *raw = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));

    inputs[1].role = QL_COMBINE_ROLE_NORMALIZER;
    EXPECT_EQ(QL_STATUS_CYCLE,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));

    inputs[1] = checked_proof("b", QL_VERDICT_PROVED_EQUIVALENT, 1u);
    inputs[1].depends_on = 1u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));
}

/* Rule 8. */
TEST(Combine, CompletionOrderNeverChangesTheOutcome) {
    const ql_combine_request_v1 request = make_request();
    std::vector<ql_combine_input_v1> inputs = {
        checked_proof("second", QL_VERDICT_PROVED_EQUIVALENT, 1u),
        checked_proof("first", QL_VERDICT_PROVED_EQUIVALENT, 0u)};
    inputs[0].completion_order = 0u;
    inputs[1].completion_order = 9u;

    ResultPtr result = combine(request, inputs);
    /* Declaration order 0 belongs to input index 1, and the later-finishing
       worker does not lose its place. */
    EXPECT_EQ(1u, view_of(result.get()).deciding_input);

    inputs[0].completion_order = 9u;
    inputs[1].completion_order = 0u;
    result = combine(request, inputs);
    EXPECT_EQ(1u, view_of(result.get()).deciding_input);
}

TEST(Combine, TiedDeclarationOrdersBreakOnTheEvidenceDigest) {
    const ql_combine_request_v1 request = make_request();
    std::vector<ql_combine_input_v1> inputs = {
        checked_proof("alpha", QL_VERDICT_PROVED_EQUIVALENT, 4u),
        checked_proof("beta", QL_VERDICT_PROVED_EQUIVALENT, 4u)};
    const ResultPtr first = combine(request, inputs);
    const std::size_t chosen = view_of(first.get()).deciding_input;

    std::vector<ql_combine_input_v1> swapped = {inputs[1], inputs[0]};
    const ResultPtr second = combine(request, swapped);
    const std::size_t chosen_again = view_of(second.get()).deciding_input;

    /* The same artifact wins whichever slot it occupies. */
    EXPECT_STREQ(inputs[chosen].method_name,
                 swapped[chosen_again].method_name);
}

TEST(Combine, EveryInputGetsAFindingInInputOrder) {
    const ql_combine_request_v1 request = make_request();
    const ResultPtr result = combine(
        request, {checked_proof("a", QL_VERDICT_PROVED_EQUIVALENT, 0u),
                  make_input("b", QL_VERDICT_UNKNOWN, 1u),
                  make_input("c", QL_VERDICT_COUNTEREXAMPLE, 2u)});
    const ql_combine_result_view_v1 view = view_of(result.get());
    ASSERT_EQ(3u, view.finding_count);
    for (std::size_t index = 0u; index < view.finding_count; ++index) {
        EXPECT_EQ(index, view.findings[index].input_index);
    }
    EXPECT_EQ(QL_COMBINE_DISPOSITION_ACCEPTED, view.findings[0].disposition);
    EXPECT_EQ(QL_COMBINE_DISPOSITION_NEUTRAL, view.findings[1].disposition);
    EXPECT_EQ(QL_COMBINE_DISPOSITION_WITHDRAWN, view.findings[2].disposition);
}

TEST(Combine, RejectsMalformedRequestsAndInputs) {
    ql_combine_request_v1 request = make_request();
    const std::vector<ql_combine_input_v1> inputs = {
        checked_proof("a", QL_VERDICT_PROVED_EQUIVALENT, 0u)};
    ql_combine_result *raw = nullptr;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), nullptr, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_combine_evaluate(nullptr, nullptr, inputs.data(),
                                  inputs.size(), &raw, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_combine_evaluate(nullptr, &request, inputs.data(), 0u, &raw,
                                  &error));

    request.abi_version += 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));
    request = make_request();
    request.schema_version += 1u;
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));
    request = make_request();
    request.trusted_backend_count = 1u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_combine_evaluate(nullptr, &request, inputs.data(),
                                  inputs.size(), &raw, &error));

    request = make_request();
    std::vector<ql_combine_input_v1> bad = inputs;
    bad[0].role = QL_COMBINE_ROLE_INVALID;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_combine_evaluate(nullptr, &request, bad.data(), bad.size(),
                                  &raw, &error));
    bad = inputs;
    bad[0].struct_size -= 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_combine_evaluate(nullptr, &request, bad.data(), bad.size(),
                                  &raw, &error));
    EXPECT_EQ(nullptr, raw);

    ql_combine_result_view_v1 view{};
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_combine_result_get_view(nullptr, &view, &error));
    ql_combine_result_release(nullptr);
}

}  // namespace
