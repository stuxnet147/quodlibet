/* Measures what a logging call site costs when its level is turned off. The
   numbers this prints are the ones recorded in docs/runtime-services/logging.md.
   Run it alone with:

     ctest --preset windows-clang -R quodlibet.LogOverhead -V
*/
#include "quodlibet/log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

#include <gtest/gtest.h>

namespace {

constexpr int kIterations = 1000000;
constexpr int kCallsPerIteration = 16;
constexpr int kRepetitions = 9;

/* Forces `value` into a register the compiler must treat as escaped and
   clobbers memory, so neither the accumulator nor the ql_log_threshold load
   can be hoisted or common-subexpression-eliminated across it. Without this
   the optimizer collapses the loop and the measurement means nothing. */
inline void do_not_optimize(std::uint64_t &value) {
#if defined(__clang__) || defined(__GNUC__)
    __asm__ __volatile__("" : "+r"(value) : : "memory");
#else
    volatile std::uint64_t escaped = value;
    value = escaped;
    std::atomic_signal_fence(std::memory_order_acq_rel);
#endif
}

/* One barrier per call site: every guard has to reload the threshold, which is
   the worst case for a disabled call. */
#define QL_BENCH_STEP(accumulator)                                         \
    do {                                                                   \
        do_not_optimize(accumulator);                                      \
        (accumulator) += 1u;                                               \
    } while (0)

#define QL_BENCH_LOGGED_STEP(accumulator)                                  \
    do {                                                                   \
        do_not_optimize(accumulator);                                      \
        QL_LOGT("bench", "iteration %d payload %s",                        \
                static_cast<int>(accumulator), "unused");                  \
        (accumulator) += 1u;                                               \
    } while (0)

#define QL_BENCH_REPEAT_16(step, accumulator)                              \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator);                                                     \
    step(accumulator)

std::uint64_t run_baseline_loop() {
    std::uint64_t accumulator = 0u;

    for (int index = 0; index < kIterations; ++index) {
        QL_BENCH_REPEAT_16(QL_BENCH_STEP, accumulator);
    }
    do_not_optimize(accumulator);
    return accumulator;
}

std::uint64_t run_disabled_call_loop() {
    std::uint64_t accumulator = 0u;

    for (int index = 0; index < kIterations; ++index) {
        QL_BENCH_REPEAT_16(QL_BENCH_LOGGED_STEP, accumulator);
    }
    do_not_optimize(accumulator);
    return accumulator;
}

double nanoseconds_per_call(std::uint64_t (*loop)()) {
    double best = 0.0;

    for (int repetition = 0; repetition < kRepetitions; ++repetition) {
        std::chrono::steady_clock::time_point start =
            std::chrono::steady_clock::now();
        std::uint64_t result = loop();
        std::chrono::steady_clock::time_point stop =
            std::chrono::steady_clock::now();
        do_not_optimize(result);
        double elapsed =
            std::chrono::duration_cast<
                std::chrono::duration<double, std::nano> >(stop - start)
                .count();
        double per_call = elapsed / (double)kIterations / kCallsPerIteration;
        if (repetition == 0 || per_call < best) {
            best = per_call;
        }
    }
    return best;
}

TEST(LogOverhead, DisabledLevelCallSiteIsCheap) {
    ql_log_reset();
    ASSERT_EQ(QL_LOG_LEVEL_OFF, ql_log_get_level());
    ASSERT_FALSE(QL_LOG_IS_ENABLED(QL_LOG_LEVEL_TRACE));

    /* Warm up the code paths and the clock before the timed repetitions. */
    (void)nanoseconds_per_call(run_baseline_loop);
    (void)nanoseconds_per_call(run_disabled_call_loop);

    double baseline = nanoseconds_per_call(run_baseline_loop);
    double with_call = nanoseconds_per_call(run_disabled_call_loop);
    double overhead = with_call - baseline;

    std::printf(
        "[log-overhead] engine=%s iterations=%d calls_per_iteration=%d "
        "repetitions=%d\n"
        "[log-overhead] baseline=%.4f ns/step with_disabled_call=%.4f ns/step\n"
        "[log-overhead] disabled_call_overhead=%.4f ns/call\n",
        ql_log_available() != 0u ? "on" : "off", kIterations,
        kCallsPerIteration, kRepetitions, baseline, with_call, overhead);
    RecordProperty("disabled_call_overhead_picoseconds",
                   static_cast<int>(overhead * 1000.0));

    ql_log_statistics_v1 statistics{};
    ql_log_statistics_init(&statistics);
    ql_log_get_statistics(&statistics);
    /* Nothing may have been formatted, queued or dropped: the guard has to
       reject the call before ql_log_write is entered. */
    EXPECT_EQ(0u, statistics.emitted);
    EXPECT_EQ(0u, statistics.dropped_no_sink);
    EXPECT_EQ(0u, statistics.truncated);

    /* A generous ceiling. It exists to catch a regression that turns the guard
       into real work, not to pin a machine-specific number. */
    EXPECT_LT(overhead, 25.0);
}

}  // namespace
