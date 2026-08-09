/* The pinned zf_log engine reaches for Linux thread helpers behind
   _GNU_SOURCE, so the macro has to precede every header in this translation
   unit. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE 1
#endif

#include "quodlibet/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef QL_ENABLE_LOGGING
#  define QL_ENABLE_LOGGING 1
#endif

/* Read by QL_LOG_IS_ENABLED before any logging argument is evaluated. It is a
   plain aligned 32-bit object on purpose: a torn read is impossible on every
   supported target and an acquire load would defeat the point of the fast
   path. Writers hold the configuration lock; readers may observe a threshold
   that is one configuration change stale. */
QL_API int32_t ql_log_threshold = (int32_t)QL_LOG_LEVEL_OFF;

static const char *const ql_log_level_names[] = {
    "trace", "debug", "info", "warn", "error", "fatal", "off"
};

const char *QL_CALL ql_log_level_string(ql_log_level level) {
    if ((int)level < 0 || (int)level > (int)QL_LOG_LEVEL_OFF) {
        return "invalid";
    }
    return ql_log_level_names[(size_t)level];
}

ql_status QL_CALL ql_log_level_parse(const char *text, ql_log_level *level,
                                     ql_error *error) {
    size_t index;

    if (text == NULL || level == NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log level text and output are required");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0u;
         index < sizeof(ql_log_level_names) / sizeof(ql_log_level_names[0]);
         ++index) {
        if (strcmp(text, ql_log_level_names[index]) == 0) {
            *level = (ql_log_level)index;
            ql_error_clear(error);
            return QL_STATUS_OK;
        }
    }
    ql_error_set(error, QL_STATUS_PARSE_ERROR, "unknown log level '%s'", text);
    return QL_STATUS_PARSE_ERROR;
}

void QL_CALL ql_log_config_init(ql_log_config_v1 *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = QL_LOG_ABI_VERSION;
    config->level = QL_LOG_LEVEL_OFF;
    config->details = QL_LOG_DETAIL_DEFAULT;
    config->timestamp_precision = QL_LOG_TIMESTAMP_MICROSECONDS;
    config->clock = QL_LOG_CLOCK_UTC;
}

void QL_CALL ql_log_sink_init(ql_log_sink_v1 *sink) {
    if (sink == NULL) {
        return;
    }
    memset(sink, 0, sizeof(*sink));
    sink->struct_size = sizeof(*sink);
    sink->abi_version = QL_LOG_ABI_VERSION;
}

void QL_CALL ql_log_statistics_init(ql_log_statistics_v1 *statistics) {
    if (statistics == NULL) {
        return;
    }
    memset(statistics, 0, sizeof(*statistics));
    statistics->struct_size = sizeof(*statistics);
    statistics->abi_version = QL_LOG_ABI_VERSION;
}

static ql_status validate_config(const ql_log_config_v1 *config,
                                 ql_error *error) {
    if (config == NULL || config->struct_size < sizeof(*config)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log config v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (config->abi_version != QL_LOG_ABI_VERSION) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "unsupported log ABI version %u", config->abi_version);
        return QL_STATUS_ABI_MISMATCH;
    }
    if ((int)config->level < (int)QL_LOG_LEVEL_TRACE ||
        (int)config->level > (int)QL_LOG_LEVEL_OFF) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log level %d is out of range", (int)config->level);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((config->details & ~(uint32_t)QL_LOG_DETAIL_ALL) != 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log details 0x%08x contain an unknown bit",
                     config->details);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((int)config->timestamp_precision < (int)QL_LOG_TIMESTAMP_SECONDS ||
        (int)config->timestamp_precision >
            (int)QL_LOG_TIMESTAMP_NANOSECONDS) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log timestamp precision %d is out of range",
                     (int)config->timestamp_precision);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if ((int)config->clock != (int)QL_LOG_CLOCK_UTC &&
        (int)config->clock != (int)QL_LOG_CLOCK_LOCAL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log clock %d is out of range", (int)config->clock);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

static ql_status validate_sink(const ql_log_sink_v1 *sink, ql_error *error) {
    if (sink == NULL) {
        return QL_STATUS_OK;
    }
    if (sink->struct_size < sizeof(*sink)) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log sink v1 has an invalid size");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (sink->abi_version != QL_LOG_ABI_VERSION) {
        ql_error_set(error, QL_STATUS_ABI_MISMATCH,
                     "unsupported log ABI version %u", sink->abi_version);
        return QL_STATUS_ABI_MISMATCH;
    }
    if (sink->write == NULL && sink->close != NULL) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a log sink with a close callback needs a write callback");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

static ql_status validate_category(const char *category, ql_error *error) {
    size_t length;

    if (category == NULL) {
        return QL_STATUS_OK;
    }
    length = strlen(category);
    if (length == 0u) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "a log category override needs a non-empty name");
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (length > QL_LOG_MAX_CATEGORY_LENGTH) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log category '%s' exceeds %u characters", category,
                     QL_LOG_MAX_CATEGORY_LENGTH);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    return QL_STATUS_OK;
}

