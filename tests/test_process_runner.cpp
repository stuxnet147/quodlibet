/* The process runner, tested directly rather than through a backend.
 *
 * src/process_runner.c was extracted from the Bitwuzla adapter so that
 * prove.aig-sat can run CaDiCaL and the LRAT checker under the same discipline
 * instead of growing a second implementation of it. The adapter's own tests
 * still pass unchanged, which is what makes the extraction behaviour
 * preserving; these tests are what stops the discipline from being quietly
 * reintroduced somewhere else, because they name the deadline, the output
 * bound, the cancellation hook, the snapshot and the digest as properties of
 * the runner and of nothing above it.
 *
 * The child is a copy of this test binary. tests/test_fault_injection.cpp
 * installs a static constructor that reads QL_SOLVER_FAULT_PROCESS_MODE before
 * main and misbehaves to order, and it is linked into the same executable, so
 * spawning ourselves with that variable set gives a child whose behaviour the
 * test chooses. An empty stdin still means "version probe" to that child, so
 * every case that wants a fault sends a byte first.
 */

#include "quodlibet/status.h"

/* A shared build exports only the public ABI, so the runner is not linkable
   from here. The static build is where this file has anything to say. */
#if !defined(QL_BUILD_SHARED)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#endif

#include <gtest/gtest.h>

#include "quodlibet/allocator.h"
#include "quodlibet/hash.h"
#include "quodlibet/status.h"

#include "process_runner.h"

namespace {

constexpr char kFaultEnvironment[] = "QL_SOLVER_FAULT_PROCESS_MODE";

bool set_mode(const char *value) {
#if defined(_WIN32)
    return _putenv_s(kFaultEnvironment, value) == 0;
#else
    return value[0] == '\0' ? unsetenv(kFaultEnvironment) == 0
                            : setenv(kFaultEnvironment, value, 1) == 0;
#endif
}

struct ModeGuard {
    explicit ModeGuard(const char *value) { EXPECT_TRUE(set_mode(value)); }
    ~ModeGuard() { (void)set_mode(""); }
};

std::filesystem::path this_executable() {
#if defined(_WIN32)
    std::wstring path(32768u, L'\0');
    const DWORD size =
        GetModuleFileNameW(nullptr, path.data(),
                           static_cast<DWORD>(path.size()));
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

std::string host_path() {
    const std::filesystem::path host = this_executable();
    EXPECT_FALSE(host.empty());
    return host.string();
}

std::uint32_t QL_CALL always_cancelled(const void *) { return 1u; }

/* Enough of a query to keep the child out of its version-probe branch. */
constexpr char kSomeInput[] = "(exit)\n";

}  // namespace

TEST(ProcessRunner, ARunCapturesStdoutAndTheExitStatusOfTheChild) {
    ModeGuard guard("silent");
    ql_process_limits_v1 limits{};
    ql_process_result_v1 result{};
    ql_error error{};
    const std::string host = host_path();

    ql_process_limits_init(&limits);
    limits.timeout_ms = 20000u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_process_run(ql_default_allocator(), host.c_str(), nullptr,
                             0u, nullptr, 0u, &limits, &result, &error))
        << error.message;
    /* Empty stdin is the child's version probe, so this is its answer. */
    EXPECT_EQ(0, result.exit_status);
    EXPECT_EQ(0, result.term_signal);
    EXPECT_EQ(0u, result.timed_out);
    EXPECT_EQ(0u, result.cancelled);
    ASSERT_NE(nullptr, result.stdout_text);
    /* Always terminated, so a caller may scan the capture as text without
       copying it, and an empty capture is an empty string. */
    EXPECT_EQ('\0', result.stdout_text[result.stdout_size]);
    EXPECT_NE(0u, result.stdout_size);
    ASSERT_NE(nullptr, result.stderr_text);
    EXPECT_EQ(0u, result.stderr_size);
    EXPECT_EQ('\0', result.stderr_text[0]);
    ql_process_result_dispose(ql_default_allocator(), &result);
    EXPECT_EQ(nullptr, result.stdout_text);
}

/* A child that fails is not a runner error. The status describes whether the
   run could be carried out; what the tool concluded lives in the result, and
   a caller that conflates the two turns a crash into a verdict. */
TEST(ProcessRunner, AChildThatExitsNonZeroIsAResultRatherThanAFailedRun) {
    ModeGuard guard("crash");
    ql_process_limits_v1 limits{};
    ql_process_result_v1 result{};
    ql_error error{};
    const std::string host = host_path();

    ql_process_limits_init(&limits);
    limits.timeout_ms = 20000u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_process_run(ql_default_allocator(), host.c_str(), nullptr,
                             0u, kSomeInput, std::strlen(kSomeInput), &limits,
                             &result, &error))
        << error.message;
    EXPECT_EQ(3, result.exit_status);
    EXPECT_EQ(0u, result.killed);
    ql_process_result_dispose(ql_default_allocator(), &result);
}

