#include "quodlibet/policy.h"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "quodlibet/budget.h"

namespace {

/* Accepts only checked proofs, treats a replayed counterexample as a failure,
   and abstains on everything else. */
const char *const kStrictPolicy = R"({
  "schema_version": 1,
  "name": "strict-proof-only",
  "description": "reinforcement-learning reward: only a checked proof passes",
  "classes": [
    {
      "name": "accept",
      "disposition": "pass",
      "claims": "proof",
      "verdicts": ["proved_equivalent", "proved_left_refines_right",
                   "proved_right_refines_left"],
      "proof_trust": "checked",
      "score": 1.0
    },
    {
      "name": "refuted",
      "disposition": "fail",
      "claims": "evidence",
      "verdicts": ["counterexample"],
      "require_replayed_witness": true,
      "score": -1.0
    },
    {
      "name": "open",
      "disposition": "abstain",
      "claims": "none",
      "verdicts": ["unknown", "bounded_clean"],
      "score": 0.0
    }
  ],
  "default_class": "open"
})";

/* A fast screening filter: bounded clean counts as a pass, but it is recorded
   as heuristic and its verdict stays BOUNDED_CLEAN. */
const char *const kScreeningPolicy = R"({
  "schema_version": 1,
  "name": "fast-filter",
  "classes": [
    {
      "name": "clean",
      "disposition": "pass",
      "claims": "heuristic",
      "verdicts": ["bounded_clean"],
      "score": 0.5
    },
    {
      "name": "proved",
      "disposition": "pass",
      "claims": "proof",
      "verdicts": ["proved_equivalent"],
      "proof_trust": "trusted_backend",
      "score": 1.0
    },
    {
      "name": "refuted",
      "disposition": "fail",
      "claims": "evidence",
      "verdicts": ["counterexample"],
      "require_replayed_witness": true,
      "score": 0.0
    },
    {
      "name": "open",
      "disposition": "abstain",
      "claims": "none",
      "verdicts": ["unknown", "proved_left_refines_right",
                   "proved_right_refines_left"],
      "score": 0.0
    }
  ],
  "default_class": "open",
  "trust": { "trusted_backends": ["bitwuzla 0.9.1"] }
})";

class PolicyHandle {
public:
    explicit PolicyHandle(const char *json) {
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK, ql_policy_parse(nullptr, json,
                                                std::strlen(json), &policy_,
                                                &error))
            << error.message;
    }
    ~PolicyHandle() { ql_policy_destroy(policy_); }
    PolicyHandle(const PolicyHandle &) = delete;
    PolicyHandle &operator=(const PolicyHandle &) = delete;

    ql_policy *get() const { return policy_; }

private:
    ql_policy *policy_ = nullptr;
};

ql_outcome_v1 make_outcome(ql_verdict verdict, std::uint64_t bound = 0u) {
    ql_outcome_v1 outcome{};
    outcome.struct_size = sizeof(outcome);
    outcome.schema_version = 1u;
    outcome.verdict = verdict;
    outcome.checked_bound = bound;
    return outcome;
}

ql_policy_result_v1 evaluate(const ql_policy *policy,
                             const ql_outcome_v1 &outcome,
                             const ql_policy_evidence_v1 &evidence) {
    ql_policy_result_v1 result{};
    ql_policy_result_init(&result);
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_policy_evaluate(policy, &outcome, &evidence, &result, &error))
        << error.message;
    return result;
}

ql_policy_evidence_v1 evidence_of(std::uint32_t replayed,
                                  std::uint32_t checked,
                                  const char *backend = nullptr) {
    ql_policy_evidence_v1 evidence{};
    ql_policy_evidence_init(&evidence);
    evidence.witness_replayed = replayed;
    evidence.proof_checked = checked;
    evidence.backend_identity = backend;
    return evidence;
}