#if QL_ENABLE_LOGGING

#include <stdatomic.h>

#include "uv.h"

/* The engine is compiled into this translation unit rather than linked as a
   separate target. See DEPENDENCIES.md: it keeps zf_log under the core warning,
   visibility and sanitizer flags, and it lets the message context, category and
   source-location layouts be rendered by the callbacks below so that every
   verbosity axis stays runtime-configurable. */
struct zf_log_message;
static void ql_log_put_context(struct zf_log_message *message);
static void ql_log_put_category(struct zf_log_message *message,
                                const char *category);
static void ql_log_put_source(struct zf_log_message *message,
                              const char *function, const char *file,
                              unsigned line_number);

#define ZF_LOG_LIBRARY_PREFIX ql_
#define ZF_LOG_BUF_SZ 2048
#define ZF_LOG_MESSAGE_CTX_FORMAT (F_INIT((ql_log_put_context(msg))))
#define ZF_LOG_MESSAGE_TAG_FORMAT (F_INIT((ql_log_put_category(msg, tag))))
#define ZF_LOG_MESSAGE_SRC_FORMAT                                          \
    (F_INIT((ql_log_put_source(msg, src->func, src->file, src->line))))

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wconversion"
#  pragma clang diagnostic ignored "-Wshadow"
#  pragma clang diagnostic ignored "-Wsign-conversion"
#  pragma clang diagnostic ignored "-Wunused-function"
#  pragma clang diagnostic ignored "-Wunused-macros"
#  pragma clang diagnostic ignored "-Wreserved-identifier"
#  pragma clang diagnostic ignored "-Wreserved-macro-identifier"
#  pragma clang diagnostic ignored "-Wdeclaration-after-statement"
#  pragma clang diagnostic ignored "-Wextra-semi-stmt"
#  pragma clang diagnostic ignored "-Wcast-qual"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wconversion"
#  pragma GCC diagnostic ignored "-Wshadow"
#  pragma GCC diagnostic ignored "-Wsign-conversion"
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../third_party/zf_log/zf_log/zf_log.c"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

typedef struct ql_log_category_rule {
    char name[QL_LOG_MAX_CATEGORY_LENGTH + 1u];
    ql_log_level level;
} ql_log_category_rule;

typedef struct ql_log_state {
    uv_rwlock_t lock;
    uv_mutex_t sink_lock;
    ql_log_config_v1 config;
    ql_log_sink_v1 sink;
    ql_log_category_rule rules[QL_LOG_MAX_CATEGORY_RULES];
    size_t rule_count;
    _Atomic uint32_t has_sink;
    _Atomic uint64_t emitted;
    _Atomic uint64_t dropped_no_sink;
    _Atomic uint64_t dropped_reentrant;
    _Atomic uint64_t truncated;
    uint32_t ready;
} ql_log_state;

/* Per-record facts captured before the engine composes the line. The context,
   category and source callbacks read this snapshot, so a configuration change
   in another thread can never split one line across two configurations. */