/* The bound is on the host's memory, not on the child's manners. */
TEST(ProcessRunner, OutputAboveTheLimitFailsInsteadOfGrowingTheHost) {
    ModeGuard guard("runaway");
    ql_process_limits_v1 limits{};
    ql_process_result_v1 result{};
    ql_error error{};
    const std::string host = host_path();

    ql_process_limits_init(&limits);
    limits.timeout_ms = 20000u;
    limits.stdout_limit_bytes = 4096u;
    EXPECT_EQ(QL_STATUS_METHOD_ERROR,
              ql_process_run(ql_default_allocator(), host.c_str(), nullptr,
                             0u, kSomeInput, std::strlen(kSomeInput), &limits,
                             &result, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "solver stdout"))
        << "message: " << error.message;
    /* A failed run hands back nothing to free. */
    EXPECT_EQ(nullptr, result.stdout_text);
    ql_process_result_dispose(ql_default_allocator(), &result);
}

/* The watchdog is what makes a wall-clock budget mean anything at the process
   boundary: a tool that never answers must be stopped and reported as stopped,
   never allowed to look like an answer. */
TEST(ProcessRunner, AHangingChildIsStoppedAtTheDeadlineAndSaysSo) {
    ModeGuard guard("hang");
    ql_process_limits_v1 limits{};
    ql_process_result_v1 result{};
    ql_error error{};
    const std::string host = host_path();

    ql_process_limits_init(&limits);
    limits.timeout_ms = 300u;
    ASSERT_EQ(QL_STATUS_OK,
              ql_process_run(ql_default_allocator(), host.c_str(), nullptr,
                             0u, kSomeInput, std::strlen(kSomeInput), &limits,
                             &result, &error))
        << error.message;
    EXPECT_EQ(1u, result.timed_out);
    EXPECT_EQ(1u, result.killed);
    EXPECT_EQ(0u, result.cancelled);
    ql_process_result_dispose(ql_default_allocator(), &result);
}

TEST(ProcessRunner, ACancellationHookStopsTheChildAndIsReportedAsCancelled) {
    ModeGuard guard("hang");
    ql_process_limits_v1 limits{};
    ql_process_result_v1 result{};
    ql_error error{};
    const std::string host = host_path();

    ql_process_limits_init(&limits);
    limits.timeout_ms = 20000u;
    limits.is_cancelled = always_cancelled;
    ASSERT_EQ(QL_STATUS_OK,
              ql_process_run(ql_default_allocator(), host.c_str(), nullptr,
                             0u, kSomeInput, std::strlen(kSomeInput), &limits,
                             &result, &error))
        << error.message;
    EXPECT_EQ(1u, result.cancelled);
    EXPECT_EQ(1u, result.killed);
    EXPECT_EQ(0u, result.timed_out);
    ql_process_result_dispose(ql_default_allocator(), &result);
}

TEST(ProcessRunner, AnExecutableThatIsNotThereIsNotFound) {
    ql_process_result_v1 result{};
    ql_error error{};
    const std::string missing =
        (std::filesystem::temp_directory_path() /
         "quodlibet-no-such-executable-9d3f")
            .string();

    EXPECT_EQ(QL_STATUS_NOT_FOUND,
              ql_process_run(ql_default_allocator(), missing.c_str(), nullptr,
                             0u, nullptr, 0u, nullptr, &result, &error));
    EXPECT_EQ(nullptr, result.stdout_text);
}

/* The digest is of the snapshot, because the snapshot is what runs. A binary
   swapped underneath the run cannot change what answered, and the evidence
   names the bytes rather than the path. */
TEST(ProcessRunner, ASnapshotIsAPrivateCopyWhoseDigestIsOfTheBytesThatRun) {
    namespace fs = std::filesystem;
    ql_process_snapshot snapshot{};
    ql_digest source_digest{};
    ql_error error{};
    const std::string host = host_path();
#if defined(_WIN32)
    const char *name = "probe.exe";
#else
    const char *name = "probe";
#endif

    ASSERT_EQ(QL_STATUS_OK,
              ql_process_executable_digest(host.c_str(), &source_digest,
                                           &error))
        << error.message;
    ASSERT_EQ(QL_STATUS_OK,
              ql_process_snapshot_create(ql_default_allocator(), host.c_str(),
                                         name, &snapshot, &error))
        << error.message;
    ASSERT_NE(nullptr, snapshot.executable_path);
    ASSERT_NE(nullptr, snapshot.directory);
    const fs::path copy(snapshot.executable_path);
    const fs::path directory(snapshot.directory);
    EXPECT_TRUE(fs::exists(copy));
    EXPECT_EQ(name, copy.filename().string());
    EXPECT_TRUE(ql_digest_equal(&source_digest, &snapshot.digest));
    /* The directory is named after the tool, not after the file: an extension
       in a directory name reads like a mistake, and the adapter's own
       snapshots are still quodlibet-bitwuzla-<pid>-* because of it. */
    EXPECT_EQ(0u, directory.filename().string().rfind("quodlibet-probe-", 0u));

    ql_process_snapshot_dispose(&snapshot);
    EXPECT_FALSE(fs::exists(copy));
    EXPECT_FALSE(fs::exists(directory));
    EXPECT_EQ(nullptr, snapshot.executable_path);
    /* Safe on a zeroed snapshot, so a failed create needs no special case. */
    ql_process_snapshot_dispose(&snapshot);
}

TEST(ProcessRunner, ASnapshotNameMayNotCarryAPathSeparator) {
    ql_process_snapshot snapshot{};
    ql_error error{};
    const std::string host = host_path();

    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_process_snapshot_create(ql_default_allocator(), host.c_str(),
                                         "../escape", &snapshot, &error));
    EXPECT_EQ(nullptr, snapshot.executable_path);
}

#endif /* !QL_BUILD_SHARED */
