/* Fuzzing of the C parser, the lowering, and the IR decoder.

   The targets below take a byte string and must not crash on any of them.
   They are written without a test framework so that one libFuzzer executable
   can compile this same file with QL_FUZZ_LIBFUZZER defined and call
   ql_fuzz_one directly, while the ordinary test build runs a bounded,
   deterministic campaign over the same targets on every ctest run.

   Not crashing is the weakest of the properties checked here. The targets
   also assert the invariants that make the correctness devices meaningful:

     - a module the decoder accepts must survive verification without
       crashing, whether or not it verifies;
     - a lowering that reports SUPPORTED must verify, because G8 says the
       verifier passes on every lowering output and a mutated source is still
       a source;
     - a module that verifies must run in the interpreter without crashing.

   A fuzzer that only checked for crashes would pass while the lowering
   emitted IR nobody could justify. */

#include "quodlibet/artifact.h"
#include "quodlibet/c_lower.h"
#include "quodlibet/ir_interp.h"
#include "quodlibet/ir_verify.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ql_fuzz {
namespace {

struct Violation {
    bool failed = false;
    std::string message;
};

/* A fuzz campaign that never reaches its target passes without testing
   anything. These counters make that visible so the driver can fail on it
   instead of reporting a green run. */
struct Reach {
    std::size_t parsed = 0u;
    std::size_t lowered = 0u;
    std::size_t decoded = 0u;
    std::size_t verified = 0u;
};

Violation g_violation;
Reach g_reach;

void Require(bool condition, const char *message) {
    if (condition || g_violation.failed) {
        return;
    }
    g_violation.failed = true;
    g_violation.message = message;
#if defined(QL_FUZZ_LIBFUZZER)
    std::fprintf(stderr, "quodlibet fuzz invariant violated: %s\n", message);
    std::abort();
#endif
}

/* Runs the interpreter with zeroed inputs. The point is that a verified
   module never crashes the interpreter, not what it computes. */
void RunInterpreter(ql_ir *ir) {
    ql_ir_view_v1 view{};
    std::vector<std::vector<uint8_t>> storage;
    std::vector<ql_ir_interp_input_v1> inputs;
    ql_ir_interp_result_v1 result{};
    ql_ir_interp_options_v1 options{};
    ql_error error{};

    view.struct_size = sizeof(view);
    if (ql_ir_get_view(ir, &view, &error) != QL_STATUS_OK) {
        return;
    }
    for (std::size_t index = 0u; index < view.value_count; ++index) {
        ql_ir_value_view_v1 value{};
        ql_ir_type_view_v1 type{};
        std::size_t width;
        value.struct_size = sizeof(value);
        if (ql_ir_value_at(ir, index, &value, &error) != QL_STATUS_OK) {
            return;
        }
        if (value.definition_kind != QL_IR_VALUE_PARAMETER) {
            continue;
        }
        type.struct_size = sizeof(type);
        if (ql_ir_type_at(ir, value.type, &type, &error) != QL_STATUS_OK) {
            return;
        }
        width = type.kind == QL_IR_TYPE_BOOL ? 1u : type.bit_width;
        if (width == 0u || width > QL_IR_INTERP_MAX_BIT_WIDTH) {
            return;
        }
        storage.push_back(std::vector<uint8_t>((width + 7u) / 8u, 0u));
        inputs.push_back(ql_ir_interp_input_v1{});
        ql_ir_interp_input_init(&inputs.back());
        inputs.back().value = value.id;
    }
    for (std::size_t index = 0u; index < inputs.size(); ++index) {
        inputs[index].data = storage[index].data();
        inputs[index].size = storage[index].size();
    }
    ql_ir_interp_options_init(&options);
    options.step_limit = 100000u;
    result.struct_size = sizeof(result);
    (void)ql_ir_interp_run(nullptr, ir, inputs.empty() ? nullptr
                                                       : inputs.data(),
                           inputs.size(), &options, &result, &error);
}

void VerifyAndRun(ql_ir *ir, bool require_verification, const char *reason) {
    ql_ir_verify_report_v1 report{};
    ql_error error{};
    report.struct_size = sizeof(report);
    const ql_status status = ql_ir_verify(nullptr, ir, &report, &error);
    if (require_verification) {
        Require(status == QL_STATUS_OK, reason);
    }
    if (status == QL_STATUS_OK) {
        ++g_reach.verified;
        RunInterpreter(ir);
    }
}

/* ------------------------------------------------------------------ */
/* targets                                                             */
/* ------------------------------------------------------------------ */

void FuzzFrontend(const uint8_t *data, std::size_t size) {
    ql_c_frontend_unit *unit = nullptr;
    ql_c_frontend_unit_view view{};
    ql_error error{};

    if (ql_c_frontend_analyze(nullptr, reinterpret_cast<const char *>(data),
                              size, &unit, &error) != QL_STATUS_OK) {
        ql_c_frontend_unit_destroy(unit);
        return;
    }
    ++g_reach.parsed;
    view.struct_size = sizeof(view);
    if (ql_c_frontend_unit_get_view(unit, &view, &error) == QL_STATUS_OK) {
        for (std::size_t index = 0u; index < view.function_count; ++index) {
            ql_c_function_view function{};
            function.struct_size = sizeof(function);
            if (ql_c_frontend_function_at(unit, index, &function, &error) !=
                QL_STATUS_OK) {
                continue;
            }
            for (std::size_t parameter = 0u;
                 parameter < function.parameter_count; ++parameter) {
                ql_c_parameter_view argument{};
                argument.struct_size = sizeof(argument);
                (void)ql_c_frontend_parameter_at(unit, index, parameter,
                                                 &argument, &error);
            }
            for (std::size_t diagnostic = 0u;
                 diagnostic < function.diagnostic_count; ++diagnostic) {
                ql_c_frontend_diagnostic_view record{};
                record.struct_size = sizeof(record);
                (void)ql_c_frontend_function_diagnostic_at(
                    unit, index, diagnostic, &record, &error);
            }
        }
        for (std::size_t index = 0u; index < view.diagnostic_count; ++index) {
            ql_c_frontend_diagnostic_view record{};
            record.struct_size = sizeof(record);
            (void)ql_c_frontend_diagnostic_at(unit, index, &record, &error);
        }
    }
    ql_c_frontend_unit_destroy(unit);
}

void FuzzLowering(const uint8_t *data, std::size_t size) {
    ql_c_frontend_unit *unit = nullptr;
    ql_c_frontend_unit_view view{};
    ql_error error{};
    const char *source = reinterpret_cast<const char *>(data);

    if (ql_c_frontend_analyze(nullptr, source, size, &unit, &error) !=
        QL_STATUS_OK) {
        ql_c_frontend_unit_destroy(unit);
        return;
    }
    view.struct_size = sizeof(view);
    if (ql_c_frontend_unit_get_view(unit, &view, &error) == QL_STATUS_OK) {
        for (std::size_t index = 0u; index < view.function_count; ++index) {
            ql_c_function_view function{};
            ql_c_lower_result *result = nullptr;
            ql_c_lower_result_view_v1 lowered{};
            function.struct_size = sizeof(function);
            if (ql_c_frontend_function_at(unit, index, &function, &error) !=
                QL_STATUS_OK) {
                continue;
            }
            if (ql_c_lower_selected_function(nullptr, source, size, unit,
                                             &function, &result,
                                             &error) != QL_STATUS_OK) {
                ql_c_lower_result_destroy(result);
                continue;
            }
            lowered.struct_size = sizeof(lowered);
            if (ql_c_lower_result_get_view(result, &lowered, &error) ==
                    QL_STATUS_OK &&
                lowered.support == QL_C_LOWER_SUPPORTED) {
                ql_ir *ir = nullptr;
                ++g_reach.lowered;
                Require(lowered.ir_artifact != nullptr,
                        "a SUPPORTED lowering produced no IR artifact");
                if (lowered.ir_artifact != nullptr &&
                    ql_ir_open(nullptr, lowered.ir_artifact, &ir, &error) ==
                        QL_STATUS_OK) {
                    VerifyAndRun(ir, true,
                                 "the lowering reported SUPPORTED but its IR "
                                 "failed verification");
                } else {
                    Require(false,
                            "a SUPPORTED lowering produced IR the decoder "
                            "rejected");
                }
                ql_ir_release(ir);
            }
            ql_c_lower_result_destroy(result);
        }
    }
    ql_c_frontend_unit_destroy(unit);
}

void FuzzIrDecoder(const uint8_t *data, std::size_t size) {
    ql_artifact *artifact = nullptr;
    ql_ir *ir = nullptr;
    ql_error error{};

    if (ql_artifact_create(nullptr, QL_ARTIFACT_KIND_IR, 1u, data, size,
                           &artifact, &error) != QL_STATUS_OK) {
        return;
    }
    if (ql_ir_open(nullptr, artifact, &ir, &error) == QL_STATUS_OK) {
        ++g_reach.decoded;
        /* A decoded module need not verify: the decoder guarantees structure,
           not the guard obligations the verifier adds. It must not crash the
           verifier, which is what this call checks. */
        VerifyAndRun(ir, false, nullptr);
        ql_ir_release(ir);
    }
    ql_artifact_release(artifact);
}

}  // namespace