typedef struct ql_log_pending {
    uint32_t details;
    ql_log_timestamp_precision timestamp_precision;
    ql_log_clock clock;
    ql_log_level level;
    uint32_t line_number;
    uint32_t truncated;
    uint64_t thread_id;
    uint64_t process_id;
    int64_t timestamp_seconds;
    uint32_t timestamp_nanoseconds;
    const char *category;
    const char *file;
    const char *function;
    size_t message_size;
    char message[QL_LOG_MESSAGE_CAPACITY];
} ql_log_pending;

static ql_log_state g_log;
static uv_once_t g_log_once = UV_ONCE_INIT;
static _Thread_local ql_log_pending g_pending;
static _Thread_local uint32_t g_in_sink;

static void log_output_callback(const zf_log_message *message, void *argument);

static void log_initialize(void) {
    memset(&g_log, 0, sizeof(g_log));
    ql_log_config_init(&g_log.config);
    ql_log_sink_init(&g_log.sink);
    if (uv_rwlock_init(&g_log.lock) != 0) {
        return;
    }
    if (uv_mutex_init(&g_log.sink_lock) != 0) {
        uv_rwlock_destroy(&g_log.lock);
        return;
    }
    /* Never let the engine reach the caller's stderr on its own. */
    zf_log_set_output_v(ZF_LOG_PUT_STD, NULL, log_output_callback);
    zf_log_set_output_level(ZF_LOG_VERBOSE);
    ql_log_threshold = (int32_t)QL_LOG_LEVEL_OFF;
    g_log.ready = 1u;
}

static uint32_t log_ready(void) {
    uv_once(&g_log_once, log_initialize);
    return g_log.ready;
}

/* Must run under the write lock. The published threshold is the most verbose
   level any rule can admit, so the macro fast path never hides a record that a
   category override would have allowed. */
static void republish_threshold(void) {
    int32_t threshold = (int32_t)g_log.config.level;
    size_t index;

    for (index = 0u; index < g_log.rule_count; ++index) {
        if ((int32_t)g_log.rules[index].level < threshold) {
            threshold = (int32_t)g_log.rules[index].level;
        }
    }
    ql_log_threshold = threshold;
}

uint32_t QL_CALL ql_log_available(void) {
    return 1u;
}

ql_status QL_CALL ql_log_configure(const ql_log_config_v1 *config,
                                   ql_error *error) {
    ql_status status = validate_config(config, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    if (!log_ready()) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "the logging runtime failed to initialize");
        return QL_STATUS_INTERNAL_ERROR;
    }
    uv_rwlock_wrlock(&g_log.lock);
    g_log.config.level = config->level;
    g_log.config.details = config->details;
    g_log.config.timestamp_precision = config->timestamp_precision;
    g_log.config.clock = config->clock;
    republish_threshold();
    uv_rwlock_wrunlock(&g_log.lock);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_log_get_config(ql_log_config_v1 *config) {
    if (config == NULL || config->struct_size < sizeof(*config)) {
        return;
    }
    if (!log_ready()) {
        return;
    }
    uv_rwlock_rdlock(&g_log.lock);
    config->abi_version = QL_LOG_ABI_VERSION;
    config->level = g_log.config.level;
    config->details = g_log.config.details;
    config->timestamp_precision = g_log.config.timestamp_precision;
    config->clock = g_log.config.clock;
    uv_rwlock_rdunlock(&g_log.lock);
}

ql_status QL_CALL ql_log_set_level(ql_log_level level, ql_error *error) {
    if ((int)level < (int)QL_LOG_LEVEL_TRACE ||
        (int)level > (int)QL_LOG_LEVEL_OFF) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log level %d is out of range", (int)level);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    if (!log_ready()) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "the logging runtime failed to initialize");
        return QL_STATUS_INTERNAL_ERROR;
    }
    uv_rwlock_wrlock(&g_log.lock);
    g_log.config.level = level;
    republish_threshold();
    uv_rwlock_wrunlock(&g_log.lock);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_log_level QL_CALL ql_log_get_level(void) {
    ql_log_level level;

    if (!log_ready()) {
        return QL_LOG_LEVEL_OFF;
    }
    uv_rwlock_rdlock(&g_log.lock);
    level = g_log.config.level;
    uv_rwlock_rdunlock(&g_log.lock);
    return level;
}

