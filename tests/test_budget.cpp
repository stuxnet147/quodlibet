#include "quodlibet/budget.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "quodlibet/artifact.h"
#include "quodlibet/pipeline.h"
#include "quodlibet/registry.h"
#include "quodlibet/scheduler.h"

namespace {

ql_budget_limits_v1 no_limits() {
    ql_budget_limits_v1 limits{};
    ql_budget_limits_init(&limits);
    return limits;
}

ql_budget_usage_v1 usage_of(const ql_budget *budget) {
    ql_budget_usage_v1 usage{};
    ql_budget_usage_init(&usage);
    ql_error error{};
    EXPECT_EQ(QL_STATUS_OK, ql_budget_get_usage(budget, &usage, &error));
    return usage;
}

class BudgetHandle {
public:
    explicit BudgetHandle(const ql_budget_limits_v1 &limits) {
        ql_error error{};
        EXPECT_EQ(QL_STATUS_OK,
                  ql_budget_create(nullptr, &limits, &budget_, &error))
            << error.message;
    }
    ~BudgetHandle() { ql_budget_destroy(budget_); }
    BudgetHandle(const BudgetHandle &) = delete;
    BudgetHandle &operator=(const BudgetHandle &) = delete;

    ql_budget *get() const { return budget_; }

private:
    ql_budget *budget_ = nullptr;
};

TEST(Budget, RejectsMalformedLimits) {
    ql_error error{};
    ql_budget_limits_v1 limits = no_limits();
    ql_budget *budget = nullptr;

    limits.struct_size = sizeof(limits) - 1u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_budget_create(nullptr, &limits, &budget, &error));
    EXPECT_EQ(nullptr, budget);

    limits = no_limits();
    limits.abi_version = QL_BUDGET_ABI_VERSION + 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH,
              ql_budget_create(nullptr, &limits, &budget, &error));

    limits = no_limits();
    limits.memory_bytes = 1024u;
    limits.single_allocation_bytes = 4096u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_budget_create(nullptr, &limits, &budget, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "single allocation"));
}

TEST(Budget, UnlimitedBudgetNeverInterferes) {
    BudgetHandle budget(no_limits());
    ql_error error{};
    const ql_allocator *allocator = ql_budget_allocator(budget.get());
    std::vector<void *> blocks;

    ql_budget_start(budget.get());
    for (int index = 0; index < 64; ++index) {
        void *block = allocator->allocate(allocator->user_data, 4096u);
        ASSERT_NE(nullptr, block);
        blocks.push_back(block);
    }
    EXPECT_EQ(QL_STATUS_OK, ql_budget_check(budget.get(), &error));
    EXPECT_EQ(QL_BUDGET_STATE_ACTIVE, ql_budget_get_state(budget.get()));
    EXPECT_EQ(UINT64_MAX, ql_budget_remaining_ns(budget.get()));

    ql_budget_usage_v1 usage = usage_of(budget.get());
    EXPECT_EQ(64u, usage.allocation_count);
    EXPECT_EQ(64u, usage.live_allocation_count);
    EXPECT_GE(usage.memory_current_bytes, 64u * 4096u);
    EXPECT_EQ(usage.memory_peak_bytes, usage.memory_current_bytes);
    EXPECT_EQ(0u, usage.denied_allocation_count);

    for (void *block : blocks) {
        allocator->deallocate(allocator->user_data, block);
    }
    usage = usage_of(budget.get());
    EXPECT_EQ(0u, usage.memory_current_bytes);
    EXPECT_EQ(0u, usage.live_allocation_count);
    EXPECT_EQ(64u, usage.deallocation_count);
    EXPECT_GE(usage.memory_peak_bytes, 64u * 4096u);
}

