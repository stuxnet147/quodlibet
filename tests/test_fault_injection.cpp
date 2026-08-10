/* Fault injection at the solver adapter boundary.

   The existing solver tests drive a backend that behaves. These drive one
   that does not, because the adapter's job is to stand between a misbehaving
   or lying backend and the rest of the core. A backend that returns SAT while
   handing back an UNSAT proof, or that reports an error while leaving
   artifacts allocated, must not reach a caller.

   The axes already covered elsewhere are not repeated here:

     - cancellation and the watchdog-timeout classification
       (tests/test_solver.cpp, SolverUnknownReason, MockSupportsSatModel...);
     - the SMT-LIB transcript hard limit
       (SmtLibBuilder.EnforcesHardTranscriptLimitBeforeAllocation,
        BitwuzlaSolver.EnforcesHardLimitOnCompleteTerminalQuery);
     - inherited pipes outliving the direct child
       (BitwuzlaTransport.BoundsInheritedPipesAfterDirectChildExit);
     - executable snapshot tampering
       (BitwuzlaSolver.RunsPrivateSnapshotAfterOverrideIsRemovedAndCleansIt);
     - a model that decodes but does not reproduce (tests/test_replay.cpp).

   What is added is the backend that answers wrongly rather than slowly. */

#include "quodlibet/solver.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include <gtest/gtest.h>

namespace {

/* ---------------------------------------------------------------------- *
 * The process transport half: a solver executable that misbehaves.
 *
 * The pinned Bitwuzla adapter accepts an `executable` option and snapshots
 * whatever it points at, so a copy of this test binary can stand in for the
 * solver. Which fault it performs is read from the environment at spawn, so
 * one snapshot serves every case; taking a fresh snapshot per fault would
 * copy and hash the whole test executable each time.
 *
 * The child tells a version probe from a check by its stdin: the adapter
 * probes with `--version` and no input at all, and every real query ends with
 * `(exit)`. Reading argv from a static constructor is not portable; reading
 * stdin to end is.
 * ---------------------------------------------------------------------- */

constexpr char kFaultEnvironment[] = "QL_SOLVER_FAULT_PROCESS_MODE";

std::string fault_process_mode() {
#if defined(_WIN32)
    char *value = nullptr;
    std::size_t size = 0u;
    std::string result;
    if (_dupenv_s(&value, &size, kFaultEnvironment) == 0 &&
        value != nullptr) {
        result.assign(value);
    }
    std::free(value);
    return result;
#else
    const char *value = std::getenv(kFaultEnvironment);
    return value == nullptr ? std::string() : std::string(value);
#endif
}

bool set_fault_process_mode(const char *value) {
#if defined(_WIN32)
    return _putenv_s(kFaultEnvironment, value) == 0;
#else
    return value[0] == '\0' ? unsetenv(kFaultEnvironment) == 0
                            : setenv(kFaultEnvironment, value, 1) == 0;
#endif
}

struct FaultEnvironmentGuard {
    ~FaultEnvironmentGuard() { (void)set_fault_process_mode(""); }
};

void write_stdout(const char *text, std::size_t size) {
    (void)std::fwrite(text, 1u, size, stdout);
    (void)std::fflush(stdout);
}

/* Runs before main in the snapshot copy the adapter spawns. In the ordinary
   test process the environment variable is unset and this does nothing. */
struct SolverFaultProcessMode {
    SolverFaultProcessMode() {
        const std::string mode = fault_process_mode();
        std::string input;
        char chunk[4096];
        std::size_t read_size;

        if (mode.empty()) {
            return;
        }
#if defined(_WIN32)
        /* Text mode would rewrite every newline on the way out. The version
           probe tolerates both spellings, but a model artifact must be the
           exact bytes the solver wrote. */
        (void)_setmode(_fileno(stdout), _O_BINARY);
        (void)_setmode(_fileno(stderr), _O_BINARY);
#endif
        while ((read_size = std::fread(chunk, 1u, sizeof(chunk), stdin)) !=
               0u) {
            input.append(chunk, read_size);
        }
        if (input.empty()) {
            /* The version probe. It insists on the exact pinned version on
               stdout, nothing on stderr, and exit zero. */
            const std::string version =
                std::string(ql_bitwuzla_solver_descriptor()->version) + "\n";
            write_stdout(version.data(), version.size());
            std::_Exit(0);
        }
        if (mode == "crash") {
            std::_Exit(3);
        }
        if (mode == "silent") {
            std::_Exit(0);
        }
        if (mode == "truncated") {
            write_stdout("sa", 2u);
            std::_Exit(0);
        }
        if (mode == "garbage") {
            static const char body[] = "SAT?\n(model)\n";
            write_stdout(body, sizeof(body) - 1u);
            std::_Exit(0);
        }
        if (mode == "stderr-only") {
            static const char message[] = "unexpected token at line 1\n";
            (void)std::fwrite(message, 1u, sizeof(message) - 1u, stderr);
            (void)std::fflush(stderr);
            std::_Exit(0);
        }
        if (mode == "sat-without-model") {
            write_stdout("sat\n", 4u);
            std::_Exit(0);
        }
        if (mode == "corrupt-model") {
            /* A leading answer line the adapter parses, then bytes no model
               reader can make sense of, including a NUL. */
            static const char body[] =
                "sat\n(\xff\xfe not a model at all \0 )\n";
            write_stdout(body, sizeof(body) - 1u);
            std::_Exit(0);
        }
        if (mode == "runaway") {
            const std::string noise(65536u, 'x');
            write_stdout("sat\n", 4u);
            for (std::size_t round = 0u; round < 256u; ++round) {
                write_stdout(noise.data(), noise.size());
            }
            std::_Exit(0);
        }
        if (mode == "hang") {
            std::this_thread::sleep_for(std::chrono::seconds(20));
            std::_Exit(0);
        }
        /* An unrecognised mode must not look like a working solver. */
        std::_Exit(4);
    }
};

const SolverFaultProcessMode kSolverFaultProcessMode;

/* Counts live allocations so an error path that leaves an artifact behind is
   a failing test rather than a leak only a sanitizer build would notice. The
   core is C and its artifacts are reference counted, so the count returning
   to its starting value is the whole property. */
struct CountingAllocator {
    std::atomic<std::size_t> live{0u};