std::string serialize_policy(const ql_policy *policy) {
    ql_error error{};
    std::size_t needed = 0u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_policy_serialize(policy, nullptr, 0u, &needed, &error));
    EXPECT_GT(needed, 1u);
    std::vector<char> buffer(needed);
    std::size_t written = 0u;
    EXPECT_EQ(QL_STATUS_OK, ql_policy_serialize(policy, buffer.data(),
                                                buffer.size(), &written,
                                                &error))
        << error.message;
    EXPECT_EQ(needed, written);
    return std::string(buffer.data());
}

std::string serialize_result(const ql_policy_result_v1 &result) {
    ql_error error{};
    std::size_t needed = 0u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_policy_result_serialize(&result, nullptr, 0u, &needed,
                                         &error));
    std::vector<char> buffer(needed);
    std::size_t written = 0u;
    EXPECT_EQ(QL_STATUS_OK,
              ql_policy_result_serialize(&result, buffer.data(), buffer.size(),
                                         &written, &error))
        << error.message;
    EXPECT_EQ(needed, written);
    return std::string(buffer.data());
}

/* Every rejection case names the JSON location that caused it. */
void expect_rejected(const char *json, const char *location,
                     const char *phrase,
                     ql_status expected = QL_STATUS_PARSE_ERROR) {
    ql_policy *policy = nullptr;
    ql_error error{};

    EXPECT_EQ(expected, ql_policy_parse(nullptr, json, std::strlen(json),
                                        &policy, &error))
        << json;
    EXPECT_EQ(nullptr, policy);
    EXPECT_NE(nullptr, std::strstr(error.message, location))
        << "message: " << error.message;
    EXPECT_NE(nullptr, std::strstr(error.message, phrase))
        << "message: " << error.message;
}

TEST(Policy, ParsesAndExposesAValidPolicy) {
    PolicyHandle policy(kStrictPolicy);

    EXPECT_STREQ("strict-proof-only", ql_policy_name(policy.get()));
    ASSERT_EQ(3u, ql_policy_class_count(policy.get()));

    ql_policy_class_view_v1 view{};
    view.struct_size = sizeof(view);
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK,
              ql_policy_class_at(policy.get(), 0u, &view, &error));
    EXPECT_STREQ("accept", view.name);
    EXPECT_EQ(QL_POLICY_DISPOSITION_PASS, view.disposition);
    EXPECT_EQ(QL_POLICY_CLAIMS_PROOF, view.claims);
    EXPECT_EQ(QL_POLICY_PROOF_TRUST_CHECKED, view.proof_trust);
    EXPECT_EQ(3u, view.verdict_count);
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, view.verdicts[0]);
    EXPECT_DOUBLE_EQ(1.0, view.score);
}

TEST(Policy, RejectsAPolicyThatPromotesBoundedCleanToProof) {
    const char *json = R"({
      "schema_version": 1,
      "name": "cheating",
      "classes": [
        {"name":"accept","disposition":"pass","claims":"proof",
         "verdicts":["proved_equivalent","bounded_clean"],
         "proof_trust":"checked"},
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown"]}
      ],
      "default_class": "open"
    })";
    expect_rejected(json, "/classes/0/claims",
                    "a class claiming proof may only list proved verdicts");
}

TEST(Policy, RejectsAWeakeningEntryThatStrengthens) {
    const char *json = R"({
      "schema_version": 1,
      "name": "cheating",
      "classes": [
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown","bounded_clean","counterexample",
                     "proved_equivalent"],
         "require_replayed_witness": true,
         "proof_trust": "checked"}
      ],
      "default_class": "open",
      "weaken": [{"from":"bounded_clean","to":"proved_equivalent"}]
    })";
    expect_rejected(json, "/weaken/0/to", "never strengthen");
}

TEST(Policy, RejectsPromotingAnUnknownIntoACounterexample) {
    const char *json = R"({
      "schema_version": 1,
      "name": "cheating",
      "classes": [
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown","bounded_clean","counterexample",
                     "proved_equivalent"],
         "require_replayed_witness": true,
         "proof_trust": "checked"}
      ],
      "default_class": "open",
      "weaken": [{"from":"unknown","to":"counterexample"}]
    })";
    expect_rejected(json, "/weaken/0/to", "never strengthen");
}

