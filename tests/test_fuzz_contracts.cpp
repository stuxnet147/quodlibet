/* A bounded, deterministic fuzz campaign over the contract surfaces: the
   typed-precondition parser, the problem decoder, the source-signature
   decoder, and the judgement-policy JSON.

   It shares tests/fuzz/fuzz_contract_targets.h with the libFuzzer drivers in
   tests/fuzz/, which only exist under the linux-fuzz preset. This is what
   keeps those surfaces fuzzed on every ctest run and on Windows, and sharing
   the targets means the two cannot drift into disagreeing about what has been
   tested. */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace ql_fuzz_contracts {
namespace {

struct Violation {
    bool failed = false;
    std::string message;
};

/* A campaign that never reaches its target passes without testing anything.
   Counting the arrivals lets the driver fail on that instead of reporting a
   green run. */
struct Reach {
    std::size_t precondition = 0u;
    std::size_t problem_v1 = 0u;
    std::size_t problem_v2 = 0u;
    std::size_t signature = 0u;
    std::size_t policy = 0u;
    std::size_t policy_result = 0u;
};

Violation g_violation;
Reach g_reach;
/* The input the campaign is currently on. A violation that only reported its
   sentence would leave the next reader re-running the whole campaign to find
   out which byte string produced it. */
std::string g_input;

std::string Escape(const std::string &input) {
    static const char kHex[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() + 16u);
    for (std::size_t index = 0u; index < input.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(input[index]);
        if (byte >= 0x20u && byte < 0x7fu && byte != '\\') {
            output.push_back(static_cast<char>(byte));
        } else {
            output += "\\x";
            output.push_back(kHex[byte >> 4]);
            output.push_back(kHex[byte & 0x0fu]);
        }
    }
    return output;
}

void Record(bool condition, const char *message) {
    if (condition || g_violation.failed) {
        return;
    }
    g_violation.failed = true;
    g_violation.message = message != nullptr ? message : "(no reason given)";
    g_violation.message += "\n  input: ";
    g_violation.message += Escape(g_input);
}

}  // namespace
}  // namespace ql_fuzz_contracts

#define QL_FUZZ_REQUIRE(condition_, message_) \
    ::ql_fuzz_contracts::Record((condition_) != 0, (message_))
#define QL_FUZZ_REACHED(kind_) (++::ql_fuzz_contracts::g_reach.kind_)

#include "fuzz/fuzz_contract_targets.h"

#include <gtest/gtest.h>

#include "w2_fixtures.h"

namespace {

/* Records the input before handing it to the target, so a violated invariant
   names the byte string that produced it. */
template <typename Target>
void RunTarget(Target target, const void *data, std::size_t size) {
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    ql_fuzz_contracts::g_input.assign(
        reinterpret_cast<const char *>(bytes), size);
    target(bytes, size);
}

uint64_t NextRandom(uint64_t *state) {
    uint64_t value;
    *state += UINT64_C(0x9e3779b97f4a7c15);
    value = *state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

/* The signature descriptor ql_fuzz_precondition() reads off the front of its
   input, spelling the seven-argument signature the precondition tests use:
   s32, u32, pointer, u64, pointer, bool, s64 with a 64-bit pointer width.
   Mutation reaches other signatures on its own; starting from a rich one is
   what gets the type checker exercised at all. */
const char kSignaturePrefix[] = {
    static_cast<char>(15), static_cast<char>(9),  static_cast<char>(10),
    static_cast<char>(3),  static_cast<char>(14), static_cast<char>(3),
    static_cast<char>(0),  static_cast<char>(13),
};

const char *const kPreconditionSeeds[] = {
    R"({"schema_version":1,"expression":true})",
    R"({"schema_version":1,"expression":{"op":"arg","index":5}})",
    R"({"schema_version":1,"expression":{"op":"slt",
        "left":{"op":"arg","index":0},
        "right":{"op":"int","signed":true,"width":32,"value":"100"}}})",
    R"({"schema_version":1,"expression":{"op":"ult",
        "left":{"op":"uadd","left":{"op":"arg","index":1},
                "right":{"op":"int","signed":false,"width":32,"value":"1"}},
        "right":{"op":"int","signed":false,"width":32,"value":"100"}}})",
    R"({"schema_version":1,"expression":{"op":"valid_range","range":{
        "pointer":{"op":"arg","index":2},
        "offset":{"op":"arg","index":6},
        "bytes":{"op":"arg","index":3}},
        "read":true,"write":false,"alignment":8,
        "nullable":false,"alias_group":1}})",
    R"({"schema_version":1,"expression":{"op":"aligned",
        "pointer":{"op":"arg","index":2},
        "offset":{"op":"int","signed":true,"width":64,"value":"0"},
        "alignment":16}})",
    R"({"schema_version":1,"expression":{"op":"disjoint",
        "left":{"pointer":{"op":"arg","index":2},
                "offset":{"op":"int","signed":true,"width":64,"value":"0"},
                "bytes":{"op":"arg","index":3}},
        "right":{"pointer":{"op":"arg","index":4},
                 "offset":{"op":"int","signed":true,"width":64,"value":"0"},
                 "bytes":{"op":"arg","index":3}}}})",
    R"({"schema_version":1,"expression":{"op":"and","args":[
        {"op":"arg","index":5},
        {"op":"not","value":{"op":"eq",
            "left":{"op":"arg","index":2},"right":{"op":"arg","index":4}}}]}})",
    R"({"schema_version":1,"expression":{"op":"implies",
        "left":{"op":"arg","index":5},
        "right":{"op":"sge","left":{"op":"arg","index":6},
                 "right":{"op":"int","signed":true,"width":64,"value":"0"}}}})",
    R"({"schema_version":1,"expression":{"op":"or","args":[
        {"op":"arg","index":5},false]}})",
};

