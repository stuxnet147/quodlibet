/* A bounded, deterministic fuzz campaign over the same targets the libFuzzer
   drivers in tests/fuzz/ use.

   The coverage-guided fuzzers only exist under the linux-fuzz preset, so this
   is what keeps the parser, the lowering, and the IR decoder fuzzed on every
   ctest run and on Windows. Sharing tests/fuzz/fuzz_targets.h means the two
   cannot drift into disagreeing about what has been tested. */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace ql_fuzz {
namespace {

struct Violation {
    bool failed = false;
    std::string message;
};

/* A campaign that never reaches its target passes without testing anything.
   Counting the arrivals lets the driver fail on that instead of reporting a
   green run. */
struct Reach {
    std::size_t parsed = 0u;
    std::size_t lowered = 0u;
    std::size_t decoded = 0u;
    std::size_t verified = 0u;
};

Violation g_violation;
Reach g_reach;

void Record(bool condition, const char *message) {
    if (condition || g_violation.failed) {
        return;
    }
    g_violation.failed = true;
    g_violation.message = message != nullptr ? message : "(no reason given)";
}

}  // namespace
}  // namespace ql_fuzz

#define QL_FUZZ_REQUIRE(condition_, message_) \
    ::ql_fuzz::Record((condition_) != 0, (message_))
#define QL_FUZZ_REACHED(kind_) (++::ql_fuzz::g_reach.kind_)

#include "fuzz/fuzz_targets.h"

#include <gtest/gtest.h>