TEST(Policy, RejectsACounterexampleClassWithoutARequiredReplay) {
    const char *json = R"({
      "schema_version": 1,
      "name": "cheating",
      "classes": [
        {"name":"refuted","disposition":"fail","claims":"evidence",
         "verdicts":["counterexample"]},
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown"]}
      ],
      "default_class": "open"
    })";
    expect_rejected(json, "/classes/0/require_replayed_witness",
                    "an unreplayed SAT model is not a counterexample");
}

TEST(Policy, RejectsAProvedClassWithNoStatedTrust) {
    const char *json = R"({
      "schema_version": 1,
      "name": "cheating",
      "classes": [
        {"name":"accept","disposition":"pass","claims":"proof",
         "verdicts":["proved_equivalent"]},
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown"]}
      ],
      "default_class": "open"
    })";
    expect_rejected(json, "/classes/0/proof_trust",
                    "a raw solver UNSAT is not a proof");
}

TEST(Policy, RejectsATrustedBackendClaimWithNoTrustedBackend) {
    const char *json = R"({
      "schema_version": 1,
      "name": "cheating",
      "classes": [
        {"name":"accept","disposition":"pass","claims":"proof",
         "verdicts":["proved_equivalent"],
         "proof_trust":"trusted_backend"},
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown"]}
      ],
      "default_class": "open",
      "trust": { "trusted_backends": [] }
    })";
    expect_rejected(json, "/classes/0/proof_trust",
                    "requires a non-empty trust.trusted_backends list");
}

TEST(Policy, RejectsStructuralMistakesWithTheirLocation) {
    expect_rejected(R"({"schema_version": 2, "name": "x", "classes": [],
                        "default_class": "x"})",
                    "/schema_version", "must be 1", QL_STATUS_SCHEMA_MISMATCH);

    expect_rejected(R"({"schema_version": 1, "name": "x",
                        "classes": [{"name":"a","disposition":"pass",
                                     "claims":"none","verdicts":["unknown"]}],
                        "default_class": "missing"})",
                    "/default_class", "names no declared class");

    expect_rejected(R"({"schema_version": 1, "name": "x", "surprise": 1,
                        "classes": [{"name":"a","disposition":"pass",
                                     "claims":"none","verdicts":["unknown"]}],
                        "default_class": "a"})",
                    "unknown key", "surprise");

    expect_rejected(R"({"schema_version": 1, "name": "x",
                        "classes": [{"name":"a","disposition":"maybe",
                                     "claims":"none","verdicts":["unknown"]}],
                        "default_class": "a"})",
                    "/classes/0/disposition", "pass, fail or abstain");

    expect_rejected(R"({"schema_version": 1, "name": "x",
                        "classes": [{"name":"a","disposition":"pass",
                                     "claims":"none","verdicts":["maybe"]}],
                        "default_class": "a"})",
                    "/classes/0/verdicts/0", "unknown verdict");

    expect_rejected(R"({"schema_version": 1, "name": "x",
                        "classes": [
                          {"name":"a","disposition":"pass","claims":"none",
                           "verdicts":["unknown"]},
                          {"name":"b","disposition":"fail","claims":"none",
                           "verdicts":["unknown"]}],
                        "default_class": "a"})",
                    "/classes/1/verdicts/0", "already claimed");

    expect_rejected(R"({"schema_version": 1, "name": "x",
                        "classes": [
                          {"name":"a","disposition":"pass","claims":"none",
                           "verdicts":["unknown"]},
                          {"name":"a","disposition":"fail","claims":"none",
                           "verdicts":["bounded_clean"]}],
                        "default_class": "a"})",
                    "/classes/1/name", "duplicate class name");

    expect_rejected(R"({"schema_version": 1, "name": "x",
                        "classes": [{"name":"a","disposition":"pass",
                                     "claims":"none","verdicts":["unknown"],
                                     "proof_trust":"checked"}],
                        "default_class": "a"})",
                    "/classes/0/proof_trust",
                    "belongs only to a class that accepts a proved verdict");
}

