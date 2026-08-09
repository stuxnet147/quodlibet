#ifndef QUODLIBET_LOG_H
#define QUODLIBET_LOG_H

#include "quodlibet/status.h"

#define QL_LOG_ABI_VERSION 1u
#define QL_LOG_MAX_CATEGORY_RULES 32u
#define QL_LOG_MAX_CATEGORY_LENGTH 63u
#define QL_LOG_MESSAGE_CAPACITY 1024u
#define QL_LOG_LINE_CAPACITY 2048u

QL_EXTERN_C_BEGIN

/* Ordered by increasing severity. QL_LOG_LEVEL_OFF is a threshold only; it is
   never the level of a record. */
typedef enum ql_log_level {
    QL_LOG_LEVEL_TRACE = 0,
    QL_LOG_LEVEL_DEBUG = 1,
    QL_LOG_LEVEL_INFO = 2,
    QL_LOG_LEVEL_WARN = 3,
    QL_LOG_LEVEL_ERROR = 4,
    QL_LOG_LEVEL_FATAL = 5,
    QL_LOG_LEVEL_OFF = 6
} ql_log_level;

/* Verbosity axes. Each one is independent, so a caller can keep thread ids
   without timestamps, or source lines without function names. */
typedef enum ql_log_detail {
    QL_LOG_DETAIL_TIMESTAMP = UINT32_C(1) << 0,
    QL_LOG_DETAIL_THREAD_ID = UINT32_C(1) << 1,
    QL_LOG_DETAIL_PROCESS_ID = UINT32_C(1) << 2,
    QL_LOG_DETAIL_LEVEL = UINT32_C(1) << 3,
    QL_LOG_DETAIL_CATEGORY = UINT32_C(1) << 4,
    QL_LOG_DETAIL_SOURCE_FILE = UINT32_C(1) << 5,
    QL_LOG_DETAIL_SOURCE_LINE = UINT32_C(1) << 6,
    QL_LOG_DETAIL_SOURCE_FUNCTION = UINT32_C(1) << 7
} ql_log_detail;

#define QL_LOG_DETAIL_SOURCE_LOCATION                                      \
    (QL_LOG_DETAIL_SOURCE_FILE | QL_LOG_DETAIL_SOURCE_LINE |               \
     QL_LOG_DETAIL_SOURCE_FUNCTION)
#define QL_LOG_DETAIL_ALL                                                  \
    (QL_LOG_DETAIL_TIMESTAMP | QL_LOG_DETAIL_THREAD_ID |                   \
     QL_LOG_DETAIL_PROCESS_ID | QL_LOG_DETAIL_LEVEL |                      \
     QL_LOG_DETAIL_CATEGORY | QL_LOG_DETAIL_SOURCE_LOCATION)
#define QL_LOG_DETAIL_DEFAULT                                              \
    (QL_LOG_DETAIL_TIMESTAMP | QL_LOG_DETAIL_LEVEL | QL_LOG_DETAIL_CATEGORY)

typedef enum ql_log_timestamp_precision {
    QL_LOG_TIMESTAMP_SECONDS = 0,
    QL_LOG_TIMESTAMP_MILLISECONDS = 1,
    QL_LOG_TIMESTAMP_MICROSECONDS = 2,
    QL_LOG_TIMESTAMP_NANOSECONDS = 3
} ql_log_timestamp_precision;

typedef enum ql_log_clock {
    QL_LOG_CLOCK_UTC = 0,
    QL_LOG_CLOCK_LOCAL = 1
} ql_log_clock;

/* One emitted record. Every pointer is owned by the logger and is valid only
   for the duration of the sink call. A sink that needs to keep a record must
   copy it. `line` is the composed text selected by the active detail mask and
   carries no trailing newline. */
typedef struct ql_log_record_v1 {
    size_t struct_size;
    uint32_t abi_version;
    ql_log_level level;
    const char *category;
    const char *file;
    const char *function;
    uint32_t line_number;
    uint32_t message_truncated;
    uint64_t thread_id;
    uint64_t process_id;
    int64_t timestamp_seconds;
    uint32_t timestamp_nanoseconds;
    uint32_t reserved32;
    const char *message;
    size_t message_size;
    const char *line;
    size_t line_size;
    uint64_t reserved[4];
} ql_log_record_v1;

typedef void(QL_CALL *ql_log_write_fn)(void *user_data,
                                       const ql_log_record_v1 *record);
typedef void(QL_CALL *ql_log_close_fn)(void *user_data);

/* The sink is copied by ql_log_set_sink. `close` is optional and runs once,
   when the sink is replaced or the logger is reset. A sink must not call back
   into the logging API; reentrant records are dropped rather than deadlocked
   or interleaved. */
typedef struct ql_log_sink_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t reserved32;
    void *user_data;
    ql_log_write_fn write;
    ql_log_close_fn close;
    uint64_t reserved[4];
} ql_log_sink_v1;

typedef struct ql_log_config_v1 {
    size_t struct_size;
    uint32_t abi_version;
    ql_log_level level;
    uint32_t details;
    ql_log_timestamp_precision timestamp_precision;
    ql_log_clock clock;
    uint32_t reserved32;
    uint64_t reserved[4];
} ql_log_config_v1;

