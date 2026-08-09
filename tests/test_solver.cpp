#include "quodlibet/solver.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <gtest/gtest.h>

namespace {

constexpr char kPipeModeEnvironment[] =
    "QL_SOLVER_TEST_INHERITED_PIPE_MODE";
#if !defined(_WIN32)
constexpr char kPipeHostEnvironment[] =
    "QL_SOLVER_TEST_INHERITED_PIPE_HOST";
#endif

#if defined(_WIN32)
bool spawn_inherited_pipe_holder() {
    wchar_t *host_value = nullptr;
    std::size_t host_size = 0u;
    STARTUPINFOW startup{};
    PROCESS_INFORMATION process{};
    std::wstring host;
    std::wstring command;

    if (_wdupenv_s(&host_value, &host_size,
                   L"QL_SOLVER_TEST_INHERITED_PIPE_HOST") != 0 ||
        host_value == nullptr || host_size <= 1u) {
        std::free(host_value);
        return false;
    }
    host.assign(host_value);
    std::free(host_value);
    if (_putenv_s(kPipeModeEnvironment, "holder") != 0) {
        return false;
    }
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    command = L"\"" + host +
              L"\" --ql-solver-inherited-pipe-holder";
    if (CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process) == FALSE) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
#else
bool spawn_inherited_pipe_holder() {
    const char *host = std::getenv(kPipeHostEnvironment);
    pid_t child;

    if (host == nullptr || host[0] == '\0') {
        return false;
    }
    child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        if (setenv(kPipeModeEnvironment, "holder", 1) != 0) {
            std::_Exit(2);
        }
        execl(host, host, "--ql-solver-inherited-pipe-holder",
              static_cast<char *>(nullptr));
        std::_Exit(2);
    }
    return true;
}
#endif

std::string inherited_pipe_test_mode() {
#if defined(_WIN32)
    char *value = nullptr;
    std::size_t value_size = 0u;
    std::string result;

    if (_dupenv_s(&value, &value_size, kPipeModeEnvironment) == 0 &&
        value != nullptr) {
        result.assign(value);
    }
    std::free(value);
    return result;
#else
    const char *value = std::getenv(kPipeModeEnvironment);
    return value == nullptr ? std::string() : std::string(value);
#endif
}

struct InheritedPipeTestProcessMode {
    InheritedPipeTestProcessMode() {
        const std::string mode = inherited_pipe_test_mode();

        if (mode.empty()) {
            return;
        }
        if (mode == "holder") {
            /* Must outlive QL_PROCESS_DRAIN_GRACE_MS (5000ms): the parent
               exits immediately, and the property under test is that the
               transport gives up on the still-open inherited pipes at the
               drain deadline instead of waiting for this process. */
            std::this_thread::sleep_for(std::chrono::milliseconds(6500));
            std::_Exit(0);
        }
        if (mode == "version-parent") {
            if (!spawn_inherited_pipe_holder()) {
                std::_Exit(2);
            }
            std::fputs("0.9.1\n", stdout);
            std::fflush(stdout);
            std::_Exit(0);
        }
    }
};

const InheritedPipeTestProcessMode kInheritedPipeTestProcessMode;

std::filesystem::path current_test_executable() {
#if defined(_WIN32)
    std::wstring path(32768u, L'\0');
    const DWORD size = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0u || size >= path.size()) {
        return {};
    }
    path.resize(size);
    return std::filesystem::path(path);
#else
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error);
#endif
}

bool set_inherited_pipe_test_environment(
    const std::filesystem::path &host) {
#if defined(_WIN32)
    return _putenv_s(kPipeModeEnvironment, "version-parent") == 0 &&
           _wputenv_s(L"QL_SOLVER_TEST_INHERITED_PIPE_HOST",
                      host.c_str()) == 0;
#else
    return setenv(kPipeModeEnvironment, "version-parent", 1) == 0 &&
           setenv(kPipeHostEnvironment, host.c_str(), 1) == 0;
#endif
}

