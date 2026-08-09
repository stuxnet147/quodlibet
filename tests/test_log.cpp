#include "quodlibet/log.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

struct CapturedRecord {
    ql_log_level level;
    std::string category;
    std::string file;
    std::string function;
    std::uint32_t line_number;
    std::uint32_t truncated;
    std::uint64_t thread_id;
    std::uint64_t process_id;
    std::int64_t timestamp_seconds;
    std::uint32_t timestamp_nanoseconds;
    std::string message;
    std::string line;
};

class Capture {
public:
    void QL_CALL append(const ql_log_record_v1 *record) {
        std::lock_guard<std::mutex> guard(mutex_);
        CapturedRecord captured{};
        captured.level = record->level;
        captured.category =
            record->category != nullptr ? record->category : "<null>";
        captured.file = record->file != nullptr ? record->file : "";
        captured.function =
            record->function != nullptr ? record->function : "";
        captured.line_number = record->line_number;
        captured.truncated = record->message_truncated;
        captured.thread_id = record->thread_id;
        captured.process_id = record->process_id;
        captured.timestamp_seconds = record->timestamp_seconds;
        captured.timestamp_nanoseconds = record->timestamp_nanoseconds;
        captured.message = std::string(record->message, record->message_size);
        captured.line = std::string(record->line, record->line_size);
        records_.push_back(captured);
    }

    std::vector<CapturedRecord> snapshot() {
        std::lock_guard<std::mutex> guard(mutex_);
        return records_;
    }

    std::size_t size() {
        std::lock_guard<std::mutex> guard(mutex_);
        return records_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> guard(mutex_);
        records_.clear();
    }

    std::uint32_t closed = 0u;

private:
    std::mutex mutex_;
    std::vector<CapturedRecord> records_;
};

void QL_CALL capture_write(void *user_data, const ql_log_record_v1 *record) {
    static_cast<Capture *>(user_data)->append(record);
}

void QL_CALL capture_close(void *user_data) {
    ++static_cast<Capture *>(user_data)->closed;
}

ql_log_sink_v1 make_sink(Capture *capture, ql_log_close_fn close = nullptr) {
    ql_log_sink_v1 sink{};
    ql_log_sink_init(&sink);
    sink.user_data = capture;
    sink.write = capture_write;
    sink.close = close;
    return sink;
}

class LogTest : public ::testing::Test {
protected:
    void SetUp() override { ql_log_reset(); }
    void TearDown() override { ql_log_reset(); }
};

/* Verbose configuration with every axis on, so a test only has to turn off
   what it is measuring. */
ql_log_config_v1 verbose_config(std::uint32_t details = QL_LOG_DETAIL_ALL) {
    ql_log_config_v1 config{};
    ql_log_config_init(&config);
    config.level = QL_LOG_LEVEL_TRACE;
    config.details = details;
    config.timestamp_precision = QL_LOG_TIMESTAMP_MICROSECONDS;
    config.clock = QL_LOG_CLOCK_UTC;
    return config;
}

std::size_t count_char(const std::string &text, char value) {
    return static_cast<std::size_t>(
        std::count(text.begin(), text.end(), value));
}

TEST_F(LogTest, DefaultsToSilenceWithNoSink) {
    ql_log_config_v1 config{};
    ql_log_config_init(&config);
    ql_log_statistics_v1 statistics{};
    ql_log_statistics_init(&statistics);

    ql_log_get_config(&config);
    EXPECT_EQ(QL_LOG_LEVEL_OFF, config.level);
    EXPECT_EQ(QL_LOG_LEVEL_OFF, ql_log_get_level());
    EXPECT_FALSE(QL_LOG_IS_ENABLED(QL_LOG_LEVEL_FATAL));
    EXPECT_EQ(0u, ql_log_is_enabled(QL_LOG_LEVEL_FATAL, "core"));

    QL_LOGF("core", "this must not reach anyone");

    ql_log_get_statistics(&statistics);
    EXPECT_EQ(0u, statistics.emitted);
}