TEST(Budget, MemoryLimitFailsDeterministicallyAtTheSameAllocation) {
    ql_budget_limits_v1 limits = no_limits();
    limits.memory_bytes = 64u * 1024u;

    auto run = [&limits]() {
        BudgetHandle budget(limits);
        const ql_allocator *allocator = ql_budget_allocator(budget.get());
        std::vector<void *> blocks;
        int accepted = 0;

        ql_budget_start(budget.get());
        for (int index = 0; index < 256; ++index) {
            void *block = allocator->allocate(allocator->user_data, 1024u);
            if (block == nullptr) {
                break;
            }
            blocks.push_back(block);
            ++accepted;
        }
        ql_budget_usage_v1 usage = usage_of(budget.get());
        EXPECT_LE(usage.memory_current_bytes, limits.memory_bytes);
        EXPECT_EQ(QL_BUDGET_STATE_MEMORY_EXHAUSTED,
                  ql_budget_get_state(budget.get()));
        EXPECT_EQ(1u, usage.denied_allocation_count);
        for (void *block : blocks) {
            allocator->deallocate(allocator->user_data, block);
        }
        EXPECT_EQ(0u, usage_of(budget.get()).memory_current_bytes);
        return accepted;
    };

    int first = run();
    int second = run();
    int third = run();
    EXPECT_GT(first, 0);
    /* The same allocation sequence must fail at the same allocation every
       time; the refusal is a function of usage and size, not of the host
       allocator. */
    EXPECT_EQ(first, second);
    EXPECT_EQ(second, third);
}

TEST(Budget, MemoryExhaustionIsStickyButLetsUnwindingAllocate) {
    ql_budget_limits_v1 limits = no_limits();
    limits.memory_bytes = 8u * 1024u;
    BudgetHandle budget(limits);
    const ql_allocator *allocator = ql_budget_allocator(budget.get());
    ql_error error{};

    ql_budget_start(budget.get());
    void *big = allocator->allocate(allocator->user_data, 16u * 1024u);
    EXPECT_EQ(nullptr, big);
    EXPECT_EQ(QL_BUDGET_STATE_MEMORY_EXHAUSTED,
              ql_budget_get_state(budget.get()));
    EXPECT_EQ(QL_STATUS_OUT_OF_MEMORY, ql_budget_check(budget.get(), &error));

    /* The state is latched, but an in-limit allocation still succeeds so a
       caller can finish unwinding and report the failure. */
    void *small = allocator->allocate(allocator->user_data, 128u);
    ASSERT_NE(nullptr, small);
    allocator->deallocate(allocator->user_data, small);
    EXPECT_EQ(QL_BUDGET_STATE_MEMORY_EXHAUSTED,
              ql_budget_get_state(budget.get()));

    /* Restarting the budget re-arms it. */
    ql_budget_start(budget.get());
    EXPECT_EQ(QL_BUDGET_STATE_ACTIVE, ql_budget_get_state(budget.get()));
    EXPECT_EQ(QL_STATUS_OK, ql_budget_check(budget.get(), &error));
}

TEST(Budget, SingleAllocationLimitIsItsOwnAxis) {
    ql_budget_limits_v1 limits = no_limits();
    limits.single_allocation_bytes = 1024u;
    BudgetHandle budget(limits);
    const ql_allocator *allocator = ql_budget_allocator(budget.get());

    ql_budget_start(budget.get());
    void *ok = allocator->allocate(allocator->user_data, 1024u);
    ASSERT_NE(nullptr, ok);
    EXPECT_EQ(nullptr, allocator->allocate(allocator->user_data, 1025u));
    EXPECT_EQ(QL_BUDGET_STATE_MEMORY_EXHAUSTED,
              ql_budget_get_state(budget.get()));
    allocator->deallocate(allocator->user_data, ok);
}