    static void *QL_CALL Allocate(void *user_data, std::size_t size) {
        auto *self = static_cast<CountingAllocator *>(user_data);
        void *pointer = std::malloc(size);
        if (pointer != nullptr) {
            self->live.fetch_add(1u, std::memory_order_relaxed);
        }
        return pointer;
    }

    static void *QL_CALL Reallocate(void *user_data, void *pointer,
                                    std::size_t size) {
        auto *self = static_cast<CountingAllocator *>(user_data);
        void *result = std::realloc(pointer, size);
        if (pointer == nullptr && result != nullptr) {
            self->live.fetch_add(1u, std::memory_order_relaxed);
        }
        return result;
    }

    static void QL_CALL Deallocate(void *user_data, void *pointer) {
        auto *self = static_cast<CountingAllocator *>(user_data);
        if (pointer != nullptr) {
            self->live.fetch_sub(1u, std::memory_order_relaxed);
        }
        std::free(pointer);
    }

    ql_allocator View() {
        ql_allocator allocator{};
        allocator.user_data = this;
        allocator.allocate = &Allocate;
        allocator.reallocate = &Reallocate;
        allocator.deallocate = &Deallocate;
        return allocator;
    }
};

/* Everything the hostile backend does on its next check. It is global rather
   than carried in the backend state so a test can rearm it between checks on
   one solver, which is also how the process-level faults will be driven. */
enum class Fault {
    kHonestSat,
    kReturnsError,
    kReturnsErrorHoldingArtifacts,
    kSatCarryingProof,
    kSatCarryingUnsatMetadata,
    kUnsatCarryingModel,
    kUnknownCarryingModel,
    kUnrequestedModel,
    kUnrequestedDiagnostics,
    kModelWithWrongKind,
    kMissingRequestedModel,
    kZeroBackendDigest,
    kInvalidKind,
    kUnknownWithoutReason,
    kSatWithUnknownReason,
    kOutOfRangeUnknownReason,
    kRewritesAbiVersion,
    kShrinksStructSize,
};

Fault g_fault = Fault::kHonestSat;

struct HostileState {
    ql_allocator allocator;
};

ql_status QL_CALL hostile_create(const ql_allocator *allocator, const char *,
                                 void **output, ql_error *error) {
    auto *state = static_cast<HostileState *>(
        allocator->allocate(allocator->user_data, sizeof(HostileState)));
    if (state == nullptr) {
        ql_error_set(error, QL_STATUS_OUT_OF_MEMORY, "hostile backend OOM");
        return QL_STATUS_OUT_OF_MEMORY;
    }
    state->allocator = *allocator;
    *output = state;
    return QL_STATUS_OK;
}

void QL_CALL hostile_destroy(void *opaque) {
    auto *state = static_cast<HostileState *>(opaque);
    if (state != nullptr) {
        const ql_allocator allocator = state->allocator;
        allocator.deallocate(allocator.user_data, state);
    }
}

ql_status QL_CALL hostile_add(void *, const char *, std::size_t, ql_error *) {
    return QL_STATUS_OK;
}

ql_status QL_CALL hostile_stack(void *, std::uint32_t, ql_error *) {
    return QL_STATUS_OK;
}

ql_artifact *MakeArtifact(HostileState *state, const char *kind,
                          const char *text) {
    ql_artifact *artifact = nullptr;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK,
              ql_artifact_create(&state->allocator, kind,
                                 QL_SOLVER_ARTIFACT_SCHEMA_VERSION, text,
                                 std::strlen(text), &artifact, &error))
        << error.message;
    return artifact;
}