TEST_F(LogTest, LevelStringsRoundTrip) {
    for (int level = QL_LOG_LEVEL_TRACE; level <= QL_LOG_LEVEL_OFF; ++level) {
        const char *text =
            ql_log_level_string(static_cast<ql_log_level>(level));
        ql_log_level parsed = QL_LOG_LEVEL_OFF;
        ql_error error{};
        ASSERT_EQ(QL_STATUS_OK, ql_log_level_parse(text, &parsed, &error))
            << text;
        EXPECT_EQ(level, static_cast<int>(parsed));
    }

    ql_log_level parsed = QL_LOG_LEVEL_OFF;
    ql_error error{};
    EXPECT_EQ(QL_STATUS_PARSE_ERROR,
              ql_log_level_parse("verbose", &parsed, &error));
    EXPECT_NE(nullptr, std::strstr(error.message, "verbose"));
}

TEST_F(LogTest, RejectsMalformedConfigurationAndSink) {
    ql_error error{};
    ql_log_config_v1 config = verbose_config();

    config.struct_size = sizeof(config) - 1u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT, ql_log_configure(&config, &error));

    config = verbose_config();
    config.abi_version = QL_LOG_ABI_VERSION + 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH, ql_log_configure(&config, &error));

    config = verbose_config();
    config.details = 0xffffffffu;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT, ql_log_configure(&config, &error));

    config = verbose_config();
    config.level = static_cast<ql_log_level>(QL_LOG_LEVEL_OFF + 1);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT, ql_log_configure(&config, &error));

    config = verbose_config();
    config.timestamp_precision =
        static_cast<ql_log_timestamp_precision>(QL_LOG_TIMESTAMP_NANOSECONDS +
                                                1);
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT, ql_log_configure(&config, &error));

    Capture capture;
    ql_log_sink_v1 sink = make_sink(&capture);
    sink.abi_version = QL_LOG_ABI_VERSION + 1u;
    EXPECT_EQ(QL_STATUS_ABI_MISMATCH, ql_log_set_sink(&sink, &error));

    sink = make_sink(&capture);
    sink.struct_size = sizeof(sink) - 1u;
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT, ql_log_set_sink(&sink, &error));

    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_log_set_level(static_cast<ql_log_level>(-1), &error));
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_log_set_category_level("", QL_LOG_LEVEL_INFO, &error));
    std::string long_name(QL_LOG_MAX_CATEGORY_LENGTH + 1u, 'c');
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_log_set_category_level(long_name.c_str(), QL_LOG_LEVEL_INFO,
                                        &error));
}

/* A QL_ENABLE_LOGGING=0 build keeps the whole API callable and silent. Every
   engine test asserts that contract instead of skipping, so both build
   configurations run the same CTest list with no skipped cases. */
void expect_disabled_build_is_silent() {
    Capture capture;
    ql_log_sink_v1 sink = make_sink(&capture, capture_close);
    ql_error error{};
    ql_log_config_v1 config = verbose_config();

    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));
    /* A build without the engine has to hand the sink back at once. */
    EXPECT_EQ(1u, capture.closed);
    ASSERT_EQ(QL_STATUS_OK, ql_log_configure(&config, &error));
    EXPECT_EQ(QL_LOG_LEVEL_OFF, ql_log_get_level());
    EXPECT_EQ(QL_LOG_LEVEL_OFF, ql_log_get_category_level("core"));
    EXPECT_EQ(0u, ql_log_is_enabled(QL_LOG_LEVEL_FATAL, "core"));
    EXPECT_FALSE(QL_LOG_IS_ENABLED(QL_LOG_LEVEL_FATAL));

    QL_LOGF("core", "dropped");
    ql_log_write(QL_LOG_LEVEL_FATAL, "core", __FILE__, "fn", 1u, "%s",
                 "dropped");
    EXPECT_EQ(0u, capture.size());

    ql_log_statistics_v1 statistics{};
    ql_log_statistics_init(&statistics);
    ql_log_get_statistics(&statistics);
    EXPECT_EQ(0u, statistics.emitted);
}

#define QL_REQUIRE_ENGINE()                                                \
    do {                                                                   \
        if (ql_log_available() == 0u) {                                    \
            expect_disabled_build_is_silent();                             \
            return;                                                        \
        }                                                                  \
    } while (0)

TEST_F(LogTest, EngineAvailabilityMatchesBehaviour) {
    QL_REQUIRE_ENGINE();
    EXPECT_EQ(1u, ql_log_available());
}