const char *const kPolicySeeds[] = {
    R"({"schema_version":1,"name":"strict","classes":[
        {"name":"accept","disposition":"pass","claims":"proof",
         "verdicts":["proved_equivalent"],"proof_trust":"checked","score":1.0},
        {"name":"refuted","disposition":"fail","claims":"evidence",
         "verdicts":["counterexample"],"require_replayed_witness":true,
         "score":-1.0},
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown","bounded_clean"],"score":0.0}],
        "default_class":"open"})",
    R"({"schema_version":1,"name":"fast-filter","classes":[
        {"name":"clean","disposition":"pass","claims":"heuristic",
         "verdicts":["bounded_clean"],"score":0.5},
        {"name":"proved","disposition":"pass","claims":"proof",
         "verdicts":["proved_equivalent","proved_left_refines_right"],
         "proof_trust":"trusted_backend","score":1.0},
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown","proved_right_refines_left"],"score":0.0}],
        "default_class":"open",
        "trust":{"trusted_backends":["bitwuzla 0.9.1"]}})",
    R"({"schema_version":1,"name":"weakening","classes":[
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown"],"score":0.0}],
        "default_class":"open",
        "weaken":[{"from":"bounded_clean","to":"unknown"},
                  {"from":"unknown","to":"unknown"}]})",
    /* The rejections the schema exists for. A mutation that turns one of
       these into an accepted policy is exactly what the invariants catch. */
    R"({"schema_version":1,"name":"promote","classes":[
        {"name":"accept","disposition":"pass","claims":"proof",
         "verdicts":["bounded_clean"],"score":1.0}],
        "default_class":"accept"})",
    R"({"schema_version":1,"name":"unreplayed","classes":[
        {"name":"refuted","disposition":"fail","claims":"evidence",
         "verdicts":["counterexample"],"score":-1.0}],
        "default_class":"refuted"})",
    R"({"schema_version":1,"name":"untrusted","classes":[
        {"name":"accept","disposition":"pass","claims":"proof",
         "verdicts":["proved_equivalent"],"score":1.0}],
        "default_class":"accept"})",
    /* A score of -0.0, which is what first broke the canonical form: it was
       written as `-0`, read back as the integer zero and rewritten as `0`.
       Kept as a seed so mutation keeps exploring numeric spellings from a
       shape that already reached the writer. The same input is in
       tests/fuzz/corpus/policy/ for the coverage-guided drivers. */
    R"({"schema_version":1,"name":"fixed-point","classes":[
        {"name":"open","disposition":"abstain","claims":"none",
         "verdicts":["unknown"],"score":-0.0}],
        "default_class":"open"})",
};

/* Tokens the two JSON schemas care about. Splicing these in reaches
   structurally interesting inputs that byte flipping almost never produces. */