TEST(Budget, ReallocationAccountsGrowthAndShrink) {
    ql_budget_limits_v1 limits = no_limits();
    limits.memory_bytes = 32u * 1024u;
    BudgetHandle budget(limits);
    const ql_allocator *allocator = ql_budget_allocator(budget.get());

    ql_budget_start(budget.get());
    void *block = allocator->reallocate(allocator->user_data, nullptr, 1024u);
    ASSERT_NE(nullptr, block);
    std::memset(block, 0xab, 1024u);
    uint64_t after_first = usage_of(budget.get()).memory_current_bytes;
    EXPECT_GE(after_first, 1024u);

    block = allocator->reallocate(allocator->user_data, block, 8192u);
    ASSERT_NE(nullptr, block);
    EXPECT_EQ(0xab, static_cast<unsigned char *>(block)[0]);
    uint64_t after_grow = usage_of(budget.get()).memory_current_bytes;
    EXPECT_EQ(after_first + 7168u, after_grow);

    block = allocator->reallocate(allocator->user_data, block, 512u);
    ASSERT_NE(nullptr, block);
    uint64_t after_shrink = usage_of(budget.get()).memory_current_bytes;
    EXPECT_EQ(after_grow - 7680u, after_shrink);

    /* A refused growth leaves the original block and the accounting alone. */
    void *refused = allocator->reallocate(allocator->user_data, block,
                                          64u * 1024u);
    EXPECT_EQ(nullptr, refused);
    EXPECT_EQ(after_shrink, usage_of(budget.get()).memory_current_bytes);
    EXPECT_EQ(0xab, static_cast<unsigned char *>(block)[0]);

    allocator->deallocate(allocator->user_data, block);
    EXPECT_EQ(0u, usage_of(budget.get()).memory_current_bytes);
}

TEST(Budget, TotalWallClockAxisExpiresOnItsOwn) {
    ql_budget_limits_v1 limits = no_limits();
    limits.total_wall_clock_ns = 20u * 1000u * 1000u;
    BudgetHandle budget(limits);
    ql_error error{};

    ql_budget_start(budget.get());
    EXPECT_EQ(QL_STATUS_OK, ql_budget_check(budget.get(), &error));
    EXPECT_LE(ql_budget_remaining_ns(budget.get()),
              limits.total_wall_clock_ns);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    EXPECT_EQ(QL_STATUS_CANCELLED, ql_budget_check(budget.get(), &error));
    EXPECT_EQ(QL_BUDGET_STATE_TOTAL_TIME_EXHAUSTED,
              ql_budget_get_state(budget.get()));
    EXPECT_EQ(0u, ql_budget_remaining_ns(budget.get()));
    EXPECT_EQ(1u, ql_budget_is_cancelled(budget.get()));
    EXPECT_NE(nullptr, std::strstr(error.message, "total-time-exhausted"));
}