void FuzzOne(const uint8_t *data, std::size_t size) {
    if (size == 0u) {
        return;
    }
    switch (data[0] % 3u) {
    case 0u:
        FuzzFrontend(data + 1u, size - 1u);
        break;
    case 1u:
        FuzzLowering(data + 1u, size - 1u);
        break;
    default:
        FuzzIrDecoder(data + 1u, size - 1u);
        break;
    }
}

}  // namespace ql_fuzz

#if defined(QL_FUZZ_LIBFUZZER)

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, std::size_t size) {
    ql_fuzz::FuzzOne(data, size);
    return 0;
}

#else

#include <iostream>

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
    "int f(int *p) { return *p; }",
    "struct s { int a; };\nint f(struct s v) { return v.a; }",
    "int f(void) { for (;;) { } }",
    "#define M 1\nint f(int a) { return a + M; }",
    "int f(int a, ...) { return a; }",
    "int f();",
    "",
};

/* A pool of tokens the C grammar cares about. Splicing these in reaches
   syntactically interesting inputs that pure byte flipping almost never
   produces. */
const char *const kTokens[] = {
    "int",   "unsigned", "long",  "short",  "char",  "_Bool", "void",
    "if",    "else",     "while", "return", "goto",  "switch", "case",
    "const", "volatile", "_Atomic", "struct", "union", "enum", "static",
    "*",     "&",        "[",     "]",      "(",     ")",     "{",
    "}",     ";",        ",",     "/",      "%",     "<<",    ">>",
    "?",     ":",        "->",    ".",      "...",   "sizeof", "0x",
    "9223372036854775808", "'\\0'", "\"\"", "\\", "\n", "//", "/*",
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
                kTokens[NextRandom(state) % (sizeof(kTokens) /
                                             sizeof(kTokens[0]))];
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
                kSeeds[NextRandom(state) % (sizeof(kSeeds) /
                                            sizeof(kSeeds[0]))];
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
               raising one whole field is far more likely to reach a table
               extent than flipping single bits. */
            if (output.size() >= 4u) {
                const std::size_t at =
                    (NextRandom(state) % (output.size() - 3u)) & ~std::size_t{3u};
                const uint32_t value =
                    static_cast<uint32_t>(NextRandom(state) % 5u == 0u
                                              ? UINT32_MAX
                                              : NextRandom(state) % 0x10000u);
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

/* The leading byte selects the target, so it has to survive being zero. */
std::string Frame(char target, const std::string &body) {
    std::string framed;
    framed.push_back(target);
    framed.append(body);
    return framed;
}

ql_fuzz::Reach ExpectNoViolation() {
    const ql_fuzz::Reach reach = ql_fuzz::g_reach;
    EXPECT_FALSE(ql_fuzz::g_violation.failed)
        << ql_fuzz::g_violation.message;
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
        const std::string framed = Frame('\0', Mutate(seed, &state, false));
        ql_fuzz::FuzzOne(reinterpret_cast<const uint8_t *>(framed.data()),
                         framed.size());
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
        const std::string framed =
            Frame('\x01', Mutate(seed, &state, (round & 3u) != 0u));
        ql_fuzz::FuzzOne(reinterpret_cast<const uint8_t *>(framed.data()),
                         framed.size());
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
        std::vector<uint8_t> input = MutateBytes(seed, &state);
        input.insert(input.begin(), static_cast<uint8_t>(2u));
        ql_fuzz::FuzzOne(input.data(), input.size());
    }
    /* Mutations that never decode would only be exercising the length checks
       at the front of the reader. */
    EXPECT_GT(ExpectNoViolation().decoded, 0u);
}

TEST(Fuzz, TargetsAcceptArbitraryBytes) {
    uint64_t state = UINT64_C(0x7b3f5d9021ce4a86);
    for (std::size_t round = 0u; round < 30000u; ++round) {
        std::vector<uint8_t> input(NextRandom(&state) % 192u);
        for (std::size_t index = 0u; index < input.size(); ++index) {
            input[index] = static_cast<uint8_t>(NextRandom(&state));
        }
        ql_fuzz::FuzzOne(input.data(), input.size());
    }
    ExpectNoViolation();
}

}  // namespace

#endif