ql_status QL_CALL hostile_check(void *opaque,
                                const ql_solver_check_request_v1 *,
                                ql_solver_check_result_v1 *result,
                                ql_error *error) {
    auto *state = static_cast<HostileState *>(opaque);

    ql_digest_data("hostile-backend", 15u, &result->backend_binary_digest);
    ql_digest_data("hostile-query", 13u, &result->query_digest);
    result->kind = QL_SOLVER_CHECK_SAT;

    switch (g_fault) {
    case Fault::kHonestSat:
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        break;
    case Fault::kReturnsError:
        ql_error_set(error, QL_STATUS_IO_ERROR, "hostile backend failed");
        return QL_STATUS_IO_ERROR;
    case Fault::kReturnsErrorHoldingArtifacts:
        /* The contract is that the adapter owns whatever the backend wrote
           into the result, including on the failing return. A backend that
           allocated and then failed must not leak through it. */
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        result->diagnostics_artifact = MakeArtifact(
            state, QL_ARTIFACT_KIND_SOLVER_DIAGNOSTICS, "stderr noise\n");
        ql_error_set(error, QL_STATUS_IO_ERROR,
                     "hostile backend failed after allocating");
        return QL_STATUS_IO_ERROR;
    case Fault::kSatCarryingProof:
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        result->proof_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_PROOF, "proof\n");
        break;
    case Fault::kSatCarryingUnsatMetadata:
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        result->unsat_metadata_artifact = MakeArtifact(
            state, QL_ARTIFACT_KIND_SOLVER_UNSAT_METADATA, "backend=x\n");
        break;
    case Fault::kUnsatCarryingModel:
        result->kind = QL_SOLVER_CHECK_UNSAT;
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        break;
    case Fault::kUnknownCarryingModel:
        result->kind = QL_SOLVER_CHECK_UNKNOWN;
        result->unknown_reason = QL_SOLVER_UNKNOWN_INCOMPLETE;
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        break;
    case Fault::kUnrequestedModel:
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        break;
    case Fault::kUnrequestedDiagnostics:
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        result->diagnostics_artifact = MakeArtifact(
            state, QL_ARTIFACT_KIND_SOLVER_DIAGNOSTICS, "noise\n");
        break;
    case Fault::kModelWithWrongKind:
        /* A model-shaped payload wearing another kind's label. Reading the
           label rather than trusting the slot is the whole point. */
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_PROOF, "x=#x2a\n");
        break;
    case Fault::kMissingRequestedModel:
        break;
    case Fault::kZeroBackendDigest:
        std::memset(&result->backend_binary_digest, 0,
                    sizeof(result->backend_binary_digest));
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        break;
    case Fault::kInvalidKind:
        result->kind = static_cast<ql_solver_check_kind>(99);
        break;
    case Fault::kUnknownWithoutReason:
        result->kind = QL_SOLVER_CHECK_UNKNOWN;
        result->unknown_reason = QL_SOLVER_UNKNOWN_NONE;
        break;
    case Fault::kSatWithUnknownReason:
        result->unknown_reason = QL_SOLVER_UNKNOWN_TIMEOUT;
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        break;
    case Fault::kOutOfRangeUnknownReason:
        result->kind = QL_SOLVER_CHECK_UNKNOWN;
        result->unknown_reason = static_cast<ql_solver_unknown_reason>(77);
        break;
    case Fault::kRewritesAbiVersion:
        result->abi_version = QL_SOLVER_ABI_VERSION + 1u;
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        break;
    case Fault::kShrinksStructSize:
        result->struct_size = sizeof(std::size_t);
        result->model_artifact =
            MakeArtifact(state, QL_ARTIFACT_KIND_SOLVER_MODEL, "x=#x2a\n");
        break;
    }
    return QL_STATUS_OK;
}