TEST(Policy, ReportsAByteOffsetForMalformedJson) {
    ql_policy *policy = nullptr;
    ql_error error{};

    static const char kMalformed[] = "{\"schema_version\": 1,,}";
    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_policy_parse(nullptr, kMalformed, std::strlen(kMalformed),
                              &policy, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "at byte"));
}

/* Both parsers take a pointer and a length. `json_size` is never a request to
   measure the pointer, so neither may read a byte past it. These pass buffers
   with no NUL anywhere in them, which is what turns a read past the length
   into a heap overflow the sanitizer sees rather than a silent one. */
TEST(Policy, ParserRejectsAnEmptyDocumentWithoutReadingThePointer) {
    /* Exactly one byte, no terminator: strlen() on this would run off the
       allocation. This is the input the 2026-08-10 fuzz campaign crashed on. */
    std::vector<char> unterminated(1u, '{');
    ql_policy *policy = nullptr;
    ql_error error{};

    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_policy_parse(nullptr, unterminated.data(), 0u, &policy,
                              &error));
    EXPECT_EQ(nullptr, policy);
    EXPECT_NE(nullptr, std::strstr(error.message, "empty"));
}

TEST(Policy, ResultParserRejectsAnEmptyDocumentWithoutReadingThePointer) {
    std::vector<char> unterminated(1u, '{');
    ql_policy_result_v1 parsed{};
    ql_policy_result_init(&parsed);
    ql_error error{};

    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_policy_result_parse(unterminated.data(), 0u, &parsed,
                                     &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "empty"));
}

TEST(Policy, ParsersStopAtTheLengthTheyWereGiven) {
    /* The valid document, followed by trailing garbage and no terminator. A
       parser that measured the pointer instead of honouring the length would
       either read the garbage or run off the end. */
    const std::string policy_json(kStrictPolicy);
    std::vector<char> buffer(policy_json.begin(), policy_json.end());
    const std::size_t length = buffer.size();
    buffer.insert(buffer.end(), 64u, '!');

    ql_policy *policy = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_policy_parse(nullptr, buffer.data(), length,
                                            &policy, &error))
        << error.message;
    EXPECT_NE(nullptr, policy);
    ql_policy_destroy(policy);
}

TEST(Policy, ClassifiesTheSameVerdictDifferentlyUnderTwoPolicies) {
    PolicyHandle strict(kStrictPolicy);
    PolicyHandle screening(kScreeningPolicy);
    ql_outcome_v1 outcome = make_outcome(QL_VERDICT_BOUNDED_CLEAN, 4096u);
    ql_policy_evidence_v1 evidence = evidence_of(0u, 0u);

    ql_policy_result_v1 strict_result =
        evaluate(strict.get(), outcome, evidence);
    EXPECT_EQ(QL_POLICY_DISPOSITION_ABSTAIN, strict_result.disposition);
    EXPECT_STREQ("open", strict_result.class_name);
    EXPECT_DOUBLE_EQ(0.0, strict_result.score);

    ql_policy_result_v1 screening_result =
        evaluate(screening.get(), outcome, evidence);
    EXPECT_EQ(QL_POLICY_DISPOSITION_PASS, screening_result.disposition);
    EXPECT_STREQ("clean", screening_result.class_name);
    EXPECT_DOUBLE_EQ(0.5, screening_result.score);
    /* Counting bounded clean as a pass never renames the verdict: the record
       still says BOUNDED_CLEAN and the claim is heuristic, not proof. */
    EXPECT_EQ(QL_VERDICT_BOUNDED_CLEAN, screening_result.effective_verdict);
    EXPECT_EQ(QL_POLICY_CLAIMS_HEURISTIC, screening_result.claims);
    EXPECT_EQ(0u, screening_result.gated);
}