ql_status QL_CALL ql_log_set_category_level(const char *category,
                                            ql_log_level level,
                                            ql_error *error) {
    ql_status status;
    size_t index;

    if ((int)level < (int)QL_LOG_LEVEL_TRACE ||
        (int)level > (int)QL_LOG_LEVEL_OFF) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log level %d is out of range", (int)level);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_category(category, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    if (!log_ready()) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "the logging runtime failed to initialize");
        return QL_STATUS_INTERNAL_ERROR;
    }

    uv_rwlock_wrlock(&g_log.lock);
    if (category == NULL) {
        g_log.rule_count = 0u;
        republish_threshold();
        uv_rwlock_wrunlock(&g_log.lock);
        ql_error_clear(error);
        return QL_STATUS_OK;
    }
    for (index = 0u; index < g_log.rule_count; ++index) {
        if (strcmp(g_log.rules[index].name, category) == 0) {
            g_log.rules[index].level = level;
            republish_threshold();
            uv_rwlock_wrunlock(&g_log.lock);
            ql_error_clear(error);
            return QL_STATUS_OK;
        }
    }
    if (g_log.rule_count == QL_LOG_MAX_CATEGORY_RULES) {
        uv_rwlock_wrunlock(&g_log.lock);
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "at most %u log category overrides are supported",
                     QL_LOG_MAX_CATEGORY_RULES);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    memcpy(g_log.rules[g_log.rule_count].name, category,
           strlen(category) + 1u);
    g_log.rules[g_log.rule_count].level = level;
    ++g_log.rule_count;
    republish_threshold();
    uv_rwlock_wrunlock(&g_log.lock);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

/* Must run under either lock. */
static ql_log_level effective_level_locked(const char *category) {
    size_t index;

    if (category != NULL && category[0] != '\0') {
        for (index = 0u; index < g_log.rule_count; ++index) {
            if (strcmp(g_log.rules[index].name, category) == 0) {
                return g_log.rules[index].level;
            }
        }
    }
    return g_log.config.level;
}

ql_log_level QL_CALL ql_log_get_category_level(const char *category) {
    ql_log_level level;

    if (!log_ready()) {
        return QL_LOG_LEVEL_OFF;
    }
    uv_rwlock_rdlock(&g_log.lock);
    level = effective_level_locked(category);
    uv_rwlock_rdunlock(&g_log.lock);
    return level;
}

uint32_t QL_CALL ql_log_is_enabled(ql_log_level level, const char *category) {
    uint32_t enabled;

    if ((int)level < (int)QL_LOG_LEVEL_TRACE ||
        (int)level >= (int)QL_LOG_LEVEL_OFF) {
        return 0u;
    }
    if (!log_ready()) {
        return 0u;
    }
    uv_rwlock_rdlock(&g_log.lock);
    enabled = (int)level >= (int)effective_level_locked(category) ? 1u : 0u;
    uv_rwlock_rdunlock(&g_log.lock);
    return enabled;
}