const ql_solver_descriptor_v1 kHostileDescriptor = {
    sizeof(ql_solver_descriptor_v1),
    QL_SOLVER_ABI_VERSION,
    0u,
    "hostile",
    "1.0",
    "a backend that answers wrongly",
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
    hostile_create,
    hostile_destroy,
    hostile_add,
    hostile_stack,
    hostile_stack,
    hostile_check,
    /* No session support: this descriptor exercises the path where a caller
       asks for a session and has to fall back. */
    nullptr,
    nullptr,
    nullptr,
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

/* One check against the hostile backend under a private allocator, returning
   the status. `expect_no_leak` says whether nothing the backend allocated may
   survive; see RewrittenResultHeaderLeaksWhatTheBackendAllocated for the one
   case where that does not hold today. */
ql_status CheckUnderFault(Fault fault, std::uint32_t artifact_requests,
                          bool expect_no_leak,
                          ql_solver_check_result_v1 *result,
                          ql_error *error) {
    CountingAllocator counter;
    const ql_allocator allocator = counter.View();
    ql_solver *solver = nullptr;
    ql_solver_check_request_v1 request{};
    ql_status status;

    g_fault = fault;
    EXPECT_EQ(QL_STATUS_OK,
              ql_solver_create(&allocator, &kHostileDescriptor, nullptr,
                               &solver, error))
        << error->message;
    if (solver == nullptr) {
        return QL_STATUS_INTERNAL_ERROR;
    }
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.maximum_bv_width = 8u;
    request.artifact_requests = artifact_requests;
    ql_solver_check_result_init(result);
    status = ql_solver_check(solver, &request, result, error);
    ql_solver_check_result_clear(result);
    ql_solver_destroy(solver);
    g_fault = Fault::kHonestSat;
    /* Whatever the backend allocated is the adapter's to release, on the
       failing return as much as on the succeeding one. */
    if (expect_no_leak) {
        EXPECT_EQ(0u, counter.live.load(std::memory_order_relaxed))
            << "the adapter leaked what the backend allocated";
    }
    return status;
}

struct FaultCase {
    Fault fault;
    std::uint32_t artifact_requests;
    ql_status expected;
    const char *phrase;
};

const FaultCase kHeaderRewriteCases[] = {
    {Fault::kRewritesAbiVersion, QL_SOLVER_REQUEST_MODEL,
     QL_STATUS_ABI_MISMATCH, "ABI"},
    {Fault::kShrinksStructSize, QL_SOLVER_REQUEST_MODEL,
     QL_STATUS_ABI_MISMATCH, "ABI"},
};

TEST(SolverFaultInjection, RejectsEveryContradictoryResultAndLeaksNothing) {
    const FaultCase cases[] = {
        {Fault::kHonestSat, QL_SOLVER_REQUEST_MODEL, QL_STATUS_OK, nullptr},
        {Fault::kSatCarryingProof, QL_SOLVER_REQUEST_MODEL,
         QL_STATUS_TYPE_MISMATCH, "SAT result cannot carry"},
        {Fault::kSatCarryingUnsatMetadata, QL_SOLVER_REQUEST_MODEL,
         QL_STATUS_TYPE_MISMATCH, "SAT result cannot carry"},
        {Fault::kUnsatCarryingModel, QL_SOLVER_REQUEST_MODEL,
         QL_STATUS_TYPE_MISMATCH, "UNSAT result cannot carry a model"},
        {Fault::kUnknownCarryingModel, QL_SOLVER_REQUEST_MODEL,
         QL_STATUS_TYPE_MISMATCH, "UNKNOWN result cannot carry"},
        {Fault::kUnrequestedModel, 0u, QL_STATUS_TYPE_MISMATCH,
         "unrequested model"},
        {Fault::kUnrequestedDiagnostics, QL_SOLVER_REQUEST_MODEL,
         QL_STATUS_TYPE_MISMATCH, "unrequested diagnostics"},
        {Fault::kModelWithWrongKind, QL_SOLVER_REQUEST_MODEL,
         QL_STATUS_TYPE_MISMATCH, "is not"},
        {Fault::kMissingRequestedModel, QL_SOLVER_REQUEST_MODEL,
         QL_STATUS_INVALID_ARGUMENT, "missing"},
        {Fault::kZeroBackendDigest, QL_SOLVER_REQUEST_MODEL,
         QL_STATUS_INVALID_ARGUMENT, "binary digest"},
        {Fault::kInvalidKind, 0u, QL_STATUS_INVALID_ARGUMENT,
         "kind is invalid"},
        {Fault::kUnknownWithoutReason, 0u, QL_STATUS_INVALID_ARGUMENT,
         "disagree"},
        {Fault::kSatWithUnknownReason, QL_SOLVER_REQUEST_MODEL,
         QL_STATUS_INVALID_ARGUMENT, "disagree"},
        {Fault::kOutOfRangeUnknownReason, 0u, QL_STATUS_INVALID_ARGUMENT,
         nullptr},
        {Fault::kReturnsError, QL_SOLVER_REQUEST_MODEL, QL_STATUS_IO_ERROR,
         "hostile backend failed"},
        {Fault::kReturnsErrorHoldingArtifacts,
         QL_SOLVER_REQUEST_MODEL | QL_SOLVER_REQUEST_DIAGNOSTICS,
         QL_STATUS_IO_ERROR, "after allocating"},
    };

    for (std::size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        const FaultCase &item = cases[index];
        ql_solver_check_result_v1 result{};
        ql_error error{};
        SCOPED_TRACE(::testing::Message() << "fault case " << index);

        EXPECT_EQ(item.expected,
                  CheckUnderFault(item.fault, item.artifact_requests, true,
                                  &result, &error))
            << error.message;
        if (item.phrase != nullptr && item.expected != QL_STATUS_OK) {
            EXPECT_NE(nullptr, std::strstr(error.message, item.phrase))
                << "message: " << error.message;
        }
    }
}

/* A backend that overwrites the result header is rejected, and everything it
   allocated is still released.

   Both halves matter and they used to disagree. ql_solver_check_result_clear
   only releases when abi_version and struct_size still look right, which is
   the correct refusal for a structure of unknown layout arriving from
   outside: those pointer fields might not be artifacts at all. Inside
   ql_solver_check the layout is not unknown - the adapter initialised the
   structure itself and the backend only overwrote fields in it - so the
   adapter restores the two header fields it knows are true before clearing.
   Without that, a backend could strand every artifact it allocated simply by
   scribbling on a header field. */
TEST(SolverFaultInjection, ARewrittenResultHeaderIsRejectedWithoutLeaking) {
    for (std::size_t index = 0u;
         index < sizeof(kHeaderRewriteCases) / sizeof(kHeaderRewriteCases[0]);
         ++index) {
        const FaultCase &item = kHeaderRewriteCases[index];
        CountingAllocator counter;
        const ql_allocator allocator = counter.View();
        ql_solver *solver = nullptr;
        ql_solver_check_request_v1 request{};
        ql_solver_check_result_v1 result{};
        ql_error error{};
        SCOPED_TRACE(::testing::Message() << "header rewrite case " << index);

        g_fault = item.fault;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_solver_create(&allocator, &kHostileDescriptor, nullptr,
                                   &solver, &error))
            << error.message;
        ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
        request.maximum_bv_width = 8u;
        request.artifact_requests = item.artifact_requests;
        ql_solver_check_result_init(&result);
        EXPECT_EQ(item.expected,
                  ql_solver_check(solver, &request, &result, &error));
        EXPECT_NE(nullptr, std::strstr(error.message, item.phrase))
            << "message: " << error.message;
        EXPECT_EQ(QL_SOLVER_CHECK_INVALID, result.kind);
        ql_solver_check_result_clear(&result);
        ql_solver_destroy(solver);
        g_fault = Fault::kHonestSat;
        EXPECT_EQ(0u, counter.live.load(std::memory_order_relaxed))
            << "the adapter leaked what the backend allocated";
    }
}