TEST(Policy, AnUnreplayedCounterexampleIsWithdrawnAtRuntime) {
    PolicyHandle policy(kStrictPolicy);
    ql_outcome_v1 outcome = make_outcome(QL_VERDICT_COUNTEREXAMPLE);

    ql_policy_result_v1 without =
        evaluate(policy.get(), outcome, evidence_of(0u, 0u));
    EXPECT_EQ(1u, without.gated);
    EXPECT_STREQ("unreplayed-sat-model", without.gate_reason);
    EXPECT_EQ(QL_VERDICT_UNKNOWN, without.effective_verdict);
    EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, without.reported_verdict);
    EXPECT_EQ(QL_POLICY_DISPOSITION_ABSTAIN, without.disposition);

    ql_policy_result_v1 with =
        evaluate(policy.get(), outcome, evidence_of(1u, 0u));
    EXPECT_EQ(0u, with.gated);
    EXPECT_EQ(QL_VERDICT_COUNTEREXAMPLE, with.effective_verdict);
    EXPECT_EQ(QL_POLICY_DISPOSITION_FAIL, with.disposition);
    EXPECT_DOUBLE_EQ(-1.0, with.score);
}

TEST(Policy, AnUncheckedProofIsWithdrawnAtRuntime) {
    PolicyHandle policy(kStrictPolicy);
    ql_outcome_v1 outcome = make_outcome(QL_VERDICT_PROVED_EQUIVALENT, 0u);

    ql_policy_result_v1 without =
        evaluate(policy.get(), outcome, evidence_of(0u, 0u));
    EXPECT_EQ(1u, without.gated);
    EXPECT_STREQ("unchecked-proof", without.gate_reason);
    EXPECT_EQ(QL_VERDICT_UNKNOWN, without.effective_verdict);
    EXPECT_EQ(QL_POLICY_DISPOSITION_ABSTAIN, without.disposition);

    ql_policy_result_v1 with =
        evaluate(policy.get(), outcome, evidence_of(0u, 1u));
    EXPECT_EQ(0u, with.gated);
    EXPECT_EQ(QL_POLICY_DISPOSITION_PASS, with.disposition);
    EXPECT_EQ(QL_POLICY_CLAIMS_PROOF, with.claims);
}

TEST(Policy, ATrustedBackendProofNeedsThatExactBackend) {
    PolicyHandle policy(kScreeningPolicy);
    ql_outcome_v1 outcome = make_outcome(QL_VERDICT_PROVED_EQUIVALENT);

    ql_policy_result_v1 wrong =
        evaluate(policy.get(), outcome, evidence_of(0u, 0u, "cvc5 1.2.0"));
    EXPECT_EQ(1u, wrong.gated);
    EXPECT_STREQ("untrusted-backend", wrong.gate_reason);
    EXPECT_EQ(QL_VERDICT_UNKNOWN, wrong.effective_verdict);

    ql_policy_result_v1 missing =
        evaluate(policy.get(), outcome, evidence_of(0u, 0u, nullptr));
    EXPECT_EQ(1u, missing.gated);

    ql_policy_result_v1 right =
        evaluate(policy.get(), outcome, evidence_of(0u, 0u, "bitwuzla 0.9.1"));
    EXPECT_EQ(0u, right.gated);
    EXPECT_EQ(QL_POLICY_DISPOSITION_PASS, right.disposition);
    EXPECT_STREQ("proved", right.class_name);
}

TEST(Policy, AProvedVerdictInAnAbstainingClassIsNotPromoted) {
    PolicyHandle policy(kScreeningPolicy);
    /* proved_left_refines_right sits in the abstaining class. That class banks
       nothing on the verdict, so there is nothing to withdraw and nothing to
       promote: the verdict survives and the disposition is abstain. */
    ql_outcome_v1 outcome =
        make_outcome(QL_VERDICT_PROVED_LEFT_REFINES_RIGHT);

    ql_policy_result_v1 result =
        evaluate(policy.get(), outcome, evidence_of(0u, 0u, nullptr));
    EXPECT_EQ(0u, result.gated);
    EXPECT_EQ(QL_VERDICT_PROVED_LEFT_REFINES_RIGHT, result.effective_verdict);
    EXPECT_EQ(QL_POLICY_DISPOSITION_ABSTAIN, result.disposition);
    EXPECT_EQ(QL_POLICY_CLAIMS_NONE, result.claims);
    EXPECT_STREQ("open", result.class_name);
}