TEST_F(LogTest, LevelThresholdSelectsRecords) {
    QL_REQUIRE_ENGINE();
    Capture capture;
    ql_log_sink_v1 sink = make_sink(&capture);
    ql_log_config_v1 config = verbose_config();
    ql_error error{};

    config.level = QL_LOG_LEVEL_WARN;
    ASSERT_EQ(QL_STATUS_OK, ql_log_configure(&config, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));

    QL_LOGT("core", "trace");
    QL_LOGD("core", "debug");
    QL_LOGI("core", "info");
    QL_LOGW("core", "warn");
    QL_LOGE("core", "error");
    QL_LOGF("core", "fatal");

    std::vector<CapturedRecord> records = capture.snapshot();
    ASSERT_EQ(3u, records.size());
    EXPECT_EQ(QL_LOG_LEVEL_WARN, records[0].level);
    EXPECT_EQ("warn", records[0].message);
    EXPECT_EQ(QL_LOG_LEVEL_ERROR, records[1].level);
    EXPECT_EQ(QL_LOG_LEVEL_FATAL, records[2].level);

    ql_log_statistics_v1 statistics{};
    ql_log_statistics_init(&statistics);
    ql_log_get_statistics(&statistics);
    EXPECT_EQ(3u, statistics.emitted);
}

TEST_F(LogTest, CategoryOverrideBeatsTheGlobalLevel) {
    QL_REQUIRE_ENGINE();
    Capture capture;
    ql_log_sink_v1 sink = make_sink(&capture);
    ql_log_config_v1 config = verbose_config();
    ql_error error{};

    config.level = QL_LOG_LEVEL_ERROR;
    ASSERT_EQ(QL_STATUS_OK, ql_log_configure(&config, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_log_set_category_level("solver", QL_LOG_LEVEL_TRACE, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_log_set_category_level("noisy", QL_LOG_LEVEL_OFF, &error));

    EXPECT_EQ(QL_LOG_LEVEL_TRACE, ql_log_get_category_level("solver"));
    EXPECT_EQ(QL_LOG_LEVEL_OFF, ql_log_get_category_level("noisy"));
    EXPECT_EQ(QL_LOG_LEVEL_ERROR, ql_log_get_category_level("other"));
    /* The macro fast path must not hide a record an override would admit. */
    EXPECT_TRUE(QL_LOG_IS_ENABLED(QL_LOG_LEVEL_TRACE));

    QL_LOGT("solver", "solver trace");
    QL_LOGT("other", "other trace");
    QL_LOGF("noisy", "silenced fatal");
    QL_LOGE("other", "other error");

    std::vector<CapturedRecord> records = capture.snapshot();
    ASSERT_EQ(2u, records.size());
    EXPECT_EQ("solver trace", records[0].message);
    EXPECT_EQ("other error", records[1].message);

    ASSERT_EQ(QL_STATUS_OK,
              ql_log_set_category_level(nullptr, QL_LOG_LEVEL_TRACE, &error));
    EXPECT_EQ(QL_LOG_LEVEL_ERROR, ql_log_get_category_level("solver"));
}

TEST_F(LogTest, CategoryOverrideTableIsBounded) {
    QL_REQUIRE_ENGINE();
    ql_error error{};

    for (unsigned index = 0u; index < QL_LOG_MAX_CATEGORY_RULES; ++index) {
        std::string name = "category" + std::to_string(index);
        ASSERT_EQ(QL_STATUS_OK, ql_log_set_category_level(
                                    name.c_str(), QL_LOG_LEVEL_INFO, &error))
            << name;
    }
    EXPECT_EQ(QL_STATUS_INVALID_ARGUMENT,
              ql_log_set_category_level("overflow", QL_LOG_LEVEL_INFO,
                                        &error));
    /* Updating an existing rule still works once the table is full. */
    EXPECT_EQ(QL_STATUS_OK,
              ql_log_set_category_level("category0", QL_LOG_LEVEL_WARN,
                                        &error));
}

TEST_F(LogTest, EveryVerbosityAxisIsIndependent) {
    QL_REQUIRE_ENGINE();
    Capture capture;
    ql_log_sink_v1 sink = make_sink(&capture);
    ql_error error{};

    auto emit_with = [&](std::uint32_t details) {
        capture.clear();
        ql_log_config_v1 config = verbose_config(details);
        EXPECT_EQ(QL_STATUS_OK, ql_log_configure(&config, &error));
        EXPECT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));
        ql_log_write(QL_LOG_LEVEL_INFO, "axis", "dir/source.c", "run", 42u,
                     "%s", "payload");
        std::vector<CapturedRecord> records = capture.snapshot();
        EXPECT_EQ(1u, records.size());
        return records.empty() ? CapturedRecord{} : records.front();
    };

    CapturedRecord all = emit_with(QL_LOG_DETAIL_ALL);
    EXPECT_EQ("axis", all.category);
    EXPECT_EQ("dir/source.c", all.file);
    EXPECT_EQ("run", all.function);
    EXPECT_EQ(42u, all.line_number);
    EXPECT_NE(0u, all.thread_id);
    EXPECT_NE(0u, all.process_id);
    EXPECT_NE(0, all.timestamp_seconds);
    EXPECT_NE(std::string::npos, all.line.find("axis"));
    EXPECT_NE(std::string::npos, all.line.find("run@source.c:42"));
    EXPECT_NE(std::string::npos, all.line.find("info"));
    EXPECT_NE(std::string::npos, all.line.find("payload"));

    CapturedRecord message_only = emit_with(0u);
    EXPECT_EQ("", message_only.category);
    EXPECT_EQ("", message_only.file);
    EXPECT_EQ("", message_only.function);
    EXPECT_EQ(0u, message_only.line_number);
    EXPECT_EQ(0u, message_only.thread_id);
    EXPECT_EQ(0u, message_only.process_id);
    EXPECT_EQ(0, message_only.timestamp_seconds);
    EXPECT_EQ("payload", message_only.line);

    CapturedRecord no_thread =
        emit_with(QL_LOG_DETAIL_ALL & ~QL_LOG_DETAIL_THREAD_ID);
    EXPECT_EQ(0u, no_thread.thread_id);
    EXPECT_NE(0u, no_thread.process_id);

    CapturedRecord line_only =
        emit_with(QL_LOG_DETAIL_SOURCE_LINE | QL_LOG_DETAIL_CATEGORY);
    EXPECT_EQ("", line_only.file);
    EXPECT_EQ("", line_only.function);
    EXPECT_EQ(42u, line_only.line_number);
    EXPECT_EQ("axis 42 payload", line_only.line);

    CapturedRecord function_only =
        emit_with(QL_LOG_DETAIL_SOURCE_FUNCTION);
    EXPECT_EQ("run", function_only.function);
    EXPECT_EQ("", function_only.file);
    EXPECT_EQ(0u, function_only.line_number);
    EXPECT_EQ("run payload", function_only.line);
}