/* Repeating it must not accumulate either. A single call leaking three
   allocations is invisible next to a process that is about to exit; a scorer
   running millions of judgements is not. */
TEST(SolverFaultInjection, RepeatedHeaderRewritesDoNotAccumulate) {
    CountingAllocator counter;
    const ql_allocator allocator = counter.View();
    ql_solver *solver = nullptr;
    ql_solver_check_request_v1 request{};
    ql_error error{};

    g_fault = Fault::kRewritesAbiVersion;
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_create(&allocator, &kHostileDescriptor, nullptr,
                               &solver, &error))
        << error.message;
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.maximum_bv_width = 8u;
    request.artifact_requests = QL_SOLVER_REQUEST_MODEL;
    for (std::size_t round = 0u; round < 64u; ++round) {
        ql_solver_check_result_v1 result{};
        ql_solver_check_result_init(&result);
        EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
                  ql_solver_check(solver, &request, &result, &error));
        ql_solver_check_result_clear(&result);
    }
    ql_solver_destroy(solver);
    g_fault = Fault::kHonestSat;
    EXPECT_EQ(0u, counter.live.load(std::memory_order_relaxed));
}

/* A rejected check must not hand the caller a half-populated result. A caller
   that read `kind` after an error would see SAT from a backend the adapter
   just refused. */