const char *const kJsonTokens[] = {
    "\"op\"", "\"arg\"", "\"int\"", "\"index\"", "\"width\"", "\"signed\"",
    "\"value\"", "\"expression\"", "\"schema_version\"", "\"args\"",
    "\"left\"", "\"right\"", "\"range\"", "\"pointer\"", "\"offset\"",
    "\"bytes\"", "\"alignment\"", "\"nullable\"", "\"alias_group\"",
    "\"read\"", "\"write\"", "\"valid_range\"", "\"aligned\"", "\"disjoint\"",
    "\"implies\"", "\"not\"", "\"and\"", "\"or\"", "\"slt\"", "\"ugt\"",
    "\"umul\"", "\"ssub\"", "\"classes\"", "\"disposition\"", "\"claims\"",
    "\"verdicts\"", "\"proof_trust\"", "\"require_replayed_witness\"",
    "\"default_class\"", "\"weaken\"", "\"trust\"", "\"trusted_backends\"",
    "\"proof\"", "\"checked\"", "\"trusted_backend\"", "\"pass\"",
    "\"proved_equivalent\"", "\"counterexample\"", "\"bounded_clean\"",
    "\"unknown\"", "true", "false", "null", "0", "1", "-1", "64", "4096",
    "18446744073709551616", "1e400", "{", "}", "[", "]", ":", ",", "\"",
    "\\u0000", "\\", "\n",
};

std::string MutateText(const std::string &input, uint64_t *state,
                       bool gentle) {
    std::string output = input;
    const std::size_t rounds =
        gentle ? 1u + (NextRandom(state) % 2u) : 1u + (NextRandom(state) % 6u);

    for (std::size_t round = 0u; round < rounds; ++round) {
        if (output.size() > 8192u) {
            output.resize(8192u);
        }
        /* Half the gentle edits keep the text valid JSON. Byte flipping alone
           almost always lands inside a keyword, so a campaign built on it
           reaches the syntax errors over and over and the type checker
           hardly at all. Retyping a digit is what actually varies a width, an
           argument index, an alignment or a bound. */
        if (gentle && (NextRandom(state) & 1u) != 0u) {
            const std::size_t at =
                output.empty() ? 0u : NextRandom(state) % output.size();
            if (!output.empty() && output[at] >= '0' && output[at] <= '9') {
                output[at] = static_cast<char>('0' + (NextRandom(state) % 10u));
            } else {
                output.insert(at, 1u, ' ');
            }
            continue;
        }
        switch (NextRandom(state) % (gentle ? 3u : 6u)) {
        case 0u:
            if (!output.empty()) {
                output[NextRandom(state) % output.size()] ^=
                    static_cast<char>(1u << (NextRandom(state) % 8u));
            }
            break;
        case 1u: {
            const char *token =
                kJsonTokens[NextRandom(state) %
                            (sizeof(kJsonTokens) / sizeof(kJsonTokens[0]))];
            const std::size_t at =
                output.empty() ? 0u : NextRandom(state) % output.size();
            output.insert(at, token);
            break;
        }
        case 2u:
            if (!output.empty()) {
                output[NextRandom(state) % output.size()] =
                    static_cast<char>(NextRandom(state) % 128u);
            }
            break;
        case 3u:
            if (output.size() > 1u) {
                const std::size_t at = NextRandom(state) % output.size();
                const std::size_t count =
                    1u + (NextRandom(state) % (output.size() - at));
                output.erase(at, count);
            }
            break;
        case 4u: {
            /* Duplicating a span is how a member gets stated twice, which is
               the case a hand-rolled object reader is most likely to get
               wrong. */
            if (output.size() > 8u) {
                const std::size_t at = NextRandom(state) % output.size();
                const std::size_t count =
                    1u + (NextRandom(state) % (output.size() - at));
                output.insert(at, output.substr(at, count));
            }
            break;
        }
        default:
            if (!output.empty()) {
                /* Every parser takes an explicit size, so nothing may assume
                   the text stops at the first zero byte. */
                output[NextRandom(state) % output.size()] = '\0';
            }
            break;
        }
    }
    return output;
}

/* `gentle` keeps the artifact's framing intact. Heavy mutation is what
   reaches the length and extent checks at the front of a decoder, but it
   almost never leaves a payload the decoder still accepts, and a campaign
   that never decodes cannot check what an accepted artifact must promise. */