TEST_F(LogTest, TimestampPrecisionSelectsFractionDigits) {
    QL_REQUIRE_ENGINE();
    Capture capture;
    ql_log_sink_v1 sink = make_sink(&capture);
    ql_error error{};

    auto fraction_digits = [&](ql_log_timestamp_precision precision) {
        capture.clear();
        ql_log_config_v1 config = verbose_config(QL_LOG_DETAIL_TIMESTAMP);
        config.timestamp_precision = precision;
        EXPECT_EQ(QL_STATUS_OK, ql_log_configure(&config, &error));
        EXPECT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));
        QL_LOGI("stamp", "x");
        std::vector<CapturedRecord> records = capture.snapshot();
        EXPECT_EQ(1u, records.size());
        const std::string &line = records.at(0).line;
        std::size_t dot = line.find('.');
        if (dot == std::string::npos) {
            return static_cast<std::size_t>(0u);
        }
        std::size_t end = line.find('Z', dot);
        EXPECT_NE(std::string::npos, end);
        return end - dot - 1u;
    };

    EXPECT_EQ(0u, fraction_digits(QL_LOG_TIMESTAMP_SECONDS));
    EXPECT_EQ(3u, fraction_digits(QL_LOG_TIMESTAMP_MILLISECONDS));
    EXPECT_EQ(6u, fraction_digits(QL_LOG_TIMESTAMP_MICROSECONDS));
    EXPECT_EQ(9u, fraction_digits(QL_LOG_TIMESTAMP_NANOSECONDS));

    capture.clear();
    ql_log_config_v1 local = verbose_config(QL_LOG_DETAIL_TIMESTAMP);
    local.clock = QL_LOG_CLOCK_LOCAL;
    ASSERT_EQ(QL_STATUS_OK, ql_log_configure(&local, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));
    QL_LOGI("stamp", "x");
    std::vector<CapturedRecord> records = capture.snapshot();
    ASSERT_EQ(1u, records.size());
    EXPECT_EQ(std::string::npos, records.at(0).line.find('Z'));
}