TEST(SolverFaultInjection, ARejectedCheckLeavesTheCallerResultUntouched) {
    CountingAllocator counter;
    const ql_allocator allocator = counter.View();
    ql_solver *solver = nullptr;
    ql_solver_check_request_v1 request{};
    ql_solver_check_result_v1 result{};
    ql_error error{};

    g_fault = Fault::kSatCarryingProof;
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_create(&allocator, &kHostileDescriptor, nullptr,
                               &solver, &error))
        << error.message;
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.maximum_bv_width = 8u;
    request.artifact_requests = QL_SOLVER_REQUEST_MODEL;
    ql_solver_check_result_init(&result);

    EXPECT_EQ(QL_STATUS_TYPE_MISMATCH,
              ql_solver_check(solver, &request, &result, &error));
    EXPECT_EQ(QL_SOLVER_CHECK_INVALID, result.kind);
    EXPECT_EQ(nullptr, result.model_artifact);
    EXPECT_EQ(nullptr, result.proof_artifact);
    EXPECT_EQ(nullptr, result.unsat_metadata_artifact);
    EXPECT_EQ(nullptr, result.diagnostics_artifact);

    ql_solver_check_result_clear(&result);
    ql_solver_destroy(solver);
    g_fault = Fault::kHonestSat;
    EXPECT_EQ(0u, counter.live.load(std::memory_order_relaxed));
}

/* The adapter stamps backend identity from the descriptor rather than taking
   the backend's word for it, so a backend cannot answer under another name.
   Identity is what a trusted-backend proof policy is keyed on. */
TEST(SolverFaultInjection, BackendIdentityComesFromTheDescriptor) {
    CountingAllocator counter;
    const ql_allocator allocator = counter.View();
    ql_solver *solver = nullptr;
    ql_solver_check_request_v1 request{};
    ql_solver_check_result_v1 result{};
    ql_error error{};

    g_fault = Fault::kHonestSat;
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_create(&allocator, &kHostileDescriptor, nullptr,
                               &solver, &error))
        << error.message;
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.maximum_bv_width = 8u;
    request.artifact_requests = QL_SOLVER_REQUEST_MODEL;
    ql_solver_check_result_init(&result);
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_check(solver, &request, &result, &error))
        << error.message;
    EXPECT_STREQ(kHostileDescriptor.name, result.backend_name);
    EXPECT_STREQ(kHostileDescriptor.version, result.backend_version);

    ql_solver_check_result_clear(&result);
    ql_solver_destroy(solver);
    EXPECT_EQ(0u, counter.live.load(std::memory_order_relaxed));
}

/* Repeating a rejected check on the same solver must stay rejected and must
   not accumulate. A per-check leak is invisible in a single call. */
TEST(SolverFaultInjection, RepeatedRejectedChecksDoNotAccumulate) {
    CountingAllocator counter;
    const ql_allocator allocator = counter.View();
    ql_solver *solver = nullptr;
    ql_solver_check_request_v1 request{};
    ql_error error{};
    std::size_t after_first = 0u;

    g_fault = Fault::kReturnsErrorHoldingArtifacts;
    ASSERT_EQ(QL_STATUS_OK,
              ql_solver_create(&allocator, &kHostileDescriptor, nullptr,
                               &solver, &error))
        << error.message;
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.maximum_bv_width = 8u;
    request.artifact_requests =
        QL_SOLVER_REQUEST_MODEL | QL_SOLVER_REQUEST_DIAGNOSTICS;
    for (std::size_t round = 0u; round < 64u; ++round) {
        ql_solver_check_result_v1 result{};
        ql_solver_check_result_init(&result);
        EXPECT_EQ(QL_STATUS_IO_ERROR,
                  ql_solver_check(solver, &request, &result, &error));
        ql_solver_check_result_clear(&result);
        if (round == 0u) {
            after_first = counter.live.load(std::memory_order_relaxed);
        }
    }
    EXPECT_EQ(after_first, counter.live.load(std::memory_order_relaxed));

    ql_solver_destroy(solver);
    g_fault = Fault::kHonestSat;
    EXPECT_EQ(0u, counter.live.load(std::memory_order_relaxed));
}


/* ---------------------------------------------------------------------- *
 * Process transport faults.
 * ---------------------------------------------------------------------- */

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

/* One solver whose executable is a copy of this test binary, plus the
   scratch directory holding that copy. The snapshot the adapter takes at
   create time is the expensive step, so every fault in a test reuses it and
   only the environment changes between checks. */
class FakeSolver {
public:
    FakeSolver() = default;
    FakeSolver(const FakeSolver &) = delete;
    FakeSolver &operator=(const FakeSolver &) = delete;

    ~FakeSolver() {
        ql_artifact_release(formula_);
        ql_solver_destroy(solver_);
        if (!directory_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(directory_, ignored);
        }
        (void)set_fault_process_mode("");
    }