TEST(Budget, NodeAndSolverAxesAreSetSeparately) {
    ql_budget_limits_v1 limits = no_limits();
    limits.node_wall_clock_ns = 500u * 1000u * 1000u;
    limits.solver_wall_clock_ns = 20u * 1000u * 1000u;
    BudgetHandle budget(limits);
    ql_error error{};
    ql_budget_scope *node = nullptr;
    ql_budget_scope *solver = nullptr;

    ql_budget_start(budget.get());
    ASSERT_EQ(QL_STATUS_OK,
              ql_budget_scope_begin(budget.get(), nullptr,
                                    QL_BUDGET_SCOPE_NODE, "lower", &node,
                                    &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_budget_scope_begin(budget.get(), node,
                                    QL_BUDGET_SCOPE_SOLVER, "bitwuzla",
                                    &solver, &error));

    /* The solver scope inherits the tighter of the two deadlines. */
    EXPECT_LE(ql_budget_scope_remaining_ns(solver),
              limits.solver_wall_clock_ns);
    EXPECT_GT(ql_budget_scope_remaining_ns(node), limits.solver_wall_clock_ns);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    EXPECT_EQ(QL_STATUS_CANCELLED, ql_budget_scope_check(solver, &error));
    EXPECT_EQ(QL_BUDGET_STATE_SOLVER_TIME_EXHAUSTED,
              ql_budget_get_state(budget.get()));
    EXPECT_EQ(0u, ql_budget_scope_remaining_ns(solver));
    ql_budget_scope_end(solver);
    ql_budget_scope_end(node);
}

TEST(Budget, NodeAxisExpiresWithoutATotalLimit) {
    ql_budget_limits_v1 limits = no_limits();
    limits.node_wall_clock_ns = 20u * 1000u * 1000u;
    BudgetHandle budget(limits);
    ql_error error{};
    ql_budget_scope *node = nullptr;

    ql_budget_start(budget.get());
    ASSERT_EQ(QL_STATUS_OK,
              ql_budget_scope_begin(budget.get(), nullptr,
                                    QL_BUDGET_SCOPE_NODE, "lower", &node,
                                    &error));
    EXPECT_EQ(UINT64_MAX, ql_budget_remaining_ns(budget.get()));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    EXPECT_EQ(QL_STATUS_CANCELLED, ql_budget_scope_check(node, &error));
    EXPECT_EQ(QL_BUDGET_STATE_NODE_TIME_EXHAUSTED,
              ql_budget_get_state(budget.get()));
    ql_budget_scope_end(node);
}

TEST(Budget, RemainingMillisecondsNeverRoundALiveBudgetToZero) {
    ql_budget_limits_v1 limits = no_limits();
    limits.solver_wall_clock_ns = 400u * 1000u; /* 0.4 ms */
    BudgetHandle budget(limits);
    ql_error error{};
    ql_budget_scope *solver = nullptr;

    ql_budget_start(budget.get());
    ASSERT_EQ(QL_STATUS_OK,
              ql_budget_scope_begin(budget.get(), nullptr,
                                    QL_BUDGET_SCOPE_SOLVER, "bitwuzla",
                                    &solver, &error));
    /* A backend reads zero as "no limit", so a live sub-millisecond budget
       must round up rather than truncate. */
    EXPECT_EQ(1u, ql_budget_scope_remaining_ms(solver));
    ql_budget_scope_end(solver);
}

TEST(Budget, CancellationIsAStateAndTheFirstReasonWins) {
    ql_budget_limits_v1 limits = no_limits();
    limits.memory_bytes = 1024u;
    BudgetHandle budget(limits);
    const ql_allocator *allocator = ql_budget_allocator(budget.get());
    ql_error error{};

    ql_budget_start(budget.get());
    ql_budget_cancel(budget.get());
    EXPECT_EQ(QL_BUDGET_STATE_CANCELLED, ql_budget_get_state(budget.get()));
    EXPECT_EQ(QL_STATUS_CANCELLED, ql_budget_check(budget.get(), &error));

    /* A later memory refusal must not overwrite the recorded reason. */
    EXPECT_EQ(nullptr, allocator->allocate(allocator->user_data, 4096u));
    EXPECT_EQ(QL_BUDGET_STATE_CANCELLED, ql_budget_get_state(budget.get()));
}

TEST(Budget, ScopesCannotOpenUnderAnExhaustedBudget) {
    ql_budget_limits_v1 limits = no_limits();
    BudgetHandle budget(limits);
    ql_error error{};
    ql_budget_scope *scope = nullptr;

    ql_budget_start(budget.get());
    ql_budget_cancel(budget.get());
    EXPECT_EQ(QL_STATUS_CANCELLED,
              ql_budget_scope_begin(budget.get(), nullptr,
                                    QL_BUDGET_SCOPE_NODE, "node", &scope,
                                    &error));
    EXPECT_EQ(nullptr, scope);
}

TEST(Budget, ExhaustionCannotBecomeALogicalVerdict) {
    ql_budget_limits_v1 limits = no_limits();
    limits.total_wall_clock_ns = 1u;
    ql_error error{};

    const ql_verdict promoted[] = {
        QL_VERDICT_PROVED_EQUIVALENT,
        QL_VERDICT_PROVED_LEFT_REFINES_RIGHT,
        QL_VERDICT_PROVED_RIGHT_REFINES_LEFT,
        QL_VERDICT_COUNTEREXAMPLE,
        QL_VERDICT_BOUNDED_CLEAN,
    };

    for (ql_verdict verdict : promoted) {
        BudgetHandle budget(limits);
        ql_outcome_v1 outcome{};
        outcome.struct_size = sizeof(outcome);
        outcome.schema_version = 1u;
        outcome.verdict = verdict;
        outcome.checked_bound = 4096u;

        ql_budget_start(budget.get());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

        ql_status status =
            ql_budget_guard_outcome(budget.get(), &outcome, &error);
        EXPECT_EQ(QL_STATUS_CANCELLED, status)
            << ql_verdict_string(verdict);
        /* The rewrite happens even for a caller that ignores the status. */
        EXPECT_EQ(QL_VERDICT_UNKNOWN, outcome.verdict)
            << ql_verdict_string(verdict);
        EXPECT_EQ(0u, outcome.checked_bound);
        EXPECT_EQ(QL_OUTCOME_FLAG_BUDGET_EXHAUSTED,
                  outcome.flags & QL_OUTCOME_FLAG_BUDGET_EXHAUSTED);
    }
}

TEST(Budget, MemoryExhaustionAlsoWithdrawsAVerdict) {
    ql_budget_limits_v1 limits = no_limits();
    limits.memory_bytes = 512u;
    BudgetHandle budget(limits);
    const ql_allocator *allocator = ql_budget_allocator(budget.get());
    ql_outcome_v1 outcome{};
    ql_error error{};

    outcome.struct_size = sizeof(outcome);
    outcome.schema_version = 1u;
    outcome.verdict = QL_VERDICT_PROVED_EQUIVALENT;

    ql_budget_start(budget.get());
    EXPECT_EQ(nullptr, allocator->allocate(allocator->user_data, 4096u));
    EXPECT_EQ(QL_STATUS_OUT_OF_MEMORY,
              ql_budget_guard_outcome(budget.get(), &outcome, &error));
    EXPECT_EQ(QL_VERDICT_UNKNOWN, outcome.verdict);
}

TEST(Budget, AnActiveBudgetLeavesAVerdictAlone) {
    BudgetHandle budget(no_limits());
    ql_outcome_v1 outcome{};
    ql_error error{};

    outcome.struct_size = sizeof(outcome);
    outcome.schema_version = 1u;
    outcome.verdict = QL_VERDICT_PROVED_EQUIVALENT;
    outcome.checked_bound = 64u;

    ql_budget_start(budget.get());
    EXPECT_EQ(QL_STATUS_OK,
              ql_budget_guard_outcome(budget.get(), &outcome, &error));
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, outcome.verdict);
    EXPECT_EQ(64u, outcome.checked_bound);
    EXPECT_EQ(0u, outcome.flags & QL_OUTCOME_FLAG_BUDGET_EXHAUSTED);

    /* A null budget is the no-budget case and must not rewrite either. */
    EXPECT_EQ(QL_STATUS_OK, ql_budget_guard_outcome(nullptr, &outcome, &error));
    EXPECT_EQ(QL_VERDICT_PROVED_EQUIVALENT, outcome.verdict);
}

