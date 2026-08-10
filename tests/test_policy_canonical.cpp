/* The canonical form of a policy and of a policy result is a fixed point.

   Writing a policy, reading it back and writing it again must reach the same
   bytes. If it did not, the same policy would have two texts, and anything
   keyed on the text - a cache, a digest, a record of which policy a run was
   judged under - could answer for the wrong one.

   The property is checked as a property, over every policy the suite has and
   over a table of doubles chosen to hit the ways a number can fail to survive
   a round trip. It is stated generally on purpose: negative zero is what
   broke it (written as `-0`, read back as the integer zero, rewritten as
   `0`), but a test written only about negative zero would not have caught the
   next value that fails the same way. */

#include "quodlibet/policy.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct PolicyDeleter {
    void operator()(ql_policy *policy) const { ql_policy_destroy(policy); }
};

using PolicyPtr = std::unique_ptr<ql_policy, PolicyDeleter>;

PolicyPtr Parse(const std::string &json, ql_error *error) {
    ql_policy *policy = nullptr;
    const ql_status status =
        ql_policy_parse(nullptr, json.data(), json.size(), &policy, error);
    if (status != QL_STATUS_OK) {
        return PolicyPtr{};
    }
    return PolicyPtr{policy};
}

/* The writers report the size they need including the terminator, so the
   caller sizes and then writes. The JSON is that size minus one. */
std::string Serialize(const ql_policy *policy) {
    ql_error error{};
    std::size_t needed = 0u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_policy_serialize(policy, nullptr, 0u, &needed, &error));
    EXPECT_GT(needed, 1u);
    std::vector<char> buffer(needed);
    std::size_t written = 0u;
    EXPECT_EQ(QL_STATUS_OK,
              ql_policy_serialize(policy, buffer.data(), buffer.size(),
                                  &written, &error))
        << error.message;
    EXPECT_EQ(needed, written);
    return std::string(buffer.data(), written - 1u);
}

std::string SerializeResult(const ql_policy_result_v1 &result) {
    ql_error error{};
    std::size_t needed = 0u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_policy_result_serialize(&result, nullptr, 0u, &needed,
                                         &error));
    std::vector<char> buffer(needed);
    std::size_t written = 0u;
    EXPECT_EQ(QL_STATUS_OK,
              ql_policy_result_serialize(&result, buffer.data(),
                                         buffer.size(), &written, &error))
        << error.message;
    return std::string(buffer.data(), written - 1u);
}

/* A one-class policy carrying a chosen score, which is the field a double
   reaches the canonical form through. */
std::string PolicyWithScore(const std::string &score_text) {
    return std::string(R"({"schema_version":1,"name":"fixed-point",)"
                       R"("classes":[{"name":"open",)"
                       R"("disposition":"abstain","claims":"none",)"
                       R"("verdicts":["unknown"],"score":)") +
           score_text + R"(}],"default_class":"open"})";
}

/* Asserts the property itself: one round trip reaches a fixed point. */
void ExpectFixedPoint(const std::string &json) {
    ql_error error{};
    const PolicyPtr first = Parse(json, &error);
    ASSERT_NE(nullptr, first.get()) << error.message << "\n" << json;
    const std::string once = Serialize(first.get());

    const PolicyPtr second = Parse(once, &error);
    ASSERT_NE(nullptr, second.get())
        << "the canonical form was rejected by the parser that produced it: "
        << error.message << "\n" << once;
    const std::string twice = Serialize(second.get());

    EXPECT_EQ(once, twice)
        << "the canonical form moved on the first round trip";
}

const char *const kPolicies[] = {
    R"({"schema_version":1,"name":"strict","classes":[
        {"name":"accept","disposition":"pass","claims":"proof",
         "verdicts":["proved_equivalent"],"proof_trust":"checked","score":1.0},
        {"name":"refuted","disposition":"fail","claims":"evidence",
         "verdicts":["counterexample"],"require_replayed_witness":true,
         "score":-1.0},
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown","bounded_clean"],"score":0.0}],
        "default_class":"open"})",
    R"({"schema_version":1,"name":"trusted","classes":[
        {"name":"proved","disposition":"pass","claims":"proof",
         "verdicts":["proved_equivalent"],"proof_trust":"trusted_backend",
         "score":1.0},
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown"],"score":0.0}],
        "default_class":"open",
        "trust":{"trusted_backends":["bitwuzla 0.9.1"]}})",
    R"({"schema_version":1,"name":"weakening","classes":[
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown"],"score":0.0}],
        "default_class":"open",
        "weaken":[{"from":"bounded_clean","to":"unknown"}]})",
};