    /* Returns false when Bitwuzla support is off, which is the only reason
       to skip: the adapter under test is the Bitwuzla one. */
    bool Start() {
        namespace fs = std::filesystem;
        const ql_solver_descriptor_v1 *descriptor =
            ql_bitwuzla_solver_descriptor();
        ql_error error{};

        if (descriptor->capability.availability == QL_SOLVER_UNAVAILABLE) {
            return false;
        }
        const fs::path host = this_executable();
        EXPECT_FALSE(host.empty());
        if (host.empty()) {
            return false;
        }
        const auto unique = std::chrono::high_resolution_clock::now()
                                .time_since_epoch()
                                .count();
        directory_ = fs::temp_directory_path() /
                     ("quodlibet-fault-injection-" + std::to_string(unique));
        EXPECT_TRUE(fs::create_directory(directory_));
#if defined(_WIN32)
        const fs::path override_path = directory_ / "override.exe";
#else
        const fs::path override_path = directory_ / "override";
#endif
        EXPECT_TRUE(fs::copy_file(host, override_path));
        /* Armed before create so the version probe is answered by the copy
           rather than by a process that thinks it is a test runner. */
        EXPECT_TRUE(set_fault_process_mode("silent"));
        const std::string options = std::string("{\"executable\":\"") +
                                    override_path.generic_string() + "\"}";
        const ql_status status = ql_solver_create(
            nullptr, descriptor, options.c_str(), &solver_, &error);
        EXPECT_EQ(QL_STATUS_OK, status) << error.message;
        if (status != QL_STATUS_OK) {
            return false;
        }
        ql_smt2_builder *builder = nullptr;
        EXPECT_EQ(QL_STATUS_OK,
                  ql_smt2_builder_create(nullptr, QL_SOLVER_LOGIC_QF_BV,
                                         &builder, &error))
            << error.message;
        EXPECT_EQ(QL_STATUS_OK,
                  ql_smt2_builder_declare_bv(builder, "x", 8u, &error));
        EXPECT_EQ(QL_STATUS_OK,
                  ql_smt2_builder_assert(builder, "(= x #x2a)", &error));
        EXPECT_EQ(QL_STATUS_OK,
                  ql_smt2_builder_build(builder, &formula_, &error));
        ql_smt2_builder_destroy(builder);
        EXPECT_EQ(QL_STATUS_OK,
                  ql_solver_add_smt2(solver_, formula_, &error))
            << error.message;
        return true;
    }

    ql_status CheckUnder(const char *mode,
                         const ql_solver_check_request_v1 &request,
                         ql_solver_check_result_v1 *result,
                         ql_error *error) {
        EXPECT_TRUE(set_fault_process_mode(mode));
        ql_solver_check_result_init(result);
        return ql_solver_check(solver_, &request, result, error);
    }

private:
    std::filesystem::path directory_;
    ql_solver *solver_ = nullptr;
    ql_artifact *formula_ = nullptr;
};

ql_solver_check_request_v1 BaseRequest() {
    ql_solver_check_request_v1 request{};
    ql_solver_check_request_init(&request, QL_SOLVER_LOGIC_QF_BV);
    request.maximum_bv_width = 8u;
    request.artifact_requests = 0u;
    return request;
}

/* Every way the solver process can answer badly short of answering slowly.
   None of them may produce a check kind: a caller that read `kind` after one
   of these would be reading a verdict out of a broken process. */