struct InheritedPipeEnvironmentGuard {
    ~InheritedPipeEnvironmentGuard() {
#if defined(_WIN32)
        (void)_putenv_s(kPipeModeEnvironment, "");
        (void)_wputenv_s(L"QL_SOLVER_TEST_INHERITED_PIPE_HOST", L"");
#else
        (void)unsetenv(kPipeModeEnvironment);
        (void)unsetenv(kPipeHostEnvironment);
#endif
    }
};

struct MockState {
    ql_allocator allocator;
    bool unsat;
};

ql_status QL_CALL mock_create(const ql_allocator *allocator, const char *,
                              void **output, ql_error *error) {
    auto *state = static_cast<MockState *>(
        allocator->allocate(allocator->user_data, sizeof(MockState)));
    if (state == nullptr) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, "mock allocation failed");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    state->allocator = *allocator;
    state->unsat = false;
    *output = state;
    return QL_STATUS_OK;
}

void QL_CALL mock_destroy(void *opaque) {
    auto *state = static_cast<MockState *>(opaque);
    if (state != nullptr) {
        const ql_allocator allocator = state->allocator;
        allocator.deallocate(allocator.user_data, state);
    }
}

ql_status QL_CALL mock_add(void *opaque, const char *commands,
                           std::size_t size, ql_error *) {
    auto *state = static_cast<MockState *>(opaque);
    const std::string text(commands, size);
    state->unsat = text.find("(assert false)") != std::string::npos;
    return QL_STATUS_OK;
}

ql_status QL_CALL mock_stack(void *, std::uint32_t, ql_error *) {
    return QL_STATUS_OK;
}

ql_status make_mock_artifact(MockState *state, const char *kind,
                             const char *text, ql_artifact **output,
                             ql_error *error) {
    return ql_artifact_create(&state->allocator, kind,
                              QL_SOLVER_ARTIFACT_SCHEMA_VERSION, text,
                              std::strlen(text), output, error);
}

ql_status QL_CALL mock_check(void *opaque,
                             const ql_solver_check_request_v1 *request,
                             ql_solver_check_result_v1 *result,
                             ql_error *error) {
    auto *state = static_cast<MockState *>(opaque);
    ql_digest_data("mock-backend", 12u, &result->backend_binary_digest);
    ql_digest_data("mock-query", 10u, &result->query_digest);
    if (request->is_cancelled != nullptr &&
        request->is_cancelled(request->cancel_state) != 0u) {
        result->kind = QL_SOLVER_CHECK_UNKNOWN;
        result->unknown_reason = QL_SOLVER_UNKNOWN_CANCELLED;
        return QL_STATUS_OK;
    }
    if (state->unsat) {
        result->kind = QL_SOLVER_CHECK_UNSAT;
        if ((request->artifact_requests & QL_SOLVER_REQUEST_PROOF) != 0u &&
            make_mock_artifact(state, QL_ARTIFACT_KIND_SOLVER_PROOF,
                               "mock raw proof", &result->proof_artifact,
                               error) != QL_STATUS_OK) {
            return error->code;
        }
        if ((request->artifact_requests &
             QL_SOLVER_REQUEST_UNSAT_METADATA) != 0u &&
            make_mock_artifact(
                state, QL_ARTIFACT_KIND_SOLVER_UNSAT_METADATA,
                "backend=mock\n", &result->unsat_metadata_artifact,
                error) != QL_STATUS_OK) {
            return error->code;
        }
    } else {
        result->kind = QL_SOLVER_CHECK_SAT;
        if ((request->artifact_requests & QL_SOLVER_REQUEST_MODEL) != 0u &&
            make_mock_artifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL,
                               "x=#x2a\n", &result->model_artifact,
                               error) != QL_STATUS_OK) {
            return error->code;
        }
    }
    return QL_STATUS_OK;
}

const ql_solver_descriptor_v1 kMockDescriptor = {
    sizeof(ql_solver_descriptor_v1),
    QL_SOLVER_ABI_VERSION,
    0u,
    "mock",
    "1.0",
    "test backend",
    {
        sizeof(ql_solver_capability_v1),
        QL_SOLVER_ABI_VERSION,
        QL_SOLVER_AVAILABLE,
        QL_SOLVER_LOGIC_QF_BV,
        1u,
        256u,
        QL_SOLVER_FEATURE_INCREMENTAL | QL_SOLVER_FEATURE_MODELS |
            QL_SOLVER_FEATURE_PROOFS | QL_SOLVER_FEATURE_CANCELLATION |
            QL_SOLVER_FEATURE_TIMEOUT,
        {0u, 0u, 0u, 0u, 0u, 0u},
    },
    mock_create,
    mock_destroy,
    mock_add,
    mock_stack,
    mock_stack,
    mock_check,
    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
};