std::vector<uint8_t> MutateBytes(const std::vector<uint8_t> &input,
                                 uint64_t *state, bool gentle) {
    std::vector<uint8_t> output = input;
    const std::size_t rounds =
        gentle ? 1u : 1u + (NextRandom(state) % 5u);

    for (std::size_t round = 0u; round < rounds; ++round) {
        if (output.empty()) {
            output.push_back(static_cast<uint8_t>(NextRandom(state)));
            continue;
        }
        if (gentle) {
            /* One bit, and never in the header the reader checks first, so
               the payload keeps its identity as a problem or a signature. */
            const std::size_t at =
                output.size() <= 16u
                    ? NextRandom(state) % output.size()
                    : 16u + (NextRandom(state) % (output.size() - 16u));
            output[at] ^= static_cast<uint8_t>(1u << (NextRandom(state) % 8u));
            continue;
        }
        switch (NextRandom(state) % 5u) {
        case 0u:
            output[NextRandom(state) % output.size()] ^=
                static_cast<uint8_t>(1u << (NextRandom(state) % 8u));
            break;
        case 1u:
            output[NextRandom(state) % output.size()] =
                static_cast<uint8_t>(NextRandom(state));
            break;
        case 2u:
            /* Counts and offsets live in little-endian 32-bit fields, so
               raising one whole field reaches a table extent far more often
               than flipping single bits does. */
            if (output.size() >= 4u) {
                const std::size_t at =
                    (NextRandom(state) % (output.size() - 3u)) &
                    ~std::size_t{3u};
                const uint32_t value = static_cast<uint32_t>(
                    NextRandom(state) % 5u == 0u ? UINT32_MAX
                                                 : NextRandom(state) %
                                                       0x10000u);
                std::memcpy(output.data() + at, &value, sizeof(value));
            }
            break;
        case 3u:
            output.resize(NextRandom(state) % (output.size() + 1u));
            break;
        default:
            output.insert(output.begin() +
                              static_cast<std::ptrdiff_t>(
                                  NextRandom(state) % (output.size() + 1u)),
                          static_cast<uint8_t>(NextRandom(state)));
            break;
        }
    }
    return output;
}

std::vector<uint8_t> ArtifactBytes(const ql_artifact *artifact) {
    ql_artifact_view view{};
    ql_error error{};
    std::vector<uint8_t> bytes;

    view.struct_size = sizeof(view);
    if (ql_artifact_get_view(artifact, &view, &error) != QL_STATUS_OK) {
        ADD_FAILURE() << error.message;
        return bytes;
    }
    const uint8_t *data = static_cast<const uint8_t *>(view.data);
    bytes.assign(data, data + view.size);
    return bytes;
}

const char *const kLeftSource = "int add(int a, int b) { return a + b; }";
const char *const kRightSource =
    "int sum(int x, int y) { int t; t = y + x; return t; }";

/* A schema v1 and a schema v2 problem payload over the same two lowered
   functions, so the decoder campaign starts from bytes that actually decode
   rather than from noise the length checks reject up front. */
struct ProblemSeeds {
    std::vector<uint8_t> v1;
    std::vector<uint8_t> v2;
    std::vector<uint8_t> signature;
};

ProblemSeeds BuildSeeds() {
    ProblemSeeds seeds;
    w2::CFunction left;
    w2::CFunction right;
    ql_artifact *artifact = nullptr;
    ql_error error{};

    w2::BuildOrFail(&left, kLeftSource, "add");
    w2::BuildOrFail(&right, kRightSource, "sum");
    if (::testing::Test::HasFatalFailure()) {
        return seeds;
    }
    seeds.signature = ArtifactBytes(left.signature_artifact());

    ql_problem_definition_v1 definition{};
    ql_problem_definition_init(&definition);
    definition.left_source = left.source().c_str();
    definition.left_source_size = left.source().size();
    definition.left_function_name = "add";
    definition.left_function_name_size = 3u;
    definition.right_source = right.source().c_str();
    definition.right_source_size = right.source().size();
    definition.right_function_name = "sum";
    definition.right_function_name_size = 3u;
    if (ql_problem_artifact_create(nullptr, &definition, &artifact, &error) ==
        QL_STATUS_OK) {
        seeds.v1 = ArtifactBytes(artifact);
    } else {
        ADD_FAILURE() << error.message;
    }
    ql_artifact_release(artifact);
    artifact = nullptr;

    constexpr char kPrecondition[] =
        R"({"schema_version":1,"expression":{"op":"slt",)"
        R"("left":{"op":"arg","index":0},)"
        R"("right":{"op":"int","signed":true,"width":32,"value":"100"}}})";
    ql_problem_definition_v2 definition2{};
    ql_problem_definition_v2_init(&definition2);
    definition2.left_source = left.source().c_str();
    definition2.left_source_size = left.source().size();
    definition2.left_function_name = "add";
    definition2.left_function_name_size = 3u;
    definition2.right_source = right.source().c_str();
    definition2.right_source_size = right.source().size();
    definition2.right_function_name = "sum";
    definition2.right_function_name_size = 3u;
    definition2.left_signature = left.signature_artifact();
    definition2.right_signature = right.signature_artifact();
    ql_problem_argument_binding_v1 bindings[2]{};
    for (std::uint32_t index = 0u; index < 2u; ++index) {
        bindings[index].struct_size = sizeof(bindings[index]);
        bindings[index].left_index = index;
        bindings[index].right_index = index;
    }
    definition2.argument_bindings = bindings;
    definition2.argument_binding_count = 2u;
    definition2.contract.precondition_json = kPrecondition;
    definition2.contract.precondition_json_size = sizeof(kPrecondition) - 1u;
    if (ql_problem_artifact_create_v2(nullptr, &definition2, &artifact,
                                      &error) == QL_STATUS_OK) {
        seeds.v2 = ArtifactBytes(artifact);
    } else {
        ADD_FAILURE() << error.message;
    }
    ql_artifact_release(artifact);
    return seeds;
}