TEST(Policy, AnAbstainingClassStillMayNotClaimProofWithoutTrust) {
    const char *json = R"({
      "schema_version": 1,
      "name": "cheating",
      "classes": [
        {"name":"open","disposition":"abstain","claims":"proof",
         "verdicts":["proved_equivalent"]},
        {"name":"rest","disposition":"abstain","claims":"none",
         "verdicts":["unknown"]}
      ],
      "default_class": "rest"
    })";
    expect_rejected(json, "/classes/0/proof_trust",
                    "a raw solver UNSAT is not a proof");
}

TEST(Policy, ABudgetExhaustedOutcomeIsWithdrawnWhicheverWayItArrives) {
    PolicyHandle policy(kScreeningPolicy);

    ql_outcome_v1 flagged = make_outcome(QL_VERDICT_BOUNDED_CLEAN, 100u);
    flagged.flags |= QL_OUTCOME_FLAG_BUDGET_EXHAUSTED;
    ql_policy_result_v1 by_flag =
        evaluate(policy.get(), flagged, evidence_of(0u, 0u));
    EXPECT_EQ(1u, by_flag.gated);
    EXPECT_STREQ("budget-exhausted", by_flag.gate_reason);
    EXPECT_EQ(QL_VERDICT_UNKNOWN, by_flag.effective_verdict);
    EXPECT_EQ(0u, by_flag.checked_bound);
    EXPECT_EQ(QL_POLICY_DISPOSITION_ABSTAIN, by_flag.disposition);

    ql_outcome_v1 plain = make_outcome(QL_VERDICT_BOUNDED_CLEAN, 100u);
    ql_policy_evidence_v1 evidence = evidence_of(0u, 0u);
    evidence.budget_exhausted = 1u;
    ql_policy_result_v1 by_evidence = evaluate(policy.get(), plain, evidence);
    EXPECT_EQ(1u, by_evidence.gated);
    EXPECT_STREQ("budget-exhausted", by_evidence.gate_reason);
}

TEST(Policy, WeakeningIsAppliedAndRecorded) {
    const char *json = R"({
      "schema_version": 1,
      "name": "no-bounded-results",
      "classes": [
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown","bounded_clean"]}
      ],
      "default_class": "open",
      "weaken": [{"from":"bounded_clean","to":"unknown"}]
    })";
    PolicyHandle policy(json);
    ql_outcome_v1 outcome = make_outcome(QL_VERDICT_BOUNDED_CLEAN, 512u);

    ql_policy_result_v1 result =
        evaluate(policy.get(), outcome, evidence_of(0u, 0u));
    EXPECT_EQ(1u, result.weakened);
    EXPECT_EQ(0u, result.gated);
    EXPECT_EQ(QL_VERDICT_BOUNDED_CLEAN, result.reported_verdict);
    EXPECT_EQ(QL_VERDICT_UNKNOWN, result.effective_verdict);
    EXPECT_EQ(0u, result.checked_bound);
}

TEST(Policy, PolicySerializationRoundTrips) {
    PolicyHandle original(kScreeningPolicy);
    std::string once = serialize_policy(original.get());

    ql_policy *reparsed = nullptr;
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_policy_parse(nullptr, once.c_str(), once.size(),
                                            &reparsed, &error))
        << error.message << "\n" << once;
    std::string twice = serialize_policy(reparsed);
    EXPECT_EQ(once, twice);

    /* The reparsed policy has to classify identically, not just look alike. */
    ql_outcome_v1 outcome = make_outcome(QL_VERDICT_PROVED_EQUIVALENT);
    ql_policy_evidence_v1 evidence = evidence_of(0u, 0u, "bitwuzla 0.9.1");
    ql_policy_result_v1 before =
        evaluate(original.get(), outcome, evidence);
    ql_policy_result_v1 after = evaluate(reparsed, outcome, evidence);
    EXPECT_EQ(before.disposition, after.disposition);
    EXPECT_STREQ(before.class_name, after.class_name);
    EXPECT_DOUBLE_EQ(before.score, after.score);
    ql_policy_destroy(reparsed);
}