std::uint32_t QL_CALL always_cancelled(const void *) {
    return 1u;
}

struct CancellationProbe {
    std::uint32_t calls;
};

std::uint32_t QL_CALL counted_immediate_cancellation(const void *opaque) {
    auto *probe = const_cast<CancellationProbe *>(
        static_cast<const CancellationProbe *>(opaque));
    ++probe->calls;
    return 1u;
}

std::set<std::filesystem::path> private_solver_snapshots() {
    namespace fs = std::filesystem;
    std::set<fs::path> snapshots;
    const std::string prefix =
        "quodlibet-bitwuzla-" +
#if defined(_WIN32)
        std::to_string(static_cast<unsigned long>(GetCurrentProcessId())) +
#else
        std::to_string(static_cast<long long>(getpid())) +
#endif
        "-";
    for (const fs::directory_entry &entry :
         fs::directory_iterator(fs::temp_directory_path())) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory() &&
            name.rfind(prefix, 0u) == 0u) {
            snapshots.insert(entry.path());
        }
    }
    return snapshots;
}

struct TemporaryTree {
    std::filesystem::path path;

    ~TemporaryTree() {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};

struct SolverGuard {
    ql_solver *value = nullptr;

    ~SolverGuard() {
        ql_solver_destroy(value);
    }
};

ql_artifact *build_formula(const char *term) {
    ql_smt2_builder *builder = nullptr;
    ql_artifact *artifact = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_smt2_builder_create(nullptr, QL_SOLVER_LOGIC_QF_BV,
                                     &builder, &error))
        << error.message;
    if (builder == nullptr) {
        return nullptr;
    }
    EXPECT_EQ(QL_STATUS_OK,
              ql_smt2_builder_declare_bv(builder, "x", 8u, &error))
        << error.message;
    EXPECT_EQ(QL_STATUS_OK,
              ql_smt2_builder_assert(builder, term, &error))
        << error.message;
    EXPECT_EQ(QL_STATUS_OK,
              ql_smt2_builder_build(builder, &artifact, &error))
        << error.message;
    ql_smt2_builder_destroy(builder);
    return artifact;
}

TEST(SolverABI, UsesVersionedAppendOnlyStructures) {
    static_assert(std::is_standard_layout_v<ql_solver_capability_v1>);
    static_assert(std::is_standard_layout_v<ql_solver_check_request_v1>);
    static_assert(std::is_standard_layout_v<ql_solver_check_result_v1>);
    static_assert(std::is_standard_layout_v<ql_solver_descriptor_v1>);
    static_assert(
        std::is_standard_layout_v<ql_solver_checked_proof_binding_v1>);
    static_assert(offsetof(ql_solver_capability_v1, struct_size) == 0u);
    static_assert(offsetof(ql_solver_descriptor_v1, struct_size) == 0u);
    static_assert(sizeof(ql_solver_logic) == sizeof(std::uint64_t));
    EXPECT_EQ(UINT64_C(31), static_cast<std::uint64_t>(QL_SOLVER_LOGIC_ALL));
}