TEST_F(LogTest, SinkReplacementClosesThePreviousSink) {
    QL_REQUIRE_ENGINE();
    Capture first;
    Capture second;
    ql_error error{};
    ql_log_config_v1 config = verbose_config();

    ASSERT_EQ(QL_STATUS_OK, ql_log_configure(&config, &error));
    ql_log_sink_v1 sink = make_sink(&first, capture_close);
    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));
    QL_LOGI("core", "first");

    sink = make_sink(&second, capture_close);
    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));
    EXPECT_EQ(1u, first.closed);
    QL_LOGI("core", "second");

    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(nullptr, &error));
    EXPECT_EQ(1u, second.closed);
    QL_LOGI("core", "dropped");

    EXPECT_EQ(1u, first.size());
    EXPECT_EQ(1u, second.size());

    ql_log_statistics_v1 statistics{};
    ql_log_statistics_init(&statistics);
    ql_log_get_statistics(&statistics);
    EXPECT_EQ(2u, statistics.emitted);
    EXPECT_EQ(1u, statistics.dropped_no_sink);
}

struct ReentrantState {
    Capture capture;
    std::uint32_t attempts = 0u;
};

void QL_CALL reentrant_write(void *user_data,
                             const ql_log_record_v1 *record) {
    ReentrantState *state = static_cast<ReentrantState *>(user_data);
    state->capture.append(record);
    ++state->attempts;
    /* A sink that logs must not deadlock or splice itself into the line. */
    QL_LOGE("sink", "reentrant");
}

TEST_F(LogTest, ReentrantSinkRecordsAreDropped) {
    QL_REQUIRE_ENGINE();
    ReentrantState state;
    ql_log_sink_v1 sink{};
    ql_error error{};
    ql_log_config_v1 config = verbose_config();

    ql_log_sink_init(&sink);
    sink.user_data = &state;
    sink.write = reentrant_write;

    ASSERT_EQ(QL_STATUS_OK, ql_log_configure(&config, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));
    QL_LOGI("core", "outer");

    EXPECT_EQ(1u, state.attempts);
    EXPECT_EQ(1u, state.capture.size());

    ql_log_statistics_v1 statistics{};
    ql_log_statistics_init(&statistics);
    ql_log_get_statistics(&statistics);
    EXPECT_EQ(1u, statistics.emitted);
    EXPECT_EQ(1u, statistics.dropped_reentrant);
}

TEST_F(LogTest, OverlongMessagesAreTruncatedAndCounted) {
    QL_REQUIRE_ENGINE();
    Capture capture;
    ql_log_sink_v1 sink = make_sink(&capture);
    ql_error error{};
    ql_log_config_v1 config = verbose_config(0u);
    std::string payload(QL_LOG_MESSAGE_CAPACITY * 2u, 'x');

    ASSERT_EQ(QL_STATUS_OK, ql_log_configure(&config, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));
    QL_LOGI("core", "%s", payload.c_str());

    std::vector<CapturedRecord> records = capture.snapshot();
    ASSERT_EQ(1u, records.size());
    EXPECT_EQ(1u, records.at(0).truncated);
    EXPECT_EQ(QL_LOG_MESSAGE_CAPACITY - 1u, records.at(0).message.size());

    ql_log_statistics_v1 statistics{};
    ql_log_statistics_init(&statistics);
    ql_log_get_statistics(&statistics);
    EXPECT_EQ(1u, statistics.truncated);
}

TEST_F(LogTest, PercentSignsInMessagesAreNotReinterpreted) {
    QL_REQUIRE_ENGINE();
    Capture capture;
    ql_log_sink_v1 sink = make_sink(&capture);
    ql_error error{};
    ql_log_config_v1 config = verbose_config(0u);

    ASSERT_EQ(QL_STATUS_OK, ql_log_configure(&config, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));
    QL_LOGI("core", "%s", "100%s %n %d");

    std::vector<CapturedRecord> records = capture.snapshot();
    ASSERT_EQ(1u, records.size());
    EXPECT_EQ("100%s %n %d", records.at(0).message);
    EXPECT_EQ("100%s %n %d", records.at(0).line);
}