ql_status QL_CALL ql_log_set_sink(const ql_log_sink_v1 *sink,
                                  ql_error *error) {
    ql_status status = validate_sink(sink, error);
    ql_log_sink_v1 previous;

    if (status != QL_STATUS_OK) {
        return status;
    }
    if (!log_ready()) {
        ql_error_set(error, QL_STATUS_INTERNAL_ERROR,
                     "the logging runtime failed to initialize");
        return QL_STATUS_INTERNAL_ERROR;
    }

    uv_mutex_lock(&g_log.sink_lock);
    previous = g_log.sink;
    ql_log_sink_init(&g_log.sink);
    if (sink != NULL && sink->write != NULL) {
        g_log.sink.user_data = sink->user_data;
        g_log.sink.write = sink->write;
        g_log.sink.close = sink->close;
        atomic_store_explicit(&g_log.has_sink, 1u, memory_order_release);
    } else {
        atomic_store_explicit(&g_log.has_sink, 0u, memory_order_release);
    }
    if (previous.close != NULL) {
        previous.close(previous.user_data);
    }
    uv_mutex_unlock(&g_log.sink_lock);
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_log_reset(void) {
    ql_log_sink_v1 previous;

    if (!log_ready()) {
        return;
    }
    uv_mutex_lock(&g_log.sink_lock);
    previous = g_log.sink;
    ql_log_sink_init(&g_log.sink);
    atomic_store_explicit(&g_log.has_sink, 0u, memory_order_release);
    if (previous.close != NULL) {
        previous.close(previous.user_data);
    }
    uv_mutex_unlock(&g_log.sink_lock);

    uv_rwlock_wrlock(&g_log.lock);
    ql_log_config_init(&g_log.config);
    g_log.rule_count = 0u;
    republish_threshold();
    uv_rwlock_wrunlock(&g_log.lock);

    atomic_store_explicit(&g_log.emitted, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_log.dropped_no_sink, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_log.dropped_reentrant, 0u, memory_order_relaxed);
    atomic_store_explicit(&g_log.truncated, 0u, memory_order_relaxed);
}

void QL_CALL ql_log_get_statistics(ql_log_statistics_v1 *statistics) {
    if (statistics == NULL || statistics->struct_size < sizeof(*statistics)) {
        return;
    }
    if (!log_ready()) {
        return;
    }
    statistics->abi_version = QL_LOG_ABI_VERSION;
    statistics->emitted =
        atomic_load_explicit(&g_log.emitted, memory_order_relaxed);
    statistics->dropped_no_sink =
        atomic_load_explicit(&g_log.dropped_no_sink, memory_order_relaxed);
    statistics->dropped_reentrant =
        atomic_load_explicit(&g_log.dropped_reentrant, memory_order_relaxed);
    statistics->truncated =
        atomic_load_explicit(&g_log.truncated, memory_order_relaxed);
}

static uint64_t current_thread_id(void) {
#if defined(_WIN32)
    return (uint64_t)GetCurrentThreadId();
#elif defined(__linux__)
    return (uint64_t)syscall(SYS_gettid);
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

static void current_time(int64_t *seconds, uint32_t *nanoseconds) {
    struct timespec now;

    if (timespec_get(&now, TIME_UTC) == TIME_UTC) {
        *seconds = (int64_t)now.tv_sec;
        *nanoseconds = (uint32_t)now.tv_nsec;
        return;
    }
    *seconds = 0;
    *nanoseconds = 0u;
}

static uint32_t break_down_time(int64_t seconds, ql_log_clock clock,
                                struct tm *output) {
    time_t value = (time_t)seconds;

#if defined(_WIN32)
    if (clock == QL_LOG_CLOCK_LOCAL) {
        return localtime_s(output, &value) == 0 ? 1u : 0u;
    }
    return gmtime_s(output, &value) == 0 ? 1u : 0u;
#else
    if (clock == QL_LOG_CLOCK_LOCAL) {
        return localtime_r(&value, output) != NULL ? 1u : 0u;
    }
    return gmtime_r(&value, output) != NULL ? 1u : 0u;
#endif
}

static void put_text(zf_log_message *message, const char *text, size_t size) {
    size_t available = (size_t)(message->e - message->p);

    if (size > available) {
        size = available;
    }
    if (size != 0u) {
        memcpy(message->p, text, size);
        message->p += size;
    }
}

static void put_cstr(zf_log_message *message, const char *text) {
    put_text(message, text, strlen(text));
}

static void put_uint64(zf_log_message *message, uint64_t value) {
    char digits[21];
    int written = snprintf(digits, sizeof(digits), "%llu",
                           (unsigned long long)value);

    if (written > 0) {
        put_text(message, digits, (size_t)written);
    }
}

static void ql_log_put_context(struct zf_log_message *message) {
    const ql_log_pending *pending = &g_pending;
    zf_log_message *target = message;
    char stamp[48];
    struct tm parts;
    int written;

    if ((pending->details & (uint32_t)QL_LOG_DETAIL_TIMESTAMP) != 0u) {
        if (break_down_time(pending->timestamp_seconds, pending->clock,
                            &parts)) {
            written = snprintf(stamp, sizeof(stamp),
                               "%04d-%02d-%02dT%02d:%02d:%02d",
                               parts.tm_year + 1900, parts.tm_mon + 1,
                               parts.tm_mday, parts.tm_hour, parts.tm_min,
                               parts.tm_sec);
            if (written > 0) {
                put_text(target, stamp, (size_t)written);
            }
            switch (pending->timestamp_precision) {
            case QL_LOG_TIMESTAMP_MILLISECONDS:
                written = snprintf(stamp, sizeof(stamp), ".%03u",
                                   pending->timestamp_nanoseconds / 1000000u);
                break;
            case QL_LOG_TIMESTAMP_MICROSECONDS:
                written = snprintf(stamp, sizeof(stamp), ".%06u",
                                   pending->timestamp_nanoseconds / 1000u);
                break;
            case QL_LOG_TIMESTAMP_NANOSECONDS:
                written = snprintf(stamp, sizeof(stamp), ".%09u",
                                   pending->timestamp_nanoseconds);
                break;
            case QL_LOG_TIMESTAMP_SECONDS:
            default:
                written = 0;
                break;
            }
            if (written > 0) {
                put_text(target, stamp, (size_t)written);
            }
            put_cstr(target,
                     pending->clock == QL_LOG_CLOCK_UTC ? "Z " : " ");
        }
    }
    if ((pending->details & (uint32_t)QL_LOG_DETAIL_PROCESS_ID) != 0u) {
        put_uint64(target, pending->process_id);
        put_cstr(target, " ");
    }
    if ((pending->details & (uint32_t)QL_LOG_DETAIL_THREAD_ID) != 0u) {
        put_uint64(target, pending->thread_id);
        put_cstr(target, " ");
    }
    if ((pending->details & (uint32_t)QL_LOG_DETAIL_LEVEL) != 0u) {
        put_cstr(target, ql_log_level_string(pending->level));
        put_cstr(target, " ");
    }
}

static void ql_log_put_category(struct zf_log_message *message,
                                const char *category) {
    const ql_log_pending *pending = &g_pending;
    zf_log_message *target = message;

    target->tag_b = target->p;
    if ((pending->details & (uint32_t)QL_LOG_DETAIL_CATEGORY) != 0u &&
        category != NULL && category[0] != '\0') {
        put_cstr(target, category);
        target->tag_e = target->p;
        put_cstr(target, " ");
        return;
    }
    target->tag_e = target->p;
}

static const char *base_name(const char *path) {
    const char *cursor = path;
    const char *result = path;

    if (path == NULL) {
        return NULL;
    }
    for (; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            result = cursor + 1;
        }
    }
    return result;
}

static void ql_log_put_source(struct zf_log_message *message,
                              const char *function, const char *file,
                              unsigned line_number) {
    const ql_log_pending *pending = &g_pending;
    zf_log_message *target = message;
    uint32_t want_function =
        (pending->details & (uint32_t)QL_LOG_DETAIL_SOURCE_FUNCTION) != 0u &&
        function != NULL;
    uint32_t want_file =
        (pending->details & (uint32_t)QL_LOG_DETAIL_SOURCE_FILE) != 0u &&
        file != NULL;
    uint32_t want_line =
        (pending->details & (uint32_t)QL_LOG_DETAIL_SOURCE_LINE) != 0u;

    if (!want_function && !want_file && !want_line) {
        return;
    }
    if (want_function) {
        put_cstr(target, function);
        if (want_file || want_line) {
            put_cstr(target, "@");
        }
    }
    if (want_file) {
        put_cstr(target, base_name(file));
    }
    if (want_line) {
        if (want_file) {
            put_cstr(target, ":");
        }
        put_uint64(target, line_number);
    }
    put_cstr(target, " ");
}

static void log_output_callback(const zf_log_message *message,
                                void *argument) {
    const ql_log_pending *pending = &g_pending;
    ql_log_record_v1 record;

    (void)argument;
    *message->p = '\0';

    memset(&record, 0, sizeof(record));
    record.struct_size = sizeof(record);
    record.abi_version = QL_LOG_ABI_VERSION;
    record.level = pending->level;
    record.category =
        (pending->details & (uint32_t)QL_LOG_DETAIL_CATEGORY) != 0u
            ? pending->category
            : "";
    record.file = (pending->details & (uint32_t)QL_LOG_DETAIL_SOURCE_FILE) != 0u
                      ? pending->file
                      : NULL;
    record.function =
        (pending->details & (uint32_t)QL_LOG_DETAIL_SOURCE_FUNCTION) != 0u
            ? pending->function
            : NULL;
    record.line_number =
        (pending->details & (uint32_t)QL_LOG_DETAIL_SOURCE_LINE) != 0u
            ? pending->line_number
            : 0u;
    record.message_truncated = pending->truncated;
    record.thread_id =
        (pending->details & (uint32_t)QL_LOG_DETAIL_THREAD_ID) != 0u
            ? pending->thread_id
            : 0u;
    record.process_id =
        (pending->details & (uint32_t)QL_LOG_DETAIL_PROCESS_ID) != 0u
            ? pending->process_id
            : 0u;
    if ((pending->details & (uint32_t)QL_LOG_DETAIL_TIMESTAMP) != 0u) {
        record.timestamp_seconds = pending->timestamp_seconds;
        record.timestamp_nanoseconds = pending->timestamp_nanoseconds;
    }
    record.message = pending->message;
    record.message_size = pending->message_size;
    record.line = message->buf;
    record.line_size = (size_t)(message->p - message->buf);

    uv_mutex_lock(&g_log.sink_lock);
    if (g_log.sink.write != NULL) {
        g_in_sink = 1u;
        g_log.sink.write(g_log.sink.user_data, &record);
        g_in_sink = 0u;
        atomic_fetch_add_explicit(&g_log.emitted, 1u, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_log.dropped_no_sink, 1u,
                                  memory_order_relaxed);
    }
    uv_mutex_unlock(&g_log.sink_lock);
}

void QL_CALL ql_log_write(ql_log_level level, const char *category,
                          const char *file, const char *function,
                          uint32_t line_number, const char *format, ...) {
    va_list arguments;
    int written;

    if ((int)level < (int)QL_LOG_LEVEL_TRACE ||
        (int)level >= (int)QL_LOG_LEVEL_OFF || format == NULL) {
        return;
    }
    if (!log_ready()) {
        return;
    }
    if (g_in_sink != 0u) {
        atomic_fetch_add_explicit(&g_log.dropped_reentrant, 1u,
                                  memory_order_relaxed);
        return;
    }
    if (category == NULL) {
        category = "";
    }

    uv_rwlock_rdlock(&g_log.lock);
    if ((int)level < (int)effective_level_locked(category)) {
        uv_rwlock_rdunlock(&g_log.lock);
        return;
    }
    g_pending.details = g_log.config.details;
    g_pending.timestamp_precision = g_log.config.timestamp_precision;
    g_pending.clock = g_log.config.clock;
    uv_rwlock_rdunlock(&g_log.lock);

    if (atomic_load_explicit(&g_log.has_sink, memory_order_acquire) == 0u) {
        atomic_fetch_add_explicit(&g_log.dropped_no_sink, 1u,
                                  memory_order_relaxed);
        return;
    }

    g_pending.level = level;
    g_pending.category = category;
    g_pending.file = file;
    g_pending.function = function;
    g_pending.line_number = line_number;
    g_pending.truncated = 0u;
    g_pending.thread_id = current_thread_id();
    g_pending.process_id = (uint64_t)(uint32_t)uv_os_getpid();
    current_time(&g_pending.timestamp_seconds,
                 &g_pending.timestamp_nanoseconds);

    va_start(arguments, format);
    written = vsnprintf(g_pending.message, sizeof(g_pending.message), format,
                        arguments);
    va_end(arguments);
    if (written < 0) {
        g_pending.message[0] = '\0';
        g_pending.message_size = 0u;
    } else if ((size_t)written >= sizeof(g_pending.message)) {
        g_pending.message_size = sizeof(g_pending.message) - 1u;
        g_pending.truncated = 1u;
        atomic_fetch_add_explicit(&g_log.truncated, 1u, memory_order_relaxed);
    } else {
        g_pending.message_size = (size_t)written;
    }

    /* "%s" keeps a caller message containing a percent sign from being read as
       a second format string. */
    _zf_log_write_d(function, file, line_number, (int)level + 1, category,
                    "%s", g_pending.message);
}

#else /* QL_ENABLE_LOGGING */

uint32_t QL_CALL ql_log_available(void) {
    return 0u;
}

ql_status QL_CALL ql_log_configure(const ql_log_config_v1 *config,
                                   ql_error *error) {
    ql_status status = validate_config(config, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_log_get_config(ql_log_config_v1 *config) {
    if (config == NULL || config->struct_size < sizeof(*config)) {
        return;
    }
    ql_log_config_init(config);
}

ql_status QL_CALL ql_log_set_level(ql_log_level level, ql_error *error) {
    if ((int)level < (int)QL_LOG_LEVEL_TRACE ||
        (int)level > (int)QL_LOG_LEVEL_OFF) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log level %d is out of range", (int)level);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_log_level QL_CALL ql_log_get_level(void) {
    return QL_LOG_LEVEL_OFF;
}

ql_status QL_CALL ql_log_set_category_level(const char *category,
                                            ql_log_level level,
                                            ql_error *error) {
    ql_status status;

    if ((int)level < (int)QL_LOG_LEVEL_TRACE ||
        (int)level > (int)QL_LOG_LEVEL_OFF) {
        ql_error_set(error, QL_STATUS_INVALID_ARGUMENT,
                     "log level %d is out of range", (int)level);
        return QL_STATUS_INVALID_ARGUMENT;
    }
    status = validate_category(category, error);
    if (status != QL_STATUS_OK) {
        return status;
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

ql_log_level QL_CALL ql_log_get_category_level(const char *category) {
    (void)category;
    return QL_LOG_LEVEL_OFF;
}

uint32_t QL_CALL ql_log_is_enabled(ql_log_level level, const char *category) {
    (void)level;
    (void)category;
    return 0u;
}

ql_status QL_CALL ql_log_set_sink(const ql_log_sink_v1 *sink,
                                  ql_error *error) {
    ql_status status = validate_sink(sink, error);

    if (status != QL_STATUS_OK) {
        return status;
    }
    /* A build without the engine closes the sink immediately: the caller must
       not be left believing its callback owns a live subscription. */
    if (sink != NULL && sink->close != NULL) {
        sink->close(sink->user_data);
    }
    ql_error_clear(error);
    return QL_STATUS_OK;
}

void QL_CALL ql_log_reset(void) {
}

void QL_CALL ql_log_get_statistics(ql_log_statistics_v1 *statistics) {
    if (statistics == NULL || statistics->struct_size < sizeof(*statistics)) {
        return;
    }
    ql_log_statistics_init(statistics);
}

void QL_CALL ql_log_write(ql_log_level level, const char *category,
                          const char *file, const char *function,
                          uint32_t line_number, const char *format, ...) {
    (void)level;
    (void)category;
    (void)file;
    (void)function;
    (void)line_number;
    (void)format;
}

#endif /* QL_ENABLE_LOGGING */