TEST(SmtLibBuilder, SerializesDeterministicallyWithoutTerminalCommands) {
    ql_artifact *left = build_formula("(= x #x2a)");
    ql_artifact *right = build_formula("(= x #x2a)");
    ql_artifact_view left_view{};
    ql_artifact_view right_view{};
    ql_error error{};
    constexpr char expected[] =
        "(set-logic QF_BV)\n"
        "(declare-const x (_ BitVec 8))\n"
        "(assert (= x #x2a))\n";

    ASSERT_NE(nullptr, left);
    ASSERT_NE(nullptr, right);
    left_view.struct_size = sizeof(left_view);
    right_view.struct_size = sizeof(right_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(left, &left_view, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(right, &right_view, &error));
    EXPECT_STREQ(QL_ARTIFACT_KIND_SMTLIB2, left_view.kind);
    EXPECT_EQ(sizeof(expected) - 1u, left_view.size);
    EXPECT_EQ(0, std::memcmp(expected, left_view.data, left_view.size));
    EXPECT_TRUE(ql_digest_equal(&left_view.digest, &right_view.digest));
    ql_artifact_release(left);
    ql_artifact_release(right);
}

TEST(SmtLibBuilder, EnforcesHardTranscriptLimitBeforeAllocation) {
    ql_smt2_builder *builder = nullptr;
    ql_error error{};
    std::string oversized(
        static_cast<std::size_t>(QL_SOLVER_MAX_SMTLIB2_BYTES), 'x');

    ASSERT_EQ(QL_STATUS_OK,
              ql_smt2_builder_create(nullptr, QL_SOLVER_LOGIC_QF_BV,
                                     &builder, &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_smt2_builder_assert(builder, oversized.c_str(), &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "hard limit"));
    ql_smt2_builder_destroy(builder);
}

TEST(Solver, RejectsTerminalCommandsInAssertionArtifact) {
    constexpr char bad[] = "(set-logic QF_BV)\n(check-sat)\n";
    constexpr char unsafe_option[] =
        "(set-option :diagnostic-output-channel \"owned.txt\")\n";
    constexpr char unsafe_info[] =
        "(set-info :source \"untrusted metadata\")\n";
    ql_solver *solver = nullptr;
    ql_artifact *artifact = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_create(nullptr, &kMockDescriptor, nullptr, &solver,
                               &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_SMTLIB2,
                                 QL_SMTLIB2_SCHEMA_VERSION, bad,
                                 sizeof(bad) - 1u, &artifact, &error));
    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_solver_add_smt2(solver, artifact, &error));
    ql_artifact_release(artifact);

    artifact = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_SMTLIB2,
                                 QL_SMTLIB2_SCHEMA_VERSION, unsafe_option,
                                 sizeof(unsafe_option) - 1u, &artifact,
                                 &error));
    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_solver_add_smt2(solver, artifact, &error));
    ql_artifact_release(artifact);

    artifact = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_SMTLIB2,
                                 QL_SMTLIB2_SCHEMA_VERSION, unsafe_info,
                                 sizeof(unsafe_info) - 1u, &artifact,
                                 &error));
    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_solver_add_smt2(solver, artifact, &error));
    ql_artifact_release(artifact);
    ql_solver_destroy(solver);
}

TEST(Solver, MockSupportsSatModelAndCancellation) {
    ql_solver *solver = nullptr;
    ql_artifact *formula = build_formula("(= x #x2a)");
    ql_solver_check_request_v1 request{};
    ql_solver_check_result_v1 result{};
    ql_error error{};

    ASSERT_NE(nullptr, formula);
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_create(nullptr, &kMockDescriptor, nullptr, &solver,
                               &error));
    ASSERT_EQ(QL_STATUS_OK, ql_solver_add_smt2(solver, formula, &error));
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.artifact_requests = QL_SOLVER_REQUEST_MODEL;
    ql_solver_check_result_init(&result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_check(solver, &request, &result, &error))
        << error.message;
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, result.kind);
    EXPECT_NE(nullptr, result.model_artifact);
    ql_solver_check_result_clear(&result);

    request.artifact_requests = 0u;
    request.is_cancelled = always_cancelled;
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_check(solver, &request, &result, &error));
    EXPECT_EQ(QL_SOLVER_CHECK_UNKNOWN, result.kind);
    EXPECT_EQ(QL_SOLVER_UNKNOWN_CANCELLED, result.unknown_reason);
    ql_solver_check_result_clear(&result);
    ql_artifact_release(formula);
    ql_solver_destroy(solver);
}

TEST(SolverRequest, MemoryLimitRequiresAnAdvertisedCapability) {
    ql_solver_capability_v1 capability = kMockDescriptor.capability;
    ql_solver_check_request_v1 request{};
    ql_error error{};

    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.memory_limit_mb = 256u;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_solver_check_request_validate(
                  &capability, &request, &error));
    capability.features |= QL_SOLVER_FEATURE_MEMORY_LIMIT;
    EXPECT_EQ(QL_STATUS_OK,
              ql_solver_check_request_validate(
                  &capability, &request, &error))
        << error.message;
}