TEST(SolverProcessFaultInjection, RefusesEveryMalformedProcessAnswer) {
    FakeSolver fake;
    FaultEnvironmentGuard guard;

    if (!fake.Start()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    struct Case {
        const char *mode;
        std::uint32_t artifact_requests;
        ql_status expected;
        const char *phrase;
    };
    const Case cases[] = {
        {"crash", 0u, QL_STATUS_METHOD_ERROR, "exited with status"},
        {"silent", 0u, QL_STATUS_METHOD_ERROR, "does not begin with"},
        {"truncated", 0u, QL_STATUS_METHOD_ERROR, "does not begin with"},
        {"garbage", 0u, QL_STATUS_METHOD_ERROR, "does not begin with"},
        {"stderr-only", 0u, QL_STATUS_PARSE_ERROR, "unexpected token"},
        {"sat-without-model", QL_SOLVER_REQUEST_MODEL,
         QL_STATUS_METHOD_ERROR, "without the requested model"},
    };

    for (std::size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        const Case &item = cases[index];
        ql_solver_check_request_v1 request = BaseRequest();
        ql_solver_check_result_v1 result{};
        ql_error error{};
        SCOPED_TRACE(::testing::Message() << "mode " << item.mode);

        request.artifact_requests = item.artifact_requests;
        EXPECT_EQ(item.expected,
                  fake.CheckUnder(item.mode, request, &result, &error));
        EXPECT_NE(nullptr, std::strstr(error.message, item.phrase))
            << "message: " << error.message;
        EXPECT_EQ(QL_SOLVER_CHECK_INVALID, result.kind);
        EXPECT_EQ(nullptr, result.model_artifact);
        EXPECT_EQ(nullptr, result.proof_artifact);
        EXPECT_EQ(nullptr, result.unsat_metadata_artifact);
        ql_solver_check_result_clear(&result);
    }
}

/* A solver that answers SAT and then writes an unparseable model still gets
   its bytes recorded verbatim. The adapter is a transport: inventing a
   plausible model here, or silently dropping the artifact, would both hide
   the corruption from the layer that can actually judge it. Whether those
   bytes name a real counterexample is decided by replay, which
   tests/test_replay.cpp fixes. */
TEST(SolverProcessFaultInjection, ACorruptModelIsCarriedVerbatimNotRepaired) {
    FakeSolver fake;
    FaultEnvironmentGuard guard;
    ql_solver_check_request_v1 request = BaseRequest();
    ql_solver_check_result_v1 result{};
    ql_artifact_view view{};
    ql_error error{};

    if (!fake.Start()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    request.artifact_requests = QL_SOLVER_REQUEST_MODEL;
    ASSERT_EQ(QL_STATUS_OK,
              fake.CheckUnder("corrupt-model", request, &result, &error))
        << error.message;
    EXPECT_EQ(QL_SOLVER_CHECK_SAT, result.kind);
    ASSERT_NE(nullptr, result.model_artifact);
    view.struct_size = sizeof(view);
    ASSERT_EQ(QL_STATUS_OK,
              ql_artifact_get_view(result.model_artifact, &view, &error));
    EXPECT_STREQ(QL_ARTIFACT_KIND_SOLVER_MODEL, view.kind);
    const std::string bytes(static_cast<const char *>(view.data), view.size);
    EXPECT_NE(std::string::npos, bytes.find("not a model at all"));
    /* The embedded NUL survives, so nothing treated the payload as a C
       string on the way through. */
    EXPECT_NE(std::string::npos, bytes.find('\0'));
    ql_solver_check_result_clear(&result);
}

/* Output is bounded by the request, not by the solver's willingness to stop.
   A backend that streams forever must fail against the limit rather than
   grow the host's memory until something else does. */
TEST(SolverProcessFaultInjection, RunawayOutputIsBoundedByTheRequestLimit) {
    FakeSolver fake;
    FaultEnvironmentGuard guard;
    ql_solver_check_request_v1 request = BaseRequest();
    ql_solver_check_result_v1 result{};
    ql_error error{};

    if (!fake.Start()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    request.artifact_requests = QL_SOLVER_REQUEST_MODEL;
    request.stdout_limit_bytes = 4096u;
    const auto started = std::chrono::steady_clock::now();
    const ql_status status =
        fake.CheckUnder("runaway", request, &result, &error);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(QL_STATUS_METHOD_ERROR, status) << error.message;
    EXPECT_NE(nullptr, std::strstr(error.message, "solver stdout"))
        << "message: " << error.message;
    EXPECT_EQ(QL_SOLVER_CHECK_INVALID, result.kind);
    EXPECT_EQ(nullptr, result.model_artifact);
    EXPECT_LT(elapsed, std::chrono::seconds(15));
    ql_solver_check_result_clear(&result);
}

/* A solver that never answers becomes UNKNOWN with a timeout reason, never a
   kind. The watchdog is what makes the wall-clock budget in G2 mean
   something at the process boundary. */
TEST(SolverProcessFaultInjection, AHangingSolverTimesOutIntoUnknown) {
    FakeSolver fake;
    FaultEnvironmentGuard guard;
    ql_solver_check_request_v1 request = BaseRequest();
    ql_solver_check_result_v1 result{};
    ql_error error{};

    if (!fake.Start()) {
        GTEST_SKIP() << "Bitwuzla support is disabled";
    }
    request.timeout_ms = 300u;
    const auto started = std::chrono::steady_clock::now();
    const ql_status status =
        fake.CheckUnder("hang", request, &result, &error);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_EQ(QL_STATUS_OK, status) << error.message;
    EXPECT_EQ(QL_SOLVER_CHECK_UNKNOWN, result.kind);
    EXPECT_EQ(QL_SOLVER_UNKNOWN_TIMEOUT, result.unknown_reason);
    EXPECT_EQ(nullptr, result.model_artifact);
    /* The child sleeps for twenty seconds; returning well inside that is the
       property. The grace the watchdog adds is a few seconds. */
    EXPECT_LT(elapsed, std::chrono::seconds(15));
    ql_solver_check_result_clear(&result);
}

}  // namespace