/* Values chosen for the ways a double can fail to survive being written and
   read: the sign of zero, integral values a reader may take as integers,
   values needing all seventeen significant digits, and the extremes. */
const char *const kScores[] = {
    "0.0",
    "-0.0",
    "0",
    "-0",
    "1.0",
    "-1.0",
    "1",
    "-1",
    "0.5",
    "-0.5",
    "0.1",
    "3.141592653589793",
    "2.2250738585072014e-308",
    "1.7976931348623157e308",
    "-1.7976931348623157e308",
    "1e-300",
    "123456789.123456789",
    "0.30000000000000004",
};

TEST(PolicyCanonicalForm, EveryPolicyIsAFixedPointOfOneRoundTrip) {
    for (std::size_t index = 0u;
         index < sizeof(kPolicies) / sizeof(kPolicies[0]); ++index) {
        SCOPED_TRACE(::testing::Message() << "policy " << index);
        ExpectFixedPoint(kPolicies[index]);
    }
}

TEST(PolicyCanonicalForm, EveryScoreSpellingIsAFixedPointOfOneRoundTrip) {
    for (std::size_t index = 0u;
         index < sizeof(kScores) / sizeof(kScores[0]); ++index) {
        SCOPED_TRACE(::testing::Message() << "score " << kScores[index]);
        ExpectFixedPoint(PolicyWithScore(kScores[index]));
    }
}

/* Two spellings of the same number must reach the same canonical text. This
   is the property that makes a text-keyed cache correct: equal policies have
   equal keys. */
TEST(PolicyCanonicalForm, BothZerosReachTheSameCanonicalText) {
    ql_error error{};
    const PolicyPtr positive = Parse(PolicyWithScore("0.0"), &error);
    const PolicyPtr negative = Parse(PolicyWithScore("-0.0"), &error);

    ASSERT_NE(nullptr, positive.get()) << error.message;
    ASSERT_NE(nullptr, negative.get()) << error.message;
    EXPECT_EQ(Serialize(positive.get()), Serialize(negative.get()));
    /* And the canonical spelling is the one without the sign, because a
       reader that takes `-0` as an integer would drop the sign anyway. */
    EXPECT_NE(std::string::npos,
              Serialize(negative.get()).find("\"score\":0"));
    EXPECT_EQ(std::string::npos,
              Serialize(negative.get()).find("-0"));
}

/* The result writer shares the number writer, so the same property has to
   hold for it. A result is what a scorer records; two texts for one result
   would make a recorded run ambiguous. */
TEST(PolicyCanonicalForm, EveryResultScoreIsAFixedPointOfOneRoundTrip) {
    const double scores[] = {0.0,
                             -0.0,
                             1.0,
                             -1.0,
                             0.5,
                             3.141592653589793,
                             std::numeric_limits<double>::min(),
                             std::numeric_limits<double>::max()};

    for (std::size_t index = 0u;
         index < sizeof(scores) / sizeof(scores[0]); ++index) {
        ql_policy_result_v1 result{};
        ql_error error{};
        SCOPED_TRACE(::testing::Message() << "result score index " << index);

        ql_policy_result_init(&result);
        result.score = scores[index];
        std::snprintf(result.policy_name, sizeof(result.policy_name), "%s",
                      "fixed-point");
        std::snprintf(result.class_name, sizeof(result.class_name), "%s",
                      "open");
        const std::string once = SerializeResult(result);

        ql_policy_result_v1 parsed{};
        ql_policy_result_init(&parsed);
        ASSERT_EQ(QL_STATUS_OK,
                  ql_policy_result_parse(once.data(), once.size(), &parsed,
                                         &error))
            << error.message << "\n" << once;
        EXPECT_EQ(once, SerializeResult(parsed))
            << "the canonical result form moved on the first round trip";
    }
}

/* Negative zero and positive zero are the same number, so a result carrying
   either must serialize identically. */
TEST(PolicyCanonicalForm, AResultScoreOfEitherZeroReachesTheSameText) {
    ql_policy_result_v1 positive{};
    ql_policy_result_v1 negative{};

    ql_policy_result_init(&positive);
    ql_policy_result_init(&negative);
    positive.score = 0.0;
    negative.score = -0.0;
    ASSERT_TRUE(std::signbit(negative.score))
        << "this fixture needs an actual negative zero";
    EXPECT_EQ(SerializeResult(positive), SerializeResult(negative));
}

}  // namespace