namespace {

uint64_t NextRandom(uint64_t *state) {
    uint64_t value;
    *state += UINT64_C(0x9e3779b97f4a7c15);
    value = *state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

const char *const kSeeds[] = {
    "int f(int a) { return a; }",
    "int f(int a, int b) { return a / b; }",
    "unsigned int f(unsigned int a, int b) { return a << b; }",
    "int f(int a) { if (a < 3) { return 0; } return 1; }",
    "int f(int a, int b) {\n"
    "  int x;\n"
    "  if (b) { x = a % b; } else { x = 0; }\n"
    "  return x;\n"
    "}",
    "long long f(long long a, long long b) { return a * b - 1; }",
    "short f(short a, short b) { return a + b; }",
    "int f(int a, int b) { return b != 0 && a / b > 1; }",
    "_Bool f(int a) { return a > 0; }",
    "void f(int a) { int b; b = a + 1; }",
    ("typedef int TYP_0;\ntypedef TYP_0 TYP_1;\n"
     "TYP_1 f(TYP_0 a) { return (TYP_1)(a + 1); }"),
    "typedef int *TYP_0;\nint f(TYP_0 a) { return 0; }",
    "int f(int *p) { return *p; }",
    "struct s { int a; };\nint f(struct s v) { return v.a; }",
    "int f(void) { for (;;) { } }",
    "#define M 1\nint f(int a) { return a + M; }",
    "int f(int a, ...) { return a; }",
    "int f();",
    "",
};

/* Tokens the C grammar cares about. Splicing these in reaches syntactically
   interesting inputs that pure byte flipping almost never produces. */
const char *const kTokens[] = {
    "int",   "unsigned", "long",    "short",  "char",  "_Bool",  "void",
    "if",    "else",     "while",   "return", "goto",  "switch", "case",
    "const", "volatile", "_Atomic", "struct", "union", "enum",   "static",
    "typedef", "TYP_0",  "*",       "&",      "[",     "]",      "(",
    ")",     "{",        "}",       ";",      ",",     "/",      "%",
    "<<",    ">>",       "?",       ":",      "->",    ".",      "...",
    "sizeof", "0x",      "9223372036854775808", "'\\0'", "\"\"", "\\",
    "\n",    "//",       "/*",
};

/* `gentle` keeps most of the seed intact. Heavy mutation is what reaches the
   parser's error recovery, but it almost never leaves a source the lowering
   still accepts, and a campaign that never lowers cannot check the lowering's
   invariants. The two modes aim at different targets on purpose. */
std::string Mutate(const std::string &input, uint64_t *state, bool gentle) {
    std::string output = input;
    const std::size_t rounds =
        gentle ? 1u + (NextRandom(state) % 2u) : 1u + (NextRandom(state) % 6u);

    for (std::size_t round = 0u; round < rounds; ++round) {
        const uint64_t choice =
            gentle ? NextRandom(state) % 3u : NextRandom(state) % 6u;
        if (output.size() > 4096u) {
            output.resize(4096u);
        }
        switch (choice) {
        case 0u:
            if (!output.empty()) {
                output[NextRandom(state) % output.size()] ^=
                    static_cast<char>(1u << (NextRandom(state) % 8u));
            }
            break;
        case 1u:
            if (!output.empty()) {
                output[NextRandom(state) % output.size()] =
                    static_cast<char>(NextRandom(state) % 128u);
            }
            break;
        case 2u: {
            const char *token =
                kTokens[NextRandom(state) %
                        (sizeof(kTokens) / sizeof(kTokens[0]))];
            const std::size_t at =
                output.empty() ? 0u : NextRandom(state) % output.size();
            output.insert(at, token);
            break;
        }
        case 3u:
            if (output.size() > 1u) {
                const std::size_t at = NextRandom(state) % output.size();
                const std::size_t count =
                    1u + (NextRandom(state) % (output.size() - at));
                output.erase(at, count);
            }
            break;
        case 4u: {
            const char *seed =
                kSeeds[NextRandom(state) %
                       (sizeof(kSeeds) / sizeof(kSeeds[0]))];
            const std::size_t at =
                output.empty() ? 0u : NextRandom(state) % output.size();
            output.insert(at, seed);
            break;
        }
        default:
            if (!output.empty()) {
                /* An embedded NUL is a legitimate hostile input: the API
                   takes an explicit size, so nothing may assume termination
                   at the first zero byte. */
                output[NextRandom(state) % output.size()] = '\0';
            }
            break;
        }
    }
    return output;
}

std::vector<uint8_t> ValidIrBytes() {
    ql_c_frontend_unit *unit = nullptr;
    ql_c_lower_result *result = nullptr;
    ql_c_function_view function{};
    ql_c_lower_result_view_v1 view{};
    ql_artifact_view artifact{};
    ql_error error{};
    std::vector<uint8_t> bytes;
    const char *source =
        "int seed(int a, int b) {\n"
        "  int x;\n"
        "  if (b) { x = a / b; } else { x = a << 2; }\n"
        "  return x + 1;\n"
        "}";
    const std::size_t size = std::strlen(source);

    if (ql_c_frontend_analyze(nullptr, source, size, &unit, &error) !=
        QL_STATUS_OK) {
        ADD_FAILURE() << error.message;
        return bytes;
    }
    function.struct_size = sizeof(function);
    if (ql_c_frontend_select_function(unit, "seed", 4u, &function, &error) ==
            QL_STATUS_OK &&
        ql_c_lower_selected_function(nullptr, source, size, unit, &function,
                                     &result, &error) == QL_STATUS_OK) {
        view.struct_size = sizeof(view);
        if (ql_c_lower_result_get_view(result, &view, &error) ==
                QL_STATUS_OK &&
            view.support == QL_C_LOWER_SUPPORTED) {
            artifact.struct_size = sizeof(artifact);
            if (ql_artifact_get_view(view.ir_artifact, &artifact, &error) ==
                QL_STATUS_OK) {
                const uint8_t *data =
                    static_cast<const uint8_t *>(artifact.data);
                bytes.assign(data, data + artifact.size);
            }
        }
    }
    ql_c_lower_result_destroy(result);
    ql_c_frontend_unit_destroy(unit);
    EXPECT_FALSE(bytes.empty());
    return bytes;
}

std::vector<uint8_t> MutateBytes(const std::vector<uint8_t> &input,
                                 uint64_t *state) {
    std::vector<uint8_t> output = input;
    const std::size_t rounds = 1u + (NextRandom(state) % 5u);

    for (std::size_t round = 0u; round < rounds; ++round) {
        if (output.empty()) {
            output.push_back(static_cast<uint8_t>(NextRandom(state)));
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

ql_fuzz::Reach ExpectNoViolation() {
    const ql_fuzz::Reach reach = ql_fuzz::g_reach;
    EXPECT_FALSE(ql_fuzz::g_violation.failed) << ql_fuzz::g_violation.message;
    /* Printing the reach keeps the campaign honest in the log: a run whose
       counts collapse is a run that stopped testing what it claims to. */
    std::cout << "[  REACH   ] parsed=" << reach.parsed
              << " lowered=" << reach.lowered
              << " decoded=" << reach.decoded
              << " verified=" << reach.verified << std::endl;
    ql_fuzz::g_violation = ql_fuzz::Violation{};
    ql_fuzz::g_reach = ql_fuzz::Reach{};
    return reach;
}

TEST(Fuzz, ParserSurvivesMutatedSources) {
    uint64_t state = UINT64_C(0x1d872b41f7ac1e33);
    for (std::size_t round = 0u; round < 30000u; ++round) {
        const std::string seed =
            kSeeds[NextRandom(&state) % (sizeof(kSeeds) / sizeof(kSeeds[0]))];
        const std::string mutated = Mutate(seed, &state, false);
        ql_fuzz_frontend(
            reinterpret_cast<const unsigned char *>(mutated.data()),
            mutated.size());
    }
    EXPECT_GT(ExpectNoViolation().parsed, 0u);
}

TEST(Fuzz, LoweringNeverReportsSupportedForIrThatFailsVerification) {
    uint64_t state = UINT64_C(0x6f2a91c30b5de417);
    for (std::size_t round = 0u; round < 30000u; ++round) {
        const std::string seed =
            kSeeds[NextRandom(&state) % (sizeof(kSeeds) / sizeof(kSeeds[0]))];
        /* Three rounds in four keep most of the seed intact so the campaign
           actually reaches the lowering; the fourth mutates hard. */
        const std::string mutated =
            Mutate(seed, &state, (round & 3u) != 0u);
        ql_fuzz_lowering(
            reinterpret_cast<const unsigned char *>(mutated.data()),
            mutated.size());
    }
    /* If no mutated source ever lowers, this test says nothing about the
       lowering, so an empty reach is a failure. */
    EXPECT_GT(ExpectNoViolation().lowered, 0u);
}

TEST(Fuzz, IrDecoderSurvivesMutatedArtifacts) {
    const std::vector<uint8_t> seed = ValidIrBytes();
    uint64_t state = UINT64_C(0x2c9a7e5510b3d641);
    ASSERT_FALSE(seed.empty());
    for (std::size_t round = 0u; round < 60000u; ++round) {
        const std::vector<uint8_t> input = MutateBytes(seed, &state);
        ql_fuzz_ir_decoder(input.data(), input.size());
    }
    /* Mutations that never decode would only be exercising the length checks
       at the front of the reader. */
    EXPECT_GT(ExpectNoViolation().decoded, 0u);
}

TEST(Fuzz, TargetsAcceptArbitraryBytes) {
    uint64_t state = UINT64_C(0x7b3f5d9021ce4a86);
    for (std::size_t round = 0u; round < 20000u; ++round) {
        std::vector<uint8_t> input(NextRandom(&state) % 192u);
        for (std::size_t index = 0u; index < input.size(); ++index) {
            input[index] = static_cast<uint8_t>(NextRandom(&state));
        }
        ql_fuzz_frontend(input.data(), input.size());
        ql_fuzz_lowering(input.data(), input.size());
        ql_fuzz_ir_decoder(input.data(), input.size());
    }
    EXPECT_GT(ExpectNoViolation().parsed, 0u);
}

}  // namespace
