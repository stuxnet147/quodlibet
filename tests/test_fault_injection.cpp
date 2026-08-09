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
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

namespace {

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
    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
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

/* A backend that overwrites the result header is rejected, and the rejection
   is what this fixes in place. The artifacts it allocated are NOT released
   today, which is a defect rather than a decision:

   ql_solver_check_result_clear() only releases when abi_version and
   struct_size still look right, which is correct for a struct of unknown
   layout arriving from outside. Inside ql_solver_check() the layout is not
   unknown: the adapter created local_result with
   ql_solver_check_result_init() and the backend overwrote fields in a struct
   the adapter owns. Restoring the two header fields before clearing would
   release them. src/solver.c belongs to W2, so this is recorded in
   docs/workstreams/W7-progress.md and reported to the coordinator rather than
   changed here.

   The leak is asserted as a bounded quantity, not as a correct outcome: if W2
   fixes it this test fails and gets tightened to expect zero. */
TEST(SolverFaultInjection, RewrittenResultHeaderIsRejectedButStillLeaks) {
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
        /* One unreleased model artifact: its handle, its copied bytes, and
           its kind string. Bounded and known, but not zero. */
        EXPECT_EQ(3u, counter.live.load(std::memory_order_relaxed))
            << "the known leak changed size; re-read the comment above";
    }
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

}  // namespace