TEST(Budget, AccountingSurvivesConcurrentAllocators) {
    constexpr int kThreads = 8;
    constexpr int kBlocksPerThread = 500;
    BudgetHandle budget(no_limits());
    const ql_allocator *allocator = ql_budget_allocator(budget.get());

    ql_budget_start(budget.get());
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([allocator]() {
            std::vector<void *> blocks;
            blocks.reserve(kBlocksPerThread);
            for (int index = 0; index < kBlocksPerThread; ++index) {
                void *block = allocator->allocate(allocator->user_data,
                                                  64u + (std::size_t)index);
                if (block != nullptr) {
                    blocks.push_back(block);
                }
            }
            for (void *block : blocks) {
                allocator->deallocate(allocator->user_data, block);
            }
        });
    }
    for (std::thread &worker : workers) {
        worker.join();
    }

    ql_budget_usage_v1 usage = usage_of(budget.get());
    EXPECT_EQ(0u, usage.memory_current_bytes);
    EXPECT_EQ(0u, usage.live_allocation_count);
    EXPECT_EQ(static_cast<std::uint64_t>(kThreads * kBlocksPerThread),
              usage.allocation_count);
    EXPECT_EQ(static_cast<std::uint64_t>(kThreads * kBlocksPerThread),
              usage.deallocation_count);
    EXPECT_GT(usage.memory_peak_bytes, 0u);
    EXPECT_EQ(QL_BUDGET_STATE_ACTIVE, ql_budget_get_state(budget.get()));
}

/* Pipeline integration. The method sleeps so that a node axis can expire while
   the node is running, and it always produces an artifact so a leak of a
   partial result would be observable. */
struct SlowMethodState {
    std::uint32_t milliseconds;
};

ql_status QL_CALL slow_create(const ql_host_v1 *host, const char *options_json,
                              void **instance, ql_error *error) {
    (void)host;
    (void)options_json;
    (void)error;
    static SlowMethodState state{30u};
    *instance = &state;
    return QL_STATUS_OK;
}