TEST(SolverUnknownReason, DistinguishesRawUnknownFromWatchdogTimeout) {
    EXPECT_EQ(QL_SOLVER_UNKNOWN_BACKEND,
              ql_solver_unknown_reason_classify(0u, 0u));
    EXPECT_EQ(QL_SOLVER_UNKNOWN_TIMEOUT,
              ql_solver_unknown_reason_classify(0u, 1u));
    EXPECT_EQ(QL_SOLVER_UNKNOWN_CANCELLED,
              ql_solver_unknown_reason_classify(1u, 0u));
    EXPECT_EQ(QL_SOLVER_UNKNOWN_CANCELLED,
              ql_solver_unknown_reason_classify(1u, 1u));
}

TEST(SolverProofBinding, ValidatesStructureWithoutGrantingProofAuthority) {
    ql_solver *solver = nullptr;
    ql_artifact *formula = build_formula("false");
    ql_artifact *checked_artifact = nullptr;
    ql_artifact_view raw_view{};
    ql_solver_check_request_v1 request{};
    ql_solver_check_result_v1 result{};
    ql_solver_checked_proof_binding_v1 binding{};
    ql_error error{};

    ASSERT_NE(nullptr, formula);
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_create(nullptr, &kMockDescriptor, nullptr, &solver,
                               &error));
    ASSERT_EQ(QL_STATUS_OK, ql_solver_add_smt2(solver, formula, &error));
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.artifact_requests = QL_SOLVER_REQUEST_PROOF |
                                QL_SOLVER_REQUEST_UNSAT_METADATA;
    ql_solver_check_result_init(&result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_check(solver, &request, &result, &error));
    ASSERT_EQ(QL_SOLVER_CHECK_UNSAT, result.kind);
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_solver_checked_proof_binding_validate(
                  &result, &binding, &error));

    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_PROOF, 1u,
                                 "checked", 7u, &checked_artifact, &error));
    raw_view.struct_size = sizeof(raw_view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(result.proof_artifact, &raw_view,
                                   &error));
    binding.struct_size = sizeof(binding);
    binding.abi_version = QL_SOLVER_ABI_VERSION;
    binding.backend_binary_digest = result.backend_binary_digest;
    binding.query_digest = result.query_digest;
    binding.raw_proof_digest = raw_view.digest;
    binding.checker_name = "caller-claimed-checker";
    binding.checker_version = "1.0";
    binding.checked_proof_artifact = checked_artifact;
    EXPECT_EQ(QL_STATUS_OK,
              ql_solver_checked_proof_binding_validate(
                  &result, &binding, &error))
        << error.message;

    ql_artifact_release(checked_artifact);
    ql_solver_check_result_clear(&result);
    ql_artifact_release(formula);
    ql_solver_destroy(solver);
}

TEST(BitwuzlaSolver, AdvertisesNoProofCapability) {
    const ql_solver_descriptor_v1 *descriptor =
        ql_bitwuzla_solver_descriptor();
    ql_solver_check_request_v1 request{};
    ql_error error{};

    ASSERT_NE(nullptr, descriptor);
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_descriptor_validate(descriptor, &error));
    EXPECT_EQ(ql_bitwuzla_executable_path()[0] == '\0' ?
                  QL_SOLVER_UNAVAILABLE : QL_SOLVER_AVAILABLE,
              descriptor->capability.availability);
    EXPECT_NE(0u, descriptor->capability.features &
                      QL_SOLVER_FEATURE_PROCESS_ISOLATION);
    EXPECT_NE(0u, descriptor->capability.features &
                      QL_SOLVER_FEATURE_MEMORY_LIMIT);
    EXPECT_EQ(0u,
              descriptor->capability.features & QL_SOLVER_FEATURE_PROOFS);
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.artifact_requests = QL_SOLVER_REQUEST_PROOF;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_solver_check_request_validate(
                  &descriptor->capability, &request, &error));
}