ql_fuzz_contracts::Reach ExpectNoViolation() {
    const ql_fuzz_contracts::Reach reach = ql_fuzz_contracts::g_reach;
    EXPECT_FALSE(ql_fuzz_contracts::g_violation.failed)
        << ql_fuzz_contracts::g_violation.message;
    /* Printing the reach keeps the campaign honest in the log: a run whose
       counts collapse is a run that stopped testing what it claims to. */
    std::cout << "[  REACH   ] precondition=" << reach.precondition
              << " problem_v1=" << reach.problem_v1
              << " problem_v2=" << reach.problem_v2
              << " signature=" << reach.signature
              << " policy=" << reach.policy
              << " policy_result=" << reach.policy_result << std::endl;
    ql_fuzz_contracts::g_violation = ql_fuzz_contracts::Violation{};
    ql_fuzz_contracts::g_reach = ql_fuzz_contracts::Reach{};
    return reach;
}

std::string WithSignaturePrefix(const std::string &json) {
    return std::string(kSignaturePrefix, sizeof(kSignaturePrefix)) + json;
}

TEST(FuzzContracts, PreconditionParserSurvivesMutatedContracts) {
    uint64_t state = UINT64_C(0x51f0c3a6d827b419);
    for (std::size_t round = 0u; round < 120000u; ++round) {
        const std::string seed =
            kPreconditionSeeds[NextRandom(&state) %
                               (sizeof(kPreconditionSeeds) /
                                sizeof(kPreconditionSeeds[0]))];
        /* Three rounds in four keep most of the seed intact so the campaign
           actually reaches an accepted contract; the fourth mutates hard to
           reach the rejection paths. */
        const std::string mutated =
            WithSignaturePrefix(MutateText(seed, &state, (round & 3u) != 0u));
        RunTarget(ql_fuzz_precondition, mutated.data(), mutated.size());
    }
    /* A campaign that never parsed a contract says nothing about the
       canonicalisation invariants. */
    EXPECT_GT(ExpectNoViolation().precondition, 0u);
}

TEST(FuzzContracts, PreconditionParserSurvivesArbitraryBytes) {
    uint64_t state = UINT64_C(0x0b9d4e27fa315c68);
    for (std::size_t round = 0u; round < 60000u; ++round) {
        std::vector<uint8_t> input(NextRandom(&state) % 256u);
        for (std::size_t index = 0u; index < input.size(); ++index) {
            input[index] = static_cast<uint8_t>(NextRandom(&state));
        }
        RunTarget(ql_fuzz_precondition, input.data(), input.size());
    }
    ExpectNoViolation();
}