/* What the running method saw through the run context, for the tests below
   that assert a method can learn how long it has left rather than only that
   time is already up. */
std::atomic<std::uint64_t> observed_remaining_ns{UINT64_MAX};
std::atomic<bool> observed_remaining_present{false};

ql_status QL_CALL slow_run(void *instance, const ql_run_context_v1 *context,
                           ql_artifact *const *inputs, std::size_t input_count,
                           ql_artifact **output, ql_error *error) {
    SlowMethodState *state = static_cast<SlowMethodState *>(instance);
    (void)inputs;
    (void)input_count;

    if (context->struct_size >= offsetof(ql_run_context_v1, reserved) &&
        context->remaining_ns != nullptr) {
        observed_remaining_present.store(true);
        observed_remaining_ns.store(
            context->remaining_ns(context->cancel_state));
    }

    for (std::uint32_t elapsed = 0u; elapsed < state->milliseconds;
         elapsed += 2u) {
        if (context->is_cancelled != nullptr &&
            context->is_cancelled(context->cancel_state) != 0u) {
            ql_error_set(error, QL_STATUS_CANCELLED,
                         "slow method observed cancellation");
            return QL_STATUS_CANCELLED;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return ql_artifact_create(&ql_default_host()->allocator, "test.slow", 1u,
                              "done", 4u, output, error);
}

ql_method_v1 make_slow_method() {
    ql_method_v1 method{};
    method.struct_size = sizeof(method);
    method.abi_version = QL_ABI_VERSION;
    method.name = "test.slow";
    method.description = "sleeps, then emits an artifact";
    method.output_kind = "test.slow";
    method.minimum_inputs = 0u;
    method.maximum_inputs = 8u;
    method.create = slow_create;
    method.run = slow_run;
    return method;
}

class PipelineBudgetTest : public ::testing::Test {
protected:
    void SetUp() override {
        ql_error error{};
        ASSERT_EQ(QL_STATUS_OK,
                  ql_registry_create(nullptr, &registry_, &error));
        method_ = make_slow_method();
        ASSERT_EQ(QL_STATUS_OK,
                  ql_registry_register(registry_, &method_, &error));
        ASSERT_EQ(QL_STATUS_OK,
                  ql_scheduler_create(nullptr, 2u, &scheduler_, &error));
        ASSERT_EQ(QL_STATUS_OK,
                  ql_artifact_create(nullptr, "test.input", 1u, "in", 2u,
                                     &input_, &error));
        ASSERT_EQ(QL_STATUS_OK,
                  ql_pipeline_create(registry_, nullptr, &pipeline_, &error));
        ql_node_id first = QL_INVALID_NODE_ID;
        ql_node_id second = QL_INVALID_NODE_ID;
        ASSERT_EQ(QL_STATUS_OK,
                  ql_pipeline_add_node(pipeline_, "first", "test.slow", "{}",
                                       nullptr, 0u, &first, &error));
        ASSERT_EQ(QL_STATUS_OK,
                  ql_pipeline_add_node(pipeline_, "second", "test.slow", "{}",
                                       &first, 1u, &second, &error));
        ASSERT_EQ(QL_STATUS_OK, ql_pipeline_compile(pipeline_, &error));
    }

    void TearDown() override {
        ql_pipeline_destroy(pipeline_);
        ql_artifact_release(input_);
        ql_scheduler_destroy(scheduler_);
        ql_registry_destroy(registry_);
    }

    ql_registry *registry_ = nullptr;
    ql_scheduler *scheduler_ = nullptr;
    ql_pipeline *pipeline_ = nullptr;
    ql_artifact *input_ = nullptr;
    ql_method_v1 method_{};
};

TEST_F(PipelineBudgetTest, RunsToCompletionUnderAGenerousBudget) {
    ql_budget_limits_v1 limits = no_limits();
    limits.total_wall_clock_ns = 30ull * 1000ull * 1000ull * 1000ull;
    BudgetHandle budget(limits);
    ql_pipeline_result *result = nullptr;
    ql_error error{};

    ql_budget_start(budget.get());
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_run_with_budget(pipeline_, scheduler_, input_,
                                          nullptr, budget.get(), &result,
                                          &error))
        << error.message;
    ASSERT_NE(nullptr, result);
    EXPECT_EQ(1u, ql_pipeline_result_count(result));
    ql_pipeline_result_destroy(result);
    EXPECT_EQ(QL_BUDGET_STATE_ACTIVE, ql_budget_get_state(budget.get()));
    EXPECT_GE(usage_of(budget.get()).scope_count, 2u);
}

TEST_F(PipelineBudgetTest, TotalTimeExhaustionStopsTheRunAndKeepsNoResult) {
    ql_budget_limits_v1 limits = no_limits();
    limits.total_wall_clock_ns = 10u * 1000u * 1000u;
    BudgetHandle budget(limits);
    ql_pipeline_result *result = nullptr;
    ql_error error{};

    ql_budget_start(budget.get());
    EXPECT_EQ(QL_STATUS_CANCELLED,
              ql_pipeline_run_with_budget(pipeline_, scheduler_, input_,
                                          nullptr, budget.get(), &result,
                                          &error));
    /* No partial result may escape, and every artifact the run produced has
       been released by the time it returns. */
    EXPECT_EQ(nullptr, result);
    EXPECT_NE(QL_BUDGET_STATE_ACTIVE, ql_budget_get_state(budget.get()));
}

TEST_F(PipelineBudgetTest, NodeTimeExhaustionStopsTheRun) {
    ql_budget_limits_v1 limits = no_limits();
    limits.node_wall_clock_ns = 6u * 1000u * 1000u;
    BudgetHandle budget(limits);
    ql_pipeline_result *result = nullptr;
    ql_error error{};

    ql_budget_start(budget.get());
    EXPECT_EQ(QL_STATUS_CANCELLED,
              ql_pipeline_run_with_budget(pipeline_, scheduler_, input_,
                                          nullptr, budget.get(), &result,
                                          &error));
    EXPECT_EQ(nullptr, result);
    EXPECT_EQ(QL_BUDGET_STATE_NODE_TIME_EXHAUSTED,
              ql_budget_get_state(budget.get()));
}

/* Runs the whole pipeline through the accounting allocator, so the budget's
   own live-allocation counter is an exact leak detector for the aborted run:
   anything the run allocated and failed to release stays counted. */
TEST(PipelineBudgetLeak, AnAbortedRunReleasesEverythingItAllocated) {
    ql_budget_limits_v1 limits = no_limits();
    limits.node_wall_clock_ns = 6u * 1000u * 1000u;
    BudgetHandle budget(limits);
    const ql_allocator *allocator = ql_budget_allocator(budget.get());
    ql_registry *registry = nullptr;
    ql_scheduler *scheduler = nullptr;
    ql_pipeline *pipeline = nullptr;
    ql_artifact *input = nullptr;
    ql_pipeline_result *result = nullptr;
    ql_method_v1 method = make_slow_method();
    ql_node_id first = QL_INVALID_NODE_ID;
    ql_node_id second = QL_INVALID_NODE_ID;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK, ql_registry_create(allocator, &registry, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_registry_register(registry, &method, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_scheduler_create(allocator, 2u, &scheduler, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_artifact_create(allocator, "test.input", 1u,
                                               "in", 2u, &input, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_create(registry, allocator, &pipeline, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_add_node(pipeline, "first", "test.slow", "{}",
                                   nullptr, 0u, &first, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_add_node(pipeline, "second", "test.slow", "{}",
                                   &first, 1u, &second, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_pipeline_compile(pipeline, &error));

    ql_budget_usage_v1 before = usage_of(budget.get());
    ql_budget_start(budget.get());
    EXPECT_EQ(QL_STATUS_CANCELLED,
              ql_pipeline_run_with_budget(pipeline, scheduler, input, nullptr,
                                          budget.get(), &result, &error));
    EXPECT_EQ(nullptr, result);

    ql_budget_usage_v1 after = usage_of(budget.get());
    EXPECT_EQ(before.live_allocation_count, after.live_allocation_count);
    EXPECT_EQ(before.memory_current_bytes, after.memory_current_bytes);

    ql_pipeline_destroy(pipeline);
    ql_artifact_release(input);
    ql_scheduler_destroy(scheduler);
    ql_registry_destroy(registry);

    ql_budget_usage_v1 teardown = usage_of(budget.get());
    EXPECT_EQ(0u, teardown.live_allocation_count);
    EXPECT_EQ(0u, teardown.memory_current_bytes);
}

/* A method can only stop itself at a poll. When it hands work to something it
   cannot poll -- a solver subprocess is the case that motivated this -- it has
   to bound that work up front, and for that it needs the time remaining, not
   just a cancelled flag. */
TEST_F(PipelineBudgetTest, ARunningMethodCanReadHowLongItHasLeft) {
    ql_budget_limits_v1 limits = no_limits();
    limits.total_wall_clock_ns = 30ull * 1000ull * 1000ull * 1000ull;
    BudgetHandle budget(limits);
    ql_pipeline_result *result = nullptr;
    ql_error error{};

    observed_remaining_present.store(false);
    observed_remaining_ns.store(UINT64_MAX);
    ql_budget_start(budget.get());
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_run_with_budget(pipeline_, scheduler_, input_,
                                          nullptr, budget.get(), &result,
                                          &error))
        << error.message;
    ql_pipeline_result_destroy(result);

    EXPECT_TRUE(observed_remaining_present.load());
    const std::uint64_t remaining = observed_remaining_ns.load();
    /* Bounded by the limit that was set, and not yet spent. Asserting a range
       rather than a value: the exact figure depends on when the node started. */
    EXPECT_GT(remaining, 0u);
    EXPECT_LE(remaining, limits.total_wall_clock_ns);
}

TEST_F(PipelineBudgetTest, ATighterBudgetIsReportedAsLessTimeLeft) {
    ql_pipeline_result *result = nullptr;
    ql_error error{};
    std::uint64_t seen[2] = {0u, 0u};
    const std::uint64_t totals[2] = {30ull * 1000ull * 1000ull * 1000ull,
                                     5ull * 1000ull * 1000ull * 1000ull};

    for (int index = 0; index < 2; ++index) {
        ql_budget_limits_v1 limits = no_limits();
        limits.total_wall_clock_ns = totals[index];
        BudgetHandle budget(limits);

        observed_remaining_present.store(false);
        observed_remaining_ns.store(UINT64_MAX);
        ql_budget_start(budget.get());
        ASSERT_EQ(QL_STATUS_OK,
                  ql_pipeline_run_with_budget(pipeline_, scheduler_, input_,
                                              nullptr, budget.get(), &result,
                                              &error))
            << error.message;
        ql_pipeline_result_destroy(result);
        result = nullptr;
        ASSERT_TRUE(observed_remaining_present.load());
        seen[index] = observed_remaining_ns.load();
    }
    /* The reported figure follows the budget rather than being a constant.
       Both runs do the same work, so the gap is the budget. */
    EXPECT_LT(seen[1], seen[0]);
}

/* Without a budget there is no deadline to report, and a method must be able
   to tell that apart from "no time left". */
TEST_F(PipelineBudgetTest, NoBudgetReportsNoDeadlineRatherThanZero) {
    ql_pipeline_result *result = nullptr;
    ql_error error{};

    observed_remaining_present.store(false);
    observed_remaining_ns.store(0u);
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_run(pipeline_, scheduler_, input_, nullptr, &result,
                              &error))
        << error.message;
    ql_pipeline_result_destroy(result);

    EXPECT_TRUE(observed_remaining_present.load());
    EXPECT_EQ(UINT64_MAX, observed_remaining_ns.load());
}

TEST_F(PipelineBudgetTest, ANullBudgetBehavesLikeTheUnbudgetedRun) {
    ql_pipeline_result *plain = nullptr;
    ql_pipeline_result *budgeted = nullptr;
    ql_error error{};

    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_run(pipeline_, scheduler_, input_, nullptr, &plain,
                              &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_pipeline_run_with_budget(pipeline_, scheduler_, input_,
                                          nullptr, nullptr, &budgeted,
                                          &error));
    EXPECT_EQ(ql_pipeline_result_count(plain),
              ql_pipeline_result_count(budgeted));
    ql_pipeline_result_destroy(plain);
    ql_pipeline_result_destroy(budgeted);
}

}  // namespace