TEST_F(LogTest, ConcurrentWritersNeverInterleaveALine) {
    QL_REQUIRE_ENGINE();
    constexpr int kThreads = 8;
    constexpr int kMessagesPerThread = 400;
    Capture capture;
    ql_log_sink_v1 sink = make_sink(&capture);
    ql_error error{};
    ql_log_config_v1 config = verbose_config();

    ASSERT_EQ(QL_STATUS_OK, ql_log_configure(&config, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([worker]() {
            for (int index = 0; index < kMessagesPerThread; ++index) {
                QL_LOGI("worker", "<%d:%d:%s>", worker, index,
                        "0123456789abcdef0123456789abcdef");
            }
        });
    }
    for (std::thread &worker : workers) {
        worker.join();
    }

    std::vector<CapturedRecord> records = capture.snapshot();
    ASSERT_EQ(static_cast<std::size_t>(kThreads * kMessagesPerThread),
              records.size());

    std::set<std::string> seen;
    std::set<std::uint64_t> thread_ids;
    for (const CapturedRecord &record : records) {
        /* One well-formed payload per line: exactly one opening and one
           closing marker, and no fragment of another writer's message. */
        EXPECT_EQ(1u, count_char(record.message, '<')) << record.message;
        EXPECT_EQ(1u, count_char(record.message, '>')) << record.message;
        EXPECT_EQ(1u, count_char(record.line, '<')) << record.line;
        EXPECT_EQ(1u, count_char(record.line, '>')) << record.line;
        EXPECT_EQ('<', record.message.front());
        EXPECT_EQ('>', record.message.back());
        EXPECT_NE(std::string::npos,
                  record.message.find("0123456789abcdef0123456789abcdef"));
        EXPECT_NE(std::string::npos, record.line.find(record.message))
            << record.line;
        EXPECT_EQ("worker", record.category);
        EXPECT_TRUE(seen.insert(record.message).second) << record.message;
        thread_ids.insert(record.thread_id);
    }
    EXPECT_EQ(static_cast<std::size_t>(kThreads * kMessagesPerThread),
              seen.size());
    EXPECT_EQ(static_cast<std::size_t>(kThreads), thread_ids.size());

    ql_log_statistics_v1 statistics{};
    ql_log_statistics_init(&statistics);
    ql_log_get_statistics(&statistics);
    EXPECT_EQ(static_cast<std::uint64_t>(kThreads * kMessagesPerThread),
              statistics.emitted);
    EXPECT_EQ(0u, statistics.truncated);
}

TEST_F(LogTest, ResetRestoresTheQuietDefault) {
    QL_REQUIRE_ENGINE();
    Capture capture;
    ql_log_sink_v1 sink = make_sink(&capture, capture_close);
    ql_error error{};
    ql_log_config_v1 config = verbose_config();

    ASSERT_EQ(QL_STATUS_OK, ql_log_configure(&config, &error));
    ASSERT_EQ(QL_STATUS_OK, ql_log_set_sink(&sink, &error));
    ASSERT_EQ(QL_STATUS_OK,
              ql_log_set_category_level("core", QL_LOG_LEVEL_TRACE, &error));
    QL_LOGI("core", "before");
    ASSERT_EQ(1u, capture.size());

    ql_log_reset();
    EXPECT_EQ(1u, capture.closed);
    EXPECT_EQ(QL_LOG_LEVEL_OFF, ql_log_get_level());
    EXPECT_EQ(QL_LOG_LEVEL_OFF, ql_log_get_category_level("core"));
    EXPECT_FALSE(QL_LOG_IS_ENABLED(QL_LOG_LEVEL_FATAL));

    QL_LOGF("core", "after");
    EXPECT_EQ(1u, capture.size());

    ql_log_statistics_v1 statistics{};
    ql_log_statistics_init(&statistics);
    ql_log_get_statistics(&statistics);
    EXPECT_EQ(0u, statistics.emitted);
}

}  // namespace