/* Number of records the logger refused to emit, by reason. Counters are
   monotonic within a process and are reset by ql_log_reset. */
typedef struct ql_log_statistics_v1 {
    size_t struct_size;
    uint32_t abi_version;
    uint32_t reserved32;
    uint64_t emitted;
    uint64_t dropped_no_sink;
    uint64_t dropped_reentrant;
    uint64_t truncated;
    uint64_t reserved[4];
} ql_log_statistics_v1;

/* Threshold read by the QL_LOG_* macros before any argument is evaluated. It
   is part of the ABI so that a disabled level costs one load and one compare.
   Never assign to it; use ql_log_set_level or ql_log_configure. */
QL_API extern int32_t ql_log_threshold;

#define QL_LOG_IS_ENABLED(level) ((int32_t)(level) >= ql_log_threshold)

QL_API void QL_CALL ql_log_config_init(ql_log_config_v1 *config);
QL_API void QL_CALL ql_log_sink_init(ql_log_sink_v1 *sink);
QL_API void QL_CALL ql_log_statistics_init(ql_log_statistics_v1 *statistics);

/* Non-zero when the logging engine was compiled in. A build with
   QL_ENABLE_LOGGING=0 keeps the whole API callable and standards-compliant but
   never emits a record. */
QL_API uint32_t QL_CALL ql_log_available(void);

QL_API ql_status QL_CALL ql_log_configure(const ql_log_config_v1 *config,
                                          ql_error *error);
QL_API void QL_CALL ql_log_get_config(ql_log_config_v1 *config);
QL_API ql_status QL_CALL ql_log_set_level(ql_log_level level, ql_error *error);
QL_API ql_log_level QL_CALL ql_log_get_level(void);

/* Per-category thresholds override the global level for records tagged with
   that exact category. Passing QL_LOG_LEVEL_OFF silences the category; passing
   a null category clears every override. */
QL_API ql_status QL_CALL ql_log_set_category_level(const char *category,
                                                   ql_log_level level,
                                                   ql_error *error);
QL_API ql_log_level QL_CALL ql_log_get_category_level(const char *category);

/* Installs the caller's sink. A null sink, or a sink with a null write
   callback, detaches output. The library never writes to stdout or stderr on
   its own: with no sink installed nothing is emitted. */
QL_API ql_status QL_CALL ql_log_set_sink(const ql_log_sink_v1 *sink,
                                         ql_error *error);

/* Restores the quiet default: level QL_LOG_LEVEL_OFF, default details, no
   sink, no category overrides, zeroed statistics. */
QL_API void QL_CALL ql_log_reset(void);

QL_API void QL_CALL ql_log_get_statistics(ql_log_statistics_v1 *statistics);

/* Exact per-record admission test, including the category override. The macros
   use QL_LOG_IS_ENABLED first so this is only reached for a level that already
   passed the cheap global check. */
QL_API uint32_t QL_CALL ql_log_is_enabled(ql_log_level level,
                                          const char *category);

QL_API void QL_CALL ql_log_write(ql_log_level level, const char *category,
                                 const char *file, const char *function,
                                 uint32_t line_number, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 6, 7)))
#endif
    ;

QL_API const char *QL_CALL ql_log_level_string(ql_log_level level);
QL_API ql_status QL_CALL ql_log_level_parse(const char *text,
                                            ql_log_level *level,
                                            ql_error *error);

#if defined(__GNUC__) || defined(__clang__)
#  define QL_LOG_FUNCTION __func__
#elif defined(_MSC_VER)
#  define QL_LOG_FUNCTION __FUNCTION__
#else
#  define QL_LOG_FUNCTION ""
#endif

/* Arguments are evaluated only when the level passes the global threshold. */
#define QL_LOG_AT(level, category, ...)                                    \
    do {                                                                   \
        if (QL_LOG_IS_ENABLED(level)) {                                    \
            ql_log_write((level), (category), __FILE__, QL_LOG_FUNCTION,   \
                         (uint32_t)__LINE__, __VA_ARGS__);                 \
        }                                                                  \
    } while (0)

/* Short spellings. The QL_LOG_<LEVEL> names are taken by the plugin host
   callback's original enumerator spellings in quodlibet/method.h. */
#define QL_LOGT(category, ...)                                             \
    QL_LOG_AT(QL_LOG_LEVEL_TRACE, (category), __VA_ARGS__)
#define QL_LOGD(category, ...)                                             \
    QL_LOG_AT(QL_LOG_LEVEL_DEBUG, (category), __VA_ARGS__)
#define QL_LOGI(category, ...)                                             \
    QL_LOG_AT(QL_LOG_LEVEL_INFO, (category), __VA_ARGS__)
#define QL_LOGW(category, ...)                                             \
    QL_LOG_AT(QL_LOG_LEVEL_WARN, (category), __VA_ARGS__)
#define QL_LOGE(category, ...)                                             \
    QL_LOG_AT(QL_LOG_LEVEL_ERROR, (category), __VA_ARGS__)
#define QL_LOGF(category, ...)                                             \
    QL_LOG_AT(QL_LOG_LEVEL_FATAL, (category), __VA_ARGS__)

QL_EXTERN_C_END

#endif