TEST(BitwuzlaSolver, RunsPrivateSnapshotAfterOverrideIsRemovedAndCleansIt) {
    namespace fs = std::filesystem;
    const ql_solver_descriptor_v1 *descriptor =
        ql_bitwuzla_solver_descriptor();
    TemporaryTree source_tree;
    SolverGuard solver;
    ql_artifact *formula = nullptr;
    ql_solver_check_request_v1 request{};
    ql_solver_check_result_v1 result{};
    ql_error error{};
    ql_status status;

    if (descriptor->capability.availability == QL_SOLVER_UNAVAILABLE) {
        status = ql_solver_create(
            nullptr, descriptor, nullptr, &solver.value, &error);
        EXPECT_EQ(QL_STATUS_NOT_FOUND, status);
        EXPECT_EQ(nullptr, solver.value);
        return;
    }
    const std::set<fs::path> snapshots_before = private_solver_snapshots();
    const auto unique = std::chrono::high_resolution_clock::now()
                            .time_since_epoch()
                            .count();
    source_tree.path = fs::temp_directory_path() /
                       ("quodlibet-solver-test-" +
                        std::to_string(unique));
    ASSERT_TRUE(fs::create_directory(source_tree.path));
#if defined(_WIN32)
    const fs::path override_path = source_tree.path / "override.exe";
#else
    const fs::path override_path = source_tree.path / "override";
#endif
    ASSERT_TRUE(fs::copy_file(ql_bitwuzla_executable_path(), override_path));
    const std::string options =
        std::string("{\"executable\":\"") +
        override_path.generic_string() + "\"}";
    status = ql_solver_create(
        nullptr, descriptor, options.c_str(), &solver.value, &error);
    ASSERT_EQ(QL_STATUS_OK, status) << error.message;
    ASSERT_NE(nullptr, solver.value);
    const std::set<fs::path> snapshots_after = private_solver_snapshots();
    ASSERT_EQ(snapshots_before.size() + 1u, snapshots_after.size());
    fs::path snapshot_directory;
    for (const fs::path &candidate : snapshots_after) {
        if (snapshots_before.find(candidate) == snapshots_before.end()) {
            snapshot_directory = candidate;
            break;
        }
    }
    ASSERT_FALSE(snapshot_directory.empty());
#if defined(_WIN32)
    const fs::path snapshot_executable =
        snapshot_directory / "bitwuzla.exe";
#else
    const fs::path snapshot_executable = snapshot_directory / "bitwuzla";
#endif
    {
        std::ofstream blocked(snapshot_executable,
                              std::ios::binary | std::ios::app);
        EXPECT_FALSE(blocked.is_open());
    }

    ASSERT_TRUE(fs::remove(override_path));
    ASSERT_TRUE(fs::remove(source_tree.path));
    formula = build_formula("(= x #x2a)");
    ASSERT_NE(nullptr, formula);
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_add_smt2(solver.value, formula, &error));
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.artifact_requests = 0u;
    ql_solver_check_result_init(&result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_check(solver.value, &request, &result, &error))
        << error.message;
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, result.kind);

    ql_solver_check_result_clear(&result);
    fs::permissions(snapshot_executable, fs::perms::owner_write,
                    fs::perm_options::add);
    {
        std::ofstream tamper(snapshot_executable,
                             std::ios::binary | std::ios::app);
        ASSERT_TRUE(tamper.is_open());
        tamper.put('\0');
        ASSERT_TRUE(tamper.good());
    }
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_solver_check(solver.value, &request, &result, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "snapshot content changed"));
    ql_artifact_release(formula);
    ql_solver_destroy(solver.value);
    solver.value = nullptr;
    EXPECT_EQ(snapshots_before, private_solver_snapshots());
}