TEST(FuzzContracts, ProblemDecoderNeverLetsASchemaV1ProblemProveAnything) {
    const ProblemSeeds seeds = BuildSeeds();
    uint64_t state = UINT64_C(0x3a7c1e58d90b2f46);
    ASSERT_FALSE(seeds.v1.empty());
    ASSERT_FALSE(seeds.v2.empty());
    for (std::size_t round = 0u; round < 120000u; ++round) {
        const std::vector<uint8_t> &seed =
            (round & 1u) != 0u ? seeds.v2 : seeds.v1;
        const std::vector<uint8_t> input =
            MutateBytes(seed, &state, (round & 3u) != 0u);
        RunTarget(ql_fuzz_problem_decoder, input.data(), input.size());
    }
    const ql_fuzz_contracts::Reach reach = ExpectNoViolation();
    /* Mutations that never decode would only be exercising the length checks
       at the front of the reader. Both schemas have to be reached, because
       the proof-binding gate is the property that differs between them. */
    EXPECT_GT(reach.problem_v1, 0u);
    EXPECT_GT(reach.problem_v2, 0u);
}

TEST(FuzzContracts, SignatureDecoderSurvivesMutatedArtifacts) {
    const ProblemSeeds seeds = BuildSeeds();
    uint64_t state = UINT64_C(0x62d8b409ae15c73f);
    ASSERT_FALSE(seeds.signature.empty());
    for (std::size_t round = 0u; round < 120000u; ++round) {
        const std::vector<uint8_t> input =
            MutateBytes(seeds.signature, &state, (round & 3u) != 0u);
        RunTarget(ql_fuzz_signature_decoder, input.data(), input.size());
    }
    EXPECT_GT(ExpectNoViolation().signature, 0u);
}

/* tests/fuzz/corpus/policy/empty-input and its policy_result twin, replayed
   here so the crash they found stays found. The 2026-08-10 campaign died on
   both targets at the first execution: the parsers read json_size == 0 as a
   request to call strlen() on a buffer libFuzzer had allocated to the exact
   input size, with no terminator. The vector is one byte with no NUL in it,
   which is what makes a read past the length a heap overflow the sanitizer
   catches rather than a silent walk into whatever follows. */
TEST(FuzzContracts, PolicyTargetsSurviveAnEmptyInput) {
    std::vector<char> unterminated(1u, '{');

    RunTarget(ql_fuzz_policy, unterminated.data(), 0u);
    RunTarget(ql_fuzz_policy_result, unterminated.data(), 0u);
    ExpectNoViolation();
}

TEST(FuzzContracts, PolicyParserNeverAcceptsAPromotingPolicy) {
    uint64_t state = UINT64_C(0x74e2af106b3d5c98);
    for (std::size_t round = 0u; round < 120000u; ++round) {
        const std::string seed =
            kPolicySeeds[NextRandom(&state) %
                         (sizeof(kPolicySeeds) / sizeof(kPolicySeeds[0]))];
        const std::string mutated =
            MutateText(seed, &state, (round & 3u) != 0u);
        RunTarget(ql_fuzz_policy, mutated.data(), mutated.size());
    }
    EXPECT_GT(ExpectNoViolation().policy, 0u);
}

TEST(FuzzContracts, PolicyResultParserSurvivesMutatedResults) {
    /* Seeded from the canonical serialisation of a real evaluation so the
       campaign starts inside the grammar rather than outside it. */
    ql_policy *policy = nullptr;
    ql_policy_result_v1 result{};
    ql_policy_evidence_v1 evidence{};
    ql_outcome_v1 outcome{};
    ql_error error{};
    std::vector<char> buffer(4096u);
    std::size_t written = 0u;
    uint64_t state = UINT64_C(0x1c96e30845b7fa2d);
    ASSERT_EQ(QL_STATUS_OK,
              ql_policy_parse(nullptr, kPolicySeeds[0],
                              std::strlen(kPolicySeeds[0]), &policy, &error))
        << error.message;
    ql_policy_evidence_init(&evidence);
    ql_policy_result_init(&result);
    outcome.struct_size = sizeof(outcome);
    outcome.schema_version = 1u;
    outcome.verdict = QL_VERDICT_UNKNOWN;
    ASSERT_EQ(QL_STATUS_OK, ql_policy_evaluate(policy, &outcome, &evidence,
                                               &result, &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_policy_result_serialize(&result, buffer.data(),
                                         buffer.size(), &written, &error))
        << error.message;
    ql_policy_destroy(policy);

    /* `written` counts the terminator, which is not part of the JSON. */
    const std::string seed(buffer.data(), written - 1u);
    for (std::size_t round = 0u; round < 120000u; ++round) {
        const std::string mutated =
            MutateText(seed, &state, (round & 3u) != 0u);
        RunTarget(ql_fuzz_policy_result, mutated.data(), mutated.size());
    }
    EXPECT_GT(ExpectNoViolation().policy_result, 0u);
}

}  // namespace