TEST(Policy, ResultSerializationRoundTrips) {
    PolicyHandle policy(kStrictPolicy);
    ql_outcome_v1 outcome = make_outcome(QL_VERDICT_COUNTEREXAMPLE, 7u);
    ql_policy_result_v1 result =
        evaluate(policy.get(), outcome, evidence_of(1u, 0u));

    std::string json = serialize_result(result);
    ql_policy_result_v1 parsed{};
    ql_policy_result_init(&parsed);
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_policy_result_parse(json.c_str(), json.size(),
                                                   &parsed, &error))
        << error.message << "\n" << json;

    EXPECT_STREQ(result.policy_name, parsed.policy_name);
    EXPECT_STREQ(result.class_name, parsed.class_name);
    EXPECT_STREQ(result.gate_reason, parsed.gate_reason);
    EXPECT_EQ(result.disposition, parsed.disposition);
    EXPECT_EQ(result.claims, parsed.claims);
    EXPECT_EQ(result.reported_verdict, parsed.reported_verdict);
    EXPECT_EQ(result.effective_verdict, parsed.effective_verdict);
    EXPECT_EQ(result.weakened, parsed.weakened);
    EXPECT_EQ(result.gated, parsed.gated);
    EXPECT_EQ(result.checked_bound, parsed.checked_bound);
    EXPECT_DOUBLE_EQ(result.score, parsed.score);

    EXPECT_EQ(json, serialize_result(parsed));
}

TEST(Policy, GatedResultSerializationRoundTrips) {
    PolicyHandle policy(kStrictPolicy);
    ql_outcome_v1 outcome = make_outcome(QL_VERDICT_PROVED_EQUIVALENT, 9u);
    ql_policy_result_v1 result =
        evaluate(policy.get(), outcome, evidence_of(0u, 0u));
    ASSERT_EQ(1u, result.gated);

    std::string json = serialize_result(result);
    EXPECT_NE(std::string::npos, json.find("\"gated\":true"));
    EXPECT_NE(std::string::npos,
              json.find("\"effective_verdict\":\"unknown\""));
    EXPECT_NE(std::string::npos,
              json.find("\"reported_verdict\":\"proved_equivalent\""));

    ql_policy_result_v1 parsed{};
    ql_policy_result_init(&parsed);
    ql_error error{};
    ASSERT_EQ(QL_STATUS_OK, ql_policy_result_parse(json.c_str(), json.size(),
                                                   &parsed, &error));
    EXPECT_EQ(json, serialize_result(parsed));
}

TEST(Policy, ResultParserRejectsAWrongSchemaVersion) {
    ql_policy_result_v1 parsed{};
    ql_policy_result_init(&parsed);
    ql_error error{};

    static const char kWrongSchema[] = "{\"schema_version\":9}";
    EXPECT_EQ(QL_STATUS_SCHEMA_MISMATCH,
              ql_policy_result_parse(kWrongSchema, std::strlen(kWrongSchema),
                                     &parsed, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "/schema_version"));
}

TEST(Policy, VerdictNamesRoundTrip) {
    const ql_verdict verdicts[] = {
        QL_VERDICT_UNKNOWN,          QL_VERDICT_PROVED_EQUIVALENT,
        QL_VERDICT_PROVED_LEFT_REFINES_RIGHT,
        QL_VERDICT_PROVED_RIGHT_REFINES_LEFT,
        QL_VERDICT_COUNTEREXAMPLE,   QL_VERDICT_BOUNDED_CLEAN};

    for (ql_verdict verdict : verdicts) {
        ql_verdict parsed = QL_VERDICT_UNKNOWN;
        ql_error error{};
        ASSERT_EQ(QL_STATUS_OK,
                  ql_policy_verdict_parse(ql_policy_verdict_name(verdict),
                                          &parsed, &error));
        EXPECT_EQ(verdict, parsed);
    }
}

}  // namespace