TEST(BitwuzlaTransport, BoundsInheritedPipesAfterDirectChildExit) {
    namespace fs = std::filesystem;
    const ql_solver_descriptor_v1 *descriptor =
        ql_bitwuzla_solver_descriptor();
    const fs::path host = current_test_executable();
    const std::set<fs::path> snapshots_before = private_solver_snapshots();
    TemporaryTree source_tree;
    InheritedPipeEnvironmentGuard environment_guard;
    ql_solver *solver = nullptr;
    ql_error error{};

    if (descriptor->capability.availability == QL_SOLVER_UNAVAILABLE) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    ASSERT_FALSE(host.empty());
    const auto unique = std::chrono::high_resolution_clock::now()
                            .time_since_epoch()
                            .count();
    source_tree.path = fs::temp_directory_path() /
                       ("quodlibet-solver-drain-test-" +
                        std::to_string(unique));
    ASSERT_TRUE(fs::create_directory(source_tree.path));
#if defined(_WIN32)
    const fs::path override_path = source_tree.path / "override.exe";
#else
    const fs::path override_path = source_tree.path / "override";
#endif
    ASSERT_TRUE(fs::copy_file(host, override_path));
    ASSERT_TRUE(set_inherited_pipe_test_environment(host));
    const std::string options =
        std::string("{\"executable\":\"") +
        override_path.generic_string() + "\"}";
    const auto started = std::chrono::steady_clock::now();
    const ql_status status = ql_solver_create(
        nullptr, descriptor, options.c_str(), &solver, &error);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(QL_STATUS_IO_ERROR, status) << error.message;
    EXPECT_EQ(nullptr, solver);
    EXPECT_NE(nullptr, std::strstr(error.message, "drain deadline"));
    // The drain deadline itself is 5 seconds, and ASan/UBSan builds spend
    // several more copying and hashing the large instrumented executable.
    // Keep this below CTest's 30-second timeout while the transport error
    // above verifies that pipe draining itself is bounded.
    EXPECT_LT(elapsed, std::chrono::seconds(20));
    EXPECT_EQ(snapshots_before, private_solver_snapshots());
}

TEST(BitwuzlaSolver, EnforcesHardLimitOnCompleteTerminalQuery) {
    const ql_solver_descriptor_v1 *descriptor =
        ql_bitwuzla_solver_descriptor();
    ql_solver *solver = nullptr;
    ql_artifact *artifact = nullptr;
    ql_solver_check_request_v1 request{};
    ql_solver_check_result_v1 result{};
    ql_error error{};

    if (descriptor->capability.availability == QL_SOLVER_UNAVAILABLE) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_create(nullptr, descriptor, nullptr, &solver,
                               &error))
        << error.message;
    std::string transcript(
        static_cast<std::size_t>(QL_SOLVER_MAX_SMTLIB2_BYTES), 'x');
    constexpr char prefix[] = "(assert true)\n;";
    std::memcpy(transcript.data(), prefix, sizeof(prefix) - 1u);
    transcript.back() = '\n';
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_SMTLIB2,
                                 QL_SMTLIB2_SCHEMA_VERSION,
                                 transcript.data(), transcript.size(),
                                 &artifact, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_solver_add_smt2(solver, artifact, &error))
        << error.message;
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    ql_solver_check_result_init(&result);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_solver_check(solver, &request, &result, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "hard limit"));

    ql_solver_check_result_clear(&result);
    ql_artifact_release(artifact);
    ql_solver_destroy(solver);
}

TEST(BitwuzlaSolver, SolvesRealBitVectorSatAndUnsatQueries) {
    const ql_solver_descriptor_v1 *descriptor =
        ql_bitwuzla_solver_descriptor();
    ql_solver *solver = nullptr;
    ql_artifact *formula = nullptr;
    ql_artifact_view view{};
    ql_solver_check_request_v1 request{};
    ql_solver_check_result_v1 result{};
    ql_solver_checked_proof_binding_v1 no_proof{};
    CancellationProbe cancellation_probe{};
    ql_error error{};
    ql_status status =
        ql_solver_create(nullptr, descriptor, nullptr, &solver, &error);

    if (status == QL_STATUS_NOT_FOUND) {
        GTEST_SKIP() << error.message;
    }
    ASSERT_EQ(QL_STATUS_OK, status) << error.message;
    formula = build_formula("(= x #x2a)");
    ASSERT_NE(nullptr, formula);
    ASSERT_EQ(QL_STATUS_OK, ql_solver_add_smt2(solver, formula, &error));
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.artifact_requests = QL_SOLVER_REQUEST_MODEL |
                                QL_SOLVER_REQUEST_DIAGNOSTICS;
    ql_solver_check_result_init(&result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_check(solver, &request, &result, &error))
        << error.message;
    ASSERT_EQ(QL_SOLVER_CHECK_SAT, result.kind);
    ASSERT_NE(nullptr, result.model_artifact);
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(result.model_artifact, &view, &error));
    EXPECT_NE(std::string::npos,
              std::string(static_cast<const char *>(view.data), view.size)
                  .find("define-fun x"));
    ql_solver_check_result_clear(&result);

    request.artifact_requests = QL_SOLVER_REQUEST_DIAGNOSTICS;
    request.cancel_state = &cancellation_probe;
    request.is_cancelled = counted_immediate_cancellation;
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_check(solver, &request, &result, &error))
        << error.message;
    EXPECT_EQ(QL_SOLVER_CHECK_UNKNOWN, result.kind);
    EXPECT_EQ(QL_SOLVER_UNKNOWN_CANCELLED, result.unknown_reason);
    EXPECT_EQ(1u, cancellation_probe.calls);
    ql_solver_check_result_clear(&result);

    request.artifact_requests = 0u;
    request.cancel_state = nullptr;
    request.is_cancelled = nullptr;
    request.memory_limit_mb = 1024u;
    request.required_features = QL_SOLVER_FEATURE_MEMORY_LIMIT;
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_check(solver, &request, &result, &error))
        << error.message;
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, result.kind);
    ql_solver_check_result_clear(&result);

    request.stdout_limit_bytes = 2u;
    EXPECT_EQ(QL_STATUS_METHOD_ERROR,
              ql_solver_check(solver, &request, &result, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "hard limit"));
    request.stdout_limit_bytes = QL_SOLVER_MAX_STDOUT_BYTES;
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_check(solver, &request, &result, &error))
        << error.message;
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, result.kind);
    ql_solver_check_result_clear(&result);
    ql_artifact_release(formula);
    ql_solver_destroy(solver);

    solver = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_create(nullptr, descriptor, nullptr, &solver,
                               &error));
    constexpr char invalid_term[] = "(assert banana)\n";
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_create(nullptr, QL_ARTIFACT_KIND_SMTLIB2,
                                 QL_SMTLIB2_SCHEMA_VERSION, invalid_term,
                                 sizeof(invalid_term) - 1u, &formula,
                                 &error));
    ASSERT_EQ(QL_STATUS_OK, ql_solver_add_smt2(solver, formula, &error));
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.stderr_limit_bytes = 2u;
    ql_solver_check_result_init(&result);
    EXPECT_EQ(QL_STATUS_METHOD_ERROR,
              ql_solver_check(solver, &request, &result, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "hard limit"));
    ql_solver_check_result_clear(&result);
    ql_artifact_release(formula);
    ql_solver_destroy(solver);

    solver = nullptr;
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_create(nullptr, descriptor, nullptr, &solver,
                               &error));
    formula = build_formula("(distinct x x)");
    ASSERT_NE(nullptr, formula);
    ASSERT_EQ(QL_STATUS_OK, ql_solver_add_smt2(solver, formula, &error));
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    ql_solver_check_result_init(&result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_check(solver, &request, &result, &error))
        << error.message;
    EXPECT_EQ(QL_SOLVER_CHECK_UNSAT, result.kind);
    EXPECT_EQ(nullptr, result.proof_artifact);
    EXPECT_NE(nullptr, result.unsat_metadata_artifact);
    view = {};
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(result.unsat_metadata_artifact, &view,
                                   &error));
    char binary_digest[QL_DIGEST_HEX_SIZE]{};
    ql_digest_hex(&result.backend_binary_digest, binary_digest);
    EXPECT_NE(std::string::npos,
              std::string(static_cast<const char *>(view.data), view.size)
                  .find(std::string("binary_digest=") + binary_digest));
    no_proof.struct_size = sizeof(no_proof);
    no_proof.abi_version = QL_SOLVER_ABI_VERSION;
    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_solver_checked_proof_binding_validate(
                  &result, &no_proof, &error));

    ql_solver_check_result_clear(&result);
    ql_artifact_release(formula);
    ql_solver_destroy(solver);
}

}  // namespace
